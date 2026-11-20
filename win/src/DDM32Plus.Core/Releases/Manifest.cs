// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core.Releases;

/// <summary>A refused manifest, signature or version (signed_manifest.ManifestError).</summary>
public sealed class ManifestException(string message) : Exception(message);

/// <summary>Release notes in both languages.</summary>
public sealed record Notes(string Tr, string En)
{
    public string For(string lang) =>
        lang == "en" ? (En.Length > 0 ? En : Tr) : (Tr.Length > 0 ? Tr : En);
}

/// <summary>A downloadable asset of a signed manifest (firmware zip or program zip).</summary>
public record AssetEntry(string Version, string File, string Url, string Sha256, long Size, Notes? Notes);

/// <summary>A firmware entry of a signed manifest (all fields checked by SignedManifest.CheckManifest).</summary>
public sealed record FirmwareEntry(string Version, string Channel, string File, string Url, string Sha256, long Size,
    string ApplicationSha256, long ApplicationSize, int SettingsFormat, int CodeplugFormat,
    string MinCompatibleVersion, Notes Notes) : AssetEntry(Version, File, Url, Sha256, Size, Notes);

/// <summary>
/// A verified manifest. <see cref="App"/> is the Python program's zip ("app"); <see cref="AppNet"/> is this Windows
/// program's zip ("app_net", optional; a key the Python checker ignores), so neither program updates itself to the
/// other one.
/// </summary>
public sealed record Manifest(ManifestSchema Schema, string Release, string CreatedUtc,
    IReadOnlyList<FirmwareEntry> Firmware, AssetEntry? App, AssetEntry? AppNet);
