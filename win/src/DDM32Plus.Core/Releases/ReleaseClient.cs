// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Text.Json;

namespace DDM32Plus.Core.Releases;

/// <summary>Why a download was refused (the UI shows a translated text for the kind).</summary>
public enum VerifyFailure
{
    HashMismatch,
    UnexpectedContents,
    MemberTooLarge,
    ApplicationMismatch,
    PackageNotInstallable,
    NotInstallable,
}

/// <summary>A download that does not match the signed manifest (releases.VerifyError).</summary>
public sealed class VerifyException(VerifyFailure kind, string message) : Exception(message)
{
    public VerifyFailure Kind { get; } = kind;
}

/// <summary>
/// A downloaded release folder (releases.Package): rfoundry_dm32.bin/.json/.map with the signed manifest that lists
/// the application. Installable only when the gate passes and all three files are there.
/// </summary>
public sealed class Package
{
    public string Folder { get; }
    public string Image => Path.Combine(Folder, "rfoundry_dm32.bin");
    public List<string> Problems { get; } = [];
    public FirmwareEntry? Entry { get; }
    public string Sha256 { get; } = "";
    public long Size { get; }
    public string Version { get; }

    public Package(string folder, string? key = null)
    {
        Folder = folder;
        try
        {
            var app = File.ReadAllBytes(Image);
            Sha256 = SignedManifest.Sha256Hex(app);
            Size = app.Length;
            Entry = SignedManifest.ReleaseGate(app, folder, key);
            foreach (var name in ReleaseConstants.PackageFiles)
                if (!File.Exists(Path.Combine(folder, name)))
                    Problems.Add($"missing {name}");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Problems.Add($"unreadable ({e.GetType().Name})");
        }
        catch (ManifestException e)
        {
            Problems.Add($"signature check failed: {e.Message}");
        }
        Version = Entry?.Version ?? Path.GetFileName(folder);
    }

    public bool Installable => Entry is not null && Problems.Count == 0;
    public string Channel => Entry?.Channel ?? "";
}

/// <summary>
/// The signed releases on GitHub (pc/ddm32_app/releases.py): the list, the verified cache, firmware downloads with
/// hash checks, the program download. Network access only through <see cref="IReleaseNet"/>.
/// </summary>
public static class ReleaseClient
{
    private static string CacheDir(string releasesRoot) => Path.Combine(releasesRoot, ReleaseConstants.ManifestCache);

    private static string Store(string cache, string tag, ManifestSchema schema, byte[] data, byte[] sig)
    {
        var d = Path.Combine(cache, tag);
        Directory.CreateDirectory(d);
        File.WriteAllBytes(Path.Combine(d, schema.ManifestName), data);
        File.WriteAllBytes(Path.Combine(d, schema.SigName), sig);
        return d;
    }

    /// <summary>
    /// The signed releases on GitHub: one API call, then each release's manifest and signature. Verified manifests
    /// are kept under releasesRoot/manifests/TAG/ for offline use. A release with the old DDM25 manifest names is
    /// verified with those names and listed as legacy.
    /// </summary>
    public static async Task<Catalog> LoadCatalogAsync(string releasesRoot, IReleaseNet net, string? key = null,
        Func<string>? nowUtc = null, CancellationToken ct = default)
    {
        var cat = new Catalog { Source = "github", CheckedUtc = (nowUtc ?? NowUtc)() };
        var body = await net.FetchAsync(ReleaseConstants.ApiUrl, ReleaseConstants.MaxApi, ct).ConfigureAwait(false);
        JsonDocument listing;
        try
        {
            listing = JsonDocument.Parse(body);
        }
        catch (JsonException)
        {
            throw new NetException("GitHub answered with something that is not a release list");
        }
        using (listing)
        {
            if (listing.RootElement.ValueKind != JsonValueKind.Array)
                throw new NetException("GitHub answered with something that is not a release list");
            var cache = CacheDir(releasesRoot);
            foreach (var r in listing.RootElement.EnumerateArray())
            {
                if (r.ValueKind != JsonValueKind.Object || PyTruthy(SignedManifest.Get(r, "draft")))
                    continue;
                var tag = PyStr(SignedManifest.Get(r, "tag_name")) ?? "";
                var assets = Assets(SignedManifest.Get(r, "assets"));
                var schema = ReleaseConstants.Schemas.FirstOrDefault(s =>
                    assets.ContainsKey(s.ManifestName) && assets.ContainsKey(s.SigName));
                if (schema is null)
                {
                    cat.Rejected.Add((tag, "no signed manifest"));
                    continue;
                }
                byte[] data, sig;
                Manifest m;
                try
                {
                    var urls = new[] { schema.ManifestName, schema.SigName }
                        .Select(n => SignedManifest.Get(assets[n], "browser_download_url") is { ValueKind: JsonValueKind.String } u
                            ? u.GetString()! : "")
                        .ToArray();
                    // The asset URLs come from this repository's API (renamed from DDM25-Plus): always the current name
                    foreach (var u in urls)
                        if (!u.StartsWith(ReleaseConstants.Current.UrlPrefix + tag + "/", StringComparison.Ordinal))
                            throw new ManifestException("manifest asset outside this release");
                    data = await net.FetchAsync(urls[0], ReleaseConstants.MaxManifest, ct).ConfigureAwait(false);
                    sig = await net.FetchAsync(urls[1], ReleaseConstants.MaxSig, ct).ConfigureAwait(false);
                    m = SignedManifest.VerifyBytes(data, sig, key, schema);
                    if (m.Release != tag)
                        throw new ManifestException($"manifest is for release {m.Release}, not {tag}");
                }
                catch (Exception e) when (e is ManifestException or NetException)
                {
                    cat.Rejected.Add((tag, e.Message));
                    continue;
                }
                cat.Add(tag, m, Store(cache, tag, schema, data, sig));
            }
        }
        cat.Sort();
        return cat;
    }

