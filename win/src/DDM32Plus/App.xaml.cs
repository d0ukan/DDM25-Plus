// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Threading;
using DDM32Plus.Core.Update;
using DDM32Plus.Localization;
using DDM32Plus.Services;
using DDM32Plus.Views;

namespace DDM32Plus;

/// <summary>
/// Start-up: DDM32Plus.exe --self-update ... is the update helper (no window). Otherwise: the language question at
/// the first start, then the main window; the disclaimer comes first when it was not accepted yet.
/// --update-check FILE: started by the update helper; the marker FILE is written once the window is up.
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var args = e.Args.ToList();
        if (args.Count > 0 && args[0] == SelfUpdate.HelperArg)
        {
            Shutdown(SelfUpdate.HelperMain(args.Skip(1).ToList()));
            return;
        }
        string? show = null;
        var si = args.IndexOf("--show");
        if (si >= 0 && si + 1 < args.Count)
            show = args[si + 1];
        string? releaseFolder = null;
        var ri = args.IndexOf("--release-folder");
        if (ri >= 0 && ri + 1 < args.Count && Core.Radio.RadioAccess.IsOn)      // developer builds only
            releaseFolder = args[ri + 1];
        string? marker = null;
        var i = args.IndexOf(SelfUpdate.CheckArg);
        if (i >= 0 && i + 1 < args.Count)
            marker = args[i + 1];

        AppState state;
        try
        {
            state = AppState.Create();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show($"DDM32 Plus: {ex.Message}", "DDM32 Plus", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
            return;
        }

        var lang = state.Settings.Language;
        if (lang is null)
        {
            var dialog = new LanguageDialog();
            if (dialog.ShowDialog() != true || dialog.Choice is null)
            {
                Shutdown(0);
                return;
            }
            lang = dialog.Choice;
            state.Settings.Language = lang;
        }
        Loc.Instance.SetLanguage(lang);

        var window = new MainWindow(state);
        MainWindow = window;
        ShutdownMode = ShutdownMode.OnMainWindowClose;
        window.Show();
        Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, window.FirstStart);
        if (releaseFolder is not null)
            Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, () => window.LoadLocalRelease(releaseFolder));
        if (show is not null)
            Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, () => window.ShowFromArgs(show));
        if (marker is not null)
        {
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1.5) };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                SelfUpdate.WriteMarker(marker, AppState.ProgramVersion);
            };
            timer.Start();
        }
    }
}
