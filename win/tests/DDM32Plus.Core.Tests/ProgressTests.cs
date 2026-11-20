// SPDX-License-Identifier: GPL-3.0-or-later
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>The install and restore tools' progress lines as the pages show them (the recorded runs' own lines).</summary>
public sealed class ProgressTests
{
    private static List<(string Key, int Percent)> Track(IEnumerable<string> lines)
    {
        var t = new ProgressTracker();
        return [.. lines.Select(t.Feed).OfType<(string, int)>()];
    }

    private static IEnumerable<string> Lines(string scenario) =>
        RadioFixtures.Scenario(scenario).GetProperty("lines").EnumerateArray().Select(l => l.GetString()!);

    [Fact]
    public void InstallPhasesInOrder()
    {
        var p = Track(Lines("install_ok"));
        Assert.Equal(["Prog_Compare", "Prog_Write", "Prog_Verify"], p.Select(x => x.Key).Distinct());
        var compare = p.Where(x => x.Key == "Prog_Compare").Select(x => x.Percent).ToList();
        Assert.Equal(compare.Order(), compare);                 // one bar from 0 to 100: program, then settings chip
        Assert.Equal(100, compare[^1]);
        Assert.Contains(50, compare);
        Assert.Equal(100, p.Where(x => x.Key == "Prog_Write").Max(x => x.Percent));
        Assert.Equal(100, p[^1].Percent);
    }

    [Fact]
    public void TheOwnersLines()
    {
        var t = new ProgressTracker();
        Assert.Equal(("Prog_Compare", 20), t.Feed("Read current firmware: 40%"));
        Assert.Equal(("Prog_Compare", 75), t.Feed("Check settings chip: 50%"));
        Assert.Equal(("Prog_Write", 25), t.Feed("Written and sector-readback checked: 25% (1/4)"));
        Assert.Equal(("Prog_Verify", 60), t.Feed("Recheck untouched settings chip: 20%"));
        Assert.Null(t.Feed("program flash JEDEC ID c84014"));
        Assert.Null(t.Feed("progmem pass 1: 25% (262144/1048576)"));     // the backup's own lines: not these phases
        Assert.Null(t.Feed("READY on COM14: turn the radio OFF, then ON normally, no buttons held. Waiting 600s."));
        var nb = new ProgressTracker();
        Assert.Equal(("Prog_Read", 50), nb.Feed("Read current firmware (once, kept in memory): 100%"));
        Assert.Equal(("Prog_Read", 100), nb.Feed("Check settings chip: 100%"));
    }

    [Fact]
    public void RestorePhases()
    {
        var p = Track(Lines("restore_stock_ok"));
        Assert.Equal(["Prog_Write", "Prog_Verify"], p.Select(x => x.Key).Distinct());
        p = Track(Lines("restore_ok"));
        Assert.Contains(p, x => x.Key == "Prog_Verify");
    }
}
