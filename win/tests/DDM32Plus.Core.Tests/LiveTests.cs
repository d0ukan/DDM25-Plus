// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json.Nodes;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// A recorded debug-link conversation (Fixtures/live-results.json, make_live_fixtures.py): each written frame must be
/// the next recorded one, and the radio's recorded answer to it is queued for reading. A frame out of order fails the
/// test at once.
/// </summary>
internal sealed class DebugTranscriptPort(JsonArray steps) : IRawSerial
{
    private readonly Queue<byte> pending = new();
    private int next;
    public bool Opened { get; private set; }
    public int Closes { get; private set; }
    public double Timeout { get; set; }
    public int BaudRate { get; set; }
    public int Remaining => steps.Count - next;

    public void Open() => Opened = true;

    public void Close() => Closes++;

    public void ResetInputBuffer() => pending.Clear();

    public void Flush()
    {
    }

    public int? Write(byte[] data)
    {
        Assert.True(next < steps.Count, "a frame after the recorded run ended: " + Convert.ToHexStringLower(data));
        var step = steps[next++]!;
        Assert.Equal((string)step["write"]!, Convert.ToHexStringLower(data));
        foreach (var b in Convert.FromHexString((string)step["reply"]!))
            pending.Enqueue(b);
        return data.Length;
    }

    public byte[] Read(int n)
    {
        var out_ = new List<byte>();
        while (out_.Count < n && pending.Count > 0)
            out_.Add(pending.Dequeue());
        return [.. out_];
    }
}

/// <summary>A fake radio that answers every allowed request with a fixed view, counting what reaches it.</summary>
internal sealed class FakeDebugRadio : IRawSerial
{
    private readonly Queue<byte> pending = new();
    public List<byte[]> Frames { get; } = [];
    public bool Silent { get; set; }
    public double Timeout { get; set; }
    public int BaudRate { get; set; }
    public int Closes { get; private set; }

    public void Open()
    {
    }

    public void Close() => Closes++;

    public void ResetInputBuffer() => pending.Clear();

    public void Flush()
    {
    }

    public int? Write(byte[] data)
    {
        Frames.Add(data);
        if (!Silent)
        {
            var body = new List<byte> { 0, 0x50, 0x40, 0x01, 0x00, 5 };
            body.AddRange("state=rx\nrssi_dbm=-90\n"u8.ToArray());
            foreach (var b in DebugFrame.Build((byte)(data[2] | 0x80), data[3], body.ToArray()))
                pending.Enqueue(b);
        }
        return data.Length;
    }

    public byte[] Read(int n)
    {
        var out_ = new List<byte>();
        while (out_.Count < n && pending.Count > 0)
            out_.Add(pending.Dequeue());
        return [.. out_];
    }
}

public class LiveTests
{
    private static readonly JsonNode Fixture = JsonNode.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Fixtures", "live-results.json")))!;

    private static IEnumerable<JsonNode> Cases(string tool) =>
        Fixture["cases"]!.AsArray().Select(c => c!).Where(c => (string)c["tool"]! == tool);

    public static TheoryData<string> LiveCases() => [.. Cases("live").Select(c => (string)c["name"]!)];

    public static TheoryData<string> HealthCases() => [.. Cases("health").Select(c => (string)c["name"]!)];

    private static JsonNode Case(string name) => Fixture["cases"]!.AsArray().First(c => (string)c!["name"]! == name)!;

    [Fact]
    public void FramesAndCrcEqualThePinnedProtocol()
    {
        foreach (var f in Fixture["frames"]!.AsArray())
        {
            var frame = DebugFrame.Build((byte)(int)f!["cmd"]!, (byte)(int)f["seq"]!, Convert.FromHexString((string)f["payload"]!));
            Assert.Equal((string)f["frame"]!, Convert.ToHexStringLower(frame));
        }
        foreach (var c in Fixture["crc16"]!.AsArray())
            Assert.Equal((int)c!["crc"]!, DebugFrame.Crc16(Convert.FromHexString((string)c["data"]!)));
    }

