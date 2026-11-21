// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.RegularExpressions;

namespace DDM32Plus.Core.Radio;

/// <summary>The radio's health from the debug-link views tasks, crash, reset (parsers.health_summary).</summary>
public sealed record HealthSummary(
    long? UptimeMs, long? HeapFree, long? HeapMinFree, string? MinTask, long? MinFree, bool CrashValid, string? CrashBuilt,
    string? CrashTask, string? CrashPc, long? CrashUptimeMs, string? ResetCause, IReadOnlyList<(string Name, long? Free)> Tasks,
    bool Ok);

/// <summary>Health_tab's traffic light.</summary>
public enum HealthVerdict
{
    Grey,
    Green,
    Amber,
    Red,
}

/// <summary>What the live panel shows from one poll (parsers.live_summary).</summary>
public sealed record LiveSignal(
    bool Ok, long? RssiDbm, long? Noise, string? State, string? Mode, string? Zone, string? Channel, long? ChannelNumber,
    bool Vfo, long? RxHz, long? TxHz, long? Squelch, bool Monitor, long? UptimeMs);

public sealed record LiveNative(bool Ok, int? Status, string? State, string? Reason, long? FrequencyHz, long? RxCount,
    long? RxSeen, long? P25Sync, long? Faults);

public sealed record LiveSummary(LiveSignal Signal, LiveNative Native, IReadOnlyDictionary<string, long?>? Scan,
    int? ScanStatus, HealthSummary? Health);

/// <summary>One '--- NAME (ID) status CODE' block of the readers' output and its key=value lines.</summary>
public sealed class LiveBlock
{
    public int Status { get; init; }
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);
    public List<string> Lines { get; } = [];
}

/// <summary>
/// Reads what the health and live readers print (pc/ddm32_app/parsers.py parse_health, health_summary, parse_blocks,
/// live_summary, s_meter, format_mhz, format_uptime; health_tab.verdict; live_tab.dbm_percent). Held to the Python
/// results in LiveTests.cs (live-results.json).
/// </summary>
public static partial class LiveData
{
    public const int LowStack = 200;        // bytes: the radio test plan's "each > 200 B"
    public const int LowHeap = 1024;        // heap_min_free > 1 KB
    public const int DbmLow = -140, DbmHigh = -40;
    public const int AutoSeconds = 30;
    public static readonly int[] Intervals = [1, 2, 3, 4, 5];
    public const int DefaultInterval = 2;

    [GeneratedRegex(@"^--- view (\w+) \((\d+)\) status (-?\d+)")]
    private static partial Regex ViewRe();

    [GeneratedRegex(@"^--- (.+?) \((\S+)\) status (-?\d+)")]
    private static partial Regex BlockRe();

    [GeneratedRegex(@"^=== (poll|end) (\d+)")]
    private static partial Regex PollRe();

