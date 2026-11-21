// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using DDM32Plus.Core;
using DDM32Plus.Core.Radio;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>A row of the table (strings in the page's language).</summary>
public sealed record SearchRow(string First, string Time, string Mode, string Mhz, string Code, string Slot, string Group, string Source,
    string Calls);

/// <summary>
/// Arama kaydı / Search log: "Kaydı başlat" is the user's approval to open the port for this page. Every second
/// DebugReaders.SearchLog reads the radio's P25 and DMR search finds (read only, not counted as a look); every new call
/// becomes a row (newest first) and a line in logs\search-YYYYMMDD.csv, until "Durdur". The radio runs the search
/// itself (Tools > P25 search / DMR search); this page only keeps what it finds, all night if left on.
/// </summary>
public partial class SearchLogPage : UserControl, IPage
{
    private AppState? state;
    private ToolRunner? runner;
    private CancellationTokenSource? cancel;
    private List<string>? block;
    private bool stopping;
    private string status = "";
    private string error = "";
    private SearchLogBook book = new();
    private readonly ObservableCollection<SearchRow> rows = [];
    private string? csvPath;
    private DateTime day = DateTime.Now;

    public SearchLogPage() => InitializeComponent();

    public bool Running => runner?.Busy == true;

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        runner = new ToolRunner(Dispatcher);
        Grid.ItemsSource = rows;
        Columns();
    }

    private void Columns()
    {
        Grid.Columns.Clear();
        foreach (var (key, path) in new[] { ("SLog_First", "First"), ("SLog_Time", "Time"), ("SLog_Mode", "Mode"), ("SLog_Mhz", "Mhz"), ("SLog_Code", "Code"),
                     ("SLog_Slot", "Slot"), ("SLog_Group", "Group"), ("SLog_Source", "Source"), ("SLog_Calls", "Calls") })
            Grid.Columns.Add(new DataGridTextColumn { Header = Loc.T(key), Binding = new Binding(path) });
    }

    public void Refresh()
    {
        if (state is null)
            return;
        Columns();
        CableText.Text = CableScanner.Text(state.Cable);
        var busy = Running;
        StartButton.IsEnabled = !busy && !ToolRunner.AnyBusy && state.Port is not null && state.DisclaimerAccepted;
        StopButton.IsEnabled = busy && !stopping;
        ClearButton.IsEnabled = rows.Count > 0 || (csvPath is not null && File.Exists(csvPath));
        StatusText.Text = status.Length > 0 ? status : Loc.T("SLog_NotRunning");
        SearchText.Text = Running ? Searches() : "";
        ErrorText.Text = error;
        ErrorText.Visibility = error.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private string Searches()
    {
        string One(string name, bool? on, long? at) => on switch
        {
            true => Loc.F("SLog_SearchOn", name, at is { } hz ? SearchLogBook.Mhz(hz) : "-"),
            false => Loc.F("SLog_SearchOff", name),
            _ => Loc.F("SLog_SearchNone", name),
        };
        return One("P25", book.P25On, book.P25AtHz) + "   " + One("DMR", book.DmrOn, book.DmrAtHz) + "   " +
               One(Loc.T("SLog_All"), book.AllOn, book.AllAtHz) + "   " +
               Loc.F("SLog_Count", book.TotalCalls, book.Frequencies);
    }

    private async void OnStart(object sender, RoutedEventArgs e)
    {
        if (state?.Port is not { } port || runner is null || Running || ToolRunner.AnyBusy || !state.DisclaimerAccepted)
            return;
        block = null;
        stopping = false;
        error = "";
        status = Loc.F("Live_Connecting", port);
        cancel = new CancellationTokenSource();
        Refresh();
        var end = await runner.RunAsync(DebugReaders.SearchLog, ["--port", port], null, OnLine, writes: false, cancel.Token);
        var stamp = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        if (end.AccessOff)
            status = Loc.T("Radio_Disabled");
        else if (stopping || end.Error is null)
            status = Loc.F("Live_Stopped", stamp);
        else
        {
            status = Loc.T("Live_Lost");
            error = end.Error.Record;
        }
        stopping = false;
        cancel.Dispose();
        cancel = null;
        Refresh();
    }

    private void OnStop(object sender, RoutedEventArgs e) => Stop();

    public void Stop()
    {
        if (cancel is null || stopping)
            return;
        stopping = true;
        cancel.Cancel();
        Refresh();
    }

    /// <summary>"Kayıtları temizle": after a confirmation, the table and the day's CSV file are emptied; a running log goes
    /// on from nothing (what the radio still lists counts again as found).</summary>
    private void OnClear(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(Window.GetWindow(this), Loc.T("SLog_ClearAsk"), AppInfo.Name, MessageBoxButton.YesNo,
                MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        rows.Clear();
        book = new SearchLogBook();
        try
        {
            if (csvPath is not null && File.Exists(csvPath))
                File.Delete(csvPath);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            error = ex.Message;
        }
        csvPath = null;
        day = DateTime.Now;
        Refresh();
    }

    private void OnFolder(object sender, RoutedEventArgs e)
    {
        if (state is null)
            return;
        Directory.CreateDirectory(state.Paths.Logs);
        Process.Start(new ProcessStartInfo(state.Paths.Logs) { UseShellExecute = true });
    }

    private void OnLine(string line)
    {
        var s = line.Trim();
        var m = LiveData.PollMarker(s);
        if (m is ("poll", _))
        {
            block = [];
            return;
        }
        if (m is ("end", _))
        {
            if (block is not null)
                Take(block);
            block = null;
            return;
        }
        block?.Add(s);
    }

    private void Take(List<string> lines)
    {
        var now = DateTime.Now;
        if (book.Add(lines, now) && state is not null)
        {
            var groups = book.Groups;
            try
            {
                csvPath = SearchLogBook.Write(state.Paths.Logs, groups, day);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                error = ex.Message;
            }
            rows.Clear();
            foreach (var g in groups)
                rows.Add(Row(g));
        }
        status = Loc.F("SLog_Running", now.ToString("HH:mm:ss", CultureInfo.InvariantCulture)) +
                 (csvPath is null ? "" : "  " + Loc.F("SLog_File", csvPath));
        Refresh();
    }

    private static SearchRow Row(SearchGroup c) => new(
        c.First.ToString("dd.MM HH:mm:ss", CultureInfo.InvariantCulture),
        c.Last.ToString("dd.MM HH:mm:ss", CultureInfo.InvariantCulture), c.Mode, SearchLogBook.Mhz(c.Hz), c.Code, c.Slot,
        c.Group == 0 ? "-" : c.Group.ToString(CultureInfo.InvariantCulture),
        c.Caller.Length > 0 ? c.Caller : c.Source == 0 ? "-" : c.Source.ToString(CultureInfo.InvariantCulture) + (c.Encrypted ? " (enc)" : ""),
        c.Calls.ToString(CultureInfo.InvariantCulture));
}
