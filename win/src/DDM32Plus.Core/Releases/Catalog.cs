// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core.Releases;

/// <summary>A firmware version from a verified manifest (releases.FirmwareVersion).</summary>
public sealed class FirmwareVersion(FirmwareEntry entry, string release, string? manifestDir, ManifestSchema schema)
{
    public FirmwareEntry Entry { get; } = entry;
    public string Release { get; } = release;
    public string? ManifestDir { get; } = manifestDir;
    public ManifestSchema Schema { get; } = schema;
    public string Version => Entry.Version;
    public string Channel => Entry.Channel;
    public string ApplicationSha256 => Entry.ApplicationSha256;
    public long ApplicationSize => Entry.ApplicationSize;
    public bool IsBeta => Channel == "beta";

    /// <summary>From a release that still uses the DDM25 names: listed, never installed by this program.</summary>
    public bool IsLegacy => Schema.IsLegacy;

    public ReleaseVersion Key => ReleaseVersion.Parse(Version);
    public string Notes(string lang) => Entry.Notes?.For(lang) ?? "";
}

/// <summary>A program zip from a verified manifest, with its release tag and cache folder.</summary>
public sealed record AppRelease(AssetEntry Entry, string Release, string? ManifestDir);

/// <summary>The verified releases (releases.Catalog): firmware newest first, program updates, refused releases.</summary>
public sealed class Catalog
{
    public List<FirmwareVersion> Firmware { get; private set; } = [];
    public List<AppRelease> Apps { get; private set; } = [];
    public List<(string Tag, string Reason)> Rejected { get; } = [];
    public string? CheckedUtc { get; set; }
    public string Source { get; set; } = "";

    public void Add(string tag, Manifest manifest, string? manifestDir)
    {
        var known = Firmware.Select(f => f.Version).ToHashSet(StringComparer.Ordinal);
        foreach (var e in manifest.Firmware)
            if (known.Add(e.Version))
                Firmware.Add(new FirmwareVersion(e, tag, manifestDir, manifest.Schema));
        // Only this Windows program's own zip ("app_net") is a program update; never from a legacy manifest
        if (manifest.AppNet is not null && !manifest.Schema.IsLegacy)
            Apps.Add(new AppRelease(manifest.AppNet, tag, manifestDir));
    }

    /// <summary>Newest first; a stable sort, like Python's list.sort.</summary>
    public void Sort()
    {
        Firmware = [.. Firmware.OrderByDescending(f => ReleaseVersion.Parse(f.Version))];
        Apps = [.. Apps.OrderByDescending(a => ReleaseVersion.Parse(a.Entry.Version))];
    }

    public FirmwareVersion? Find(string version) => Firmware.FirstOrDefault(f => f.Version == version);

    public FirmwareVersion? ByApplication(string sha) => Firmware.FirstOrDefault(f => f.ApplicationSha256 == sha);

    /// <summary>The newest installable (not legacy) firmware, betas only when wanted.</summary>
    public FirmwareVersion? Newest(bool includeBeta = true) =>
        Firmware.FirstOrDefault(f => !f.IsLegacy && (includeBeta || !f.IsBeta));

    /// <summary>A program newer than current (prereleases only when includeBeta), else null.</summary>
    public AppRelease? NewerApp(string current, bool includeBeta = true)
    {
        var cur = ReleaseVersion.Parse(current);
        return Apps.FirstOrDefault(a => ReleaseVersion.Parse(a.Entry.Version) > cur &&
                                        (includeBeta || !a.Entry.Version.Contains('-')));
    }
}

/// <summary>What the user sees for a firmware version.</summary>
public enum ReleaseLabel
{
    /// <summary>The newest stable version.</summary>
    Latest,
    /// <summary>An older stable version.</summary>
    Stable,
    Beta,
    /// <summary>A test or alpha build: asks "are you sure?" before it is used.</summary>
    TestAlpha,
    /// <summary>A release with the old DDM25 names: shown, not installable here.</summary>
    Legacy,
}

/// <summary>
/// Maps the manifest's channels (beta, stable) to the labels the user sees. The manifest format has only beta and
/// stable; a test or alpha build is a beta whose prerelease starts with alpha, test or dev (e.g. 1.2.0-alpha.1).
/// </summary>
public static class ReleaseLabels
{
    private static readonly string[] TestPrefixes = ["alpha", "test", "dev"];

    public static bool IsTestOrAlpha(string version)
    {
        var pre = ReleaseVersion.TryParse(version)?.Prerelease;
        if (pre is null)
            return false;
        var first = pre.Split('.')[0].ToLowerInvariant();
        return TestPrefixes.Any(p => first.StartsWith(p, StringComparison.Ordinal));
    }

    public static ReleaseLabel For(FirmwareVersion fv, IEnumerable<FirmwareVersion> all)
    {
        if (fv.IsLegacy)
            return ReleaseLabel.Legacy;
        if (IsTestOrAlpha(fv.Version))
            return ReleaseLabel.TestAlpha;
        if (fv.IsBeta)
            return ReleaseLabel.Beta;
        var newestStable = all.Where(f => !f.IsLegacy && !f.IsBeta && !IsTestOrAlpha(f.Version))
            .OrderByDescending(f => f.Key).FirstOrDefault();
        return newestStable is not null && newestStable.Version == fv.Version ? ReleaseLabel.Latest : ReleaseLabel.Stable;
    }

    /// <summary>A second "are you sure?" before this label is downloaded or installed.</summary>
    public static bool NeedsConfirmation(ReleaseLabel label) => label == ReleaseLabel.TestAlpha;

    public static bool Installable(ReleaseLabel label) => label != ReleaseLabel.Legacy;
}
