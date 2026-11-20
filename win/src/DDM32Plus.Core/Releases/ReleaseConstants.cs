// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core.Releases;

/// <summary>
/// The names a signed release manifest uses. <see cref="ReleaseConstants.Current"/> is what the release tool
/// (pc/release/make_release.py) writes since the rename to DDM32 Plus; <see cref="ReleaseConstants.Legacy"/> is what
/// the first release (v1.0.0-beta.1, still named DDM25 Plus) carries. A legacy release is listed and its signature
/// verified, but it is never offered for install or as a program update (the Python installer only accepts the
/// current names).
/// </summary>
public sealed record ManifestSchema(string Format, string Repo, string ManifestName, bool IsLegacy)
{
    public string SigName => ManifestName + ".sig";
    public string UrlPrefix => $"https://github.com/{Repo}/releases/download/";
}

/// <summary>Every constant of the signed release format in one place (pc/radiotools/signed_manifest.py).</summary>
public static class ReleaseConstants
{
    /// <summary>The release signing key's public half (pc/radiotools/signed_manifest.py PUBLIC_KEY_HEX).</summary>
    public const string PublicKeyHex = "70fce9e822ac07f0787a991795b27ab7650f35dfc83bcd7ef0ad550fa675b411";

    /// <summary>The first 16 hex digits of SHA-256(public key): shown to the user.</summary>
    public const string PublicKeyId = "dda8c4a85c4600df";

    public static readonly ManifestSchema Current = new("ddm32-manifest/1", "d0ukan/DDM32-Plus", "ddm32-manifest.json", false);
    public static readonly ManifestSchema Legacy = new("ddm25-manifest/1", "d0ukan/DDM25-Plus", "ddm25-manifest.json", true);
    public static readonly IReadOnlyList<ManifestSchema> Schemas = [Current, Legacy];

    public const string ApiUrl = "https://api.github.com/repos/d0ukan/DDM32-Plus/releases?per_page=100";

    public static readonly IReadOnlyList<string> AllowedHosts =
        ["api.github.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"];

    public static readonly IReadOnlyList<string> Channels = ["beta", "stable"];
    public static readonly IReadOnlyList<string> PackageFiles = ["rfoundry_dm32.bin", "rfoundry_dm32.json", "rfoundry_dm32.map"];

    public const int MaxApi = 8 << 20;
    public const int MaxManifest = 1 << 20;
    public const int MaxSig = 1024;
    public const long MaxAssetSize = 512L << 20;
    public const long MaxPackageMember = 64L << 20;
    public const int AppStart = 0xC000;
    public const int CodecSlot = 0xE8000;
    public const string ManifestCache = "manifests";
}
