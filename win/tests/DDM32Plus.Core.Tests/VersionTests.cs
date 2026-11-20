// SPDX-License-Identifier: GPL-3.0-or-later
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>Version order equal to Python's parse_version keys (signed_manifest.py).</summary>
public class VersionTests
{
    [Fact]
    public void PythonTestOrder()
    {
        string[] order = ["0.9.0", "1.0.0-beta.1", "1.0.0-beta.2", "1.0.0-beta.10", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.10.0"];
        var sorted = order.Reverse().OrderBy(ReleaseVersion.Parse).ToArray();
        Assert.Equal(order, sorted);
        Assert.Throws<ManifestException>(() => ReleaseVersion.Parse("v1.0.0"));
    }

    [Fact]
    public void SameAcceptanceAsPython()
    {
        var v = TestData.Python.GetProperty("versions");
        foreach (var s in v.GetProperty("valid").EnumerateArray())
            Assert.NotNull(ReleaseVersion.TryParse(s.GetString()));
        foreach (var s in v.GetProperty("invalid").EnumerateArray())
            Assert.True(ReleaseVersion.TryParse(s.GetString()) is null, $"'{s.GetString()}' should be refused");
    }

    [Fact]
    public void EveryPairComparesLikePython()
    {
        var v = TestData.Python.GetProperty("versions");
        var valid = v.GetProperty("valid").EnumerateArray().Select(e => ReleaseVersion.Parse(e.GetString())).ToArray();
        var cmp = v.GetProperty("compare").EnumerateArray().Select(r => r.EnumerateArray().Select(x => x.GetInt32()).ToArray()).ToArray();
        Assert.True(valid.Length > 25);
        for (var i = 0; i < valid.Length; i++)
            for (var j = 0; j < valid.Length; j++)
                Assert.True(cmp[i][j] == Math.Sign(valid[i].CompareTo(valid[j])),
                    $"{valid[i].Text.Trim()} vs {valid[j].Text.Trim()}: Python {cmp[i][j]}");
    }

    [Fact]
    public void Operators()
    {
        var a = ReleaseVersion.Parse("1.0.0-beta.1");
        var b = ReleaseVersion.Parse("1.0.0");
        Assert.True(a < b && b > a && a <= ReleaseVersion.Parse("1.0.0-beta.1") && b >= a && a != b);
        Assert.True(ReleaseVersion.Parse("1.0.0-007") == ReleaseVersion.Parse("1.0.0-7"));
        Assert.True(a.IsPrerelease);
        Assert.Equal("beta.1", a.Prerelease);
    }
}
