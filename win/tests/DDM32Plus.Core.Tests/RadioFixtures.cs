// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The synthetic inputs of make_radio_fixtures.py (and pc/tests/rtfixtures.py), made again here byte for byte (the
/// tests check their SHA-256 against the recorded ones), and the recorded runs of the Python tools.
/// </summary>
internal static class RadioFixtures
{
    public const int Start = 0xC000, Size = 0x100000, Sector = 4096, Slot = 0xE8000, DataSize = 0x1000000;
    public const long StockCodeAt = 0x030C68B8, StockTablesAt = 0x03072658;

    private static readonly Lazy<JsonDocument> Index_ = new(() =>
        JsonDocument.Parse(File.ReadAllBytes(TestData.FixturePath("radio", "index.json"))));

    public static JsonElement Index => Index_.Value.RootElement;

    public static JsonElement Scenario(string name)
    {
        using var gz = new GZipStream(File.OpenRead(TestData.FixturePath("radio", name + ".json.gz")), CompressionMode.Decompress);
        return JsonDocument.Parse(gz).RootElement.Clone();
    }

    public static IEnumerable<string> ScenarioNames() =>
        Directory.GetFiles(TestData.FixturePath("radio"), "*.json.gz").Select(f => Path.GetFileName(f)[..^8]).Order(StringComparer.Ordinal);

    public static byte[] Pattern(int n, int k) => TestData.Pattern(n, k);

    public static byte[] Rep(int n) => [.. Enumerable.Range(0, n).Select(i => (byte)(i & 255))];

    private static byte[] Ff(int n) => Enumerable.Repeat((byte)0xFF, n).ToArray();

    public static readonly byte[] Boot = [.. Pattern(0x1000, 7), .. Rep(0xB000)];
    public static readonly byte[] AppClassic = [.. Pattern(0x800, 3), .. Rep(0x7000), .. "DDM32 Plus"u8];
    public static readonly byte[] PlusApp = [.. Pattern(0x800, 9), .. Rep(0x5000), .. "DDM32 Plus test build"u8];

