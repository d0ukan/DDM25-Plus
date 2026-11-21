// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using DDM32Plus.Core;
using DDM32Plus.Core.Radio;
using DDM32Plus.Core.Storage;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>
/// Geri dön (restore_tab.py): the user's own backup, its detection and restore plan (files only), then the restore
/// with one confirmation. A stock backup goes back with the proven stock restore (install_firstboot), a Classic-layout
/// backup with install_plus --restore. The radio tools get their port only through RadioAccess.
/// </summary>
public partial class RestorePage : UserControl, IPage
{
    private AppState? state;
    private ToolRunner? runner;
    private readonly RestoreFlow flow = new();
    private readonly DispatcherTimer countdown = new() { Interval = TimeSpan.FromSeconds(1) };
    private int remaining;
    private string? note;
    private ProgressTracker progress = new();

    public RestorePage()
    {
        InitializeComponent();
        countdown.Tick += (_, _) =>
        {
            remaining = Math.Max(0, remaining - 1);
            BannerCount.Text = Loc.F("Banner_Count", remaining / 60, remaining % 60);
            if (remaining == 0)
                countdown.Stop();
        };
    }

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        runner = new ToolRunner(Dispatcher);

    }

    public bool Busy => flow.Running is not null;

    private static string Msg((string Key, object?[] Args) m) => m.Key.Length == 0 ? "" : Loc.F(m.Key, m.Args);

    public void Refresh()
    {
        if (state is null)
            return;
        if (flow.Running != "restore")
            flow.Port = state.Port;
        CableText.Text = CableScanner.Text(state.Cable);
        var accessNote = AccessNotes.Current();
        AccessNote.Visibility = accessNote is null ? Visibility.Collapsed : Visibility.Visible;
        AccessTitle.Text = accessNote is null ? "" : Loc.T(accessNote.Value.Title);
        AccessText.Text = accessNote is null ? "" : Loc.T(accessNote.Value.Text);
        var free = flow.Running is null;
        PreviousButton.IsEnabled = StockButton.IsEnabled = OtherButton.IsEnabled = free;
        RestoreButton.IsEnabled = flow.CanRestore() && state.DisclaimerAccepted;
        var lines = new List<string>();
        if (note is not null)
            lines.Add(note);
        if (flow.BackupDir is not null)
            lines.Add(Loc.F("Res_Chosen", Path.GetFileName(flow.BackupDir)));
        if (flow.PlanData is { } p)
            lines.Add(Loc.F("Upd_PlanSummary", p["changed_sectors"], p["first"] ?? "-", p["last"] ?? "-"));
        if (Msg(flow.Message) is { Length: > 0 } m)
            lines.Add(m);
        Status.Text = string.Join("\n", lines);
        DetailText.Text = flow.PlanData is not null ? PyJson.Dumps(flow.PlanData) : flow.Detection is not null ? PyJson.Dumps(flow.Detection) : "";
        var (_, verified) = AppPaths.CountBackups(state.Paths.Backups);
        OwnBackups.Text = Loc.F("Res_OwnBackups", state.Paths.Backups, verified);
        PythonBackups.Text = string.Join("\n", state.PythonData.Select(d => Loc.F("Home_PythonData", d.Backups, d.BackupFolders, d.VerifiedBackups)));
        PythonBackups.Visibility = state.PythonData.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private string? DefaultBackup(string kind)
    {
        var cands = kind == "previous" ? [state!.Settings.GetString("restore_previous_backup") ?? ""] : state!.StockBackups();
        return cands.Select(state.Paths.MapLegacy).FirstOrDefault(c => c.Length > 0 && AppPaths.IsVerifiedBackup(c));
    }

    private async Task Choose(string kind, string? folder = null)
    {
        if (flow.Running is not null || state is null)
            return;
        folder ??= DefaultBackup(kind);
        if (folder is null)
        {
            note = Loc.T("Res_NoBackup") + "\n" + Loc.T(kind == "previous" ? "Res_NoPrevious" : "Res_NoStock");
            Refresh();
            return;
        }
        note = null;
        flow.Choose(kind, folder);
        Result.Text = "";
        Progress.Value = 0;
        flow.StartDetect();
        Refresh();
        try
        {
            flow.DetectFinished(await Task.Run(() => FileSteps.DetectBackup(folder, Codec.Dm32)));
        }
        catch (Exception e) when (e is ToolError or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            flow.DetectFinished(null, e.Message);
        }
        if (flow.CanPlan() && flow.Tool is { } tool)
        {
            flow.StartPlan();
            try
            {
                var plan = await Task.Run(() => FileSteps.RestorePlan(tool.Mode, folder));
                flow.PlanFinished(plan, safe: FileSteps.Safe(plan));
            }
            catch (Exception e) when (e is ToolError or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
            {
                flow.PlanFinished(null, e.Message);
            }
        }
        Refresh();
    }

    private async void OnPrevious(object sender, RoutedEventArgs e) => await Choose("previous");

    private async void OnStock(object sender, RoutedEventArgs e) => await Choose("stock");

    private async void OnOther(object sender, RoutedEventArgs e)
    {
        if (state is null)
            return;
        var dlg = new Microsoft.Win32.OpenFolderDialog { InitialDirectory = state.Paths.Backups, Title = Loc.T("Res_Other") };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true)
            return;
        if (!AppPaths.IsVerifiedBackup(dlg.FolderName))
        {
            MessageBox.Show(Window.GetWindow(this)!, Loc.T("Upd_NotVerifiedBackup"), AppInfo.Name, MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        await Choose(flow.Kind == "stock" ? "stock" : "previous", dlg.FolderName);
    }

    private void OnLine(string line)
    {
        LogText.AppendText(line + "\n");
        LogText.ScrollToEnd();
        if (ToolRunner.Ready(line) is { } r)
        {
            remaining = r.Seconds;
            BannerCount.Text = Loc.F("Banner_Count", remaining / 60, remaining % 60);
            Banner.Visibility = Visibility.Visible;
            countdown.Start();
        }
        else if (line.StartsWith("boot ROM answered", StringComparison.Ordinal))
        {
            countdown.Stop();
            Banner.Visibility = Visibility.Collapsed;
        }
        if (progress.Feed(line) is { } s)
        {
            Progress.Value = s.Percent;
            Result.Text = Loc.F(s.Key, s.Percent);
        }
    }

    private async void OnRestore(object sender, RoutedEventArgs e)
    {
        if (state is null || runner is null || !flow.CanRestore() || flow.Tool is not { } tool || !state.DisclaimerAccepted)
            return;
        var text = Loc.F("Res_Confirm", Loc.F("Res_Chosen", flow.BackupDir), Status.Text, Loc.T("Disc_Short"));
        if (MessageBox.Show(Window.GetWindow(this)!, text, Loc.T("Res_ConfirmTitle"), MessageBoxButton.YesNo, MessageBoxImage.Warning,
                MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        var outDir = AppPaths.UniqueFolder(state.Paths.Backups, $"restore-{AppPaths.Today()}");
        flow.StartRestore();
        progress = new ProgressTracker();
        Progress.Value = 0;
        LogText.Clear();
        Refresh();
        List<string> argv = ["--port", flow.Port!, "--backup", flow.BackupDir!, "--out", outDir, "--wait", "600", .. tool.Args];
        Action<IReadOnlyList<string>, ToolContext> run = tool.Tool == "install_firstboot" ? RestoreStockTool.Run : InstallTool.Run;
        var end = await runner.RunAsync(run, argv, outDir + ".log", OnLine, writes: true);
        countdown.Stop();
        Banner.Visibility = Visibility.Collapsed;
        if (end.AccessOff)
        {
            flow.RestoreFinished(new StepResult(false, false, false, Loc.T("Radio_DisabledTitle")));
            MessageBox.Show(Window.GetWindow(this)!, Loc.T(RadioAccess.IsOn ? "Radio_WritesDisabled" : "Radio_Disabled"),
                Loc.T("Radio_DisabledTitle"), MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            flow.RestoreFinished(StepResult.Install(outDir, end.Error));
            if (flow.Restore == StepState.Ok)
            {
                state.Settings.Save("installed_sha256", null);
                state.Settings.Save("installed_firmware", flow.Kind == "stock" ? "stock" : "");
                state.NotifyChanged();
            }
        }
        Result.Text = Msg(flow.Message);
        Refresh();
    }
}
