// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>The Python results (Fixtures/python-results.json, from make_fixtures.py) and C# test data builders.</summary>
internal static class TestData
{
    private static readonly Lazy<JsonDocument> Results = new(() =>
        JsonDocument.Parse(File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "Fixtures", "python-results.json"))));

    public static JsonElement Python => Results.Value.RootElement;

    public static string FixturePath(params string[] parts) =>
        Path.Combine([AppContext.BaseDirectory, "Fixtures", .. parts]);

    // A TEST key: its secret is public (pc/tests/rtfixtures.py), so nothing it signs can be a release
    public static readonly byte[] TestSecret = [.. Enumerable.Range(0, 32).Select(i => (byte)i)];
    public static readonly byte[] OtherSecret = [.. Enumerable.Range(1, 32).Select(i => (byte)i)];
    public static readonly string TestPublicHex = Convert.ToHexStringLower(Crypto.Ed25519.SecretToPublic(TestSecret));

    public static byte[] Pattern(int n, int k)
    {
        var b = new byte[n];
        for (var i = 0; i < n; i++)
            b[i] = (byte)((i * k + (i >> 8) + 1) & 255);
        return b;
    }

    public static byte[] ReleaseApp(string version) =>
        [.. Enumerable.Repeat(Encoding.ASCII.GetBytes("DDM32 Plus test " + version), 64).SelectMany(x => x)];

    public static string Url(string release, string name, ManifestSchema? schema = null) =>
        (schema ?? ReleaseConstants.Current).UrlPrefix + release + "/" + name;

    public static JsonObject FirmwareEntry(byte[] app, string version = "1.0.0", string channel = "stable",
        int settingsFormat = 1, int codeplugFormat = 1, string? minCompatible = null, string? release = null,
        byte[]? zip = null)
    {
        release ??= "v" + version;
        var name = $"ddm32-plus-{version}.zip";
        zip ??= [.. Encoding.ASCII.GetBytes("zip:"), .. app.Take(16)];
        return new JsonObject
        {
            ["version"] = version, ["channel"] = channel, ["file"] = name, ["url"] = Url(release, name),
            ["sha256"] = SignedManifest.Sha256Hex(zip), ["size"] = zip.Length,
            ["application_sha256"] = SignedManifest.Sha256Hex(app), ["application_size"] = app.Length,
            ["settings_format"] = settingsFormat, ["codeplug_format"] = codeplugFormat,
            ["min_compatible_version"] = minCompatible ?? version,
            ["notes"] = new JsonObject { ["tr"] = $"Sürüm {version}", ["en"] = $"Version {version}" },
        };
    }

    public static JsonObject AppEntry(string version, byte[] zip, string? release = null)
    {
        release ??= "v" + version;
        var name = $"DDM32Plus-{version}.zip";
        return new JsonObject
        {
            ["version"] = version, ["file"] = name, ["url"] = Url(release, name),
            ["sha256"] = SignedManifest.Sha256Hex(zip), ["size"] = zip.Length,
            ["notes"] = new JsonObject { ["tr"] = "Program", ["en"] = "Program" },
        };
    }

    public static JsonObject Manifest(IEnumerable<JsonObject> firmware, string release = "v1.0.0",
        JsonObject? app = null, JsonObject? appNet = null)
    {
        var fw = new JsonArray();
        foreach (var e in firmware)
            fw.Add(e.DeepClone());
        var m = new JsonObject
        {
            ["format"] = ReleaseConstants.Current.Format, ["repo"] = ReleaseConstants.Current.Repo,
            ["release"] = release, ["created_utc"] = "2026-11-20T00:00:00Z", ["firmware"] = fw,
        };
        if (app is not null)
            m["app"] = app.DeepClone();
        if (appNet is not null)
            m["app_net"] = appNet.DeepClone();
        return m;
    }

    /// <summary>(manifest bytes, signature text bytes)</summary>
    public static (byte[] Data, byte[] Sig) Signed(JsonObject m, byte[]? secret = null)
    {
        var data = Encoding.UTF8.GetBytes(m.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        return (data, Encoding.ASCII.GetBytes(SignedManifest.SignBytes(data, secret ?? TestSecret)));
    }

    public static void WriteSigned(string folder, JsonObject m, byte[]? secret = null)
    {
        Directory.CreateDirectory(folder);
        var (data, sig) = Signed(m, secret);
        File.WriteAllBytes(Path.Combine(folder, ReleaseConstants.Current.ManifestName), data);
        File.WriteAllBytes(Path.Combine(folder, ReleaseConstants.Current.SigName), sig);
    }

    public static byte[] Zip(params (string Name, byte[] Data)[] members)
    {
        var buf = new MemoryStream();
        using (var z = new ZipArchive(buf, ZipArchiveMode.Create, leaveOpen: true))
        {
            foreach (var (name, data) in members)
            {
                var e = z.CreateEntry(name);
                using var s = e.Open();
                s.Write(data);
            }
        }
        return buf.ToArray();
    }

    /// <summary>A release zip with the three package files for app.</summary>
    public static byte[] ReleaseZip(byte[] app) => Zip(("rfoundry_dm32.bin", app),
        ("rfoundry_dm32.json", Encoding.ASCII.GetBytes("{\"plus_enabled\": true}")),
        ("rfoundry_dm32.map", Encoding.ASCII.GetBytes("map")));
}

/// <summary>A temporary folder deleted after the test.</summary>
internal sealed class TempDir : IDisposable
{
    public string Path { get; } = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "ddm32-test-" + Guid.NewGuid().ToString("N")[..12]);

    public TempDir() => Directory.CreateDirectory(Path);

    public string Sub(params string[] parts) => System.IO.Path.Combine([Path, .. parts]);

    public void Dispose()
    {
        try
        {
            Directory.Delete(Path, recursive: true);
        }
        catch (IOException)
        {
        }
    }
}

/// <summary>A fake network: url -> bytes; records downloads; nothing leaves the process.</summary>
internal sealed class FakeNet : IReleaseNet
{
    public Dictionary<string, byte[]> Files { get; } = new(StringComparer.Ordinal);
    public List<string> Downloads { get; } = [];
    public Func<string, byte[]>? DownloadPayload { get; set; }

    public Task<byte[]> FetchAsync(string url, int limit, CancellationToken ct = default)
    {
        if (!Files.TryGetValue(url, out var data))
            throw new NetException($"HTTP Error 404: {url}");
        if (data.Length > limit)
            throw new NetException($"Answer larger than {limit} bytes: {url}");
        return Task.FromResult(data);
    }

    public Task DownloadAsync(string url, string dest, long size, Action<long, long>? progress, CancellationToken ct = default)
    {
        Downloads.Add(url);
        var data = DownloadPayload?.Invoke(url) ?? Files[url];
        File.WriteAllBytes(dest, data);
        progress?.Invoke(data.Length, size);
        return Task.CompletedTask;
    }
}