    [Fact]
    public void NamesEqualThePythonTables()
    {
        Assert.Equal(Fixture["native_fields"]!.AsArray().Select(x => (string)x!), NativeModem.Fields);
        Assert.Equal(Fixture["native_states"]!.AsArray().Select(x => (string)x!), NativeModem.States);
        Assert.Equal(Fixture["native_reasons"]!.AsArray().Select(x => (string)x!), NativeModem.Reasons);
        Assert.Equal(Fixture["scan_fields"]!.AsArray().Select(x => (string)x!), NativeModem.ScanFields);
        var allowed = Fixture["live_allowed"]!.AsArray().Select(x => ((byte)(int)x![0]!, Convert.FromHexString((string)x[1]!)[0])).ToHashSet();
        Assert.True(allowed.SetEquals(DebugReaders.LiveAllowed));
        Assert.True(DebugReaders.HealthAllowed.IsSubsetOf(DebugReaders.LiveAllowed));
    }

    [Theory]
    [MemberData(nameof(LiveCases))]
    public void LiveReaderPrintsWhatPlusLivePrints(string name)
    {
        var c = Case(name);
        var port = new DebugTranscriptPort(c["steps"]!.AsArray());
        var lines = new List<string>();
        var ctx = new ToolContext { Print = s => lines.AddRange(s.Split('\n')), Clock = new VirtualClock(), Serial = _ => port };
        var exit = 0;
        try
        {
            DebugReaders.Live(["--port", "COM9", "--interval", "2", "--count", ((int)c["count"]!).ToString(System.Globalization.CultureInfo.InvariantCulture)], ctx);
        }
        catch (ToolExit e)
        {
            exit = int.Parse(e.Message, System.Globalization.CultureInfo.InvariantCulture);
        }
        Assert.Equal((int)c["exit"]!, exit);
        Assert.Equal("LIVE: COM9 open, read only, every 2 s", lines[0]);
        Assert.Equal((string)c["output"]!, string.Join("\n", lines.Skip(1)) + "\n");
        Assert.Equal(0, port.Remaining);
        Assert.True(port.Opened);
        Assert.Equal(1, port.Closes);
        Assert.Equal(115200, port.BaudRate);

        // the page's reading of each poll equals parsers.live_summary
        var polls = SplitPolls(lines.Skip(1).ToList());
        var expected = c["summaries"]!.AsArray();
        Assert.Equal(expected.Count, polls.Blocks.Count);
        for (var i = 0; i < expected.Count; i++)
        {
            var s = LiveData.Summary(LiveData.ParseBlocks(polls.Blocks[i]), LiveData.ParseBlocks(polls.Once));
            AssertJson(expected[i]!, SummaryJson(s));
        }
    }

    internal static (List<string> Once, List<List<string>> Blocks) SplitPolls(List<string> lines)
    {
        var once = new List<string>();
        var blocks = new List<List<string>>();
        List<string>? block = null;
        foreach (var l in lines)
        {
            var m = LiveData.PollMarker(l);
            if (m is ("poll", _))
                block = [];
            else if (m is ("end", _))
            {
                if (block is not null)
                    blocks.Add(block);
                block = null;
            }
            else
                (block ?? once).Add(l.Trim());
        }
        return (once, blocks);
    }

    [Theory]
    [MemberData(nameof(HealthCases))]
    public void HealthReaderPrintsWhatPlusTxdiagPrints(string name)
    {
        var c = Case(name);
        var port = new DebugTranscriptPort(c["steps"]!.AsArray());
        var lines = new List<string>();
        var ctx = new ToolContext { Print = s => lines.AddRange(s.Split('\n')), Clock = new VirtualClock(), Serial = _ => port };
        string? error = null;
        try
        {
            DebugReaders.Health(["--port", "COM9", "--health"], ctx);
        }
        catch (DebugLinkError e)
        {
            error = e.Record;
        }
        Assert.Equal((string?)c["error"], error);
        var text = lines.Count == 0 ? "" : string.Join("\n", lines) + "\n";
        Assert.Equal((string)c["output"]!, text);
        Assert.Equal(0, port.Remaining);
        Assert.Equal(1, port.Closes);
        var s = LiveData.Summary(lines);
        AssertJson(c["summary"]!, HealthJson(s));
        var verdict = (string?)c["verdict"];
        if (verdict is not null)
            Assert.Equal(verdict, LiveData.Verdict(s).ToString().ToLowerInvariant());
    }

