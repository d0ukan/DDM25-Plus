// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using DDM32Plus.Core.Radio;
using DDM32Plus.Core.Storage;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>
/// Telsizin durumu / Radio health (pc/ddm32_app/health_tab.py): the CH340 cable and driver check (Windows' device
/// list, no port); one read of the radio's health per click or every 30 s when ticked (DebugReaders.Health: the
/// debug link's views tasks, crash, reset, behind the read-only DebugLinkPort; the radio stays switched on and runs
/// DDM32 Plus), shown as a traffic light, a few plain lines and the task stacks; and the newest verified backup with
/// the firmware it holds (files only).
/// </summary>
public partial class HealthPage : UserControl, IPage
{
    private AppState? state;
    private ToolRunner? runner;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(LiveData.AutoSeconds) };
    private readonly List<string> lines = [];
    private HealthSummary? last;
    private bool reading;
    private bool failed;
    private string when = "";
    private string? backupFolder;
    private PyDict? backupDetect;
    private string? backupError;

    /// <summary>One row of the task stack table.</summary>
    public sealed record TaskItem(string Name, string Text, double Value, double Max, Brush Brush);

    public HealthPage() => InitializeComponent();

    public bool Busy => reading;

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        runner = new ToolRunner(Dispatcher);
        timer.Tick += (_, _) =>
        {
            if (!reading && !ToolRunner.AnyBusy && state.Port is not null && state.DisclaimerAccepted)
                OnRead(this, new RoutedEventArgs());
        };
        _ = LoadBackupAsync();
    }

    private static bool Tr => Loc.Instance.Language == "tr";

    public void Refresh()
    {
        if (state is null)
            return;
        CableText.Text = CableScanner.Text(state.Cable);
        CableLight.Fill = (Brush)FindResource(state.Cable.State switch
        {
            CableState.Ok => "Green",
            CableState.NoCable => "Grey",
            _ => "Red",
        });
        var note = AccessNotes.Current();
        AccessNote.Visibility = note is null ? Visibility.Collapsed : Visibility.Visible;
        AccessTitle.Text = note is null ? "" : Loc.T(note.Value.Title);
        AccessText.Text = note is null ? "" : Loc.T(note.Value.Text);
        ReadButton.IsEnabled = !reading && !ToolRunner.AnyBusy && state.Port is not null && state.DisclaimerAccepted;
        ReadButton.Content = Loc.T(reading ? "Health_Reading" : "Health_Read");
        ReadButton.ToolTip = state.Port is null ? Loc.T("Health_NoPort") : Loc.T("Health_ReadTip");
        ShowResult();
        ShowBackup();
    }

    private void SetLight(HealthVerdict v, string? text = null)
    {
        var (brush, key) = v switch
        {
            HealthVerdict.Green => ("Green", "Health_Green"),
            HealthVerdict.Amber => ("Amber", "Health_Amber"),
            HealthVerdict.Red => ("Red", "Health_Red"),
            _ => ("Grey", "Health_NotRead"),
        };
        var b = (Brush)FindResource(brush);
        Light.Fill = b;
        VerdictText.Foreground = b;
        VerdictText.Text = text ?? Loc.T(key);
    }

    private void ShowResult()
    {
        if (failed)
        {
            SetLight(HealthVerdict.Grey, Loc.T("Health_Failed"));
            SummaryText.Text = Loc.T("Health_FailedText");
            WhenText.Text = when;
            return;
        }
        if (last is not { } s)
        {
            SetLight(HealthVerdict.Grey);
            SummaryText.Text = "";
            WhenText.Text = "";
            return;
        }
        SetLight(LiveData.Verdict(s));
        SummaryText.Text = string.Join("\n", PlainLines(s));
        WhenText.Text = when;
        VUptime.Text = LiveData.FormatUptime(s.UptimeMs);
        VHeap.Text = LiveData.Bytes(s.HeapFree, Tr);
        VHeapMin.Text = LiveData.Bytes(s.HeapMinFree, Tr);
        VMinTask.Text = s.MinTask is null ? "-" : $"{s.MinTask}: {LiveData.Bytes(s.MinFree, Tr)}";
        VCrash.Text = s.CrashValid ? Loc.F("Health_CrashYes", s.CrashTask, s.CrashPc, LiveData.FormatUptime(s.CrashUptimeMs))
            : s.CrashBuilt == "0" ? Loc.T("Health_CrashNotBuilt") : Loc.T("Health_CrashNone");
        VReset.Text = s.ResetCause ?? "-";
        var top = Math.Max(1024, s.Tasks.Select(t => t.Free ?? 0).DefaultIfEmpty(0).Max());
        TaskList.ItemsSource = s.Tasks.Select(t => new TaskItem(t.Name, LiveData.Bytes(t.Free, Tr), t.Free ?? 0, top,
            (Brush)FindResource(t.Free is { } f && f < LiveData.LowStack ? "Red" : "Green"))).ToList();
    }

    /// <summary>health_tab.plain_lines: how long the radio has been on, the heap low-water mark, the crash record, the
    /// last start.</summary>
    public static List<string> PlainLines(HealthSummary s)
    {
        var out_ = new List<string>();
        if (s.UptimeMs is { } up)
        {
            var m = up / 60000;
            out_.Add(m >= 60 ? Loc.F("Health_UpHours", m / 60, m % 60) : Loc.F("Health_Up", m));
        }
        if (s.HeapMinFree is { } h)
        {
            var kb = (h / 1024.0).ToString("0.0", Tr ? CultureInfo.GetCultureInfo("tr-TR") : CultureInfo.InvariantCulture);
            out_.Add(Loc.F("Health_Heap", kb, Loc.T(h < LiveData.LowHeap ? "Health_Low" : "Health_Enough")));
        }
        out_.Add(s.CrashValid ? Loc.F("Health_Crash", s.CrashTask) : Loc.T("Health_NoCrash"));
        if (!string.IsNullOrEmpty(s.ResetCause))
            out_.Add(Loc.F("Health_LastBoot", ResetName(s.ResetCause)));
        return out_;
    }

    /// <summary>health_tab.RESET_NAMES: the reset cause in plain words (the cause itself when unknown).</summary>
    public static string ResetName(string? cause)
    {
        if (string.IsNullOrEmpty(cause))
            return "-";
        var t = Loc.T("Reset_" + cause);
        return t.StartsWith('[') ? cause : t;
    }

    // --- the radio's health ------------------------------------------------------------------------------------------
    private async void OnRead(object sender, RoutedEventArgs e)
    {
        if (state?.Port is not { } port || runner is null || reading || ToolRunner.AnyBusy || !state.DisclaimerAccepted)
            return;
        reading = true;
        lines.Clear();
        LogText.Clear();
        Refresh();
        var log = Path.Combine(state.Paths.Backups, $"health-{AppPaths.Today()}.log");
        var end = await runner.RunAsync(DebugReaders.Health, ["--port", port, "--health"], log, OnLine, writes: false);
        reading = false;
        var stamp = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        var s = LiveData.Summary(lines);
        if (end.Error is not null || !s.Ok)
        {
            failed = true;
            when = end.AccessOff ? Loc.T("Radio_Disabled") : $"{stamp}: {end.Error?.Record ?? "-"}";
        }
        else
        {
            failed = false;
            last = s;
            when = Loc.F("Health_LastRead", stamp);
        }
        Refresh();
    }

    /// <summary>--show health:demo (screenshots): made-up views shown as if read; no port is opened.</summary>
    public void ShowDemo()
    {
        if (reading)
            return;
        lines.Clear();
        LogText.Clear();
        foreach (var l in DemoData.HealthLines())
            OnLine(l);
        failed = false;
        last = LiveData.Summary(lines);
        when = Loc.F("Health_LastRead", DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture)) + "  ·  " + DemoData.Note();
        DetailsBox.IsExpanded = true;
        Refresh();
    }

    private void OnLine(string line)
    {
        lines.Add(line);
        LogText.AppendText(line + "\n");
        LogText.ScrollToEnd();
    }

    private void OnAuto(object sender, RoutedEventArgs e)
    {
        if (AutoBox.IsChecked == true)
            timer.Start();
        else
            timer.Stop();
    }

    private void OnCheckCable(object sender, RoutedEventArgs e)
    {
        state?.RefreshCable();
        _ = LoadBackupAsync();
    }

    // --- the newest verified backup (files only) ---------------------------------------------------------------------
    private async Task LoadBackupAsync()
    {
        if (state is null)
            return;
        var roots = new List<string> { state.Paths.Backups };
        roots.AddRange(state.PythonData.Select(d => d.Backups));
        try
        {
            var (folder, detect) = await Task.Run(() =>
            {
                var newest = AppPaths.VerifiedBackups(roots).FirstOrDefault();
                return newest is null ? (null, null) : (newest, FileSteps.DetectBackup(newest, Codec.Dm32));
            });
            (backupFolder, backupDetect, backupError) = (folder, detect, null);
        }
        catch (Exception ex) when (ex is ToolError or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            backupError = ex.Message;
        }
        ShowBackup();
    }

    private void ShowBackup()
    {
        if (state is null)
            return;
        var (all, verified) = AppPaths.CountBackups(state.Paths.Backups);
        BackupCount.Text = Loc.F("Health_Count", all, verified);
        if (backupFolder is null)
        {
            BackupText.Text = backupError ?? Loc.T("Health_BackupNone");
            return;
        }
        var date = File.GetLastWriteTime(Path.Combine(backupFolder, "backup-manifest.json")).ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture);
        var fw = (backupDetect?["firmware"] as string) switch
        {
            "ddm32" => Loc.T("Health_FwDdm32"),
            "classic" => Loc.T("Health_FwClassic"),
            "stock" => Loc.F("Health_FwStock", backupDetect?["stock_version"] ?? "?"),
            _ => Loc.T("Health_FwUnknown"),
        };
        BackupText.Text = Loc.F("Health_Backup", Path.GetFileName(backupFolder), date, fw) + (backupError is null ? "" : "\n" + backupError);
    }
}
