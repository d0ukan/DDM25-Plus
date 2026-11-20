// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;
using System.Text.Json.Nodes;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>The release list (releases.load_catalog / cached_catalog) and the channel labels. Fake network only.</summary>
public class CatalogTests
{
    private static FakeNet PythonNet()
    {
        var net = new FakeNet();
        foreach (var p in TestData.Python.GetProperty("catalog").GetProperty("files").EnumerateObject())
            net.Files[p.Name] = Convert.FromBase64String(p.Value.GetString()!);
        return net;
    }

    [Fact]
    public async Task SameListAndRefusalsAsPython()
    {
        using var tmp = new TempDir();
        var py = TestData.Python.GetProperty("catalog");
        var cat = await ReleaseClient.LoadCatalogAsync(tmp.Path, PythonNet(), py.GetProperty("key").GetString(), () => "now");
        Assert.Equal(py.GetProperty("firmware").EnumerateArray().Select(r => string.Join('|', r.EnumerateArray().Select(x => x.GetString()))),
            cat.Firmware.Select(f => $"{f.Version}|{f.Release}|{f.Channel}"));
        Assert.Equal(py.GetProperty("rejected").EnumerateArray().Select(r => string.Join('|', r.EnumerateArray().Select(x => x.GetString()))),
            cat.Rejected.Select(r => $"{r.Tag}|{r.Reason}"));
        Assert.Equal("github", cat.Source);
        Assert.Equal("now", cat.CheckedUtc);

        // verified again from disk, same list
        var cached = ReleaseClient.CachedCatalog(tmp.Path, py.GetProperty("key").GetString());
        Assert.Equal(cat.Firmware.Select(f => f.Version), cached.Firmware.Select(f => f.Version));
        Assert.Equal("cache", cached.Source);
        Assert.Empty(cached.Rejected);
    }

    [Fact]
    public async Task NoKeyListsNothing()
    {
        using var tmp = new TempDir();
        var cat = await ReleaseClient.LoadCatalogAsync(tmp.Path, PythonNet(), "");
        Assert.Empty(cat.Firmware);
        Assert.Contains(cat.Rejected, r => r.Reason.Contains("No release signing key", StringComparison.Ordinal));
    }

    [Fact]
    public async Task CacheTamperingIsCaught()
    {
        using var tmp = new TempDir();
        var key = TestData.Python.GetProperty("catalog").GetProperty("key").GetString();
        await ReleaseClient.LoadCatalogAsync(tmp.Path, PythonNet(), key);
        var p = Path.Combine(tmp.Path, "manifests", "v1.2.0", "ddm32-manifest.json");
        File.WriteAllBytes(p, Encoding.UTF8.GetBytes(File.ReadAllText(p).Replace("\"stable\"", "\"beta\"  ")));
        var cat = ReleaseClient.CachedCatalog(tmp.Path, key);
        Assert.DoesNotContain(cat.Firmware, f => f.Release == "v1.2.0");
        Assert.Contains("signature", cat.Rejected.Single(r => r.Tag == "v1.2.0").Reason);
    }

    [Fact]
    public async Task NotAReleaseListIsANetError()
    {
        using var tmp = new TempDir();
        var net = new FakeNet();
        net.Files[ReleaseConstants.ApiUrl] = "{\"message\": \"rate limited\"}"u8.ToArray();
        await Assert.ThrowsAsync<NetException>(() => ReleaseClient.LoadCatalogAsync(tmp.Path, net));
        net.Files[ReleaseConstants.ApiUrl] = "<html>"u8.ToArray();
        await Assert.ThrowsAsync<NetException>(() => ReleaseClient.LoadCatalogAsync(tmp.Path, net));
    }

