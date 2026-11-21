// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace DDM32Plus.Core.Tests;

/// <summary>The release configuration: one version everywhere, the build flags' defaults, what the setup removes.</summary>
public partial class ReleaseConfigTests
{
    private static string WinRoot()
    {
        var d = new DirectoryInfo(AppContext.BaseDirectory);
        while (d is not null && !File.Exists(Path.Combine(d.FullName, "DDM32Plus.sln")))
            d = d.Parent;
        return d?.FullName ?? throw new DirectoryNotFoundException("win/DDM32Plus.sln not found above the test folder");
    }

    [GeneratedRegex(@"#define AppVersion ""([^""]+)""")]
    private static partial Regex IssVersion();

    [Fact]
    public void TheVersionIsTheSameEverywhere()
    {
        var props = XDocument.Load(Path.Combine(WinRoot(), "Directory.Build.props"));
        var version = props.Descendants("Version").Single().Value;
        Assert.Equal(AppInfo.Version, version);
        var iss = File.ReadAllText(Path.Combine(WinRoot(), "installer", "DDM32Plus.iss"));
        Assert.Equal(AppInfo.Version, IssVersion().Match(iss).Groups[1].Value);
        Assert.Contains("OutputBaseFilename=DDM32Plus-Setup-{#AppVersion}", iss, StringComparison.Ordinal);
    }

    [Fact]
    public void TheReleaseBuildReachesTheRadioButHasNoDeveloperFeatures()
    {
        // owner, 2026-11-21: installs are done with this program, so the release defaults are radio access and
        // writes on; the developer features stay off unless -p:Developer=true
        var props = XDocument.Load(Path.Combine(WinRoot(), "Directory.Build.props"));
        string Default(string name) => props.Descendants(name).Single().Value;
        Assert.Equal("true", Default("RadioAccess"));
        Assert.Equal("true", Default("RadioWrite"));
        Assert.Equal("false", Default("Developer"));
        var pub = File.ReadAllText(Path.Combine(WinRoot(), "src", "DDM32Plus", "Properties", "PublishProfiles", "win-x64.pubxml"));
        Assert.DoesNotContain("Developer", pub, StringComparison.Ordinal);
        Assert.DoesNotContain("RadioAccess", pub, StringComparison.Ordinal);
    }

    [Fact]
    public void TheUninstallerKeepsTheBackups()
    {
        var iss = File.ReadAllText(Path.Combine(WinRoot(), "installer", "DDM32Plus.iss"));
        var delete = iss[iss.IndexOf("[UninstallDelete]", StringComparison.Ordinal)..iss.IndexOf("[Code]", StringComparison.Ordinal)];
        Assert.DoesNotContain(delete.Split('\n'), l => !l.TrimStart().StartsWith(';') && l.Contains("yedekler", StringComparison.Ordinal));
        Assert.Contains(@"{app}\paketler", delete, StringComparison.Ordinal);
        Assert.Contains(@"Name: ""{app}\yedekler""; Flags: uninsneveruninstall", iss, StringComparison.Ordinal);
        Assert.Contains(AppInfo.PacksFolder, AppInfo.ReservedNames);
        Assert.Contains(AppInfo.BackupsFolder, AppInfo.ReservedNames);
    }
}