    /// <summary>The catalog from the manifests verified earlier (each verified again now): for offline use.</summary>
    public static Catalog CachedCatalog(string releasesRoot, string? key = null)
    {
        var cat = new Catalog { Source = "cache" };
        var cache = CacheDir(releasesRoot);
        if (Directory.Exists(cache))
        {
            foreach (var d in Directory.GetDirectories(cache).OrderBy(Path.GetFileName, StringComparer.OrdinalIgnoreCase))
            {
                var name = Path.GetFileName(d);
                var schema = ReleaseConstants.Schemas.FirstOrDefault(s => File.Exists(Path.Combine(d, s.ManifestName)))
                             ?? ReleaseConstants.Current;
                try
                {
                    var m = SignedManifest.VerifyFiles(d, key, schema);
                    if (m.Release != name)
                        throw new ManifestException("release mismatch");
                    cat.Add(name, m, d);
                }
                catch (ManifestException e)
                {
                    cat.Rejected.Add((name, e.Message));
                }
            }
        }
        cat.Sort();
        return cat;
    }

    /// <summary>Downloaded release folders, newest version first.</summary>
    public static List<Package> ListPackages(string releasesRoot, string? key = null)
    {
        var out_ = new List<Package>();
        if (Directory.Exists(releasesRoot))
            foreach (var d in Directory.GetDirectories(releasesRoot))
                if (File.Exists(Path.Combine(d, "rfoundry_dm32.bin")))
                    out_.Add(new Package(d, key));
        // Python: key (1, version) or (0, ()) when the name is not a version; reverse=True, stable
        var order = Comparer<Package>.Create((a, b) =>
        {
            var va = ReleaseVersion.TryParse(a.Version);
            var vb = ReleaseVersion.TryParse(b.Version);
            if (va is null || vb is null)
                return (va is null ? 0 : 1).CompareTo(vb is null ? 0 : 1);
            return va.CompareTo(vb);
        });
        return [.. out_.OrderByDescending(p => p, order)];
    }

    public static string PackageFolder(string releasesRoot, string version) => Path.Combine(releasesRoot, version);

