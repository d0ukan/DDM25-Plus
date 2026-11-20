// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using DDM32Plus.Core;
using DDM32Plus.Core.Releases;
using DDM32Plus.Core.Update;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>One row of the version table.</summary>
public sealed record VersionRow(string Version, string Label, Brush LabelBrush, FontWeight LabelWeight, string State,
    string Notes, string NotesTip, FirmwareVersion Firmware, ReleaseLabel Kind);

/// <summary>Sürümler (versions_tab.py). Network access only on the check button or the start-up check.</summary>
public partial class VersionsPage : UserControl, IPage
{
    private AppState? state;
    private MainWindow? window;
    private bool updating;
    private string? appError;

    public VersionsPage() => InitializeComponent();

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        this.window = window;
    }

    private Brush Res(string key) => (Brush)FindResource(key);

    public void Refresh()
    {
        if (state is null)
            return;
        ColVersion.Header = Loc.T("Ver_ColVersion");
        ColLabel.Header = Loc.T("Ver_ColLabel");
        ColState.Header = Loc.T("Ver_ColState");
        ColNotes.Header = Loc.T("Ver_ColNotes");
        AutoBox.IsChecked = state.Settings.AutoCheck;
        BetaBox.IsChecked = state.Settings.IncludeBeta;
        CheckButton.Content = Loc.T(state.Checking ? "Ver_Checking" : "Ver_Check");
        CheckButton.IsEnabled = !state.Checking && !updating;

        var cat = state.Catalog;
        StatusText.Foreground = Brushes.Black;
        if (state.Checking)
            StatusText.Text = Loc.T("Ver_Checking");
        else if (state.LastCheckError is { } err)
        {
            StatusText.Text = Loc.F("Ver_CheckFailed", err);
            StatusText.Foreground = Res("Red");
        }
        else if (cat.Source == "github")
        {
            StatusText.Text = Loc.F("Ver_Checked", cat.Firmware.Count);
            StatusText.Foreground = Res("Green");
        }
        else
            StatusText.Text = "";

        var selected = (Table.SelectedItem as VersionRow)?.Version;
        var installed = state.Installed();
        var packages = state.Packages();
        var lang = Loc.Instance.Language;
        var rows = new List<VersionRow>();
        foreach (var fv in cat.Firmware)
        {
            var kind = ReleaseLabels.For(fv, cat.Firmware);
            var states = new List<string>();
            if (installed?.Version == fv.Version)
                states.Add(Loc.T("State_OnRadio"));
            if (kind == ReleaseLabel.Legacy)
                states.Add(Loc.T("State_Legacy"));
            else
                states.Add(Loc.T(packages.Any(p => p.Version == fv.Version && p.Installable) ? "State_Downloaded" : "State_NotDownloaded"));
            var brush = kind switch
            {
                ReleaseLabel.Latest => Res("Green"),
                ReleaseLabel.Beta => Res("AmberDark"),
                ReleaseLabel.TestAlpha => Res("Red"),
                ReleaseLabel.Legacy => Res("Grey"),
                _ => Brushes.Black,
            };
            rows.Add(new VersionRow(fv.Version, AppState.LabelText(kind), brush,
                kind == ReleaseLabel.Latest ? FontWeights.Bold : FontWeights.Normal, string.Join(", ", states),
                fv.Notes(lang), $"TR: {fv.Notes("tr")}\nEN: {fv.Notes("en")}", fv, kind));
        }
        Table.ItemsSource = rows;
        Table.SelectedItem = rows.FirstOrDefault(r => r.Version == selected);

        var newest = state.Newest();
        var cur = installed;
        if (newest is not null && (cur is null ? state.Settings.GetString("installed_sha256") is not null : newest.Key > cur.Key))
        {
            Banner.Text = Loc.F("Home_StateUpdate", newest.Version) + "\n" + newest.Notes(lang);
            Banner.Foreground = Res("Amber");
        }
        else
        {
            Banner.Text = rows.Count == 0 ? Loc.T("Ver_Empty") : "";
            Banner.Foreground = Brushes.Black;
        }
        Banner.Visibility = Banner.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;

        KeyText.Text = Loc.F("Ver_Key", ReleaseConstants.PublicKeyId);
        RejectedText.Text = cat.Rejected.Count > 0
            ? Loc.T("Ver_Rejected") + "\n" + string.Join("\n", cat.Rejected.Select(r => $"{r.Tag}: {r.Reason}"))
            : Loc.T("Ver_NoneRejected");

        var app = state.AppUpdate();
        var text = Loc.F("Ver_ThisProgram", AppState.ProgramVersion);
        if (app is not null)
            text += "\n" + Loc.F("Ver_NewProgram", app.Entry.Version) +
                    (app.Entry.Notes is { } n ? "\n" + n.For(lang) : "");
        if (!AppState.CanSelfUpdate)
            text += "\n" + Loc.T("Ver_DevCopy");
        if (appError is not null)
            text += "\n" + appError;
        AppLabel.Text = text;
        AppButton.IsEnabled = app is not null && AppState.CanSelfUpdate && !state.Checking && !updating;
        RefreshButtons();
    }

    private void RefreshButtons()
    {
        var row = Table.SelectedItem as VersionRow;
        SwitchButton.IsEnabled = row is not null && ReleaseLabels.Installable(row.Kind) && state?.Checking == false && !updating;
    }

    private async void OnCheck(object sender, RoutedEventArgs e)
    {
        if (state is not null)
            await state.CheckAsync();
    }

    private void OnAuto(object sender, RoutedEventArgs e)
    {
        if (state is not null)
            state.Settings.AutoCheck = AutoBox.IsChecked == true;
    }

    private void OnBeta(object sender, RoutedEventArgs e)
    {
        if (state is null)
            return;
        state.Settings.IncludeBeta = BetaBox.IsChecked == true;
        state.NotifyChanged();
    }

    private void OnSelected(object sender, SelectionChangedEventArgs e) => RefreshButtons();

    private void OnSwitch(object sender, RoutedEventArgs e)
    {
        if (Table.SelectedItem is not VersionRow row || window is null)
            return;
        if (row.Kind == ReleaseLabel.Legacy)
        {
            MessageBox.Show(window, Loc.F("Ver_LegacyNote", row.Version), AppInfo.Name);
            return;
        }
        if (ReleaseLabels.NeedsConfirmation(row.Kind) && MessageBox.Show(window, Loc.F("Ver_TestConfirm", row.Version),
                Loc.T("Ver_TestConfirmTitle"), MessageBoxButton.YesNo, MessageBoxImage.Warning,
                MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        window.Update.SelectVersion(row.Version);
    }

    /// <summary>The program's own update: confirm, download and verify, stage, start the helper, close.</summary>
    private async void OnUpdateApp(object sender, RoutedEventArgs e)
    {
        if (state?.AppUpdate() is not { } app || window is null || !AppState.CanSelfUpdate || updating)
            return;
        var text = Loc.F("Ver_UpdateConfirm", AppState.ProgramVersion, app.Entry.Version, Loc.T("Disc_Short"));
        if (MessageBox.Show(window, text, Loc.T("Ver_UpdateConfirmTitle"), MessageBoxButton.YesNo,
                MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        updating = true;
        appError = null;
        AppProgress.Visibility = Visibility.Visible;
        Refresh();
        try
        {
            var progress = new Progress<double>(v => AppProgress.Value = v);
            if (await state.StartProgramUpdateAsync(app, (d, t) =>
                    ((IProgress<double>)progress).Report(Math.Min(100, d * 100.0 / Math.Max(1, t)))))
            {
                window.Close();
                return;
            }
            appError = Loc.T("Ver_HelperFailed");
        }
        catch (Exception ex) when (ex is NetException or VerifyException or UpdateException or IOException
                                       or UnauthorizedAccessException or InvalidDataException
                                       or System.ComponentModel.Win32Exception)
        {
            appError = Loc.F("Ver_UpdateFailed", ex.Message);
        }
        updating = false;
        AppProgress.Visibility = Visibility.Collapsed;
        Refresh();
    }
}
