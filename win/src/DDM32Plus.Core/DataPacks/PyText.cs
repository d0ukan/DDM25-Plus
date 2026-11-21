// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text;

namespace DDM32Plus.Core.DataPacks;

/// <summary>The few Python string rules the pack builder depends on, so the C# builder gives datapack.py's bytes:
/// str.strip / split / splitlines whitespace, int(), upper() of a callsign, ASCII encode with 'replace', ordering by
/// code point.</summary>
internal static class PyText
{
    /// <summary>str.isspace for one UTF-16 unit: .NET's white space plus U+001C..U+001F (Python counts them).</summary>
    public static bool IsSpace(char c) => char.IsWhiteSpace(c) || c is >= '\x1c' and <= '\x1f';

    public static string Strip(string s)
    {
        int a = 0, b = s.Length;
        while (a < b && IsSpace(s[a]))
            a++;
        while (b > a && IsSpace(s[b - 1]))
            b--;
        return s[a..b];
    }

    /// <summary>str.split() with no argument: runs of white space separate, none at the ends.</summary>
    public static List<string> Split(string s)
    {
        var out_ = new List<string>();
        var sb = new StringBuilder();
        foreach (var c in s)
        {
            if (IsSpace(c))
            {
                if (sb.Length > 0)
                {
                    out_.Add(sb.ToString());
                    sb.Clear();
                }
            }
            else
                sb.Append(c);
        }
        if (sb.Length > 0)
            out_.Add(sb.ToString());
        return out_;
    }

    /// <summary>str.splitlines(): \n, \r, \r\n, \v, \f, \x1c, \x1d, \x1e, \x85, U+2028, U+2029.</summary>
    public static List<string> SplitLines(string s)
    {
        var out_ = new List<string>();
        var start = 0;
        for (var i = 0; i < s.Length; i++)
        {
            var c = s[i];
            if (c is '\n' or '\r' or '\v' or '\f' or '\x1c' or '\x1d' or '\x1e' or '\x85' or '\u2028' or '\u2029')
            {
                out_.Add(s[start..i]);
                if (c == '\r' && i + 1 < s.Length && s[i + 1] == '\n')
                    i++;
                start = i + 1;
            }
        }
        if (start < s.Length)
            out_.Add(s[start..]);
        return out_;
    }

    /// <summary>int(s) of a decimal string (already stripped): an optional sign, Unicode decimal digits, single
    /// underscores between digits. Null where Python raises ValueError.</summary>
    public static long? ParseInt(string s)
    {
        s = Strip(s);
        var i = 0;
        var neg = false;
        if (i < s.Length && s[i] is '+' or '-')
        {
            neg = s[i] == '-';
            i++;
        }
        if (i >= s.Length)
            return null;
        long v = 0;
        var lastUnderscore = true;          // no underscore first
        for (; i < s.Length; i++)
        {
            var c = s[i];
            if (c == '_')
            {
                if (lastUnderscore)
                    return null;
                lastUnderscore = true;
                continue;
            }
            var d = CharUnicodeInfo.GetDecimalDigitValue(c);
            if (d < 0 || char.IsSurrogate(c))
                return null;
            lastUnderscore = false;
            v = v > (long.MaxValue - d) / 10 ? long.MaxValue : v * 10 + d;      // only "too big" matters to the caller
        }
        if (lastUnderscore)
            return null;
        return neg ? -v : v;
    }

    /// <summary>str.upper() as far as it decides whether a callsign is A-Z / 0-9: the full mappings that end in ASCII
    /// (ß, the Latin ligatures) and the simple ones for the rest (ı -> I, ſ -> S).</summary>
    public static string Upper(string s)
    {
        var sb = new StringBuilder(s.Length);
        foreach (var c in s)
        {
            sb.Append(c switch
            {
                'ß' => "SS",
                'ı' => "I",                       // .NET's invariant casing keeps the dotless i
                'ſ' => "S",
                '\uFB00' => "FF",
                '\uFB01' => "FI",
                '\uFB02' => "FL",
                '\uFB03' => "FFI",
                '\uFB04' => "FFL",
                '\uFB05' or '\uFB06' => "ST",
                _ => char.ToUpperInvariant(c).ToString(),
            });
        }
        return sb.ToString();
    }

    /// <summary>s.encode('ascii', 'replace'): one '?' a code point outside ASCII.</summary>
    public static byte[] AsciiReplace(string s)
    {
        var out_ = new List<byte>(s.Length);
        foreach (var r in s.EnumerateRunes())
            out_.Add(r.Value < 0x80 ? (byte)r.Value : (byte)'?');
        return [.. out_];
    }

    /// <summary>b.decode('ascii', 'replace'): U+FFFD a byte outside ASCII.</summary>
    public static string AsciiDecode(ReadOnlySpan<byte> b)
    {
        var sb = new StringBuilder(b.Length);
        foreach (var x in b)
            sb.Append(x < 0x80 ? (char)x : '\uFFFD');
        return sb.ToString();
    }

    public static ReadOnlySpan<byte> RStripNul(ReadOnlySpan<byte> b)
    {
        var n = b.Length;
        while (n > 0 && b[n - 1] == 0)
            n--;
        return b[..n];
    }

    /// <summary>Python's string order: by code point (UTF-16 order differs only above U+FFFF).</summary>
    public static int CompareCodePoints(string? a, string? b)
    {
        a ??= "";
        b ??= "";
        var ea = a.EnumerateRunes().GetEnumerator();
        var eb = b.EnumerateRunes().GetEnumerator();
        while (true)
        {
            var ha = ea.MoveNext();
            var hb = eb.MoveNext();
            if (!ha || !hb)
                return ha == hb ? 0 : ha ? 1 : -1;
            var c = ea.Current.Value.CompareTo(eb.Current.Value);
            if (c != 0)
                return c;
        }
    }

    /// <summary>s[:n] for ASCII text (the builder cuts only folded, ASCII-only text).</summary>
    public static string Left(string s, int n) => s.Length <= n ? s : s[..n];

    /// <summary>s.ljust(n)</summary>
    public static string PadRight(string s, int n) => s.PadRight(n);
}
