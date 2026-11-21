// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;

namespace DDM32Plus.Core.DataPacks;

/// <summary>What the RadioID list reading left out (iddb.read's "skipped").</summary>
public sealed record RadioIdSkipped(int Callsign, int Id, int Duplicate);

/// <summary>One ID taken from the list: the callsign (upper case, A-Z / 0-9, up to 6), its base-37 number, the short
/// name.</summary>
public sealed record RadioIdRow(int Id, string Call, long Packed, string Name);

/// <summary>
/// RadioID.net's user list ("user.csv": RADIO_ID, CALLSIGN, FIRST_NAME, LAST_NAME, CITY, STATE, COUNTRY), read as
/// tools/iddb.py reads it for the pack builder (iddb.read, fold, short_name, pack_call), with Python's csv rules
/// (the excel dialect). The user downloads the file from radioid.net themselves; it is never redistributed.
/// </summary>
public static class RadioIdCsv
{
    public const string DownloadPage = "https://radioid.net/database/dumps";
    public const string DownloadUrl = "https://radioid.net/static/user.csv";
    public const int NameMax = 10;
    public const int CallMax = 6;
    public const int IdMax = 0xFFFFFF;
    private const string Alphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    private static readonly Dictionary<string, string> CountryAliases = new() { ["turkey"] = "turkiye" };

    private static readonly Dictionary<char, string> Special = new()
    {
        ['ı'] = "i", ['İ'] = "I", ['ß'] = "ss", ['ø'] = "o", ['Ø'] = "O", ['æ'] = "ae", ['Æ'] = "AE", ['đ'] = "d",
        ['Đ'] = "D", ['ł'] = "l", ['Ł'] = "L", ['œ'] = "oe", ['Œ'] = "OE", ['þ'] = "th", ['Þ'] = "Th", ['ð'] = "d",
        ['Ð'] = "D",
    };

    /// <summary>iddb.fold: ASCII only (the Turkish dotless / dotted i and a few letters first, then accents off).</summary>
    public static string Fold(string text)
    {
        var sb = new StringBuilder(text.Length);
        foreach (var c in text)
        {
            if (Special.TryGetValue(c, out var r))
                sb.Append(r);
            else
                sb.Append(c);
        }
        var n = sb.ToString().Normalize(NormalizationForm.FormKD);
        var out_ = new StringBuilder(n.Length);
        foreach (var c in n)
        {
            if (c < 128)
                out_.Append(c);
        }
        return out_.ToString();
    }

    public static string CountryKey(string name)
    {
        var k = PyText.Strip(Fold(name)).ToLowerInvariant();
        return CountryAliases.TryGetValue(k, out var a) ? a : k;
    }

    private static bool IsAsciiAlnum(char c) => c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9';

    private static bool IsUpper(string w) => w.Any(char.IsAsciiLetter) && !w.Any(char.IsAsciiLetterLower);

    private static bool IsLower(string w) => w.Any(char.IsAsciiLetter) && !w.Any(char.IsAsciiLetterUpper);

    /// <summary>iddb.short_name: the first word of the first name (else of the last name), ASCII, letters / digits /
    /// ' - . only, at most 10 characters; an all-upper or all-lower name in title case.</summary>
    public static string ShortName(string first, string last)
    {
        foreach (var field in new[] { first, last })
        {
            var cleaned = new string([.. Fold(field).Select(c => IsAsciiAlnum(c) || c is '\'' or '-' or '.' ? c : ' ')]);
            var words = PyText.Split(cleaned);
            if (words.Count > 0)
            {
                var w = PyText.Left(words[0], NameMax);
                if (IsUpper(w) || IsLower(w))
                    w = w[..1].ToUpperInvariant() + w[1..].ToLowerInvariant();
                return w;
            }
        }
        return "";
    }

    /// <summary>iddb.pack_call: base-37 of up to 6 characters (A-Z, 0-9) padded with ' ' on the right; null when it
    /// does not fit.</summary>
    public static long? PackCall(string call)
    {
        call = PyText.Upper(PyText.Strip(call));
        if (call.Length == 0 || call.Length > CallMax || call.Any(c => Alphabet.IndexOf(c, 1) < 0))
            return null;
        long v = 0;
        foreach (var c in call.PadRight(CallMax))
            v = v * 37 + Alphabet.IndexOf(c);
        return v;
    }

    /// <summary>raw.decode('utf-8-sig', errors='replace')</summary>
    public static string Decode(byte[] raw)
    {
        var start = raw.Length >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
        return new UTF8Encoding(false, false).GetString(raw, start, raw.Length - start);
    }