    [Fact]
    public void GuardRefusesEverythingOutsideItsList()
    {
        var radio = new FakeDebugRadio();
        var guard = new DebugLinkPort(radio, DebugReaders.LiveAllowed, "plus_live");
        byte[][] refused =
        [
            DebugFrame.Build(0x06, 1, []), DebugFrame.Build(0x07, 1, [1]), DebugFrame.Build(0x0A, 1, [1]),
            DebugFrame.Build(0x21, 1, [1]), DebugFrame.Build(0x21, 1, [0x0A, 1]), DebugFrame.Build(0x09, 1, [0x09]),
            DebugFrame.Build(0x0F, 1, [2]), DebugFrame.Build(0x0D, 1, []), DebugFrame.Build(0x20, 1, [0x0D]),
            DebugFrame.Build(0x09, 1, [0x00, 0x00]), [0xA5, 0x5A, 0x09, 1, 1, 0, 0x00, 0, 0],
            [0x09, 0x00], [], DebugFrame.Build(0x09, 1, [0x00])[..^1],
        ];
        foreach (var f in refused)
            Assert.Throws<ValueError>(() => guard.Write(f));
        Assert.Empty(radio.Frames);
        Assert.Equal(refused.Length, guard.Refusals.Count);
        Assert.Throws<ValueError>(() => guard.BaudRate = 9600);
        Assert.Throws<ValueError>(() => guard.Write(DebugFrame.Build(0x09, 1, [0x14, 0x00])));
        guard.Write(DebugFrame.Build(0x09, 1, [0x00]));
        Assert.Single(radio.Frames);

        var health = new DebugLinkPort(radio, DebugReaders.HealthAllowed, "plus_txdiag");
        Assert.Throws<ValueError>(() => health.Write(DebugFrame.Build(0x09, 2, [0x00])));      // core view: live only
        Assert.Throws<ValueError>(() => health.Write(DebugFrame.Build(0x21, 2, [0x00])));
        health.Write(DebugFrame.Build(0x09, 2, [0x14]));
    }

    [Fact]
    public void LiveStopsOnCancelAndClosesThePort()
    {
        var radio = new FakeDebugRadio();
        using var cts = new CancellationTokenSource();
        var polls = 0;
        var ctx = new ToolContext
        {
            Print = s =>
            {
                if (s.StartsWith("=== end ", StringComparison.Ordinal) && ++polls == 3)
                    cts.Cancel();
            },
            Clock = new VirtualClock(), Serial = _ => radio, Cancel = cts.Token,
        };
        DebugReaders.Live(["--port", "COM9", "--interval", "1"], ctx);
        Assert.Equal(3, polls);
        Assert.Equal(1, radio.Closes);
        Assert.Equal(2 + 3 * 4, radio.Frames.Count);
        Assert.All(radio.Frames, f => Assert.Contains(((byte, byte))(f[2], f[6]), DebugReaders.LiveAllowed));
    }

    [Fact]
    public void RefusedArgumentsAndPortsOpenNothing()
    {
        var opened = 0;
        var ctx = new ToolContext { Serial = _ => { opened++; return new FakeDebugRadio(); }, Clock = new VirtualClock() };
        Assert.Throws<ToolExit>(() => DebugReaders.Live(["--port", "COM9", "--interval", "9"], ctx));
        Assert.Throws<ToolExit>(() => DebugReaders.Live(["--port", "COM9", "--count", "-1"], ctx));
        Assert.Throws<ToolExit>(() => DebugReaders.Live(["--interval", "2"], ctx));
        Assert.Throws<ToolExit>(() => DebugReaders.Health(["--port", "COM9", "--write"], ctx));
        Assert.Throws<ValueError>(() => DebugReaders.Health(["--port", "udp:10.0.0.1"], ctx));
        Assert.Throws<ValueError>(() => DebugReaders.Live(["--port", "rfc2217://host:1"], ctx));
        Assert.Equal(0, opened);
        Assert.Throws<ToolContext.NoPortGivenException>(() => DebugReaders.Health(["--port", "COM9"], new ToolContext()));
    }

