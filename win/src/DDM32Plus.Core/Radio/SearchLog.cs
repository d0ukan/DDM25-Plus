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

/// <summary>
/// The search log (firmware tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the radio's P25, DMR and all
/// digital searches keep 8 finds each; read every second over the debug link (GET_VIEW sections p25s = 28, dmrs = 29,
/// alls = 30, read only, not counted as a look), every new call becomes a row here and in the day's CSV file. A call is new when an
/// entry appears, its call counter grows, or its talkgroup / source changes. A radio restart (counters back down) is
/// taken as a new start, not as calls.
/// </summary>
public sealed class SearchLogBook
{
    public const byte SectionP25 = 0x1C;
    public const byte SectionDmr = 0x1D;
    public const byte SectionAll = 0x1E;

    private sealed record Seen(int Calls, long Group, long Source, string Caller);

    // keyed by the search too: the all digital search's list is its own (its P25 / DMR finds next to the P25 / DMR
    // searches' own lists, which the radio keeps reporting after they stopped)
    private readonly Dictionary<(string Search, string Mode, long Hz), Seen> seen = [];
    private readonly List<SearchCall> calls = [];

    public IReadOnlyList<SearchCall> Calls => calls;

    /// <summary>Whether the radio reported each search running at the last poll (null = no answer / not built).</summary>
    public bool? P25On { get; private set; }
    public bool? DmrOn { get; private set; }
    public bool? AllOn { get; private set; }
    public long? P25AtHz { get; private set; }
    public long? DmrAtHz { get; private set; }
    public long? AllAtHz { get; private set; }

    /// <summary>Distinct frequencies with a call.</summary>
    public int Frequencies => calls.Select(c => (c.Mode, c.Hz)).Distinct().Count();

    /// <summary>One poll's key=value lines (all three sections); returns the new calls, oldest first.</summary>
    public List<SearchCall> Add(IEnumerable<string> lines, DateTime now)
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
        var fresh = new List<SearchCall>();
        // oldest entry first, so the rows come in the order the radio heard them
        for (var i = 7; i >= 0; i--)
        {
            if (kv.TryGetValue($"p25s.f{i}", out var p) && ParseP25(p, now) is { } pc)
                Note("p25s", pc, fresh);
            if (kv.TryGetValue($"dmrs.f{i}", out var d) && ParseDmr(d, now) is { } dc)
                Note("dmrs", dc, fresh);
            if (kv.TryGetValue($"alls.f{i}", out var a) && ParseAll(a, now) is { } ac)
                Note("alls", ac, fresh);
        }
        fresh.Sort((a, b) => a.Time.CompareTo(b.Time));
        calls.AddRange(fresh);
        return fresh;
    }

    private void Note(string search, (SearchCall Call, int Age) e, List<SearchCall> fresh)
    {
        var c = e.Call;
        var key = (search, c.Mode, c.Hz);
        var now = new Seen(c.Calls, c.Group, c.Source, c.Caller);
        if (!seen.TryGetValue(key, out var was))
        {
            seen[key] = now;
            if (c.Calls > 0 || c.Group != 0 || c.Source != 0 || c.Caller.Length > 0)
                fresh.Add(c);
            return;
        }
        seen[key] = now;
        if (c.Calls < was.Calls)
            return;	// the radio started again: a new baseline
        if (c.Calls > was.Calls || c.Group != was.Group || c.Source != was.Source || c.Caller != was.Caller)
            fresh.Add(c);
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

    public const string CsvHeader = "time,mode,mhz,code,slot,group,source,encrypted,calls,caller";

    public static string CsvRow(SearchCall c) => string.Join(",",
        c.Time.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture), c.Mode, Mhz(c.Hz), c.Code, c.Slot,
        c.Group.ToString(CultureInfo.InvariantCulture), c.Source.ToString(CultureInfo.InvariantCulture),
        c.Encrypted ? "1" : "0", c.Calls.ToString(CultureInfo.InvariantCulture), c.Caller.Replace(',', ' '));

    /// <summary>Appends rows to folder/search-YYYYMMDD.csv (the header once per file); the file is the night's log.</summary>
    public static string Append(string folder, IEnumerable<SearchCall> rows, DateTime day)
    {
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, $"search-{day:yyyyMMdd}.csv");
        var sb = new StringBuilder();
        if (!File.Exists(path))
            sb.Append(CsvHeader).Append('\n');
        foreach (var r in rows)
            sb.Append(CsvRow(r)).Append('\n');
        File.AppendAllText(path, sb.ToString(), new UTF8Encoding(false));
        return path;
    }
}