    /// <summary>iddb.read(path, countries): the rows sorted by ID and what was skipped. Neither country given:
    /// Turkey.</summary>
    public static (List<RadioIdRow> Rows, RadioIdSkipped Skipped) Read(byte[] raw, IEnumerable<string> countries)
    {
        var want = countries.Select(CountryKey).ToHashSet(StringComparer.Ordinal);
        if (want.Count == 0)
            want.Add(CountryKey("Turkey"));
        var records = PyCsv.Rows(Decode(raw)).GetEnumerator();
        if (!records.MoveNext())
            throw new PackError("not a RadioID.net user.csv (needs columns CALLSIGN, COUNTRY, FIRST_NAME, RADIO_ID)");
        var names = records.Current;
        string[] need = ["CALLSIGN", "COUNTRY", "FIRST_NAME", "RADIO_ID"];
        if (!need.All(names.Contains))
            throw new PackError("not a RadioID.net user.csv (needs columns CALLSIGN, COUNTRY, FIRST_NAME, RADIO_ID)");
        var rows = new Dictionary<int, RadioIdRow>();
        int skCall = 0, skId = 0, skDup = 0;
        while (records.MoveNext())
        {
            var fields = records.Current;
            if (fields.Count == 0)
                continue;                               // csv.DictReader skips empty rows
            var r = new Dictionary<string, string>(StringComparer.Ordinal);
            for (var i = 0; i < Math.Min(names.Count, fields.Count); i++)
                r[names[i]] = fields[i];                // a repeated column name: the last one wins
            string Get(string k) => r.TryGetValue(k, out var v) ? v : "";
            var call = PyText.Upper(PyText.Strip(Get("CALLSIGN")));
            if (!want.Contains(CountryKey(Get("COUNTRY"))))
                continue;
            var parsed = PyText.ParseInt(Get("RADIO_ID"));
            if (parsed is null)
            {
                skId++;
                continue;
            }
            if (parsed <= 0 || parsed > IdMax)
            {
                skId++;
                continue;
            }
            var rid = (int)parsed.Value;
            var packed = PackCall(call);
            if (packed is null)
            {
                skCall++;
                continue;
            }
            if (rows.ContainsKey(rid))
            {
                skDup++;
                continue;
            }
            rows[rid] = new RadioIdRow(rid, call, packed.Value, ShortName(Get("FIRST_NAME"), Get("LAST_NAME")));
        }
        return ([.. rows.Values.OrderBy(x => x.Id)], new RadioIdSkipped(skCall, skId, skDup));
    }
}

/// <summary>Python's csv.reader with the default (excel) dialect, strict off: ',' separates, '"' quotes, "" inside
/// quotes is one '"', a line ends at \r, \n or \r\n outside quotes, a stray quote inside an unquoted field is kept.</summary>
internal static class PyCsv
{
    private enum S { StartRecord, StartField, InField, InQuoted, QuoteInQuoted, EatCrnl }

    public static IEnumerable<List<string>> Rows(string text)
    {
        var fields = new List<string>();
        var field = new StringBuilder();
        var state = S.StartRecord;
        foreach (var c in text)
        {
            switch (state)
            {
                case S.StartRecord:
                    if (c is '\n' or '\r')
                    {
                        state = S.EatCrnl;
                        yield return [];
                        break;
                    }
                    state = S.StartField;
                    goto case S.StartField;
                case S.StartField:
                    if (c is '\n' or '\r')
                    {
                        fields.Add("");
                        yield return fields;
                        fields = [];
                        state = S.EatCrnl;
                    }
                    else if (c == '"')
                        state = S.InQuoted;
                    else if (c == ',')
                        fields.Add("");
                    else
                    {
                        field.Append(c);
                        state = S.InField;
                    }
                    break;
                case S.InField:
                    if (c is '\n' or '\r')
                    {
                        fields.Add(field.ToString());
                        field.Clear();
                        yield return fields;
                        fields = [];
                        state = S.EatCrnl;
                    }
                    else if (c == ',')
                    {
                        fields.Add(field.ToString());
                        field.Clear();
                        state = S.StartField;
                    }
                    else
                        field.Append(c);
                    break;
                case S.InQuoted:
                    if (c == '"')
                        state = S.QuoteInQuoted;
                    else
                        field.Append(c);
                    break;
                case S.QuoteInQuoted:
                    if (c == '"')
                    {
                        field.Append('"');
                        state = S.InQuoted;
                    }
                    else if (c == ',')
                    {
                        fields.Add(field.ToString());
                        field.Clear();
                        state = S.StartField;
                    }
                    else if (c is '\n' or '\r')
                    {
                        fields.Add(field.ToString());
                        field.Clear();
                        yield return fields;
                        fields = [];
                        state = S.EatCrnl;
                    }
                    else
                    {
                        field.Append(c);
                        state = S.InField;
                    }
                    break;
                case S.EatCrnl:
                    if (c is '\n' or '\r')
                        break;
                    state = S.StartRecord;
                    goto case S.StartRecord;
            }
        }
        if (state is S.StartField or S.InField or S.InQuoted or S.QuoteInQuoted)
        {
            if (state != S.StartField || fields.Count > 0)
            {
                fields.Add(field.ToString());
                yield return fields;
            }
        }
    }
}
