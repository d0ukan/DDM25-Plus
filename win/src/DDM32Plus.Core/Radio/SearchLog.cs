// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text;

namespace DDM32Plus.Core.Radio;

/// <summary>One call a search found: when it was heard, where and who (no audio). Mode: "P25", "DMR", "YSF" or "NXDN";
/// Code: P25 NAC ("$1C8"), DMR colour code ("CC1"), YSF DG-ID ("DG0") or NXDN RAN ("RAN1"); Slot: DMR slots seen ("TS1",
/// "TS2", "TS1+2") and RPT / DIR; Group: P25 / NXDN talkgroup or DMR talkgroup / called ID; Caller: the YSF source
/// callsign (empty otherwise).</summary>
public sealed record SearchCall(DateTime Time, string Mode, long Hz, string Code, string Slot, long Group, long Source,
    bool Encrypted, int Calls, string Caller = "");

/// <summary>One line of the log: every call heard on one mode + frequency + code (NAC / CC / DG-ID / RAN) + group,
/// counted (owner 2026-11-21: "how many frequencies, which NAC, which group have traffic"). First / Last: when it was
/// first and last heard; Source / Caller / Slot: the last call's; Calls: every call heard there since the log started.</summary>
public sealed class SearchGroup
{
    public required string Mode { get; init; }
    public required long Hz { get; init; }
    public required string Code { get; init; }
    public required long Group { get; init; }
    public DateTime First { get; set; }
    public DateTime Last { get; set; }
    public string Slot { get; set; } = "";
    public long Source { get; set; }
    public string Caller { get; set; } = "";
    public bool Encrypted { get; set; }
    /// <summary>Calls counted before the radio's counter last went down (a restart, an entry dropped and found again).</summary>
    internal int Base { get; set; }
    /// <summary>The radio's own counter for this group now (the highest of its entries).</summary>
    internal int Current { get; set; }
    public int Calls => Base + Current;
}

/// <summary>
/// The search log (firmware tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the radio's P25, DMR and all
/// digital searches keep 8 finds each; read every second over the debug link (GET_VIEW sections p25s = 28, dmrs = 29,
/// alls = 30, read only, not counted as a look). The log keeps ONE line per mode + frequency + code + group with a call
/// counter: the radio's entries for the same group (the list may hold a frequency twice) count once, by their highest
/// call counter; a counter that goes down (a restart, an entry dropped from the radio's 8 and found again) starts a new
/// base, so no call is lost and none is counted twice. The day's CSV file holds the same lines, rewritten on a change.
/// </summary>
public sealed class SearchLogBook
{
    public const byte SectionP25 = 0x1C;
    public const byte SectionDmr = 0x1D;
    public const byte SectionAll = 0x1E;

    private readonly Dictionary<(string Mode, long Hz, string Code, long Group), SearchGroup> groups = [];

    /// <summary>Every line, the most recently heard first.</summary>
    public IReadOnlyList<SearchGroup> Groups => [.. groups.Values.OrderByDescending(g => g.Last).ThenBy(g => g.Hz)];

    /// <summary>Whether the radio reported each search running at the last poll (null = no answer / not built).</summary>
    public bool? P25On { get; private set; }
    public bool? DmrOn { get; private set; }
    public bool? AllOn { get; private set; }
    public long? P25AtHz { get; private set; }
    public long? DmrAtHz { get; private set; }
    public long? AllAtHz { get; private set; }

    /// <summary>Distinct frequencies with a call.</summary>
    public int Frequencies => groups.Values.Select(g => (g.Mode, g.Hz)).Distinct().Count();

    /// <summary>Every call counted.</summary>
    public int TotalCalls => groups.Values.Sum(g => g.Calls);

