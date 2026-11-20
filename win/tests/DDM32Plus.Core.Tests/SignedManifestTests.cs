// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>The signed manifest checks give the same answer (and the same error text) as signed_manifest.py.</summary>
public class SignedManifestTests
{
    private static void CheckCase(JsonElement c)
    {
        var name = c.GetProperty("name").GetString();
        var data = Convert.FromBase64String(c.GetProperty("data").GetString()!);
        var sig = Convert.FromBase64String(c.GetProperty("sig").GetString()!);
        var key = c.GetProperty("key").ValueKind == JsonValueKind.Null ? null : c.GetProperty("key").GetString();
        var schema = c.GetProperty("schema").GetString() == "legacy" ? ReleaseConstants.Legacy : ReleaseConstants.Current;
        Manifest? m = null;
        string? error = null;
        try
        {
            m = SignedManifest.VerifyBytes(data, sig, key, schema);
        }
        catch (ManifestException e)
        {
            error = e.Message;
        }
        if (c.GetProperty("ok").GetBoolean())
        {
            Assert.True(m is not null, $"case '{name}': C# refused it: {error}");
            Assert.Equal(c.GetProperty("release").GetString(), m!.Release);
            Assert.Equal(c.GetProperty("versions").EnumerateArray().Select(v => v.GetString()), m.Firmware.Select(f => f.Version));
            var app = c.GetProperty("app");
            Assert.Equal(app.ValueKind == JsonValueKind.Null ? null : app.GetString(), m.App?.Version);
        }
        else
        {
            Assert.True(error is not null, $"case '{name}': C# accepted it");
            Assert.Equal(c.GetProperty("error").GetString(), error);
        }
    }

    [Fact]
    public void EveryManifestCaseMatchesPython()
    {
        var cases = TestData.Python.GetProperty("manifests").EnumerateArray().ToList();
        Assert.True(cases.Count > 70);
        foreach (var c in cases)
            CheckCase(c);
    }

    [Fact]
    public void LegacyManifestMatchesPythonWithOldNames()
    {
        foreach (var c in TestData.Python.GetProperty("legacy").EnumerateArray())
            CheckCase(c);
    }

    [Fact]
    public void ConstantsEqualPython()
    {
        var k = TestData.Python.GetProperty("constants");
        var cur = ReleaseConstants.Current;
        Assert.Equal(k.GetProperty("format").GetString(), cur.Format);
        Assert.Equal(k.GetProperty("repo").GetString(), cur.Repo);
        Assert.Equal(k.GetProperty("manifest_name").GetString(), cur.ManifestName);
        Assert.Equal(k.GetProperty("sig_name").GetString(), cur.SigName);
        Assert.Equal(k.GetProperty("url_prefix").GetString(), cur.UrlPrefix);
        Assert.Equal(ReleaseConstants.ApiUrl, k.GetProperty("api_url").GetString());
        Assert.Equal(k.GetProperty("allowed_hosts").EnumerateArray().Select(e => e.GetString()), ReleaseConstants.AllowedHosts);
        Assert.Equal(k.GetProperty("package_files").EnumerateArray().Select(e => e.GetString()), ReleaseConstants.PackageFiles);
        Assert.Equal(ReleaseConstants.PublicKeyHex, TestData.Python.GetProperty("release_public_key").GetString());
        Assert.Equal(ReleaseConstants.PublicKeyId, TestData.Python.GetProperty("release_key_id").GetString());
        Assert.Equal(ReleaseConstants.PublicKeyId, SignedManifest.KeyId(SignedManifest.PublicKey()));
        Assert.Equal(TestData.Python.GetProperty("test_public_key").GetString(), TestData.TestPublicHex);
    }

    [Fact]
    public void PublishedLegacyReleaseVerifiesWithTheEmbeddedKey()
    {
        var data = File.ReadAllBytes(TestData.FixturePath("legacy", "ddm25-manifest.json"));
        var sig = File.ReadAllBytes(TestData.FixturePath("legacy", "ddm25-manifest.json.sig"));
        var m = SignedManifest.VerifyBytes(data, sig, schema: ReleaseConstants.Legacy);
        Assert.True(m.Schema.IsLegacy);
        Assert.Equal("v1.0.0-beta.1", m.Release);
        Assert.Equal("1.0.0-beta.1", Assert.Single(m.Firmware).Version);
        Assert.Equal("beta", m.Firmware[0].Channel);
        Assert.Null(m.AppNet); // the Python program's zip is never this program's update
        var e = Assert.Throws<ManifestException>(() => SignedManifest.VerifyBytes(data, sig));
        Assert.Equal("Manifest field invalid: format", e.Message);
    }

    [Fact]
    public void AppNetIsReadAndChecked()
    {
        var m = TestData.Manifest([TestData.FirmwareEntry(TestData.Pattern(0x5000, 5))],
            appNet: TestData.AppEntry("2.0.0-alpha.2", [1, 2, 3]));
        var (data, sig) = TestData.Signed(m);
        var v = SignedManifest.VerifyBytes(data, sig, TestData.TestPublicHex);
        Assert.Equal("2.0.0-alpha.2", v.AppNet!.Version);
        m["app_net"]!["url"] = "https://example.com/DDM32Plus-2.0.0-alpha.2.zip";
        (data, sig) = TestData.Signed(m);
        Assert.Equal("Manifest field invalid: app_net.url",
            Assert.Throws<ManifestException>(() => SignedManifest.VerifyBytes(data, sig, TestData.TestPublicHex)).Message);
    }

    [Fact]
    public void ReleaseGateNeedsTheListedApplication()
    {
        using var tmp = new TempDir();
        var app = TestData.Pattern(0x5000, 5);
        TestData.WriteSigned(tmp.Path, TestData.Manifest([TestData.FirmwareEntry(app)]));
        Assert.Equal("1.0.0", SignedManifest.ReleaseGate(app, tmp.Path, TestData.TestPublicHex).Version);
        Assert.Throws<ManifestException>(() => SignedManifest.ReleaseGate([.. app, 0], tmp.Path, TestData.TestPublicHex));
        Assert.Throws<ManifestException>(() => SignedManifest.ReleaseGate(app, tmp.Path)); // the release key
        Assert.Throws<ManifestException>(() => SignedManifest.ReleaseGate(app, tmp.Sub("none"), TestData.TestPublicHex));
    }

    [Fact]
    public void TooLargeManifestIsRefusedBeforeTheSignature()
    {
        var e = Assert.Throws<ManifestException>(() =>
            SignedManifest.VerifyBytes(new byte[(1 << 20) + 1], "00"u8.ToArray(), TestData.TestPublicHex));
        Assert.Equal("Manifest too large or not bytes", e.Message);
    }
}
