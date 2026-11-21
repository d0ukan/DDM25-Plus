// SPDX-License-Identifier: GPL-3.0-or-later
using DDM32Plus.Core.Radio;
using Xunit;

namespace DDM32Plus.Core.Tests;

/// <summary>The search log (SearchLog.cs): the firmware's p25s / dmrs sections (searchlog_view.c) become call rows.</summary>
public sealed class SearchLogTests : IDisposable
{
    private readonly string dir = Path.Combine(Path.GetTempPath(), "ddm32-slog-" + Guid.NewGuid().ToString("N"));
    private static readonly DateTime T0 = new(2026, 11, 21, 22, 0, 0);

    public void Dispose()
    {
        if (Directory.Exists(dir))
            Directory.Delete(dir, true);
    }

    private static string[] Poll(string p25 = "", string dmr = "") =>
        ["section=p25s", "p25s.on=1", "p25s.phase=1", "p25s.at=433512500", "p25s.n=1", p25,
         "section=dmrs", "dmrs.on=1", "dmrs.phase=1", "dmrs.at=438000000", "dmrs.n=1", dmr];

    [Fact]
    public void FieldsAreParsed()
    {
        var p = SearchLogBook.ParseP25("433500000,456,655,1234567,3,10", T0)!.Value.Call;
        Assert.Equal(("P25", 433500000L, "$1C8", 655L, 1234567L, 3), (p.Mode, p.Hz, p.Code, p.Group, p.Source, p.Calls));
        Assert.Equal(T0.AddSeconds(-10), p.Time);
        var d = SearchLogBook.ParseDmr("438612500,1,2,1,1,0,2620,2620123,4,0", T0)!.Value.Call;
        Assert.Equal(("DMR", "CC1", "TS2 RPT", 2620L, 2620123L, false), (d.Mode, d.Code, d.Slot, d.Group, d.Source, d.Encrypted));
        Assert.Equal("CC?", SearchLogBook.ParseDmr("438612500,255,0,2,1,1,0,0,0,0", T0)!.Value.Call.Code);
        Assert.Null(SearchLogBook.ParseP25("garbage", T0));
        Assert.Null(SearchLogBook.ParseDmr("1,2,3", T0));
    }

    [Fact]
    public void OnlyNewCallsBecomeRows()
    {
        var b = new SearchLogBook();
        // first poll: what the radio already found
        var r = b.Add(Poll("p25s.f0=433500000,456,655,1234567,1,30", "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,5"), T0);
        Assert.Equal(2, r.Count);
        Assert.Equal("P25", r[0].Mode);	// heard earlier (30 s ago) comes first
        Assert.True(b.P25On);
        Assert.Equal(433512500L, b.P25AtHz);
        // the same counters again: nothing new
        Assert.Empty(b.Add(Poll("p25s.f0=433500000,456,655,1234567,1,31", "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,6"), T0.AddSeconds(1)));
        // a new call on the P25 frequency, another source on the DMR one
        r = b.Add(Poll("p25s.f0=433500000,456,655,7654321,2,0", "dmrs.f0=438612500,1,1,1,1,0,2620,2620999,2,0"), T0.AddSeconds(2));
        Assert.Equal(2, r.Count);
        Assert.Equal(4, b.Calls.Count);
        Assert.Equal(2, b.Frequencies);
        // the radio restarted (counters back down): a new baseline, not a call
        Assert.Empty(b.Add(Poll("p25s.f0=433500000,456,655,7654321,1,0"), T0.AddSeconds(3)));
        // no answer: the searches are unknown, nothing changes
        Assert.Empty(b.Add([], T0.AddSeconds(4)));
        Assert.Null(b.DmrOn);
    }

    [Fact]
    public void TheDaysCsvGetsOneHeader()
    {
        var b = new SearchLogBook();
        var rows = b.Add(Poll("p25s.f0=433500000,456,655,1234567,1,0"), T0);
        var path = SearchLogBook.Append(dir, rows, T0);
        SearchLogBook.Append(dir, rows, T0);
        var lines = File.ReadAllLines(path);
        Assert.Equal(Path.Combine(dir, "search-20261121.csv"), path);
        Assert.Equal(SearchLogBook.CsvHeader, lines[0]);
        Assert.Equal(3, lines.Length);
        Assert.Equal("2026-11-21 22:00:00,P25,433.5000,$1C8,,655,1234567,0,1", lines[1]);
    }

    [Fact]
    public void TheReaderSendsOnlyItsTwoViews()
    {
        Assert.Equal(2, DebugReaders.SearchLogAllowed.Count);
        Assert.Contains(((byte)0x09, (byte)28), DebugReaders.SearchLogAllowed);
        Assert.Contains(((byte)0x09, (byte)29), DebugReaders.SearchLogAllowed);
        Assert.Contains(AppInfo.LogsFolder, AppInfo.ReservedNames);
    }
}