    private static byte[] Le(params uint[] words)
    {
        var b = new byte[words.Length * 4];
        for (var i = 0; i < words.Length; i++)
            BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(i * 4), words[i]);
        return b;
    }

    /// <summary>rtfixtures.make_slot: a codec slot v2 as hrc7000_codec builds it.</summary>
    public static byte[] MakeSlot(uint tag = 47, uint magic = 0x43524444)
    {
        byte[] code = [.. Le(0x152514D8, 0x74C776CD), .. Pattern(0x800 - 8, 5)];
        var tables = Pattern(0x302, 11);
        var rels = Le(8, 0x40, 0x100);
        const int codeOff = 128;
        var tabOff = codeOff + code.Length;
        var relOff = tabOff + ((tables.Length + 3) & ~3);
        var crc = Crc32.Compute(rels, Crc32.Compute(tables, Crc32.Compute(code)));
        var header = Le(magic, 2, codeOff, (uint)code.Length, 0x16cdc, (uint)tabOff, (uint)tables.Length, 0x18000e48,
            0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, tag, crc, (uint)relOff, (uint)(rels.Length / 4));
        return [.. header, .. new byte[128 - header.Length], .. code, .. tables, .. Ff(relOff - tabOff - tables.Length), .. rels];
    }

    public static byte[] Program(byte[] app, byte[]? slot = null)
    {
        slot ??= MakeSlot();
        byte[] p = [.. Boot, .. app, .. Ff(Slot - Start - app.Length), .. slot];
        return [.. p, .. Ff(Size - p.Length)];
    }

    public static byte[] MakeData()
    {
        var d = Ff(DataSize);
        Pattern(Sector, 13).CopyTo(d, 0);
        Pattern(3 * Sector, 17).CopyTo(d, 0x800000);
        return d;
    }

    public static (byte[] Code, byte[] Tables, List<(long, long)> Refs) SyntheticCodec()
    {
        const long run = 0x16CDC;
        var code = Pattern(0x2000, 29);
        Le(0x152514D8, 0x74C776CD).CopyTo(code, 0);
        var refs = new List<(long, long)> { (0x40, run + 0x100), (0x84, run), (0x1000, run + 0x2000 - 4) };
        foreach (var (off, v) in refs)
            Le((uint)v).CopyTo(code, (int)off);
        return (code, Pattern(0x2F78, 31), refs);
    }

    public static byte[] StockProgram(byte[] code, byte[] tables)
    {
        byte[] p = [.. Boot, .. Pattern(0x800, 19), .. Rep(0x3000), .. Ff(Size - Start - 0x3800)];
        code.CopyTo(p, (int)(StockCodeAt - 0x03000000));
        tables.CopyTo(p, (int)(StockTablesAt - 0x03000000));
        return p;
    }

    public static Codec SyntheticCodecTables()
    {
        var (code, tables, refs) = SyntheticCodec();
        return Codec.Synthetic(code, tables, refs, StockCodeAt, StockTablesAt);
    }

    /// <summary>rtfixtures.write_backup: both passes of both chips and a verified manifest with the chip IDs.</summary>
    public static void WriteBackup(string folder, byte[] program, byte[] data, bool chips = true)
    {
        Directory.CreateDirectory(folder);
        var captures = new List<object?>();
        foreach (var (memory, image) in new[] { ("progmem", program), ("q128", data) })
            for (var n = 1; n <= 2; n++)
            {
                var name = $"{memory}-pass{n}.bin";
                File.WriteAllBytes(Path.Combine(folder, name), image);
                var sectors = new List<object?>();
                for (var i = 0; i < image.Length; i += Sector)
                    sectors.Add(Hash.Sha(image.AsSpan(i, Sector)));
                captures.Add(new PyDict { ["file"] = name, ["bytes"] = image.Length, ["sha256"] = Hash.Sha(image), ["sector_sha256"] = sectors });
            }
        var m = new PyDict { ["backup_verified"] = true, ["captures"] = captures };
        if (chips)
            m["chips"] = new PyDict
            {
                ["progmem"] = new PyDict { ["jedec"] = "c84014", ["bytes"] = Size },
                ["q128"] = new PyDict { ["jedec"] = "ef4018", ["bytes"] = DataSize },
            };
        File.WriteAllText(Path.Combine(folder, "backup-manifest.json"), PyJson.Dumps(m, null));
    }

    private static readonly (string Name, long Value)[] CodecSymbols =
    [
        ("__codec_slot", 0x030e8000), ("__codec_slot_end", 0x03100000), ("__codec_run_lo", 0x43a00),
        ("__codec_run_hi", 0x58000), ("__codec_tab_lo", 0x18000e48), ("__codec_tab_hi", 0x18003e38),
    ];

    /// <summary>rtfixtures.write_plus with the signed manifest the Python run used (the test key's signature).</summary>
    public static void WritePlus(string folder, byte[] app)
    {
        Directory.CreateDirectory(folder);
        File.WriteAllBytes(Path.Combine(folder, "rfoundry_dm32.bin"), app);
        var map = new StringBuilder("\nMemory Configuration\n\n .text          0x0300c000     0x7000\n");
        foreach (var (name, value) in CodecSymbols)
            map.Append($"                0x{value:x8}                {name} = 0x{value:x}\n");
        var mapping = Encoding.ASCII.GetBytes(map.ToString());
        File.WriteAllBytes(Path.Combine(folder, "rfoundry_dm32.map"), mapping);
        File.WriteAllText(Path.Combine(folder, "rfoundry_dm32.json"), PyJson.Dumps(new PyDict
        {
            ["plus_enabled"] = true, ["classic_enabled"] = true, ["forced_rebuild_identical"] = true, ["public_enabled"] = true,
            ["outputs"] = new PyDict { ["rfoundry_dm32.bin"] = Hash.Sha(app), ["rfoundry_dm32.map"] = Hash.Sha(mapping) },
        }, null));
        var inputs = Index.GetProperty("inputs");
        File.WriteAllBytes(Path.Combine(folder, "ddm32-manifest.json"), Convert.FromBase64String(inputs.GetProperty("plus_manifest").GetString()!));
        File.WriteAllText(Path.Combine(folder, "ddm32-manifest.json.sig"), inputs.GetProperty("plus_sig").GetString()!);
    }
}