    /// <summary>The published v1.0.0-beta.1 (DDM25 names, real release key): listed as legacy, not offered.</summary>
    [Fact]
    public async Task PublishedLegacyReleaseIsListedAsLegacy()
    {
        using var tmp = new TempDir();
        var net = new FakeNet();
        const string tag = "v1.0.0-beta.1";
        var assets = new JsonArray();
        foreach (var n in new[] { "ddm25-manifest.json", "ddm25-manifest.json.sig" })
        {
            var url = TestData.Url(tag, n);
            net.Files[url] = File.ReadAllBytes(TestData.FixturePath("legacy", n));
            assets.Add(new JsonObject { ["name"] = n, ["browser_download_url"] = url });
        }
        var listing = new JsonArray { new JsonObject { ["tag_name"] = tag, ["prerelease"] = true, ["assets"] = assets } };
        net.Files[ReleaseConstants.ApiUrl] = Encoding.UTF8.GetBytes(listing.ToJsonString());

        var cat = await ReleaseClient.LoadCatalogAsync(tmp.Path, net); // the embedded release key
        var fv = Assert.Single(cat.Firmware);
        Assert.Empty(cat.Rejected);
        Assert.True(fv.IsLegacy);
        Assert.Equal("1.0.0-beta.1", fv.Version);
        Assert.Equal(ReleaseLabel.Legacy, ReleaseLabels.For(fv, cat.Firmware));
        Assert.False(ReleaseLabels.Installable(ReleaseLabel.Legacy));
        Assert.Null(cat.Newest());                        // never suggested as an update
        Assert.Null(cat.NewerApp("0.0.1"));               // its Python program zip is not this program's update
        await Assert.ThrowsAsync<VerifyException>(() => ReleaseClient.DownloadFirmwareAsync(fv, tmp.Path, net));
        Assert.Empty(net.Downloads);

        var cached = ReleaseClient.CachedCatalog(tmp.Path);
        Assert.True(Assert.Single(cached.Firmware).IsLegacy);
        Assert.True(File.Exists(Path.Combine(tmp.Path, "manifests", tag, "ddm25-manifest.json")));
    }

    private static Catalog CatalogOf(params (string Version, string Channel)[] versions)
    {
        var cat = new Catalog();
        foreach (var (v, ch) in versions)
        {
            var m = TestData.Manifest([TestData.FirmwareEntry(TestData.ReleaseApp(v), v, ch)], "v" + v);
            var (data, sig) = TestData.Signed(m);
            cat.Add("v" + v, SignedManifest.VerifyBytes(data, sig, TestData.TestPublicHex), null);
        }
        cat.Sort();
        return cat;
    }

    [Fact]
    public void ChannelLabels()
    {
        var cat = CatalogOf(("1.0.0", "stable"), ("1.1.0", "stable"), ("1.2.0-beta.1", "beta"),
            ("1.3.0-alpha.1", "beta"), ("1.3.0-test.2", "beta"), ("0.9.0", "stable"));
        string Label(string v) => ReleaseLabels.For(cat.Find(v)!, cat.Firmware).ToString();
        Assert.Equal("Latest", Label("1.1.0"));
        Assert.Equal("Stable", Label("1.0.0"));
        Assert.Equal("Stable", Label("0.9.0"));
        Assert.Equal("Beta", Label("1.2.0-beta.1"));
        Assert.Equal("TestAlpha", Label("1.3.0-alpha.1"));
        Assert.Equal("TestAlpha", Label("1.3.0-test.2"));
        Assert.True(ReleaseLabels.NeedsConfirmation(ReleaseLabel.TestAlpha));
        Assert.False(ReleaseLabels.NeedsConfirmation(ReleaseLabel.Beta));
        Assert.Equal(["1.3.0-test.2", "1.3.0-alpha.1", "1.2.0-beta.1", "1.1.0", "1.0.0", "0.9.0"], cat.Firmware.Select(f => f.Version));
        Assert.Equal("1.3.0-test.2", cat.Newest()!.Version);
        Assert.Equal("1.1.0", cat.Newest(includeBeta: false)!.Version);
    }

    [Fact]
    public void ProgramUpdatesComeOnlyFromAppNet()
    {
        var cat = new Catalog();
        var m = TestData.Manifest([], "v3.0.0", app: TestData.AppEntry("9.0.0", [1]), appNet: TestData.AppEntry("2.1.0", [2]));
        var (data, sig) = TestData.Signed(m);
        cat.Add("v3.0.0", SignedManifest.VerifyBytes(data, sig, TestData.TestPublicHex), null);
        var m2 = TestData.Manifest([], "v3.1.0", appNet: TestData.AppEntry("2.2.0-beta.1", [3], "v3.1.0"));
        (data, sig) = TestData.Signed(m2);
        cat.Add("v3.1.0", SignedManifest.VerifyBytes(data, sig, TestData.TestPublicHex), null);
        cat.Sort();
        Assert.Equal("2.2.0-beta.1", cat.NewerApp("2.0.0-alpha.1")!.Entry.Version);
        Assert.Equal("2.1.0", cat.NewerApp("2.0.0-alpha.1", includeBeta: false)!.Entry.Version);
        Assert.Null(cat.NewerApp("2.2.0"));
    }
}
