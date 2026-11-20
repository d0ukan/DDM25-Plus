// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Numerics;
using System.Text.RegularExpressions;

namespace DDM32Plus.Core.Releases;

/// <summary>
/// A sortable 'MAJOR.MINOR.PATCH[-PRERELEASE]' (signed_manifest.parse_version): a prerelease sorts before its
/// release; prerelease parts compare numerically when all digits, else as text, and a number sorts before text.
/// The order is exactly Python's tuple order of parse_version's key (checked against it in the tests).
/// </summary>
public sealed partial class ReleaseVersion : IComparable<ReleaseVersion>, IEquatable<ReleaseVersion>
{
    // Same pattern as Python: \d also matches other Unicode digits there, and '$' also matches before a final '\n'
    [GeneratedRegex(@"^(\d{1,4})\.(\d{1,4})\.(\d{1,4})(?:-([0-9A-Za-z.-]{1,40}))?$")]
    private static partial Regex Pattern();

    private readonly record struct Part(int Kind, BigInteger Number, string Text);

    public string Text { get; }
    public int Major { get; }
    public int Minor { get; }
    public int Patch { get; }
    public string? Prerelease { get; }
    private readonly Part[] parts;

    private ReleaseVersion(string text, int major, int minor, int patch, string? pre)
    {
        Text = text;
        Major = major;
        Minor = minor;
        Patch = patch;
        Prerelease = pre;
        parts = pre is null ? [] : pre.Split('.').Select(ToPart).ToArray();
    }

    private static Part ToPart(string p) =>
        p.Length > 0 && p.All(char.IsAsciiDigit)
            ? new Part(0, BigInteger.Parse(p, CultureInfo.InvariantCulture), "")
            : new Part(1, BigInteger.Zero, p);

    public bool IsPrerelease => Prerelease is not null;

    public static ReleaseVersion Parse(string? text) =>
        TryParse(text) ?? throw new ManifestException($"Bad version '{text}'");

    public static ReleaseVersion? TryParse(string? text)
    {
        if (text is null)
            return null;
        var m = Pattern().Match(text);
        if (!m.Success)
            return null;
        return new ReleaseVersion(text, Digits(m.Groups[1].Value), Digits(m.Groups[2].Value), Digits(m.Groups[3].Value),
            m.Groups[4].Success ? m.Groups[4].Value : null);
    }

    // int() of the matched digits; Python's int() also reads non-ASCII decimal digits
    private static int Digits(string s)
    {
        var v = 0;
        foreach (var c in s)
            v = v * 10 + (int)char.GetNumericValue(c);
        return v;
    }

    public int CompareTo(ReleaseVersion? other)
    {
        if (other is null)
            return 1;
        var c = Major.CompareTo(other.Major);
        if (c == 0) c = Minor.CompareTo(other.Minor);
        if (c == 0) c = Patch.CompareTo(other.Patch);
        if (c != 0)
            return c;
        // Python: release (1,) and prerelease (0, part, part, ...)
        var tagA = IsPrerelease ? 0 : 1;
        var tagB = other.IsPrerelease ? 0 : 1;
        if (tagA != tagB)
            return tagA.CompareTo(tagB);
        for (var i = 0; i < Math.Min(parts.Length, other.parts.Length); i++)
        {
            var a = parts[i];
            var b = other.parts[i];
            c = a.Kind.CompareTo(b.Kind);
            if (c == 0) c = a.Number.CompareTo(b.Number);
            if (c == 0) c = string.CompareOrdinal(a.Text, b.Text);
            if (c != 0)
                return Math.Sign(c);
        }
        return parts.Length.CompareTo(other.parts.Length);
    }

    public bool Equals(ReleaseVersion? other) => other is not null && CompareTo(other) == 0;
    public override bool Equals(object? obj) => obj is ReleaseVersion v && Equals(v);
    public override int GetHashCode() => HashCode.Combine(Major, Minor, Patch, parts.Length);
    public override string ToString() => Text;

    public static bool operator <(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) < 0;
    public static bool operator >(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) > 0;
    public static bool operator <=(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) <= 0;
    public static bool operator >=(ReleaseVersion a, ReleaseVersion b) => a.CompareTo(b) >= 0;
    public static bool operator ==(ReleaseVersion? a, ReleaseVersion? b) => a?.Equals(b) ?? b is null;
    public static bool operator !=(ReleaseVersion? a, ReleaseVersion? b) => !(a == b);
}