/// <summary>The inputs of the recorded runs, in a temporary folder (one per test class).</summary>
public sealed class RadioInputs : IDisposable
{
    public string Root { get; } = Path.Combine(Path.GetTempPath(), "ddm32-radio-" + Guid.NewGuid().ToString("N")[..12]);
    public byte[] Program { get; }
    public byte[] Data { get; }
    public byte[] Stock { get; }
    public byte[] Target { get; }
    public string Backup => Path.Combine(Root, "backup");
    public string StockBackup => Path.Combine(Root, "stock-backup");
    public string Plus => Path.Combine(Root, "plus");

    public RadioInputs()
    {
        Program = RadioFixtures.Program(RadioFixtures.AppClassic);
        Data = RadioFixtures.MakeData();
        var (code, tables, _) = RadioFixtures.SyntheticCodec();
        Stock = RadioFixtures.StockProgram(code, tables);
        Target = [.. Program.AsSpan(0, RadioFixtures.Start), .. RadioFixtures.PlusApp,
            .. Enumerable.Repeat((byte)0xFF, RadioFixtures.Slot - RadioFixtures.Start - RadioFixtures.PlusApp.Length),
            .. Program.AsSpan(RadioFixtures.Slot)];
        RadioFixtures.WriteBackup(Backup, Program, Data);
        RadioFixtures.WriteBackup(StockBackup, Stock, Data);
        RadioFixtures.WritePlus(Plus, RadioFixtures.PlusApp);
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(Root, recursive: true);
        }
        catch (IOException)
        {
        }
    }
}

/// <summary>
/// A serial port that is a recorded run of the Python tool against the pinned FakeRadio: every write must be the
/// recorded write (same bytes, same order), after which the radio's recorded answer can be read; input resets,
/// open and close must come where they came. Nothing else is possible: there is no radio here.
/// </summary>
internal sealed class ReplayPort : IRawSerial
{
    private readonly List<JsonElement> events;
    private int next;
    private readonly List<byte> rx = [];
    public int Writes { get; private set; }

    public ReplayPort(JsonElement scenario)
    {
        events = [.. scenario.GetProperty("events").EnumerateArray()];
        Assert.Equal("init", events[0][0].GetString());
        rx.AddRange(Convert.FromBase64String(events[0][1].GetString()!));
        next = 1;
    }

    public double Timeout { get; set; } = 0.02;
    public int BaudRate { get; set; } = 115200;

    private JsonElement Expect(string kind, string detail = "")
    {
        Assert.True(next < events.Count, $"the C# tool did more than the Python tool: {kind} {detail} after event {next}");
        var e = events[next];
        Assert.True(e[0].GetString() == kind,
            $"event {next}: the Python tool did '{e[0].GetString()}', the C# tool '{kind}' {detail}");
        next++;
        return e;
    }

    public void Open() => Expect("open");

    public void Close() => Expect("close");

    public int? Write(byte[] data)
    {
        var e = Expect("w", Convert.ToHexString(data.AsSpan(0, Math.Min(24, data.Length))));
        var want = Convert.FromBase64String(e[1].GetString()!);
        Assert.True(want.AsSpan().SequenceEqual(data),
            $"write {Writes} (event {next - 1}) differs: Python {Convert.ToHexString(want.AsSpan(0, Math.Min(32, want.Length)))}..., " +
            $"C# {Convert.ToHexString(data.AsSpan(0, Math.Min(32, data.Length)))}...");
        rx.AddRange(Convert.FromBase64String(e[2].GetString()!));
        Writes++;
        return null;            // the FakeRadio's write returns None, like some pyserial ports
    }

    public byte[] Read(int n)
    {
        var k = Math.Min(n, rx.Count);
        var b = rx.GetRange(0, k).ToArray();
        rx.RemoveRange(0, k);
        return b;
    }

    public void ResetInputBuffer()
    {
        Expect("reset");
        rx.Clear();
    }

    public void Flush()
    {
    }

    public void AssertDone() => Assert.True(next == events.Count, $"the C# tool stopped at event {next} of {events.Count}");
}
