// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text.RegularExpressions;

namespace DDM32Plus.Core.DataPacks;

/// <summary>
/// The aerodromes' AD 2.18 tables ("ATS communication facilities") as page text -> the air band pack's frequency
/// lists: the port of datapack.parse_ad218 with tools/airfields.py's service words and channel rules. The user
/// downloads their State's AIP pages (in Turkey the DHMI eAIP) on their own PC; nothing of them is redistributed.
/// </summary>
public static partial class AirText
{
    public const int ChBaseKhz = 108000;            // air_freq_t.ch = (name - CH_BASE_KHZ) / 5
    public const int LoKhz = 118000, HiKhz = 137000;  // the VHF aeronautical mobile band (hi exclusive)

    /// <summary>airfield.h air_type_t, in display order within an airfield.</summary>
    public static readonly string[] Types =
        ["ATIS", "DEL", "GND", "RMP", "TWR", "APP", "DEP", "AFIS", "INFO", "CTAF", "PAR", "SAR", "EMR", "OPS", "OTH"];

    public static readonly IReadOnlyDictionary<string, string> TypeWords = new Dictionary<string, string>
    {
        ["ATIS"] = "ATIS", ["D-ATIS"] = "ATIS", ["D-ATIS-ARR"] = "ATIS", ["D-ATIS-DEP"] = "ATIS",
        ["DEL"] = "DEL", ["DLV"] = "DEL", ["CLD"] = "DEL", ["CLR"] = "DEL", ["CLNC"] = "DEL", ["CLEARANCE"] = "DEL",
        ["DELIVERY"] = "DEL",
        ["GND"] = "GND", ["GROUND"] = "GND", ["RMP"] = "RMP", ["RAMP"] = "RMP", ["APRON"] = "RMP",
        ["TWR"] = "TWR", ["TOWER"] = "TWR",
        ["APP"] = "APP", ["APPROACH"] = "APP", ["APP/DEP"] = "APP", ["ACP"] = "APP", ["ARR"] = "APP", ["DEP"] = "DEP",
        ["DEPARTURE"] = "DEP",
        ["AFIS"] = "AFIS", ["INFO"] = "INFO", ["FIS"] = "INFO", ["CTAF"] = "CTAF", ["UNIC"] = "CTAF", ["UNICOM"] = "CTAF",
        ["PAR"] = "PAR", ["SAR"] = "SAR", ["EMR"] = "EMR", ["EMERG"] = "EMR",
        ["HAVAS"] = "OPS", ["OPS"] = "OPS", ["POST"] = "OPS", ["PMSV"] = "OPS", ["PTD"] = "OPS", ["SFA"] = "OPS",
        ["TAFR"] = "OPS",
    };

    // Python's \s also takes U+001C..U+001F
    [GeneratedRegex(@"(?<![\d.])(1[0-3]\d[.,]\d{1,3})[\s\x1c-\x1f]*(?:MHZ|MHz|Mhz)?(?![\d.])", RegexOptions.CultureInvariant)]
    private static partial Regex Freq();

    [GeneratedRegex(@"AD[\s\x1c-\x1f]*[23][\s\x1c-\x1f]*\.[\s\x1c-\x1f]*18", RegexOptions.CultureInvariant)]
    private static partial Regex Section();

    [GeneratedRegex(@"AD[\s\x1c-\x1f]*[23][\s\x1c-\x1f]*\.[\s\x1c-\x1f]*19", RegexOptions.CultureInvariant)]
    private static partial Regex End();

    [GeneratedRegex(@"[^A-Za-z/ -]", RegexOptions.CultureInvariant)]
    private static partial Regex NotWord();

    [GeneratedRegex(@"(?<![A-Za-z0-9])(LT[A-Z]{2})(?![A-Za-z0-9])", RegexOptions.CultureInvariant)]
    private static partial Regex Code();

