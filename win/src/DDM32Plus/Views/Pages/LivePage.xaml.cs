// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using DDM32Plus.Core.Radio;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>The last readings as a line, -140 .. -40 dBm, with guide lines at -120, -93 (S9) and -60 dBm.</summary>
public sealed class RssiHistory : FrameworkElement
{
    public const int Size = 90;
    private readonly List<long?> values = [];

    public IReadOnlyList<long?> Values => values;

    public void Add(long? dbm)
    {
        values.Add(dbm);
        if (values.Count > Size)
            values.RemoveRange(0, values.Count - Size);
        InvalidateVisual();
    }

    public void Clear()
    {
        values.Clear();
        InvalidateVisual();
    }

    private static double Y(double dbm, Rect r) =>
        r.Bottom - (Math.Min(LiveData.DbmHigh, Math.Max(LiveData.DbmLow, dbm)) - LiveData.DbmLow) / (LiveData.DbmHigh - LiveData.DbmLow) * r.Height;

    protected override void OnRender(DrawingContext dc)
    {
        var r = new Rect(0.5, 0.5, Math.Max(0, ActualWidth - 1), Math.Max(0, ActualHeight - 1));
        dc.DrawRectangle(Brushes.White, new Pen(new SolidColorBrush(Color.FromRgb(0xb9, 0xb9, 0xb9)), 1), r);
        var guide = new Pen(new SolidColorBrush(Color.FromRgb(0xd0, 0xd0, 0xd0)), 1);
        var dpi = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        foreach (var dbm in new[] { -120, -93, -60 })
        {
            var y = Math.Round(Y(dbm, r)) + 0.5;
            dc.DrawLine(guide, new Point(r.Left, y), new Point(r.Right, y));
            var label = dbm.ToString(CultureInfo.InvariantCulture) + (dbm == -93 ? " (S9)" : "");
            dc.DrawText(new FormattedText(label, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Segoe UI"), 10,
                Brushes.Gray, dpi), new Point(r.Left + 3, y - 13));
        }
        var step = r.Width / Math.Max(1, Size - 1);
        var x0 = r.Right - (values.Count - 1) * step;
        var pen = new Pen(new SolidColorBrush(Color.FromRgb(0x15, 0x65, 0xc0)), 2);
        Point? prev = null;
        for (var i = 0; i < values.Count; i++)
        {
            if (values[i] is not { } v)
            {
                prev = null;
                continue;
            }
            var p = new Point(x0 + i * step, Y(v, r));
            if (prev is { } q)
                dc.DrawLine(pen, q, p);
            prev = p;
        }
    }
}

/// <summary>
/// Canlı panel / Live panel (pc/ddm32_app/live_tab.py): "Bağlan" is the user's approval to open the port for this
/// panel; DebugReaders.Live then reads the radio every 1-5 s (GET_VIEW core and tasks, the native modem status, the P25
/// zone scan counters; crash and reset once), every frame checked by the read-only DebugLinkPort, until "Durdur". The
/// page shows each poll: signal (S-meter, the last 90 readings), state, channel, P25 receiver, health, scan counters.
/// </summary>
public partial class LivePage : UserControl, IPage
{
    private AppState? state;
    private ToolRunner? runner;
    private CancellationTokenSource? cancel;
    private readonly List<string> once = [];
    private List<string>? block;
    private bool stopping;
    private string? runPort;
    private int runSeconds = LiveData.DefaultInterval;
    private LiveSummary? last;
    private string lastStamp = "";
    private string status = "";
    private string error = "";
    private bool demo;
    private readonly Dictionary<string, TextBlock> scanValues = [];
    private readonly Dictionary<string, TextBlock> scanLabels = [];

    private static readonly string[] ScanKeys =
        ["stops", "carriers", "cooled", "takes", "synced", "voiced", "ended", "tails", "quiet", "foreign", "sync_ms", "voice_ms"];

    public LivePage() => InitializeComponent();

