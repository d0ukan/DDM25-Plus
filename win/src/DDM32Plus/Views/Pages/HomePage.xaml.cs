// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using DDM32Plus.Core;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>Ana sayfa (home_tab.py).</summary>
public partial class HomePage : UserControl, IPage
{
    private AppState? state;
    private MainWindow? window;

    public HomePage() => InitializeComponent();

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        this.window = window;
    }

    private Brush Res(string key) => (Brush)FindResource(key);

    /// <summary>('current' | 'update' | 'unknown' | 'none', text): home_tab.update_state.</summary>
    public void Refresh()
    {
        if (state is null)
            return;
        Subtitle.Text = Loc.F("Home_Subtitle", AppState.ProgramVersion, AppInfo.Author);
        var ok = state.DisclaimerAccepted;
        GateLabel.Visibility = GateButton.Visibility = ok ? Visibility.Collapsed : Visibility.Visible;
        var installed = state.Installed();
        var newest = state.Newest();
        string text;
        Brush color;
        var update = false;
        if (newest is null)
            (text, color) = (Loc.T("Home_StateNone"), Res("Grey"));
        else if (installed is null)
            (text, color) = (Loc.F("Home_StateUnknown", newest.Version), Res("Grey"));
        else if (newest.Key <= installed.Key)
            (text, color) = (Loc.T("Home_StateCurrent"), Res("Green"));
        else
            (text, color, update) = (Loc.F("Home_StateUpdate", newest.Version), Res("Amber"), true);
        StateLabel.Text = text;
        StateLabel.Foreground = color;
        var notes = update && newest is not null ? newest.Notes(Loc.Instance.Language) : "";
        if (state.AppUpdate() is { } app)
            notes = (notes.Length > 0 ? notes + "\n" : "") + Loc.F("Home_AppUpdate", app.Entry.Version);
        NotesLabel.Text = notes;
        NotesLabel.Visibility = notes.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        VersionLabel.Inlines.Clear();
        VersionLabel.Inlines.Add(Loc.F("Home_Installed", ""));
        VersionLabel.Inlines.Add(new System.Windows.Documents.Bold(new System.Windows.Documents.Run(installed is not null
            ? $"DDM32 Plus {installed.Version}"
            : state.Settings.GetString("installed_firmware") ?? Loc.T("Common_Unknown"))));
        BackupsLabel.Text = Loc.F("Home_Backups", state.Paths.Backups);
        PythonDataLabel.Text = string.Join("\n", state.PythonData.Select(d =>
            Loc.F("Home_PythonData", d.Backups, d.BackupFolders, d.VerifiedBackups)));
        PythonDataLabel.Visibility = state.PythonData.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnOpen(object sender, RoutedEventArgs e)
    {
        if (sender is Button { CommandParameter: string key })
            window?.OpenPage(key);
    }

    private void OnHelp(object sender, RoutedEventArgs e)
    {
        if (window is not null && state is not null)
            InfoDialog.ShowHelp(window, state);
    }

    private void OnDisclaimer(object sender, RoutedEventArgs e) => window?.ShowDisclaimer();
}
