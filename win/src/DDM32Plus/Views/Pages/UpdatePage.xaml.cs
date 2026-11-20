// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Threading;
using DDM32Plus.Core;
using DDM32Plus.Core.Radio;
using DDM32Plus.Core.Releases;
using DDM32Plus.Core.Storage;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>
/// Telsizi güncelle (update_tab.py): Hazırlık (cable, signed version, download and verify), Yedek (the two-pass
/// backup, or a verified backup already on disk), Kontrol (what the radio runs and the write plan, from the backup:
/// no port), Yükle (one confirmation, then the install), Bitti. The rules are Core.Radio.Wizard's: a radio step
/// starts only from its own button and is never retried by itself. The radio tools get their port only through
/// RadioAccess: in a build without radio access they stop before opening one.
/// </summary>
public partial class UpdatePage : UserControl, IPage
{
    private const int Prep = 0, BackupStep = 1, Check = 2, InstallStep = 3, Done = 4;
    private static readonly string[] StepKeys = ["Step_Prep", "Step_Backup", "Step_Check", "Step_Install", "Step_Done"];
    private AppState? state;
    private MainWindow? window;
    private ToolRunner? runner;
    private readonly Wizard wizard = new();
    private int page;
    private bool downloading, reloading;
    private string? downloadMessage, installDir, stepMessage;
    private List<FirmwareVersion> choices = [];
    private readonly DispatcherTimer countdown = new() { Interval = TimeSpan.FromSeconds(1) };
    private int remaining;
    private string? lastBackup;
    private DateTime lastBackupDate;
    private bool lookedForLast, lookingForLast;