    [Fact]
    public void PresentationHelpersEqualPython()
    {
        foreach (var x in Fixture["s_meter"]!.AsArray())
            Assert.Equal((string)x![1]!, LiveData.SMeter(x[0] is null ? null : (long)x[0]!));
        foreach (var x in Fixture["format_mhz"]!.AsArray())
            Assert.Equal((string)x![1]!, LiveData.FormatMhz(x[0] is null ? null : (long)x[0]!));
        foreach (var x in Fixture["format_uptime"]!.AsArray())
            Assert.Equal((string)x![1]!, LiveData.FormatUptime(x[0] is null ? null : (long)x[0]!));
        Assert.Equal("434.25000 MHz", LiveData.FormatMhz(434_250_000, decimalComma: false));
        Assert.Equal(0, LiveData.DbmPercent(null));
        Assert.Equal(0, LiveData.DbmPercent(-150));
        Assert.Equal(53, LiveData.DbmPercent(-87));
        Assert.Equal(100, LiveData.DbmPercent(-10));
        Assert.Equal("1.200 B", LiveData.Bytes(1200));
        Assert.Equal("1,200 B", LiveData.Bytes(1200, turkish: false));
        Assert.Equal("-", LiveData.Bytes(null));
        Assert.Equal(26L, LiveData.PyInt("0x1a"));
        Assert.Equal(-87L, LiveData.PyInt("-87"));
        Assert.Null(LiveData.PyInt("012"));
        Assert.Equal(0L, LiveData.PyInt("00"));
        Assert.Null(LiveData.PyInt("abc"));
    }

    // --- the Python summaries as JSON -----------------------------------------------------------------------------
    private static void AssertJson(JsonNode expected, JsonNode actual)
    {
        Assert.True(JsonNode.DeepEquals(expected, actual), $"expected {expected.ToJsonString()}\nactual   {actual.ToJsonString()}");
        if (expected is JsonObject eo && eo["health"] is JsonObject eh)
            Assert.Equal(((JsonObject)eh["tasks"]!).Select(kv => kv.Key), ((JsonObject)actual["health"]!["tasks"]!).Select(kv => kv.Key));
        if (expected is JsonObject et && et["tasks"] is JsonObject tasks)
            Assert.Equal(tasks.Select(kv => kv.Key), ((JsonObject)actual["tasks"]!).Select(kv => kv.Key));
    }

    private static JsonObject HealthJson(HealthSummary h)
    {
        var tasks = new JsonObject();
        foreach (var (n, f) in h.Tasks)
            tasks[n] = f;
        return new JsonObject
        {
            ["uptime_ms"] = h.UptimeMs, ["heap_free"] = h.HeapFree, ["heap_min_free"] = h.HeapMinFree, ["min_task"] = h.MinTask,
            ["min_free"] = h.MinFree, ["crash_valid"] = h.CrashValid, ["crash_built"] = h.CrashBuilt, ["crash_task"] = h.CrashTask,
            ["crash_pc"] = h.CrashPc, ["crash_uptime_ms"] = h.CrashUptimeMs, ["reset_cause"] = h.ResetCause, ["tasks"] = tasks,
            ["ok"] = h.Ok,
        };
    }

    private static JsonObject SummaryJson(LiveSummary s)
    {
        var g = s.Signal;
        var n = s.Native;
        JsonObject? scan = null;
        if (s.Scan is not null)
        {
            scan = [];
            foreach (var kv in s.Scan)
                scan[kv.Key] = kv.Value;
        }
        return new JsonObject
        {
            ["signal"] = new JsonObject
            {
                ["ok"] = g.Ok, ["rssi_dbm"] = g.RssiDbm, ["noise"] = g.Noise, ["state"] = g.State, ["mode"] = g.Mode, ["zone"] = g.Zone,
                ["channel"] = g.Channel, ["channel_number"] = g.ChannelNumber, ["vfo"] = g.Vfo, ["rx_hz"] = g.RxHz, ["tx_hz"] = g.TxHz,
                ["squelch"] = g.Squelch, ["monitor"] = g.Monitor, ["uptime_ms"] = g.UptimeMs,
            },
            ["native"] = new JsonObject
            {
                ["ok"] = n.Ok, ["status"] = n.Status, ["state"] = n.State, ["reason"] = n.Reason, ["frequency_hz"] = n.FrequencyHz,
                ["rx_count"] = n.RxCount, ["rx_seen"] = n.RxSeen, ["p25_sync"] = n.P25Sync, ["faults"] = n.Faults,
            },
            ["scan"] = scan,
            ["scan_status"] = s.ScanStatus,
            ["health"] = s.Health is null ? null : HealthJson(s.Health),
        };
    }
}
