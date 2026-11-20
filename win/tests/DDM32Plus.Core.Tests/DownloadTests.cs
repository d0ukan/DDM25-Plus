// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net;
using System.Text;
using System.Text.Json.Nodes;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>Firmware and program downloads with hash checks (pc/tests/test_releases.py). Fake network only.</summary>
public sealed class DownloadTests : IDisposable
{
    private readonly TempDir tmp = new();
    private readonly string root;
    private readonly byte[] app = TestData.ReleaseApp("1.2.0");
    private readonly byte[] zip;
    private readonly JsonObject entry;
    private readonly FakeNet net = new();

    public DownloadTests()
    {
        root = tmp.Sub("surumler");
        Directory.CreateDirectory(root);
        zip = TestData.ReleaseZip(app);
        entry = TestData.FirmwareEntry(app, "1.2.0", zip: zip, release: "v1.2.0");
        AddRelease("v1.2.0", TestData.Manifest([entry], "v1.2.0"));
    }

    public void Dispose() => tmp.Dispose();

    private readonly JsonArray listing = [];

    private void AddRelease(string tag, JsonObject m)
    {
        var (data, sig) = TestData.Signed(m);
        net.Files[TestData.Url(tag, "ddm32-manifest.json")] = data;
        net.Files[TestData.Url(tag, "ddm32-manifest.json.sig")] = sig;
        listing.Add(new JsonObject
        {
            ["tag_name"] = tag,
            ["assets"] = new JsonArray
            {
                new JsonObject { ["name"] = "ddm32-manifest.json", ["browser_download_url"] = TestData.Url(tag, "ddm32-manifest.json") },
                new JsonObject { ["name"] = "ddm32-manifest.json.sig", ["browser_download_url"] = TestData.Url(tag, "ddm32-manifest.json.sig") },
            },
        });
    }

    private async Task<FirmwareVersion> Version()
    {
        net.Files[ReleaseConstants.ApiUrl] = Encoding.UTF8.GetBytes(listing.ToJsonString());
        var cat = await ReleaseClient.LoadCatalogAsync(root, net, TestData.TestPublicHex);
        return cat.Find("1.2.0")!;
    }