    /// <summary>One poll's key=value lines (all three sections); true when a line was added or changed.</summary>
    public bool Add(IEnumerable<string> lines, DateTime now)
    {
        var kv = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in lines)
        {
            var line = raw.Trim();
            var eq = line.IndexOf('=');
            if (eq > 0)
                kv[line[..eq]] = line[(eq + 1)..];
        }
        P25On = Flag(kv, "p25s.on");
        DmrOn = Flag(kv, "dmrs.on");
        AllOn = Flag(kv, "alls.on");
        P25AtHz = Num(kv, "p25s.at");
        DmrAtHz = Num(kv, "dmrs.at");
        AllAtHz = Num(kv, "alls.at");
        // every entry of the three lists, grouped; a group's counter is its entries' highest (the lists overlap: the
        // all digital search may find what the P25 search found, the radio may list a frequency twice)
        var heard = new Dictionary<(string, long, string, long), (int Calls, SearchCall Last)>();
        for (var i = 0; i < 8; i++)
        {
            foreach (var call in new[]
                     {
                         kv.TryGetValue($"p25s.f{i}", out var p) ? ParseP25(p, now)?.Call : null,
                         kv.TryGetValue($"dmrs.f{i}", out var d) ? ParseDmr(d, now)?.Call : null,
                         kv.TryGetValue($"alls.f{i}", out var x) ? ParseAll(x, now)?.Call : null,
                     })
            {
                if (call is null || (call.Calls == 0 && call.Group == 0 && call.Source == 0 && call.Caller.Length == 0))
                    continue;
                var key = (call.Mode, call.Hz, call.Code, call.Group);
                var calls = Math.Max(1, call.Calls);
                if (!heard.TryGetValue(key, out var h) || calls > h.Calls || (calls == h.Calls && call.Time > h.Last.Time))
                    heard[key] = (Math.Max(calls, h.Calls), call.Time >= (h.Last?.Time ?? DateTime.MinValue) ? call : h.Last!);
            }
        }
        var changed = false;
        foreach (var (key, h) in heard)
        {
            var c = h.Last;
            if (!groups.TryGetValue(key, out var g))
            {
                groups[key] = new SearchGroup
                {
                    Mode = c.Mode, Hz = c.Hz, Code = c.Code, Group = c.Group, First = c.Time, Last = c.Time, Slot = c.Slot,
                    Source = c.Source, Caller = c.Caller, Encrypted = c.Encrypted, Current = h.Calls,
                };
                changed = true;
                continue;
            }
            if (h.Calls < g.Current)
            {
                g.Base += g.Current;	// the radio's counter went down: what it counted stays
                g.Current = h.Calls;
                g.Last = c.Time > g.Last ? c.Time : g.Last;
                changed = true;
            }
            else if (h.Calls > g.Current || c.Source != g.Source || c.Caller != g.Caller)
            {
                g.Current = h.Calls;
                if (c.Time > g.Last)
                    g.Last = c.Time;
                g.Source = c.Source;
                g.Caller = c.Caller;
                g.Slot = c.Slot.Length > 0 ? c.Slot : g.Slot;
                g.Encrypted |= c.Encrypted;
                changed = true;
            }
        }
        return changed;
    }

    private static bool? Flag(Dictionary<string, string> kv, string key) =>
        kv.TryGetValue(key, out var v) ? v == "1" : null;

    private static long? Num(Dictionary<string, string> kv, string key) =>
        kv.TryGetValue(key, out var v) && long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) ? n : null;

    private static long[]? Fields(string text, int n)
    {
        var parts = text.Split(',');
        if (parts.Length < n)
            return null;
        var v = new long[n];
        for (var i = 0; i < n; i++)
            if (!long.TryParse(parts[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out v[i]))
                return null;
        return v;
    }

    /// <summary>p25s.fI=HZ,NAC,TG,SRC,CALLS,AGE_S</summary>
    public static (SearchCall Call, int Age)? ParseP25(string text, DateTime now)
    {
        if (Fields(text, 6) is not { } v || v[0] <= 0)
            return null;
        var age = (int)Math.Clamp(v[5], 0, 86400);
        return (new SearchCall(now.AddSeconds(-age), "P25", v[0], Nac(v[1]), "",
            v[2], v[3], false, (int)v[4]), age);
    }

    /// <summary>dmrs.fI=HZ,CC,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S</summary>
    public static (SearchCall Call, int Age)? ParseDmr(string text, DateTime now)
    {
        if (Fields(text, 10) is not { } v || v[0] <= 0)
            return null;
        var age = (int)Math.Clamp(v[9], 0, 86400);
        var cc = v[1] is >= 0 and <= 15 ? $"CC{v[1]}" : "CC?";
        return (new SearchCall(now.AddSeconds(-age), "DMR", v[0], cc, Slots(v[2], v[3]), v[6], v[7], v[5] != 0, (int)v[8]), age);
    }

    /// <summary>DMR slots seen and repeater / direct: "TS2 RPT", "TS1+2", "DIR".</summary>
    private static string Slots(long tsMask, long syncMask)
    {
        var slot = (tsMask & 3) switch { 1 => "TS1", 2 => "TS2", 3 => "TS1+2", _ => "" };
        if ((syncMask & 1) != 0)
            slot = (slot + " RPT").Trim();
        else if ((syncMask & 2) != 0)
            slot = (slot + " DIR").Trim();
        return slot;
    }

    /// <summary>alls.fI=HZ,MODE,CODE,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,CALL (MODE P25 / DMR / YSF / NXDN; CODE
    /// the NAC, colour code, DG-ID or RAN, 65535 not known; CALL the YSF callsign, the last field).</summary>
    public static (SearchCall Call, int Age)? ParseAll(string text, DateTime now)
    {
        var parts = text.Split(',', 12);
        if (parts.Length < 11)
            return null;
        var mode = parts[1].Trim();
        if (mode is not ("P25" or "DMR" or "YSF" or "NXDN"))
            return null;
        string[] nums = [parts[0], .. parts[2..11]];
        if (Fields(string.Join(',', nums), 10) is not { } v || v[0] <= 0)
            return null;
        var age = (int)Math.Clamp(v[9], 0, 86400);
        var known = v[1] is >= 0 and < 65535;
        var code = mode switch
        {
            "P25" => known ? Nac(v[1]) : "$?",
            "DMR" => v[1] is >= 0 and <= 15 ? $"CC{v[1]}" : "CC?",
            "YSF" => known ? $"DG{v[1]}" : "DG?",
            _ => known ? $"RAN{v[1]}" : "RAN?",
        };
        var slot = mode == "DMR" ? Slots(v[2], v[3]) : "";
        var caller = parts.Length > 11 ? parts[11].Trim() : "";
        return (new SearchCall(now.AddSeconds(-age), mode, v[0], code, slot, v[6], v[7], v[5] != 0, (int)v[8], caller), age);
    }

    /// <summary>A P25 NAC in both forms the radio shows: "$659/1625" (hex, decimal; owner 2026-11-21).</summary>
    public static string Nac(long nac) =>
        "$" + nac.ToString("X3", CultureInfo.InvariantCulture) + "/" + nac.ToString(CultureInfo.InvariantCulture);

    public static string Mhz(long hz) => (hz / 1e6).ToString("0.0000", CultureInfo.InvariantCulture);

    public const string CsvHeader = "first_heard,last_heard,mode,mhz,code,slot,group,last_source,caller,encrypted,calls";

    public static string CsvRow(SearchGroup g) => string.Join(",",
        g.First.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture),
        g.Last.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture), g.Mode, Mhz(g.Hz), g.Code, g.Slot,
        g.Group.ToString(CultureInfo.InvariantCulture), g.Source.ToString(CultureInfo.InvariantCulture),
        g.Caller.Replace(',', ' '), g.Encrypted ? "1" : "0", g.Calls.ToString(CultureInfo.InvariantCulture));

    /// <summary>Writes folder/search-YYYYMMDD.csv with every line of the log (rewritten whole, through a temporary file,
    /// so a crash never leaves half a file). The day is the day the log started.</summary>
    public static string Write(string folder, IEnumerable<SearchGroup> lines, DateTime day)
    {
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, $"search-{day:yyyyMMdd}.csv");
        var sb = new StringBuilder().Append(CsvHeader).Append('\n');
        foreach (var g in lines)
            sb.Append(CsvRow(g)).Append('\n');
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
        File.Move(tmp, path, overwrite: true);
        return path;
    }
}
