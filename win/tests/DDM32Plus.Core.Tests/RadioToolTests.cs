// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using System.Text.RegularExpressions;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>JSON from the recorded fixtures as the C# side's objects, for PyJson.Dumps on both sides.</summary>
internal static class JsonAs
{
    public static object? Py(JsonElement e) => e.ValueKind switch
    {
        JsonValueKind.Object => e.EnumerateObject().Aggregate(new PyDict(), (d, p) => { d[p.Name] = Py(p.Value); return d; }),
        JsonValueKind.Array => e.EnumerateArray().Select(Py).ToList(),
        JsonValueKind.String => e.GetString(),
        JsonValueKind.Number => e.TryGetInt64(out var l) ? (object)l : e.GetDouble(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        _ => null,
    };
}

/// <summary>
/// The C# radio tools against the Python tools' recorded runs (Fixtures/radio, make_radio_fixtures.py): the same
/// bytes to the radio in the same order, the same printed lines, the same error, the same files (backup images,
/// manifests, receipts). Every run is a replay: no serial port exists here.
/// </summary>
public sealed partial class RadioToolTests(RadioInputs inputs) : IClassFixture<RadioInputs>
{
    public static TheoryData<string> Names() => [.. RadioFixtures.ScenarioNames()];

    [Fact]
    public void InputsAreThePythonInputs()
    {
        var h = RadioFixtures.Index.GetProperty("inputs");
        Assert.Equal(h.GetProperty("program").GetString(), Hash.Sha(inputs.Program));
        Assert.Equal(h.GetProperty("data").GetString(), Hash.Sha(inputs.Data));
        Assert.Equal(h.GetProperty("stock").GetString(), Hash.Sha(inputs.Stock));
        Assert.Equal(h.GetProperty("target").GetString(), Hash.Sha(inputs.Target));
        Assert.Equal(h.GetProperty("plus_app").GetString(), Hash.Sha(RadioFixtures.PlusApp));
        Assert.Equal(h.GetProperty("slot").GetString(), Hash.Sha(RadioFixtures.MakeSlot()));
        var (code, tables, _) = RadioFixtures.SyntheticCodec();
        Assert.Equal(h.GetProperty("code").GetString(), Hash.Sha(code));
        Assert.Equal(h.GetProperty("tables").GetString(), Hash.Sha(tables));
        Assert.Equal(RadioFixtures.Index.GetProperty("ram_monitor_sha256").GetString(), Hash.Sha(RamMonitor.Image));
    }

    private string Generic(string s, string outDir) =>
        s.Replace(outDir, "{out}", StringComparison.Ordinal).Replace(inputs.Backup, "{backup}", StringComparison.Ordinal)
            .Replace(inputs.StockBackup, "{stock_backup}", StringComparison.Ordinal).Replace(inputs.Plus, "{plus}", StringComparison.Ordinal);