    /// <summary>("poll" or "end", number) of a poll marker line, or null.</summary>
    public static (string Kind, int Number)? PollMarker(string line)
    {
        var m = PollRe().Match(line.Trim());
        return m.Success ? (m.Groups[1].Value, int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture)) : null;
    }

    /// <summary>Python int(text, 0): an optional sign, 0x / 0o / 0b, no leading zeros, underscores between digits.</summary>
    public static long? PyInt(string? text)
    {
        if (text is null)
            return null;
        var t = text.Trim();
        var neg = false;
        if (t.StartsWith('-') || t.StartsWith('+'))
        {
            neg = t[0] == '-';
            t = t[1..];
        }
        if (t.Length == 0 || t.StartsWith('_') || t.EndsWith('_') || t.Contains("__", StringComparison.Ordinal))
            return null;
        var b = 10;
        if (t.Length > 2 && t[0] == '0' && char.ToLowerInvariant(t[1]) is 'x' or 'o' or 'b')
        {
            b = char.ToLowerInvariant(t[1]) switch { 'x' => 16, 'o' => 8, _ => 2 };
            t = t[2..].TrimStart('_');
        }
        else if (t.Length > 1 && t[0] == '0' && t.Replace("_", "", StringComparison.Ordinal).Any(c => c != '0'))
            return null;                    // "012": no leading zeros in base 0
        t = t.Replace("_", "", StringComparison.Ordinal);
        if (t.Length == 0)
            return null;
        long v = 0;
        foreach (var c in t)
        {
            var d = c is >= '0' and <= '9' ? c - '0' : c is >= 'a' and <= 'z' ? c - 'a' + 10 : c is >= 'A' and <= 'Z' ? c - 'A' + 10 : 99;
            if (d >= b)
                return null;
            v = checked(v * b + d);
        }
        return neg ? -v : v;
    }

    /// <summary>The health views' key=value lines: (status by view, uptime by view, tasks, every value).</summary>
    public static (Dictionary<string, int> Status, List<(string View, long? Ms)> Uptime, List<(string Name, long? Free)> Tasks,
        Dictionary<string, string> Values) ParseHealth(IEnumerable<string> lines)
    {
        var status = new Dictionary<string, int>(StringComparer.Ordinal);
        var uptime = new List<(string, long?)>();
        var tasks = new List<(string Name, long? Free)>();
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        string? view = null;
        foreach (var raw in lines)
        {
            var line = raw.Trim();
            var m = ViewRe().Match(line);
            if (m.Success)
            {
                view = m.Groups[1].Value;
                status[view] = int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture);
                continue;
            }
            var eq = line.IndexOf('=', StringComparison.Ordinal);
            if (eq <= 0 || line[..eq].Contains(' ', StringComparison.Ordinal))
                continue;
            var key = line[..eq];
            var value = line[(eq + 1)..];
            if (key == "uptime_ms" && view is not null)
            {
                uptime.RemoveAll(u => u.Item1 == view);
                uptime.Add((view, PyInt(value)));
                continue;
            }
            values[key] = value;
            if (key.StartsWith("tasks.free.", StringComparison.Ordinal))
            {
                var name = key["tasks.free.".Length..];
                var i = tasks.FindIndex(t => t.Name == name);
                if (i >= 0)
                    tasks[i] = (name, PyInt(value));
                else
                    tasks.Add((name, PyInt(value)));
            }
        }
        return (status, uptime, tasks, values);
    }

    private static long? Num(Dictionary<string, string> v, string key) => v.TryGetValue(key, out var s) ? PyInt(s) : null;

    private static string? Str(Dictionary<string, string> v, string key) => v.TryGetValue(key, out var s) ? s : null;

    public static HealthSummary Summary(IEnumerable<string> lines)
    {
        var (status, uptime, tasks, v) = ParseHealth(lines);
        var tasksUp = uptime.FindIndex(u => u.View == "tasks");
        long? up = tasksUp >= 0 ? uptime[tasksUp].Ms : uptime.Count > 0 ? uptime[0].Ms : null;
        var sorted = tasks.Select((t, i) => (t, i)).OrderBy(x => x.t.Free is null).ThenBy(x => x.t.Free ?? 0).ThenBy(x => x.i)
            .Select(x => x.t).ToList();
        return new HealthSummary(up, Num(v, "tasks.heap_free"), Num(v, "tasks.heap_min_free"), Str(v, "tasks.min_task"),
            Num(v, "tasks.min_free"), Str(v, "crash.valid") == "1", Str(v, "crash.built"), Str(v, "crash.task"),
            Str(v, "crash.pc"), Num(v, "crash.uptime_ms"), Str(v, "reset.cause"), sorted,
            status.Count > 0 && status.Values.All(c => c == 0));
    }

    /// <summary>Red: a crash record; amber: heap low-water under 1 KB or a stack under 200 B; else green.</summary>
    public static HealthVerdict Verdict(HealthSummary s)
    {
        if (s.CrashValid)
            return HealthVerdict.Red;
        var lowTask = s.Tasks.Any(t => t.Free is { } f && f < LowStack);
        return s.HeapMinFree is { } h && h < LowHeap || lowTask ? HealthVerdict.Amber : HealthVerdict.Green;
    }

    /// <summary>The blocks of one poll (or of the lines before the first poll): {name: block}.</summary>
    public static Dictionary<string, LiveBlock> ParseBlocks(IEnumerable<string> lines)
    {
        var out_ = new Dictionary<string, LiveBlock>(StringComparer.Ordinal);
        LiveBlock? cur = null;
        foreach (var raw in lines)
        {
            var line = raw.Trim();
            var m = BlockRe().Match(line);
            if (m.Success)
            {
                cur = out_[m.Groups[1].Value] = new LiveBlock { Status = int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture) };
                cur.Lines.Add(line);
                continue;
            }
            if (cur is null || PollRe().IsMatch(line))
                continue;
            cur.Lines.Add(line);
            var eq = line.IndexOf('=', StringComparison.Ordinal);
            if (eq > 0 && !line[..eq].Contains(' ', StringComparison.Ordinal))
                cur.Values[line[..eq]] = line[(eq + 1)..];
        }
        return out_;
    }

    public static LiveSummary Summary(Dictionary<string, LiveBlock>? blocks, Dictionary<string, LiveBlock>? once = null)
    {
        var all = new Dictionary<string, LiveBlock>(once ?? [], StringComparer.Ordinal);
        foreach (var kv in blocks ?? [])
            all[kv.Key] = kv.Value;
        Dictionary<string, string> Ok(string name) =>
            all.TryGetValue(name, out var b) && b.Status == 0 ? b.Values : new(StringComparer.Ordinal);
        var cv = Ok("view core");
        var rssi = Num(cv, "rssi_dbm");
        var signal = new LiveSignal(cv.Count > 0, rssi is null or 0 ? null : rssi, Num(cv, "noise"), Str(cv, "state"),
            Str(cv, "mode"), Str(cv, "zone_name"), Str(cv, "channel_name"), Num(cv, "channel_number"), Str(cv, "vfo") == "1",
            Num(cv, "rx_hz"), Num(cv, "tx_hz"), Num(cv, "squelch"), Str(cv, "monitor") == "1", Num(cv, "uptime_ms"));
        var nv = Ok("native");
        var native = new LiveNative(nv.Count > 0, all.TryGetValue("native", out var nb) ? nb.Status : null, Str(nv, "state_name"),
            Str(nv, "reason_name"), Num(nv, "frequency_hz"), Num(nv, "rx_count"), Num(nv, "rx_seen"), Num(nv, "p25_sync"),
            Num(nv, "faults"));
        var sv = Ok("scan");
        Dictionary<string, long?>? scan = sv.Count > 0 && !sv.ContainsKey("raw")
            ? NativeModem.ScanFields.ToDictionary(k => k, k => Num(sv, k), StringComparer.Ordinal) : null;
        var healthLines = new[] { "view tasks", "view crash", "view reset" }
            .SelectMany(n => all.TryGetValue(n, out var b) ? b.Lines : []).ToList();
        var health = healthLines.Count > 0 ? Summary(healthLines) : null;
        return new LiveSummary(signal, native, scan, all.TryGetValue("scan", out var sb) ? sb.Status : null, health);
    }

    /// <summary>VHF/UHF S-meter (IARU R.1: S9 = -93 dBm, 6 dB a unit): "S7", "S9+20", or "" without a reading.</summary>
    public static string SMeter(long? dbm)
    {
        if (dbm is null or 0)
            return "";
        if (dbm > -93)
            return "S9+" + (dbm.Value + 93).ToString(CultureInfo.InvariantCulture);
        var s = 9 - (long)Math.Round((-93.0 - dbm.Value) / 6.0, MidpointRounding.ToEven);
        return "S" + Math.Max(0, Math.Min(9, s)).ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>434250000 -> "434,25000 MHz" (Turkish) or "434.25000 MHz"; "-" without a frequency.</summary>
    public static string FormatMhz(long? hz, bool decimalComma = true)
    {
        if (hz is null or 0)
            return "-";
        var s = (hz.Value / 1e6).ToString("0.00000", CultureInfo.InvariantCulture) + " MHz";
        return decimalComma ? s.Replace('.', ',') : s;
    }

    /// <summary>h:mm:ss, "-" without a value.</summary>
    public static string FormatUptime(long? ms)
    {
        if (ms is null)
            return "-";
        var s = ms.Value / 1000;
        return $"{s / 3600}:{s / 60 % 60:00}:{s % 60:00}";
    }

    /// <summary>The signal bar: -140 .. -40 dBm as 0..100.</summary>
    public static int DbmPercent(long? dbm) =>
        dbm is null ? 0 : (int)Math.Round((Math.Min(DbmHigh, Math.Max(DbmLow, dbm.Value)) - DbmLow) * 100.0 / (DbmHigh - DbmLow),
            MidpointRounding.ToEven);

    /// <summary>"1.200 B" (Turkish thousands dot) or "1,200 B"; "-" without a value.</summary>
    public static string Bytes(long? v, bool turkish = true) =>
        v is null ? "-" : v.Value.ToString("#,0", CultureInfo.InvariantCulture).Replace(",", turkish ? "." : ",", StringComparison.Ordinal) + " B";
}
