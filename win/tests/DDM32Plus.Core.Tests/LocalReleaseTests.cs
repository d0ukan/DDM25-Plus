// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;
using DDM32Plus.Core.Radio;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>A signed release in a local folder (developer builds): verified like a downloaded one, with the key in
/// force, and refused when anything in it was changed.</summary>
public sealed class LocalReleaseTests : IDisposable
{
    private readonly TempDir tmp = new();
    private readonly string folder;
    private readonly string root;
    private readonly byte[] app = TestData.ReleaseApp("1.0.0-beta.3");
    private readonly byte[] zip;

    public LocalReleaseTests()
    {
        folder = tmp.Sub("release");
        root = tmp.Sub("surumler");
        Directory.CreateDirectory(folder);
        Directory.CreateDirectory(root);
        zip = TestData.ReleaseZip(app);
        var entry = TestData.FirmwareEntry(app, "1.0.0-beta.3", "beta", zip: zip, release: "v1.0.0-beta.3");
        File.WriteAllBytes(Path.Combine(folder, entry["file"]!.GetValue<string>()), zip);
        TestData.WriteSigned(folder, TestData.Manifest([entry], "v1.0.0-beta.3"));
    }

    public void Dispose() => tmp.Dispose();

    [Fact]
    public async Task VerifiesWithTheKeyInForceAndDownloadsFromTheFolder()
    {
        var cat = await ReleaseClientLocal.LoadAsync(folder, root, TestData.TestPublicHex);
        var fv = Assert.Single(cat.Firmware);
        Assert.True(fv.IsLocal);
        Assert.Equal("1.0.0-beta.3", fv.Version);
        Assert.Empty(cat.Rejected);
        var package = await ReleaseClient.DownloadFirmwareAsync(fv, root, fv.Source!, key: TestData.TestPublicHex);
        Assert.Equal(Path.Combine(root, "1.0.0-beta.3"), package);
        Assert.True(new Package(package, TestData.TestPublicHex).Installable);
        // the same folder is not a release for the embedded release key
        var real = await ReleaseClientLocal.LoadAsync(folder, tmp.Sub("other"));
        Assert.Empty(real.Firmware);
        Assert.Contains("signature is not valid", Assert.Single(real.Rejected).Reason);
    }

    [Fact]
    public async Task ChangedFilesAreRefused()
    {
        // the zip
        var z = Directory.GetFiles(folder, "*.zip").Single();
        var bad = (byte[])zip.Clone();
        bad[^30] ^= 1;
        File.WriteAllBytes(z, bad);
        var fv = Assert.Single((await ReleaseClientLocal.LoadAsync(folder, root, TestData.TestPublicHex)).Firmware);
        var e = await Assert.ThrowsAsync<VerifyException>(() => ReleaseClient.DownloadFirmwareAsync(fv, root, fv.Source!, key: TestData.TestPublicHex));
        Assert.Equal(VerifyFailure.HashMismatch, e.Kind);
        Assert.False(Directory.Exists(Path.Combine(root, "1.0.0-beta.3")));
        // a larger zip than signed
        File.WriteAllBytes(z, [.. zip, 0]);
        await Assert.ThrowsAsync<NetException>(() => ReleaseClient.DownloadFirmwareAsync(fv, root, fv.Source!, key: TestData.TestPublicHex));
        // the manifest
        var m = Path.Combine(folder, "ddm32-manifest.json");
        File.WriteAllText(m, File.ReadAllText(m).Replace("\"beta\"", "\"stable\"", StringComparison.Ordinal));
        var cat = await ReleaseClientLocal.LoadAsync(folder, tmp.Sub("r2"), TestData.TestPublicHex);
        Assert.Empty(cat.Firmware);
        Assert.Contains("signature is not valid", Assert.Single(cat.Rejected).Reason);
        // no signature
        File.Delete(Path.Combine(folder, "ddm32-manifest.json.sig"));
        cat = await ReleaseClientLocal.LoadAsync(folder, tmp.Sub("r3"), TestData.TestPublicHex);
        Assert.Equal("no signed manifest", Assert.Single(cat.Rejected).Reason);
    }

    [Fact]
    public async Task OnlyFilesOfTheFolderAreServed()
    {
        var net = new LocalReleaseNet(folder);
        await Assert.ThrowsAsync<NetException>(() => net.FetchAsync(TestData.Url("v1.0.0-beta.3", "..\\x.json"), 100));
        await Assert.ThrowsAsync<NetException>(() => net.FetchAsync(TestData.Url("v9.9.9", "ddm32-manifest.json"), 1 << 20));
        await Assert.ThrowsAsync<NetException>(() => net.FetchAsync("https://example.com/ddm32-manifest.json", 1 << 20));
        await Assert.ThrowsAsync<NetException>(() => net.DownloadAsync(TestData.Url("v1", "a/b.zip"), tmp.Sub("x"), 10, null));
        Assert.Throws<NetException>(() => new LocalReleaseNet(tmp.Sub("empty-" + Guid.NewGuid().ToString("N")[..4])));
        Assert.Contains("v1.0.0-beta.3", Encoding.UTF8.GetString(await net.FetchAsync(ReleaseConstants.ApiUrl, 1 << 20)));
    }

    [Fact]
    public async Task LocalVersionsJoinTheList()
    {
        var github = new Catalog();
        var local = await ReleaseClientLocal.LoadAsync(folder, root, TestData.TestPublicHex);
        github.AddLocal(local);
        Assert.True(Assert.Single(github.Firmware).IsLocal);
        github.AddLocal(local);                     // once
        Assert.Single(github.Firmware);
    }

    [Fact]
    public void WritesAreOffInThisBuild()
    {
        Assert.False(RadioAccess.WritesEnabled);
        Assert.Throws<RadioAccess.DisabledException>(() => RadioAccess.WriteFactory("COM14"));
    }
}