    [Fact]
    public async Task DownloadVerifiesHashAndContents()
    {
        var fv = await Version();
        net.DownloadPayload = _ => zip;
        var folder = await ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex);
        var p = new Package(folder, TestData.TestPublicHex);
        Assert.True(p.Installable, string.Join(", ", p.Problems));
        Assert.Equal(SignedManifest.Sha256Hex(app), p.Sha256);
        Assert.Equal(ReleaseConstants.PackageFiles.Concat(["ddm32-manifest.json", "ddm32-manifest.json.sig"]).Order(StringComparer.Ordinal),
            Directory.GetFiles(folder).Select(Path.GetFileName).Order(StringComparer.Ordinal));
        Assert.Equal(entry["url"]!.GetValue<string>(), Assert.Single(net.Downloads));
        // already there: no download
        Assert.Equal(folder, await ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex));
        Assert.Single(net.Downloads);
        Assert.Equal("1.2.0", Assert.Single(ReleaseClient.ListPackages(root, TestData.TestPublicHex)).Version);
    }

    [Fact]
    public async Task HashMismatchLeavesNothing()
    {
        var fv = await Version();
        var bad = (byte[])zip.Clone();
        bad[^30] ^= 1;
        foreach (var payload in new[] { bad, [.. zip, (byte)'x'], zip[..^1] })
        {
            net.DownloadPayload = _ => payload;
            var e = await Assert.ThrowsAsync<VerifyException>(() =>
                ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex));
            Assert.Equal(VerifyFailure.HashMismatch, e.Kind);
            Assert.Equal(["manifests"], Directory.GetFileSystemEntries(root).Select(Path.GetFileName));
        }
    }

    [Fact]
    public async Task UnexpectedZipContents()
    {
        var evil = TestData.Zip(("rfoundry_dm32.bin", app), ("../evil.py", "x"u8.ToArray()));
        listing.Clear();
        var e2 = TestData.FirmwareEntry(app, "1.2.0", zip: evil, release: "v1.2.0");
        AddRelease("v1.2.0", TestData.Manifest([e2], "v1.2.0"));
        var fv = await Version();
        net.DownloadPayload = _ => evil;
        var e = await Assert.ThrowsAsync<VerifyException>(() =>
            ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex));
        Assert.Equal(VerifyFailure.UnexpectedContents, e.Kind);
        Assert.False(File.Exists(tmp.Sub("evil.py")));
        Assert.False(File.Exists(Path.Combine(root, "evil.py")));
    }

    [Fact]
    public async Task ApplicationNotTheSignedOne()
    {
        var other = TestData.ReleaseZip(TestData.ReleaseApp("6.6.6"));
        listing.Clear();
        var e2 = TestData.FirmwareEntry(app, "1.2.0", zip: other, release: "v1.2.0");
        AddRelease("v1.2.0", TestData.Manifest([e2], "v1.2.0"));
        var fv = await Version();
        net.DownloadPayload = _ => other;
        var e = await Assert.ThrowsAsync<VerifyException>(() =>
            ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex));
        Assert.Equal(VerifyFailure.ApplicationMismatch, e.Kind);
        Assert.Equal(["manifests"], Directory.GetFileSystemEntries(root).Select(Path.GetFileName));
    }

    [Fact]
    public async Task PackageWithTamperedSignatureIsNotInstallable()
    {
        var fv = await Version();
        net.DownloadPayload = _ => zip;
        var folder = await ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex);
        var (_, otherSig) = TestData.Signed(TestData.Manifest([entry], "v1.2.0"), TestData.OtherSecret);
        File.WriteAllBytes(Path.Combine(folder, "ddm32-manifest.json.sig"), otherSig);
        var p = new Package(folder, TestData.TestPublicHex);
        Assert.False(p.Installable);
        Assert.Contains("signature", p.Problems.Single());
        // a new download replaces the damaged folder
        var again = await ReleaseClient.DownloadFirmwareAsync(fv, root, net, key: TestData.TestPublicHex);
        Assert.True(new Package(again, TestData.TestPublicHex).Installable);
        Assert.Equal(2, net.Downloads.Count);
    }

    [Fact]
    public async Task ProgramDownloadChecksTheHash()
    {
        var updates = tmp.Sub("guncelleme");
        var programZip = TestData.Zip(("DDM32Plus/DDM32Plus.exe", "exe"u8.ToArray()));
        var a = new AssetEntry("2.1.0", "DDM32Plus-2.1.0.zip", TestData.Url("v2.1.0", "DDM32Plus-2.1.0.zip"),
            SignedManifest.Sha256Hex(programZip), programZip.Length, null);
        net.DownloadPayload = _ => programZip;
        var dest = await ReleaseClient.DownloadAppAsync(a, updates, net);
        Assert.Equal(programZip, File.ReadAllBytes(dest));
        net.DownloadPayload = _ => [.. programZip[..^1], (byte)(programZip[^1] ^ 1)];
        var e = await Assert.ThrowsAsync<VerifyException>(() => ReleaseClient.DownloadAppAsync(a, updates, net));
        Assert.Equal(VerifyFailure.HashMismatch, e.Kind);
        Assert.False(File.Exists(dest));
    }

    [Theory]
    [InlineData("http://github.com/x")]
    [InlineData("https://example.com/x")]
    [InlineData("ftp://github.com/x")]
    [InlineData("https://github.com.evil.example/x")]
    [InlineData("not a url")]
    public void HttpsGithubOnly(string url) => Assert.Throws<NetException>(() => HttpReleaseNet.CheckUrl(url));

    [Fact]
    public void GithubHostsAreAllowed()
    {
        HttpReleaseNet.CheckUrl("https://objects.githubusercontent.com/github-production-release-asset/x");
        HttpReleaseNet.CheckUrl("https://GitHub.com/d0ukan/DDM32-Plus/releases/download/v1/x.zip");
        HttpReleaseNet.CheckUrl(ReleaseConstants.ApiUrl);
    }

    /// <summary>A fake HTTP handler: no socket is ever opened.</summary>
    private sealed class Handler(Func<HttpRequestMessage, HttpResponseMessage> answer) : HttpMessageHandler
    {
        public List<string> Seen { get; } = [];

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
        {
            Seen.Add(request.RequestUri!.AbsoluteUri);
            return Task.FromResult(answer(request));
        }
    }

    private static HttpResponseMessage Redirect(string to) =>
        new(HttpStatusCode.Found) { Headers = { Location = new Uri(to) } };

    [Fact]
    public async Task RedirectsAreCheckedAgain()
    {
        var h = new Handler(r => r.RequestUri!.Host == "github.com"
            ? Redirect("https://release-assets.githubusercontent.com/asset?sig=1")
            : new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent([1, 2, 3]) });
        using var http = new HttpReleaseNet(h);
        Assert.Equal([1, 2, 3], await http.FetchAsync(TestData.Url("v1", "a.json"), 100));
        Assert.Equal(2, h.Seen.Count);

        var evil = new Handler(_ => Redirect("https://evil.example/x"));
        using var http2 = new HttpReleaseNet(evil);
        var e = await Assert.ThrowsAsync<NetException>(() => http2.FetchAsync(TestData.Url("v1", "a.json"), 100));
        Assert.Contains("Refusing", e.Message);
        Assert.Single(evil.Seen);
    }

    [Fact]
    public async Task SizeLimitsHold()
    {
        var h = new Handler(_ => new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(new byte[200]) });
        using var http = new HttpReleaseNet(h);
        await Assert.ThrowsAsync<NetException>(() => http.FetchAsync(TestData.Url("v1", "a.json"), 100));
        var dest = tmp.Sub("big.zip");
        await Assert.ThrowsAsync<NetException>(() => http.DownloadAsync(TestData.Url("v1", "big.zip"), dest, 100, null));
        Assert.False(File.Exists(dest));
        Assert.False(File.Exists(dest + ".partial"));
        long seen = 0;
        await http.DownloadAsync(TestData.Url("v1", "big.zip"), dest, 200, (d, _) => seen = d);
        Assert.Equal(200, new FileInfo(dest).Length);
        Assert.Equal(200, seen);
        var notFound = new Handler(_ => new HttpResponseMessage(HttpStatusCode.NotFound));
        using var http3 = new HttpReleaseNet(notFound);
        Assert.Contains("404", (await Assert.ThrowsAsync<NetException>(() => http3.FetchAsync(ReleaseConstants.ApiUrl, 10))).Message);
    }
}