    public UpdatePage()
    {
        InitializeComponent();
        countdown.Tick += (_, _) =>
        {
            remaining = Math.Max(0, remaining - 1);
            ShowCount();
            if (remaining == 0)
                countdown.Stop();
        };
    }

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        this.window = window;
        runner = new ToolRunner(Dispatcher);
        FirmwareNote.Text = state.Settings.GetString("installed_firmware") ?? "";
    }

    public bool Busy => wizard.Running is not null || downloading;

    private FirmwareVersion? Selected() =>
        VersionCombo.SelectedIndex >= 0 && VersionCombo.SelectedIndex < choices.Count ? choices[VersionCombo.SelectedIndex] : null;

    private Package? SelectedPackage()
    {
        var fv = Selected();
        return fv is null ? null : state!.Packages().FirstOrDefault(p => p.Version == fv.Version && p.Installable);
    }

    private string Msg((string Key, object?[] Args) m) => m.Key.Length == 0 ? "" : Loc.F(m.Key, m.Args);

    // --- drawing ---------------------------------------------------------------------------------------------------
    public void Refresh()
    {
        if (state is null)
            return;
        if (wizard.Running is null)
        {
            wizard.SetPort(state.Port);
            if (!downloading)
                ReloadVersions();
        }
        Header.Inlines.Clear();
        for (var i = 0; i < StepKeys.Length; i++)
        {
            if (i > 0)
                Header.Inlines.Add(new Run("  ›  "));
            var run = new Run($"{i + 1}. {Loc.T(StepKeys[i])}");
            Header.Inlines.Add(i == page ? new Bold(new Underline(run)) : run);
        }
        TextBlock[] titles = [TitlePrep, TitleBackup, TitleCheck, TitleInstall, TitleDone];
        StackPanel[] pages = [PagePrep, PageBackup, PageCheck, PageInstall, PageDone];
        for (var i = 0; i < pages.Length; i++)
        {
            titles[i].Text = $"{i + 1}. {Loc.T(StepKeys[i])}";
            pages[i].Visibility = i == page ? Visibility.Visible : Visibility.Collapsed;
        }
        var gate = state.DisclaimerAccepted;
        AccessTitle.Text = Loc.T(RadioAccess.IsOn ? "Dev_NoteTitle" : "Radio_DisabledTitle");
        AccessText.Text = Loc.T(RadioAccess.IsOn ? "Dev_Note" : "Radio_Disabled");
        CableText.Text = CableScanner.Text(state.Cable);
        CableText.Foreground = (System.Windows.Media.Brush)FindResource(state.Cable.State == CableState.Ok ? "Green" : "Red");
        PortCombo.Visibility = state.Cable.Ports.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
        if (state.Cable.Ports.Count > 1 && !PortCombo.Items.Cast<string>().SequenceEqual(state.Cable.Ports))
        {
            PortCombo.ItemsSource = state.Cable.Ports.ToList();
            PortCombo.SelectedItem = state.Port;
        }
        var w = wizard;
        BackButton.IsEnabled = w.Running is null && page is BackupStep or Check or InstallStep && !w.Locked && w.Install != StepState.Ok;
        DownloadButton.IsEnabled = Selected() is not null && !downloading && w.Running is null;
        VersionCombo.IsEnabled = !downloading && w.Running is null;
        PrepContinue.IsEnabled = SelectedPackage() is not null && w.Port is not null && gate && !downloading && w.Running is null;
        BackupButton.IsEnabled = gate && w.CanBackup();
        ExistingButton.IsEnabled = w.Running is null;
        QuickButton.Visibility = lastBackup is not null ? Visibility.Visible : Visibility.Collapsed;
        QuickButton.Content = Loc.F("Upd_QuickBackup", lastBackupDate.ToString("yyyy-MM-dd HH:mm", System.Globalization.CultureInfo.InvariantCulture));
        QuickButton.IsEnabled = gate && w.Running is null;
        QuickText.Text = lookingForLast ? Loc.T("Upd_QuickLooking") : lastBackup is null ? Loc.T("Upd_QuickNone") : "";
        NoBackupButton.IsEnabled = gate && UnderstandBox.IsChecked == true && w.Running is null;
        BackupStatus.Text = string.Join("\n", new[] { w.Port is null ? Loc.T("Upd_NoCable") : "", Msg(w.BackupMessage) }.Where(x => x.Length > 0));
        RecheckButton.IsEnabled = w.CanDetect();
        DetectStatus.Text = w.Path == BackupPath.None ? Loc.T("Upd_NoBackupTitle") : DetectionText(w.Detection);
        CheckStatus.Text = w.Path == BackupPath.None ? Loc.T("Upd_NoBackupCheck")
            : w.Path == BackupPath.Quick && w.PlanData is not null ? PlanText() + "\n" + Loc.T("Upd_QuickCheck") : PlanText();
        CheckContinue.IsEnabled = (w.Plan == StepState.Ok || (w.Path == BackupPath.None && w.Backup == StepState.Skipped)) && w.Running is null;
        InstallButton.IsEnabled = gate && w.CanInstall();
        InstallStatus.Text = stepMessage ?? Msg(w.Message);
        ToCheckButton.Visibility = w.Install == StepState.Idle && w.Plan == StepState.Idle && w.InstallResult is not null
            ? Visibility.Visible : Visibility.Collapsed;
        ToRestoreButton.Visibility = w.Locked ? Visibility.Visible : Visibility.Collapsed;
        WarnText.Text = Warnings();
        FolderText.Text = string.Join("\n", new[] { w.BackupDir, installDir }.Where(x => x is not null));
        DetailText.Text = w.PlanData is not null ? PyJson.Dumps(w.PlanData) : w.Detection is not null ? PyJson.Dumps(w.Detection) : "";
        ShowSelected();
    }

    private void ReloadVersions()
    {
        var current = (VersionCombo.SelectedItem as ComboBoxItem)?.Tag as string;
        reloading = true;
        choices = [.. state!.Catalog.Firmware.Where(f => !f.IsLegacy)];
        VersionCombo.Items.Clear();
        foreach (var fv in choices)
        {
            var label = AppState.LabelText(ReleaseLabels.For(fv, state.Catalog.Firmware));
            VersionCombo.Items.Add(new ComboBoxItem { Content = $"DDM32 Plus {fv.Version}  ({label})", Tag = fv.Version });
        }
        var pick = choices.FindIndex(f => f.Version == current);
        if (pick < 0 && state.Newest() is { } n)
            pick = choices.FindIndex(f => f.Version == n.Version);
        VersionCombo.SelectedIndex = pick >= 0 ? pick : (choices.Count > 0 ? 0 : -1);
        reloading = false;
        if (wizard.Running is null)
            wizard.SetPackage(SelectedPackage());
    }

    private void ShowSelected()
    {
        var fv = Selected();
        if (fv is null)
        {
            VersionLabel.Text = Loc.T("Upd_NoVersions");
            ShaText.Text = "";
            SignatureText.Text = "";
            return;
        }
        ShaText.Text = fv.ApplicationSha256;
        SignatureText.Text = Loc.F("Upd_SignedList", ReleaseConstants.PublicKeyId);
        var pkg = SelectedPackage();
        VersionLabel.Text = downloadMessage ?? (pkg is not null ? Loc.F("Upd_Downloaded", pkg.Folder)
            : fv.Notes(Loc.Instance.Language) + "\n" + Loc.T("Upd_DownloadFirst"));
    }

    /// <summary>update_tab.detection_text: one plain sentence for the detection.</summary>
    private static string DetectionText(PyDict? d)
    {
        if (d is null)
            return Loc.T("Upd_DetectUnknown");
        if (d["layout"] as string == "stock")
            return Loc.F("Upd_DetectStock", d["stock_version"] ?? "?", d["stock_tested"] is true ? "" : Loc.T("Upd_DetectStockUntested"));
        if (d["layout"] as string == "classic")
        {
            var name = d["firmware"] switch
            {
                "ddm32" => "DDM32 Plus",
                "classic" => "Open25-DM32 Classic",
                _ => Loc.T("Upd_DetectClassicLayout"),
            };
            return Loc.F("Upd_DetectClassic", name);
        }
        return Loc.F("Upd_DetectUnsupported", d["reason"] as string ?? "");
    }

    private string PlanText()
    {
        var p = wizard.PlanData;
        if (p is null)
            return wizard.Running == "plan" ? Loc.T("Wiz_Planning")
                : wizard.DetectState == StepState.Ok && wizard.Package is null ? Loc.T("Upd_DownloadFirst") : Msg(wizard.Message);
        var text = Loc.F("Upd_PlanSummary", p["changed_sectors"], p["first"] ?? "-", p["last"] ?? "-");
        if (p.ContainsKey("slot_sectors"))
            text += "\n" + Loc.F("Upd_PlanSlot", p["slot_sectors"]);
        return text;
    }

    private string Warnings()
    {
        var fv = Selected();
        var lines = new List<string>();
        if (wizard.Path == BackupPath.None)
            lines.Add(Loc.T("Upd_NoBackupTitle") + ": " + Loc.T("Upd_NoBackupWarning"));
        if (fv is { IsBeta: true })
            lines.Add(Loc.T("Label_Beta") + ": " + fv.Notes(Loc.Instance.Language));
        if (wizard.Detection is { } d && d["layout"] as string == "stock")
            lines.Add(DetectionText(d));
        return string.Join("\n", lines);
    }

    private void ShowCount() => BannerCount.Text = Loc.F("Banner_Count", remaining / 60, remaining % 60);

    private void OnLine(string line)
    {
        LogText.AppendText(line + "\n");
        LogText.ScrollToEnd();
        if (ToolRunner.Ready(line) is { } r)
        {
            remaining = r.Seconds;
            ShowCount();
            Banner.Visibility = Visibility.Visible;
            countdown.Start();
        }
        else if (line.StartsWith("boot ROM answered", StringComparison.Ordinal))
        {
            countdown.Stop();
            Banner.Visibility = Visibility.Collapsed;
        }
        if (ToolRunner.BackupProgress(line) is { } b)
            BackupProgress.Value = b;
        if (ToolRunner.StepProgress(line) is { } s)
            InstallProgress.Value = s;
    }

    private void EndBanner()
    {
        countdown.Stop();
        Banner.Visibility = Visibility.Collapsed;
    }

    // --- 1. Hazırlık ------------------------------------------------------------------------------------------------
    private void OnRefreshCable(object sender, RoutedEventArgs e) => state?.RefreshCable();

    private void OnPortChanged(object sender, SelectionChangedEventArgs e)
    {
        if (state is not null && PortCombo.SelectedItem is string p && p != state.Port && wizard.Running is null)
            state.SetPort(p);
    }

    private void OnVersionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (reloading || state is null)
            return;
        downloadMessage = null;
        if (wizard.Running is null)
            wizard.SetPackage(SelectedPackage());
        Refresh();
    }

    private async void OnDownload(object sender, RoutedEventArgs e)
    {
        var fv = Selected();
        if (fv is null || state is null || downloading)
            return;
        if (ReleaseLabels.NeedsConfirmation(ReleaseLabels.For(fv, state.Catalog.Firmware)) && MessageBox.Show(Window.GetWindow(this)!,
                Loc.F("Ver_TestConfirm", fv.Version), Loc.T("Ver_TestConfirmTitle"), MessageBoxButton.YesNo,
                MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        downloading = true;
        downloadMessage = null;
        DownloadProgress.Value = 0;
        DownloadProgress.Visibility = Visibility.Visible;
        Refresh();
        try
        {
            var progress = new Progress<double>(v => DownloadProgress.Value = v);
            var folder = await ReleaseClient.DownloadFirmwareAsync(fv, state.Paths.Releases, state.Net,
                (d, t) => ((IProgress<double>)progress).Report(Math.Min(100, d * 100.0 / Math.Max(1, t))));
            downloadMessage = Loc.F("Upd_Downloaded", folder);
        }
        catch (Exception ex) when (ex is NetException or VerifyException or ManifestException or IOException
                                       or UnauthorizedAccessException or InvalidDataException)
        {
            downloadMessage = Loc.F("Upd_DownloadFailed", ex.Message);
        }
        finally
        {
            downloading = false;
            DownloadProgress.Visibility = Visibility.Collapsed;
            if (wizard.Running is null)
                wizard.SetPackage(SelectedPackage());
            Refresh();
        }
    }

    private void OnPrepContinue(object sender, RoutedEventArgs e) => Show(BackupStep);

    // --- 2. Yedek ---------------------------------------------------------------------------------------------------
    private async void OnBackup(object sender, RoutedEventArgs e)
    {
        if (state is null || runner is null || !wizard.CanBackup() || !state.DisclaimerAccepted)
            return;
        var note = FirmwareNote.Text.Trim();
        state.Settings.Save("installed_firmware", note);
        var parent = state.Paths.Backups;
        if (RadioAccess.IsOn)
        {
            // developer build: the backup goes where the owner chooses (to compare it with the Python tool's)
            var dlg = new Microsoft.Win32.OpenFolderDialog { InitialDirectory = parent, Title = Loc.T("Dev_ChooseFolder") };
            if (dlg.ShowDialog(Window.GetWindow(this)) != true)
                return;
            parent = dlg.FolderName;
            state.Settings.RememberBackupFolder(parent);     // the quick update looks there too
        }
        var outDir = AppPaths.UniqueFolder(parent, $"dm32-{AppPaths.Today()}");
        wizard.StartBackup(outDir);
        stepMessage = null;
        LogText.Clear();
        BackupProgress.Value = 0;
        Refresh();
        var end = await runner.RunAsync(BackupTool.Run,
            ["--port", wizard.Port!, "--out", outDir, "--installed-firmware", note.Length > 0 ? note : "?", "--wait", "120"],
            Path.Combine(parent, $"backup-{Path.GetFileName(outDir)}.log"), OnLine, writes: false);
        EndBanner();
        if (end.AccessOff)
        {
            wizard.BackupFinished(new StepResult(false, false, false, Loc.T("Radio_DisabledTitle")));
            MessageBox.Show(Window.GetWindow(this)!, Loc.T("Radio_Disabled"), Loc.T("Radio_DisabledTitle"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
            wizard.BackupFinished(StepResult.Backup(outDir, end.Error));
        if (wizard.Backup == StepState.Ok)
        {
            await DetectAndPlan();
            // the backup just taken is the last backup of this radio: offer it right away
            if (Wizard.IsDdm32(wizard.Detection))
            {
                lastBackup = outDir;
                lastBackupDate = Directory.GetLastWriteTime(outDir);
                lookedForLast = true;
            }
        }
        Refresh();
    }

    private async void OnExistingBackup(object sender, RoutedEventArgs e)
    {
        if (state is null || wizard.Running is not null)
            return;
        var dlg = new Microsoft.Win32.OpenFolderDialog { InitialDirectory = state.Paths.Backups, Title = Loc.T("Upd_ExistingBackup") };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true)
            return;
        if (!AppPaths.IsVerifiedBackup(dlg.FolderName))
        {
            MessageBox.Show(Window.GetWindow(this)!, Loc.T("Upd_NotVerifiedBackup"), AppInfo.Name, MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        wizard.UseExistingBackup(dlg.FolderName);
        await DetectAndPlan();
        Show(Check);
    }

    // --- 3. Kontrol: files only, no port ---------------------------------------------------------------------------
    private async Task DetectAndPlan()
    {
        if (state is null || !wizard.CanDetect())
            return;
        wizard.StartDetect();
        Refresh();
        var backup = wizard.BackupDir!;
        try
        {
            var d = await Task.Run(() => FileSteps.DetectBackup(backup, Codec.Dm32));
            wizard.DetectFinished(d);
            if (d["layout"] as string == "stock")
                state.RememberStockBackup(backup);
        }
        catch (Exception ex) when (ex is ToolError or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            wizard.DetectFinished(null, ex.Message);
        }
        if (wizard.CanPlan())
        {
            wizard.StartPlan();
            Refresh();
            var mode = wizard.Mode!;
            var plus = SelectedPackage()!.Folder;
            try
            {
                var plan = await Task.Run(() => FileSteps.Plan(mode, backup, plus, null, Codec.Dm32));
                wizard.PlanFinished(plan, safe: FileSteps.Safe(plan));
            }
            catch (Exception ex) when (ex is ToolError or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            {
                wizard.PlanFinished(null, ex is ToolError t ? t.Record : ex.Message);
            }
        }
        Refresh();
    }

    private async void OnRecheck(object sender, RoutedEventArgs e) => await DetectAndPlan();

    private void OnCheckContinue(object sender, RoutedEventArgs e) => Show(InstallStep);

    // --- 4. Yükle ----------------------------------------------------------------------------------------------------
    private async void OnInstall(object sender, RoutedEventArgs e)
    {
        var pkg = SelectedPackage();
        if (state is null || runner is null || pkg is null || !wizard.CanInstall() || !state.DisclaimerAccepted)
            return;
        var text = Loc.F("Upd_Confirm", pkg.Version, wizard.Path == BackupPath.None ? Loc.T("Upd_NoBackupWarning") : CheckStatus.Text,
            wizard.BackupDir is null ? "-" : Path.GetFileName(wizard.BackupDir), wizard.Port, Loc.T("Disc_Short"));
        if (MessageBox.Show(Window.GetWindow(this)!, text, Loc.T("Upd_ConfirmTitle"), MessageBoxButton.YesNo, MessageBoxImage.Warning,
                MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        // the install without backup asks a second time
        if (wizard.Path == BackupPath.None && MessageBox.Show(Window.GetWindow(this)!, Loc.F("Upd_NoBackupSure", Loc.T("Disc_Short")),
                Loc.T("Upd_NoBackupSureTitle"), MessageBoxButton.YesNo, MessageBoxImage.Stop, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        installDir = AppPaths.UniqueFolder(state.Paths.Backups, $"install-{AppPaths.Today()}");
        var (toolName, argv) = wizard.InstallCommand(pkg.Folder, installDir);
        var path = wizard.Path;
        wizard.StartInstall();
        stepMessage = null;
        LogText.Clear();
        InstallProgress.Value = 0;
        Refresh();
        Action<IReadOnlyList<string>, ToolContext> tool = toolName == "fast_install" ? FastInstallTool.Run : InstallTool.Run;
        var end = await runner.RunAsync(tool, argv, installDir + ".log", OnLine, writes: true);
        EndBanner();
        if (end.AccessOff)
        {
            wizard.InstallFinished(new StepResult(false, false, false, Loc.T("Radio_DisabledTitle")));
            MessageBox.Show(Window.GetWindow(this)!, Loc.T(RadioAccess.IsOn ? "Radio_WritesDisabled" : "Radio_Disabled"),
                Loc.T("Radio_DisabledTitle"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            var result = StepResult.Install(installDir, end.Error);
            wizard.InstallFinished(result);
            if (result.Passed)
            {
                state.Settings.Save("installed_sha256", pkg.Sha256);
                if (path != BackupPath.None)
                    state.Settings.Save("restore_previous_backup", wizard.BackupDir);
                state.Settings.Save("installed_firmware", $"DDM32 Plus {pkg.Version}");
                state.NotifyChanged();
                Show(Done);
                return;
            }
            if (result.WritesStarted && path != BackupPath.None)
                state.Settings.Save("restore_previous_backup", wizard.BackupDir);
        }
        Refresh();
    }

    private void OnToCheck(object sender, RoutedEventArgs e) => Show(Check);

    private void OnToRestore(object sender, RoutedEventArgs e) => window?.OpenPage("restore");

    // --- the faster paths (over an installed DDM32 Plus only) ----------------------------------------------------
    /// <summary>The newest verified backup (this program's and the Python program's) that holds DDM32 Plus: offered
    /// for the quick update. The tool checks live that the radio is the same before it writes.</summary>
    private async Task FindLastBackup()
    {
        if (state is null || lookedForLast || lookingForLast)
            return;
        lookingForLast = true;
        Refresh();
        var roots = new List<string> { state.Paths.Backups };
        roots.AddRange(state.Settings.BackupFolders());
        roots.AddRange(state.PythonData.Select(d => d.Backups));
        var found = await Task.Run(() => BackupFinder.Last(roots, Codec.Dm32));
        lastBackup = found;
        if (found is not null)
            lastBackupDate = Directory.GetLastWriteTime(found);
        lookingForLast = false;
        lookedForLast = true;
        Refresh();
    }

    private async void OnQuickBackup(object sender, RoutedEventArgs e)
    {
        if (lastBackup is null || wizard.Running is not null)
            return;
        wizard.UseQuickBackup(lastBackup);
        await DetectAndPlan();
        Show(Check);
    }

    private void OnUnderstand(object sender, RoutedEventArgs e) => Refresh();

    private void OnNoBackup(object sender, RoutedEventArgs e)
    {
        if (UnderstandBox.IsChecked != true || wizard.Running is not null)
            return;
        wizard.ChooseNoBackup(true);
        Show(Check);
    }

    // --- navigation --------------------------------------------------------------------------------------------------
    private void Show(int index)
    {
        if (wizard.Running is not null && index != page)
            return;
        page = index;
        Refresh();
        if (index == BackupStep)
            _ = FindLastBackup();
    }

    private void OnBack(object sender, RoutedEventArgs e)
    {
        if (page > 0 && wizard.Running is null)
            Show(page - 1);
    }

    /// <summary>"Bu sürüme geç" on the versions page: pick it here and show the first step.</summary>
    public void SelectVersion(string version)
    {
        if (wizard.Running is not null)
            return;
        var i = choices.FindIndex(f => f.Version == version);
        if (i >= 0)
            VersionCombo.SelectedIndex = i;
        page = Prep;
        Refresh();
        window?.OpenPage("update");
    }

    /// <summary>A verified backup on disk, as "Mevcut yedeği seç..." takes it (then detect and plan: files only).</summary>
    public async void UseBackup(string folder)
    {
        if (wizard.Running is not null || !AppPaths.IsVerifiedBackup(folder))
            return;
        wizard.UseExistingBackup(folder);
        await DetectAndPlan();
    }

    /// <summary>For screenshots and tests of the layout: show a step.</summary>
    public void ShowStep(int index) => Show(Math.Clamp(index, Prep, Done));
}
