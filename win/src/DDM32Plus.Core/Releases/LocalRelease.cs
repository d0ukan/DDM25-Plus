// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.Json.Nodes;

namespace DDM32Plus.Core.Releases;

/// <summary>
/// A signed release in a local folder (developer builds): ddm32-manifest.json, its .sig and the files it lists, as
/// pc/release/make_release.py writes them. It stands in for GitHub as the network: the release list has this one
/// release, and every file comes from the folder. From there on the code path is the downloaded release's own
/// (ReleaseClient.LoadCatalogAsync and DownloadFirmwareAsync): the signature with the key in force (the embedded
/// release key in the program), the manifest schema, the release tag, then the zip's size and SHA-256 and the
/// application inside. Only plain file names directly in the folder are served.
/// </summary>
public sealed class LocalReleaseNet : IReleaseNet
{
    private readonly string folder;
    private readonly string tag;
    private readonly ManifestSchema schema;

    public LocalReleaseNet(string folder)
    {
        this.folder = Path.GetFullPath(folder);
        schema = ReleaseConstants.Schemas.FirstOrDefault(s => File.Exists(Path.Combine(this.folder, s.ManifestName)))
                 ?? throw new NetException($"No {ReleaseConstants.Current.ManifestName} in {folder}");
        // the tag only names the release in the list; the signed manifest must say the same (checked later)
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(this.folder, schema.ManifestName)));
            tag = doc.RootElement.TryGetProperty("release", out var r) && r.ValueKind == JsonValueKind.String
                ? r.GetString()! : throw new NetException("The manifest names no release");
        }
        catch (JsonException)
        {
            throw new NetException("The manifest is not JSON");
        }
        if (!SignedManifest.IsSafeFileName(tag))
            throw new NetException("The manifest's release name is not usable");
    }

    public string Folder => folder;

    private byte[] File_(string name, long limit)
    {
        if (!SignedManifest.IsSafeFileName(name) || name.Contains("..", StringComparison.Ordinal))
            throw new NetException($"Refusing the file name {name}");
        var path = Path.Combine(folder, name);
        var info = new FileInfo(path);
        if (!info.Exists)
            throw new NetException($"Not in the release folder: {name}");
        if (info.Length > limit)
            throw new NetException($"Answer larger than {limit} bytes: {name}");
        return File.ReadAllBytes(path);
    }

    /// <summary>The file name of an asset URL of this release (current names; the release's own prefix).</summary>
    private string NameOf(string url, string prefix)
    {
        if (!url.StartsWith(prefix, StringComparison.Ordinal))
            throw new NetException($"Refusing a URL outside the release: {url}");
        var name = url[prefix.Length..];
        if (name.Contains('/') || name.Contains('\\'))
            throw new NetException($"Refusing a URL outside the release: {url}");
        return name;
    }

    public Task<byte[]> FetchAsync(string url, int limit, CancellationToken ct = default)
    {
        if (url == ReleaseConstants.ApiUrl)
        {
            var assets = new JsonArray();
            foreach (var n in new[] { schema.ManifestName, schema.SigName }.Where(n => File.Exists(Path.Combine(folder, n))))
                assets.Add(new JsonObject
                {
                    ["name"] = n, ["browser_download_url"] = ReleaseConstants.Current.UrlPrefix + tag + "/" + n,
                });
            var listing = new JsonArray { new JsonObject { ["tag_name"] = tag, ["assets"] = assets } };
            return Task.FromResult(System.Text.Encoding.UTF8.GetBytes(listing.ToJsonString()));
        }
        return Task.FromResult(File_(NameOf(url, ReleaseConstants.Current.UrlPrefix + tag + "/"), limit));
    }

    public async Task DownloadAsync(string url, string dest, long size, Action<long, long>? progress, CancellationToken ct = default)
    {
        // the manifest's asset URL: https://github.com/d0ukan/DDM32-Plus/releases/download/TAG/FILE (schema-checked)
        var rest = url.StartsWith(ReleaseConstants.Current.UrlPrefix, StringComparison.Ordinal)
            ? url[ReleaseConstants.Current.UrlPrefix.Length..] : throw new NetException($"Refusing a URL outside the release: {url}");
        var parts = rest.Split('/');
        if (parts.Length != 2)
            throw new NetException($"Refusing a URL outside the release: {url}");
        var data = File_(parts[1], size);           // more than the signed size is refused, like a download
        var part = dest + ".partial";
        await File.WriteAllBytesAsync(part, data, ct).ConfigureAwait(false);
        progress?.Invoke(data.Length, size);
        File.Move(part, dest, overwrite: true);
    }
}

public static partial class ReleaseClientLocal
{
    /// <summary>The signed release in a local folder, verified exactly like one from GitHub; its versions are marked
    /// local (their files come from the folder). Manifests verified here are kept under releasesRoot/local.</summary>
    public static async Task<Catalog> LoadAsync(string folder, string releasesRoot, string? key = null, CancellationToken ct = default)
    {
        var net = new LocalReleaseNet(folder);
        var cat = await ReleaseClient.LoadCatalogAsync(Path.Combine(releasesRoot, "local"), net, key, ct: ct).ConfigureAwait(false);
        cat.Source = "local";
        foreach (var fv in cat.Firmware)
            fv.Source = net;
        return cat;
    }
}