    /// <summary>
    /// The verified release folder for fv (downloaded when not there yet). Throws VerifyException / NetException;
    /// a failed download leaves nothing behind.
    /// </summary>
    public static async Task<string> DownloadFirmwareAsync(FirmwareVersion fv, string releasesRoot, IReleaseNet net,
        Action<long, long>? progress = null, string? key = null, CancellationToken ct = default)
    {
        if (fv.IsLegacy)
            throw new VerifyException(VerifyFailure.NotInstallable,
                "This release uses the old DDM25 names; it cannot be installed by this program");
        var folder = PackageFolder(releasesRoot, fv.Version);
        if (Directory.Exists(folder))
        {
            var p = new Package(folder, key);
            if (p.Installable && p.Sha256 == fv.ApplicationSha256)
                return folder;
            Directory.Delete(folder, recursive: true);
        }
        var tmp = Path.Combine(releasesRoot, fv.Version + ".partial");
        if (Directory.Exists(tmp))
            Directory.Delete(tmp, recursive: true);
        Directory.CreateDirectory(tmp);
        try
        {
            var entry = fv.Entry;
            var zpath = Path.Combine(tmp, entry.File);
            await net.DownloadAsync(entry.Url, zpath, entry.Size, progress, ct).ConfigureAwait(false);
            if (new FileInfo(zpath).Length != entry.Size || SignedManifest.Sha256File(zpath) != entry.Sha256)
                throw new VerifyException(VerifyFailure.HashMismatch,
                    "The downloaded file's SHA-256 does not match the signed list");
            using (var z = ZipFile.OpenRead(zpath))
            {
                var names = z.Entries.Select(e => e.FullName).Order(StringComparer.Ordinal).ToList();
                if (!names.SequenceEqual(ReleaseConstants.PackageFiles.Order(StringComparer.Ordinal)))
                    throw new VerifyException(VerifyFailure.UnexpectedContents,
                        "The release package does not hold the three expected files");
                foreach (var n in ReleaseConstants.PackageFiles)
                {
                    var ze = z.GetEntry(n)!;
                    if (ze.Length > ReleaseConstants.MaxPackageMember)
                        throw new VerifyException(VerifyFailure.MemberTooLarge, "A file in the release package is too large");
                    File.WriteAllBytes(Path.Combine(tmp, n), ReadBounded(ze, ReleaseConstants.MaxPackageMember));
                }
            }
            File.Delete(zpath);
            var app = File.ReadAllBytes(Path.Combine(tmp, "rfoundry_dm32.bin"));
            if (SignedManifest.Sha256Hex(app) != fv.ApplicationSha256 || app.Length != fv.ApplicationSize)
                throw new VerifyException(VerifyFailure.ApplicationMismatch,
                    "The application in the package does not match the signed list");
            var src = fv.ManifestDir ?? throw new VerifyException(VerifyFailure.PackageNotInstallable, "No manifest folder");
            foreach (var n in new[] { fv.Schema.ManifestName, fv.Schema.SigName })
                File.Copy(Path.Combine(src, n), Path.Combine(tmp, n));
            var pkg = new Package(tmp, key);
            if (!pkg.Installable)
                throw new VerifyException(VerifyFailure.PackageNotInstallable, string.Join(", ", pkg.Problems));
            Directory.Move(tmp, folder);
            return folder;
        }
        catch
        {
            try { Directory.Delete(tmp, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            throw;
        }
    }

    /// <summary>The program zip of a signed manifest's app entry, verified (size and SHA-256).</summary>
    public static async Task<string> DownloadAppAsync(AssetEntry entry, string updatesDir, IReleaseNet net,
        Action<long, long>? progress = null, CancellationToken ct = default)
    {
        Directory.CreateDirectory(updatesDir);
        var dest = Path.Combine(updatesDir, entry.File);
        if (File.Exists(dest))
            File.Delete(dest);
        await net.DownloadAsync(entry.Url, dest, entry.Size, progress, ct).ConfigureAwait(false);
        if (new FileInfo(dest).Length != entry.Size || SignedManifest.Sha256File(dest) != entry.Sha256)
        {
            File.Delete(dest);
            throw new VerifyException(VerifyFailure.HashMismatch,
                "The downloaded program's SHA-256 does not match the signed list");
        }
        return dest;
    }

    internal static byte[] ReadBounded(ZipArchiveEntry e, long limit)
    {
        using var s = e.Open();
        var buf = new MemoryStream();
        var block = new byte[1 << 16];
        int n;
        while ((n = s.Read(block, 0, block.Length)) > 0)
        {
            buf.Write(block, 0, n);
            if (buf.Length > limit)
                throw new VerifyException(VerifyFailure.MemberTooLarge, "A file in the release package is too large");
        }
        return buf.ToArray();
    }

    public static string NowUtc() => DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", System.Globalization.CultureInfo.InvariantCulture);

    // --- Python semantics of the API's JSON ----------------------------------------------------------------------
    private static Dictionary<string, JsonElement> Assets(JsonElement? assets)
    {
        var map = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        if (assets is null)
            return map;
        if (assets.Value.ValueKind != JsonValueKind.Array)
        {
            // Python iterates a string or an object without finding assets, and fails on null / numbers
            if (assets.Value.ValueKind is JsonValueKind.String or JsonValueKind.Object)
                return map;
            throw new NetException("GitHub answered with something that is not a release list");
        }
        foreach (var a in assets.Value.EnumerateArray())
            if (a.ValueKind == JsonValueKind.Object && SignedManifest.Get(a, "name") is { ValueKind: JsonValueKind.String } n)
                map[n.GetString()!] = a; // the last one wins, like a dict comprehension
        return map;
    }

    private static bool PyTruthy(JsonElement? e) => e?.ValueKind switch
    {
        null or JsonValueKind.Null or JsonValueKind.False or JsonValueKind.Undefined => false,
        JsonValueKind.True => true,
        JsonValueKind.String => e.Value.GetString()!.Length > 0,
        JsonValueKind.Array => e.Value.GetArrayLength() > 0,
        JsonValueKind.Object => e.Value.EnumerateObject().Any(),
        JsonValueKind.Number => e.Value.GetDouble() != 0,
        _ => false,
    };

    /// <summary>Python's str() of a JSON value, for the tag name.</summary>
    private static string? PyStr(JsonElement? e) => e?.ValueKind switch
    {
        null => null,
        JsonValueKind.String => e.Value.GetString(),
        JsonValueKind.Null => "None",
        JsonValueKind.True => "True",
        JsonValueKind.False => "False",
        _ => e.Value.GetRawText(),
    };
}
