// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using DDM32Plus.Core.Releases;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>
/// Telsizi güncelle (update_tab.py). This stage: Hazırlık works (the signed version list, download and verify; no
/// radio); the radio steps (Yedek, Kontrol, Yükle) are shown with their texts and come in the next stage.
/// </summary>
public partial class UpdatePage : UserControl, IPage
{
    private static readonly string[] StepKeys = ["Step_Prep", "Step_Backup", "Step_Check", "Step_Install", "Step_Done"];
    private AppState? state;
    private MainWindow? window;
    private int page;
    private bool downloading;
    private bool reloading;
    private string? message;
    private List<FirmwareVersion> choices = [];

    public UpdatePage() => InitializeComponent();

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        this.window = window;
    }

    private StackPanel[] Pages => [PagePrep, PageBackup, PageCheck, PageInstall, PageDone];

    public void Refresh()
    {
        if (state is null)
            return;
        Header.Inlines.Clear();
        for (var i = 0; i < StepKeys.Length; i++)
        {
            if (i > 0)
                Header.Inlines.Add(new Run("  ›  "));
            var run = new Run($"{i + 1}. {Loc.T(StepKeys[i])}");
            Header.Inlines.Add(i == page ? new Bold(new Underline(run)) : run);
        }
        TextBlock[] titles = [TitlePrep, TitleBackup, TitleCheck, TitleInstall, TitleDone];
        for (var i = 0; i < titles.Length; i++)
            titles[i].Text = $"{i + 1}. {Loc.T(StepKeys[i])}";
        for (var i = 0; i < Pages.Length; i++)
            Pages[i].Visibility = i == page ? Visibility.Visible : Visibility.Collapsed;
        BackButton.IsEnabled = page is > 0 and < 4 && !downloading;
        BackupFolder.Text = state.Paths.Backups;
        BannerCount.Text = Loc.F("Banner_Count", 2, 0);
        ReloadVersions();
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
        ShowSelected();
    }

    private FirmwareVersion? Selected() =>
        VersionCombo.SelectedIndex >= 0 && VersionCombo.SelectedIndex < choices.Count ? choices[VersionCombo.SelectedIndex] : null;

    private void ShowSelected()
    {
        var fv = Selected();
        DownloadButton.IsEnabled = fv is not null && !downloading;
        PrepContinue.IsEnabled = fv is not null && !downloading;
        VersionCombo.IsEnabled = !downloading;
        if (fv is null)
        {
            VersionLabel.Text = Loc.T("Upd_NoVersions");
            ShaText.Text = "-";
            SignatureText.Text = "-";
            return;
        }
        ShaText.Text = fv.ApplicationSha256;
        SignatureText.Text = Loc.F("Upd_SignedList", ReleaseConstants.PublicKeyId);
        var pkg = state!.Packages().FirstOrDefault(p => p.Version == fv.Version && p.Installable);
        VersionLabel.Text = message ?? (pkg is not null
            ? Loc.F("Upd_Downloaded", pkg.Folder)
            : fv.Notes(Loc.Instance.Language));
    }

    private void OnVersionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (reloading)
            return;
        message = null;
        if (state is not null)
            ShowSelected();
    }

    /// <summary>Download and verify (no radio): the zip's SHA-256 and the application against the signed list.</summary>
    private async void OnDownload(object sender, RoutedEventArgs e)
    {
        var fv = Selected();
        if (fv is null || state is null || downloading)
            return;
        var label = ReleaseLabels.For(fv, state.Catalog.Firmware);
        if (ReleaseLabels.NeedsConfirmation(label) && MessageBox.Show(Window.GetWindow(this)!,
                Loc.F("Ver_TestConfirm", fv.Version), Loc.T("Ver_TestConfirmTitle"), MessageBoxButton.YesNo,
                MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        downloading = true;
        message = null;
        DownloadProgress.Value = 0;
        DownloadProgress.Visibility = Visibility.Visible;
        Refresh();
        try
        {
            var progress = new Progress<(long Done, long Total)>(p =>
                DownloadProgress.Value = Math.Min(100, p.Done * 100 / Math.Max(1, p.Total)));
            var folder = await ReleaseClient.DownloadFirmwareAsync(fv, state.Paths.Releases, state.Net,
                (d, t) => ((IProgress<(long, long)>)progress).Report((d, t)));
            message = Loc.F("Upd_Downloaded", folder);
        }
        catch (Exception ex) when (ex is NetException or VerifyException or ManifestException or IOException
                                       or UnauthorizedAccessException or InvalidDataException)
        {
            message = Loc.F("Upd_DownloadFailed", ex.Message);
        }
        finally
        {
            downloading = false;
            DownloadProgress.Visibility = Visibility.Collapsed;
            Refresh();
        }
    }

    private void OnPrepContinue(object sender, RoutedEventArgs e)
    {
        page = 1;
        Refresh();
    }

    private void OnBack(object sender, RoutedEventArgs e)
    {
        if (page > 0)
            page--;
        Refresh();
    }

    /// <summary>"Bu sürüme geç" on the versions page: pick it here and show the first step.</summary>
    public void SelectVersion(string version)
    {
        var i = choices.FindIndex(f => f.Version == version);
        if (i >= 0)
            VersionCombo.SelectedIndex = i;
        page = 0;
        Refresh();
        window?.OpenPage("update");
    }
}