    /// <summary>The reader runs (the window asks before closing, then stops it).</summary>
    public bool Running => runner?.Busy == true;

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        runner = new ToolRunner(Dispatcher);
        RssiText.Text = "-";
        foreach (var s in LiveData.Intervals)
            IntervalBox.Items.Add(new ComboBoxItem { Tag = s });
        IntervalBox.SelectedIndex = Array.IndexOf(LiveData.Intervals, LiveData.DefaultInterval);
        foreach (var key in ScanKeys)
        {
            var panel = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 2, 12, 2) };
            var label = new TextBlock { Margin = new Thickness(0, 0, 6, 0) };
            var value = new TextBlock { Text = "-", FontWeight = FontWeights.Bold, FontSize = (double)FindResource("Size3") };
            panel.Children.Add(label);
            panel.Children.Add(value);
            ScanGrid.Children.Add(panel);
            scanLabels[key] = label;
            scanValues[key] = value;
        }
    }

    private static bool Tr => Loc.Instance.Language == "tr";

    private int Seconds() => (IntervalBox.SelectedItem as ComboBoxItem)?.Tag as int? ?? LiveData.DefaultInterval;

    public void Refresh()
    {
        if (state is null)
            return;
        CableText.Text = CableScanner.Text(state.Cable);
        foreach (ComboBoxItem it in IntervalBox.Items)
            it.Content = Loc.F("Live_Seconds", it.Tag);
        foreach (var key in ScanKeys)
            scanLabels[key].Text = Loc.T("Scan_" + key) + ":";
        var note = AccessNotes.Current();
        AccessNote.Visibility = note is null ? Visibility.Collapsed : Visibility.Visible;
        AccessTitle.Text = note is null ? "" : Loc.T(note.Value.Title);
        AccessText.Text = note is null ? "" : Loc.T(note.Value.Text);
        var busy = Running;
        ConnectButton.IsEnabled = !busy && !ToolRunner.AnyBusy && state.Port is not null && state.DisclaimerAccepted;
        StopButton.IsEnabled = busy && !stopping;
        IntervalBox.IsEnabled = !busy;
        StatusText.Text = status.Length > 0 ? status : Loc.T("Live_NotConnected");
        ErrorText.Text = error;
        ErrorText.Visibility = error.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (last is not null)
            Show(last);
    }

    // --- the reader -------------------------------------------------------------------------------------------------
    private async void OnConnect(object sender, RoutedEventArgs e)
    {
        if (state?.Port is not { } port || runner is null || Running || ToolRunner.AnyBusy || !state.DisclaimerAccepted)
            return;
        once.Clear();
        block = null;
        last = null;
        demo = false;
        stopping = false;
        runPort = port;
        runSeconds = Seconds();
        error = "";
        History.Clear();
        LogText.Clear();
        ClearValues();
        status = Loc.F("Live_Connecting", port);
        cancel = new CancellationTokenSource();
        Refresh();
        var end = await runner.RunAsync(DebugReaders.Live,
            ["--port", port, "--interval", runSeconds.ToString(CultureInfo.InvariantCulture)], null, OnLine, writes: false, cancel.Token);
        var stamp = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        if (end.AccessOff)
        {
            status = Loc.T("Radio_Disabled");
            error = "";
        }
        else if (stopping || end.Error is null)
        {
            status = Loc.F("Live_Stopped", stamp);
            error = "";
        }
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

    /// <summary>"Durdur": ends the reader at its next step and closes the port.</summary>
    public void Stop()
    {
        if (cancel is null || stopping)
            return;
        stopping = true;
        LogText.AppendText("# " + Loc.T("Live_Stop") + "\n");
        cancel.Cancel();
        Refresh();
    }

    private void OnLine(string line)
    {
        if (LogText.Text.Length > 200_000)
            LogText.Text = LogText.Text[^100_000..];
        LogText.AppendText(line + "\n");
        LogText.ScrollToEnd();
        Feed(line);
    }

    /// <summary>One printed line: poll markers open and close a poll; lines before the first poll are crash / reset.</summary>
    public void Feed(string line)
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
                ShowPoll(LiveData.Summary(LiveData.ParseBlocks(block), LiveData.ParseBlocks(once)));
            block = null;
            return;
        }
        if (block is not null)
            block.Add(s);
        else
            once.Add(s);
    }

    private void ShowPoll(LiveSummary s)
    {
        last = s;
        lastStamp = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        History.Add(s.Signal.RssiDbm);
        status = demo ? Loc.F("Live_Connected", runPort, runSeconds, lastStamp) + "  ·  " + DemoData.Note()
            : Loc.F("Live_Connected", runPort, runSeconds, lastStamp);
        Refresh();
    }

    private void ClearValues()
    {
        foreach (var t in new[] { VState, VMode, VChannel, VFreq, VNoise, VNative, VSync, VUptime, VHeap, VStack, VReset, VCrash })
        {
            t.Text = "-";
            t.Foreground = Brushes.Black;
        }
        foreach (var v in scanValues.Values)
            v.Text = "-";
        RssiBar.Value = 0;
        RssiText.Text = "-";
        ScanNote.Text = "";
    }

    private static string StateName(string? state)
    {
        if (state is null)
            return "-";
        var t = Loc.T("LiveState_" + state);
        return t.StartsWith('[') ? state : t;
    }

    private static string NativeName(string? state)
    {
        if (state is null)
            return "-";
        var t = Loc.T("Native_" + state);
        return t.StartsWith('[') ? state : t;
    }

    private void Show(LiveSummary s)
    {
        var sig = s.Signal;
        var dbm = sig.RssiDbm;
        RssiBar.Value = LiveData.DbmPercent(dbm);
        RssiText.Text = dbm is { } d ? $"{d} dBm   {LiveData.SMeter(d)}" : Loc.T("Live_NoSignal");
        if (!sig.Ok)
        {
            foreach (var t in new[] { VState, VMode, VChannel, VFreq, VNoise })
                t.Text = "-";
        }
        else
        {
            VState.Text = StateName(sig.State) + (sig.Monitor ? Loc.T("Live_Monitor") : "");
            VMode.Text = (sig.Mode ?? "-").ToUpperInvariant();
            string ch;
            if (sig.Vfo)
                ch = "VFO";
            else
            {
                ch = string.Join(" · ", new[] { sig.ChannelNumber is { } n and not 0 ? n.ToString(CultureInfo.InvariantCulture) : "", sig.Channel ?? "" }
                    .Where(x => x.Length > 0));
                if (ch.Length == 0)
                    ch = "-";
                if (!string.IsNullOrEmpty(sig.Zone))
                    ch += Loc.F("Live_Zone", sig.Zone);
            }
            VChannel.Text = ch;
            VFreq.Text = LiveData.FormatMhz(sig.RxHz, Tr);
            VNoise.Text = sig.Noise?.ToString(CultureInfo.InvariantCulture) ?? "-";
        }
        var n2 = s.Native;
        if (n2.Ok)
        {
            var text = NativeName(n2.State);
            if (!string.IsNullOrEmpty(n2.Reason) && n2.Reason != "ok")
                text += Loc.F("Live_LastReason", n2.Reason);
            VNative.Text = text;
            VSync.Text = n2.P25Sync?.ToString(CultureInfo.InvariantCulture) ?? "-";
        }
        else
        {
            VNative.Text = n2.Status is not (null or 0) ? Loc.T("Live_Unreadable") : "-";
            VSync.Text = "-";
        }
        if (s.Health is { } h)
        {
            VUptime.Text = LiveData.FormatUptime(h.UptimeMs);
            VHeap.Text = LiveData.Bytes(h.HeapMinFree, Tr);
            VStack.Text = h.MinTask is null ? "-" : $"{h.MinTask}: {LiveData.Bytes(h.MinFree, Tr)}";
            VReset.Text = HealthPage.ResetName(h.ResetCause);
            VCrash.Text = h.CrashValid ? Loc.F("Live_CrashYes", h.CrashTask) : Loc.T("Live_CrashNo");
            VCrash.Foreground = h.CrashValid ? (Brush)FindResource("Red") : Brushes.Black;
            VCrash.FontWeight = h.CrashValid ? FontWeights.Bold : FontWeights.Normal;
        }
        if (s.Scan is { } scan)
        {
            foreach (var key in ScanKeys)
                scanValues[key].Text = scan.TryGetValue(key, out var v) && v is not null ? v.Value.ToString(CultureInfo.InvariantCulture) : "-";
            ScanNote.Text = "";
        }
        else
        {
            foreach (var v in scanValues.Values)
                v.Text = "-";
            ScanNote.Text = s.ScanStatus is not (null or 0 or -1) ? Loc.T("Live_ScanNone") : "";
        }
    }

    /// <summary>--show live:demo (screenshots): made-up polls fed to the page as if read; no port is opened.</summary>
    public void ShowDemo()
    {
        if (Running)
            return;
        once.Clear();
        block = null;
        History.Clear();
        LogText.Clear();
        demo = true;
        runPort = "COM5";
        runSeconds = LiveData.DefaultInterval;
        error = "";
        foreach (var line in DemoData.LiveLines())
            OnLine(line);
    }
}