    [GeneratedRegex(@"""write_seconds"": [-0-9.e+]+")]
    private static partial Regex WriteSeconds();

    [Theory]
    [MemberData(nameof(Names))]
    public void SameBytesFilesAndLinesAsPython(string name)
    {
        var sc = RadioFixtures.Scenario(name);
        var outDir = Path.Combine(inputs.Root, "out-" + name);
        string Sub(string a) => a.Replace("{out}", outDir, StringComparison.Ordinal)
            .Replace("{backup}", inputs.Backup, StringComparison.Ordinal)
            .Replace("{stock_backup}", inputs.StockBackup, StringComparison.Ordinal)
            .Replace("{plus}", inputs.Plus, StringComparison.Ordinal);
        List<string> argv = ["--port", "SIMULATED", .. sc.GetProperty("args").EnumerateArray().Select(a => Sub(a.GetString()!))];
        ReplayPort? port = null;
        var lines = new List<string>();
        var ctx = new ToolContext
        {
            Print = s => lines.AddRange(s.Split('\n')),
            Clock = new VirtualClock(RadioFixtures.Index.GetProperty("stamp").GetString()!),
            Serial = n =>
            {
                Assert.Equal("SIMULATED", n);
                Assert.Null(port);
                return port = new ReplayPort(sc);
            },
            ReleaseKeyHex = TestData.TestPublicHex,
            Codec = sc.GetProperty("stock_codec").GetBoolean() ? RadioFixtures.SyntheticCodecTables() : Codec.Dm32,
        };
        string[]? error = null;
        try
        {
            switch (sc.GetProperty("tool").GetString())
            {
                case "backup": BackupTool.Run(argv, ctx); break;
                case "install_plus": InstallTool.Run(argv, ctx); break;
                case "restore_stock": RestoreStockTool.Run(argv, ctx); break;
                default: throw new InvalidOperationException("tool");
            }
        }
        catch (ToolError e)
        {
            error = [e.PyType, Generic(e.Message, outDir)];
        }

        // the radio saw exactly what the Python tool sent
        if (sc.GetProperty("events").ValueKind == JsonValueKind.Null)
            Assert.Null(port);
        else
        {
            Assert.NotNull(port);
            port.AssertDone();
        }
        // the same error, the same printed lines
        var want = sc.GetProperty("error");
        if (want.ValueKind == JsonValueKind.Null)
            Assert.Null(error);
        else
            Assert.Equal([want[0].GetString()!, want[1].GetString()!], error ?? ["no error", ""]);
        Assert.Equal(sc.GetProperty("lines").EnumerateArray().Select(l => l.GetString()), lines.Select(l => Generic(l, outDir)));
        // the same files
        var files = sc.GetProperty("files");
        var mine = Directory.Exists(outDir) ? Directory.GetFiles(outDir).Select(Path.GetFileName).Order(StringComparer.Ordinal).ToList() : [];
        Assert.Equal(files.EnumerateObject().Select(p => p.Name).Order(StringComparer.Ordinal), mine);
        foreach (var f in files.EnumerateObject())
        {
            var body = File.ReadAllBytes(Path.Combine(outDir, f.Name));
            if (f.Value.TryGetProperty("text", out var text))
                Assert.Equal(WriteSeconds().Replace(text.GetString()!, "\"write_seconds\": X"),
                    WriteSeconds().Replace(System.Text.Encoding.UTF8.GetString(body), "\"write_seconds\": X"));
            else
            {
                Assert.Equal(f.Value.GetProperty("bytes").GetInt64(), body.LongLength);
                Assert.Equal(f.Value.GetProperty("sha256").GetString(), Hash.Sha(body));
            }
        }
    }

    [Fact]
    public void EveryRecordedRunIsCovered()
    {
        var names = RadioFixtures.ScenarioNames().ToList();
        Assert.True(names.Count >= 25);
        foreach (var n in new[] { "backup_ok", "install_ok", "from_stock_ok", "restore_ok", "restore_stock_ok",
                     "install_readback_corrupt", "backup_damaged_frames" })
            Assert.Contains(n, names);
    }

    // --- radio_detect --------------------------------------------------------------------------------------------
    private byte[] DetectProgram(string name)
    {
        var slot = RadioFixtures.MakeSlot();
        var ff = Enumerable.Repeat((byte)0xFF, 4096).ToArray();
        return name switch
        {
            "classic_ddm32" => RadioFixtures.Program(RadioFixtures.AppClassic),
            "classic_ddm25" => RadioFixtures.Program([.. RadioFixtures.Pattern(0x900, 3), .. "DDM25 Plus"u8]),
            "classic_open25" => RadioFixtures.Program([.. "open25-classic-r13"u8, .. RadioFixtures.Pattern(0x900, 3)]),
            "classic_unknown" => RadioFixtures.Program(RadioFixtures.Pattern(0x900, 3)),
            "no_slot_classic_app" => RadioFixtures.Program(RadioFixtures.AppClassic, ff),
            "no_slot_unknown" => RadioFixtures.Program(RadioFixtures.Pattern(0x900, 5), ff),
            "bad_slot_crc" => RadioFixtures.Program(RadioFixtures.AppClassic, [.. slot.AsSpan(0, 0x40), 0, .. slot.AsSpan(0x41)]),
            "stock_047" => inputs.Stock,
            "stock_moved" => Moved(inputs.Stock),
            "classic_with_stock_codec" => inputs.Program,
            _ => throw new InvalidOperationException(name),
        };
    }

    private static byte[] Moved(byte[] stock)
    {
        var m = (byte[])stock.Clone();
        Array.Clear(m, (int)(RadioFixtures.StockCodeAt - 0x03000000), 8);
        return m;
    }

    [Fact]
    public void DetectAnswersAsPython()
    {
        var n = 0;
        foreach (var c in RadioFixtures.Index.GetProperty("detect").EnumerateArray())
        {
            var name = c.GetProperty("name").GetString()!;
            if (name == "tool_output")
            {
                Assert.Equal(c.GetProperty("printed").GetString(), Detect.ToolOutput(inputs.Backup, Codec.Dm32));
                continue;
            }
            var codec = c.GetProperty("stock").GetBoolean() ? RadioFixtures.SyntheticCodecTables() : Codec.Dm32;
            if (name == "compose_from_stock")
            {
                var (target, prof) = UnitImage.ComposeFromStock(inputs.Stock, RadioFixtures.PlusApp, codec);
                var r = c.GetProperty("result");
                Assert.Equal(r.GetProperty("target_sha256").GetString(), Hash.Sha(target));
                Assert.Equal(r.GetProperty("profile").GetString(), prof.Name);
                Assert.Equal(PyJson.Dumps(JsonAs.Py(r.GetProperty("slot"))),
                    PyJson.Dumps(codec.ParseSlot(target.AsSpan(RadioFixtures.Slot))));
                continue;
            }
            var program = DetectProgram(name);
            Assert.Equal(c.GetProperty("program").GetString(), Hash.Sha(program));
            Assert.Equal(PyJson.Dumps(JsonAs.Py(c.GetProperty("result"))), PyJson.Dumps(Detect.Run(program, codec)));
            n++;
        }
        Assert.Equal(10, n);
    }

    [Fact]
    public void EmbeddedCodecTablesAreThePinnedOnes()
    {
        var k = RadioFixtures.Index.GetProperty("codec");
        Assert.Equal(k.GetProperty("profiles").EnumerateObject().Select(p => p.Name), Codec.Dm32.Profiles.Select(p => p.Key));
        Assert.Equal(k.GetProperty("maps").EnumerateObject().Select(p => p.Name).Order(), Codec.Dm32.Maps.Keys.Order());
        foreach (var p in k.GetProperty("profiles").EnumerateObject())
        {
            var mine = Codec.Dm32.Profiles.Single(x => x.Key == p.Name).Profile;
            Assert.Equal(p.Value.GetProperty("name").GetString(), mine.Name);
            Assert.Equal(p.Value.GetProperty("code")[0].GetInt64(), mine.Code.Addr);
            Assert.Equal(p.Value.GetProperty("tables_sha256").GetString(), mine.TablesSha256);
        }
        Assert.Equal(0x030E8000, Codec.Dm32.Board.SlotLo);
        UnitImage.CheckCodec(Codec.Dm32);
    }
}