    /// <summary>airfields.channel_khz: a frequency ("121.58", "132", "118.675") -> the channel name in kHz, or the
    /// refusal reason ("not a number", "grid", "band").</summary>
    public static (int? Khz, string? Why) ChannelKhz(string text)
    {
        var t = PyText.Strip(text);
        var digits = new System.Text.StringBuilder();
        var dot = -1;
        var i = 0;
        if (i < t.Length && t[i] is '+' or '-')
            return (null, "not a number");          // never from the AD 2.18 pattern; kept simple
        for (; i < t.Length; i++)
        {
            var c = t[i];
            if (c == '.' && dot < 0)
            {
                dot = digits.Length;
                continue;
            }
            var d = CharUnicodeInfo.GetDecimalDigitValue(c);
            if (d < 0)
                return (null, "not a number");
            digits.Append((char)('0' + d));
        }
        if (digits.Length == 0)
            return (null, "not a number");
        var s = digits.ToString();
        var intPart = dot < 0 ? s : s[..dot];
        var frac = dot < 0 ? "" : s[dot..];
        var fracSig = frac.TrimEnd('0');
        if (fracSig.Length > 3)
            return (null, "grid");
        if (intPart.TrimStart('0').Length > 6)
            return (null, "band");
        var khz = (long.Parse(intPart.Length == 0 ? "0" : intPart, CultureInfo.InvariantCulture) * 1000) +
                  (fracSig.Length == 0 ? 0 : long.Parse(fracSig.PadRight(3, '0'), CultureInfo.InvariantCulture));
        if (khz is < LoKhz or >= HiKhz)
            return (null, "band");
        if (khz % 5 != 0 || khz % 25 == 20)
            return (null, "grid");
        return ((int)khz, null);
    }

    /// <summary>datapack.parse_ad218: the AD 2.18 (AD 3.18) table's lines -> [(type index, kHz)] in the table's
    /// order: each line with an air band frequency, its service named by the line's own words (or the last named one
    /// above it).</summary>
    public static List<(int Type, int Khz)> ParseAd218(string text)
    {
        var m = Section().Match(text);
        if (m.Success)
        {
            text = text[(m.Index + m.Length)..];
            var e = End().Match(text);
            if (e.Success)
                text = text[..e.Index];
        }
        var out_ = new List<(int, int)>();
        string? service = null;
        var oth = Array.IndexOf(Types, "OTH");
        foreach (var line in PyText.SplitLines(text))
        {
            var words = PyText.Split(NotWord().Replace(line, " ").ToUpperInvariant());
            var named = words.Where(TypeWords.ContainsKey).ToList();
            if (named.Count > 0)
                service = TypeWords[named[0]];
            foreach (Match f in Freq().Matches(line))
            {
                var (khz, why) = ChannelKhz(f.Groups[1].Value.Replace(',', '.'));
                if (why is not null || khz is null)
                    continue;
                var t = service is not null && Array.IndexOf(Types, service) >= 0 ? Array.IndexOf(Types, service) : oth;
                if (!out_.Contains((t, khz.Value)))
                    out_.Add((t, khz.Value));
            }
        }
        return out_;
    }

    /// <summary>The aerodrome code (LTxx) in a file name, else in the page text before its AD 2.18 heading, or null.
    /// (datapack.code_of does the same: an underscore around the code, as in LT_AD_2_LTAC_en.pdf, is no letter.)</summary>
    public static string? CodeOf(string fileName, string? text = null)
    {
        var name = Path.GetFileNameWithoutExtension(fileName);
        var ms = Code().Matches(name).Select(x => x.Groups[1].Value).ToList();
        if (ms.Count > 0)
            return ms[^1];
        if (text is not null)
        {
            var sec = Section().Match(text);
            var before = sec.Success ? text[..sec.Index] : text;
            var tm = Code().Matches(before).Select(x => x.Groups[1].Value).ToList();
            if (tm.Count > 0)
                return tm[^1];
        }
        return null;
    }

    public static string TypeName(int t) => t >= 0 && t < Types.Length ? Types[t] : "?";

    /// <summary>A channel's name as the radio shows it: 118.100, 132.005.</summary>
    public static string Mhz(int khz) => (khz / 1000.0).ToString("0.000", CultureInfo.InvariantCulture);
}
