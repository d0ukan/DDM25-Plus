// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using DDM32Plus.Core.Storage;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>Geri dön (restore_tab.py): the texts and buttons; the restore itself comes in the next stage.</summary>
public partial class RestorePage : UserControl, IPage
{
    private AppState? state;

    public RestorePage() => InitializeComponent();

    public void Init(AppState state, MainWindow window) => this.state = state;

    public void Refresh()
    {
        if (state is null)
            return;
        var (all, verified) = AppPaths.CountBackups(state.Paths.Backups);
        _ = all;
        OwnBackups.Text = Loc.F("Res_OwnBackups", state.Paths.Backups, verified);
        PythonBackups.Text = string.Join("\n", state.PythonData.Select(d =>
            Loc.F("Home_PythonData", d.Backups, d.BackupFolders, d.VerifiedBackups)));
        PythonBackups.Visibility = state.PythonData.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }
}
