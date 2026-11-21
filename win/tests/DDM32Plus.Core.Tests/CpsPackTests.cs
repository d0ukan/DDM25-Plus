// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using DDM32Plus.Core.DataPacks;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// A fake DDM32 Plus radio speaking the open-firmware CPS protocol as RadioFoundry's core/cps/cps_openfw.c does
/// (R 1 physical flash reads, R 9 radio info, X 1 prepare / X 2 merge / X 3 commit of 4 KiB sectors inside the
/// open-firmware window 0x720000-0xEFFFFF, C replies '-'), on a sparse 16 MiB data chip. Faults on request.
/// </summary>
internal sealed class FakeCpsRadio : IRawSerial
{
    public const long Chip = 1 << 24;
    private readonly Dictionary<long, byte[]> sectors = [];
    private readonly Queue<byte> replies = new();
    private byte[]? pending;
    private long pendingAddr;
    public List<byte[]> Requests { get; } = [];
    public List<long> Commits { get; } = [];
    public long RadioType { get; set; } = 11;
    public long? CorruptReadsOf { get; set; }
    public int? RefuseCommitNumber { get; set; }
    public bool Opened { get; private set; }
    public int Closes { get; private set; }
    public double Timeout { get; set; }
    public int BaudRate { get; set; } = 115200;

    public void Open() => Opened = true;

    public void Close() => Closes++;

    public void ResetInputBuffer() => replies.Clear();

    public void Flush()
    {
    }

    public byte[] Sector(long addr) => sectors.TryGetValue(addr / 4096, out var s) ? s : Erased();

    private static byte[] Erased()
    {
        var b = new byte[4096];
        Array.Fill(b, (byte)0xFF);
        return b;
    }

    public void Put(long addr, byte[] data)
    {
        for (var i = 0; i < data.Length; i++)
        {
            var a = addr + i;
            if (!sectors.TryGetValue(a / 4096, out var s))
                sectors[a / 4096] = s = Erased();
            s[a % 4096] = data[i];
        }
    }

    public byte[] Get(long addr, int n)
    {
        var out_ = new byte[n];
        for (var i = 0; i < n; i++)
            out_[i] = Sector(addr + i)[(addr + i) % 4096];
        return out_;
    }

    private void Reply(params byte[] b)
    {
        foreach (var x in b)
            replies.Enqueue(x);
    }

    private static bool Writable(long addr) => addr >= 0x720000 && addr < 0xF00000;

    public int? Write(byte[] f)
    {
        Requests.Add(f);
        switch (f[0])
        {
            case 0x52:
            {
                var addr = (long)RamMonitor.Be32(f.AsSpan(2));
                var len = f[6] << 8 | f[7];
                if (f[1] == 9)
                {
                    var info = new byte[46];
                    BinaryPrimitives.WriteUInt32LittleEndian(info, 3);
                    BinaryPrimitives.WriteUInt32LittleEndian(info.AsSpan(4), (uint)RadioType);
                    "ddm32plus-b4"u8.CopyTo(info.AsSpan(8));
                    "Nov 21 2026"u8.CopyTo(info.AsSpan(24));
                    BinaryPrimitives.WriteUInt32LittleEndian(info.AsSpan(40), 0xEF4018);
                    Reply([0x52, 0, 46, .. info]);
                    break;
                }
                if (f[1] != 1 || addr + len > Chip || len == 0)
                {
                    Reply(0x2D);
                    break;
                }
                var data = Get(addr, len);
                if (CorruptReadsOf is { } bad && bad >= addr && bad < addr + len && Commits.Contains(bad / 4096 * 4096))
                    data[bad - addr] ^= 0x40;
                Reply([0x52, (byte)(len >> 8), (byte)len, .. data]);
                break;
            }
            case 0x43:
                Reply(0x2D);
                break;
            case 0x58:
                switch (f[1])
                {
                    case 1:
                    {
                        var addr = (long)(f[2] << 16 | f[3] << 8 | f[4]) * 4096;
                        if (pending is not null || !Writable(addr))
                        {
                            pending = null;
                            Reply(0x2D);
                            break;
                        }
                        pending = (byte[])Sector(addr).Clone();
                        pendingAddr = addr;
                        Reply(0x58, 1);
                        break;
                    }
                    case 2:
                    {
                        if (pending is null)
                        {
                            Reply(0x2D);
                            break;
                        }
                        var addr = (long)RamMonitor.Be32(f.AsSpan(2));
                        var len = Math.Min(f[6] << 8 | f[7], f.Length - 8);
                        for (var i = 0; i < len; i++)
                        {
                            if (addr + i >= pendingAddr && addr + i < pendingAddr + 4096)
                                pending[addr + i - pendingAddr] = f[8 + i];
                        }
                        Reply(0x58, 2);
                        break;
                    }
                    case 3:
                        if (pending is null || (RefuseCommitNumber is { } k && Commits.Count + 1 == k))
                        {
                            pending = null;
                            Reply(0x2D);
                            break;
                        }
                        sectors[pendingAddr / 4096] = pending;
                        Commits.Add(pendingAddr);
                        pending = null;
                        Reply(0x58, 3);
                        break;
                    default:
                        Reply(0x2D);
                        break;
                }
                break;
            default:
                Reply(0x2D);
                break;
        }
        return f.Length;
    }

    public byte[] Read(int n)
    {
        var k = Math.Min(n, replies.Count);
        var b = new byte[k];
        for (var i = 0; i < k; i++)
            b[i] = replies.Dequeue();
        return b;
    }
}

public class CpsPackTests
{
    private static byte[] RadioIdImage(int n, uint seq = 0, string name = "RadioID TR")
    {
        var entries = Enumerable.Range(0, n).Select(i => (2860000 + i * 3, $"TA{i % 10}A{(char)('A' + i % 26)}", $"Name{i}"));
        var payload = DataPack.RadioIdPayload(entries);
        return DataPack.Image(DataPack.RadioId, payload, "20261121", name, seq);
    }

    private static byte[] AirImage(uint seq = 0) =>
        DataPack.Image(DataPack.Air, DataPack.AirPayload(new Dictionary<string, List<(int, int)>>
        {
            ["LTAC"] = [(0, 123600), (4, 118100)],
        }), "20261121", "TR aerodromes", seq);

    private static (List<string> Lines, Exception? Error) Run(FakeCpsRadio radio, params string[] argv)
    {
        var lines = new List<string>();
        var ctx = new ToolContext { Print = s => lines.AddRange(s.Split('\n')), Clock = new VirtualClock(), Serial = _ => radio };
        try
        {
            DataPackTool.Run(argv, ctx);
            return (lines, null);
        }
        catch (ToolError e)
        {
            return (lines, e);
        }
    }

    private static string PackFile(TempDir tmp, byte[] image, string name = "p.dpk")
    {
        var p = tmp.Sub(name);
        File.WriteAllBytes(p, image);
        return p;
    }

    private static void OnlyRegionWritten(FakeCpsRadio radio)
    {
        Assert.All(radio.Commits, a => Assert.True(a is >= 0xE70000 and < 0xF00000, $"0x{a:X}"));
        Assert.Equal(radio.Commits.Count, radio.Commits.Distinct().Count());          // each sector once: no retry
        // no reboot, no "save settings", no EEPROM / WAV writes, no W, no D
        Assert.DoesNotContain(radio.Requests, f => f[0] == 0x43 && f[1] == 6 && f.Length > 2 && f[2] != 4);
        Assert.DoesNotContain(radio.Requests, f => f[0] == 0x58 && f[1] is not (1 or 2 or 3));
        Assert.DoesNotContain(radio.Requests, f => f[0] is not (0x52 or 0x43 or 0x58));
    }

    [Fact]
    public void ReadShowsBothSlotsOfEachType()
    {
        var radio = new FakeCpsRadio();
        var img = RadioIdImage(20, 3);
        radio.Put(0xEB0000, img);
        var (lines, err) = Run(radio, "read", "--port", "COM9");
        Assert.Null(err);
        Assert.Contains(lines, l => l.Contains("\"radio_type\": 11", StringComparison.Ordinal));
        var slots = lines.Where(l => l.StartsWith("{\"slot\"", StringComparison.Ordinal)).ToList();
        Assert.Equal(4, slots.Count);
        Assert.Contains("\"in_force\": true", slots[1], StringComparison.Ordinal);
        Assert.Contains("\"count\": 20", slots[1], StringComparison.Ordinal);
        Assert.Contains("\"valid\": false", slots[0], StringComparison.Ordinal);
        Assert.Equal("{\"result\": \"READ\"}", lines[^1]);
        Assert.Empty(radio.Commits);
        Assert.DoesNotContain(radio.Requests, f => f[0] != 0x52);                  // reads only
        Assert.Equal(1, radio.Closes);
    }

    [Fact]
    public void AFirstPackGoesToSlotZeroHeaderLast()
    {
        var radio = new FakeCpsRadio();
        using var tmp = new TempDir();
        var image = RadioIdImage(700);                                              // 10,508 bytes: three sectors
        var (lines, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, image));
        Assert.Null(err);
        Assert.Equal([0xE71000L, 0xE72000, 0xE70000], radio.Commits);
        var target = DataPack.Image(DataPack.RadioId, image[64..], "20261121", "RadioID TR", 1);
        Assert.Equal(target, radio.Get(0xE70000, target.Length));
        Assert.StartsWith("{\"result\": \"PASS\", \"slot\": 0, \"seq\": 1", lines[^1], StringComparison.Ordinal);
        OnlyRegionWritten(radio);
        // the session: opening (no C 6 2), the writes, then C 5
        var controls = radio.Requests.Where(f => f[0] == 0x43).Select(f => f[1]).ToList();
        Assert.Equal([0, 1, 2, 2, 3, 6, 5], controls);
    }

    [Fact]
    public void TheNewPackGoesWhereThePackInForceIsNot()
    {
        var radio = new FakeCpsRadio();
        using var tmp = new TempDir();
        var old = RadioIdImage(30, 4, "old");
        radio.Put(0xE70000, old);
        var (_, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, RadioIdImage(40)));
        Assert.Null(err);
        Assert.All(radio.Commits, a => Assert.True(a >= 0xEB0000 && a < 0xEF0000));
        Assert.Equal(old, radio.Get(0xE70000, old.Length));                         // the old pack untouched
        var (h, _) = DataPack.Parse(radio.Get(0xEB0000, 64 + 8 + 40 * 15));
        Assert.Equal(5u, h!.Seq);

        // slot 1 now in force (seq 5): the next pack goes to slot 0 with seq 6
        radio.Commits.Clear();
        (_, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, RadioIdImage(50), "q.dpk"));
        Assert.Null(err);
        Assert.All(radio.Commits, a => Assert.True(a >= 0xE70000 && a < 0xEB0000));
        Assert.Equal(6u, DataPack.Parse(radio.Get(0xE70000, 64 + 8 + 50 * 15)).Header!.Seq);
        OnlyRegionWritten(radio);
    }

    [Fact]
    public void AnAirPackUsesItsOwnSlots()
    {
        var radio = new FakeCpsRadio();
        using var tmp = new TempDir();
        radio.Put(0xE70000, RadioIdImage(10, 8));                                   // a RadioID pack does not count
        var (lines, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, AirImage()));
        Assert.Null(err);
        Assert.Equal([0xEF0000L], radio.Commits);
        Assert.Contains("\"seq\": 1", lines[^1], StringComparison.Ordinal);
        OnlyRegionWritten(radio);
    }

    [Fact]
    public void ABadReadbackStopsAtOnceAndThePackInForceStays()
    {
        var radio = new FakeCpsRadio { CorruptReadsOf = 0xEB1000 + 77 };
        using var tmp = new TempDir();
        var old = RadioIdImage(30, 4, "old");
        radio.Put(0xE70000, old);
        var (lines, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, RadioIdImage(700)));
        Assert.Equal("VerifyError", Assert.IsType<ToolError>(err).PyType);
        Assert.Equal([0xEB1000L], radio.Commits);                                   // stopped: no other sector, no header
        Assert.Equal(old, radio.Get(0xE70000, old.Length));
        Assert.Equal(0, DataPack.ActiveSlot(DataPack.RadioId, [radio.Get(0xE70000, old.Length), radio.Get(0xEB0000, 4096)]));
        Assert.Equal([0x43, 5], radio.Requests.Last());                            // the session closed (C 5)
        Assert.DoesNotContain(lines, l => l.Contains("PASS", StringComparison.Ordinal));
        OnlyRegionWritten(radio);
    }

    [Fact]
    public void ARefusedCommitIsNeverRetried()
    {
        var radio = new FakeCpsRadio { RefuseCommitNumber = 2 };
        using var tmp = new TempDir();
        var (_, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, RadioIdImage(700)));
        Assert.IsType<CpsError>(err);
        Assert.Equal([0xE71000L], radio.Commits);
        Assert.Equal(2, radio.Requests.Count(f => f[0] == 0x58 && f[1] == 3));
        Assert.Equal(2, radio.Requests.Count(f => f[0] == 0x58 && f[1] == 1));
        OnlyRegionWritten(radio);
    }

    [Fact]
    public void AnotherRadioIsRefusedBeforeAnyWrite()
    {
        var radio = new FakeCpsRadio { RadioType = 6 };
        using var tmp = new TempDir();
        var (_, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, AirImage()));
        Assert.Equal("RadioError", Assert.IsType<ToolError>(err).PyType);
        Assert.Single(radio.Requests);                                              // only the radio info
    }

    [Fact]
    public void ABrokenPackFileOpensNoPort()
    {
        var radio = new FakeCpsRadio();
        using var tmp = new TempDir();
        var img = RadioIdImage(10);
        img[80] ^= 1;
        var (_, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, img));
        Assert.IsType<ValueError>(err);
        Assert.False(radio.Opened);
        (_, err) = Run(radio, "write", "--port", "COM9", "--pack", PackFile(tmp, [.. AirImage(), 0xFF], "x.dpk"));
        Assert.IsType<ValueError>(err);
        Assert.False(radio.Opened);
        Assert.IsType<ToolExit>(Run(radio, "erase", "--port", "COM9").Error);
    }

    private static string Verdict(CpsPackPort g, byte[] f)
    {
        try
        {
            g.Validate(f);
            return "ok";
        }
        catch (ValueError e)
        {
            return e.Message;
        }
    }

    private static byte[] X2(long addr, byte[] data) =>
        [0x58, 2, .. RamMonitor.Be32Bytes(addr), (byte)(data.Length >> 8), (byte)data.Length, .. data];

    private static byte[] X1(long addr) => [0x58, 1, (byte)(addr / 4096 >> 16), (byte)(addr / 4096 >> 8), (byte)(addr / 4096)];

    private static byte[] R1(long addr, int n) => [0x52, 1, .. RamMonitor.Be32Bytes(addr), (byte)(n >> 8), (byte)n];

    [Fact]
    public void TheGuardRefusesWhatItMust()
    {
        var g = new CpsPackPort(new FakeCpsRadio());
        // before the plan: reads inside the region and the radio info only
        Assert.Equal("ok", Verdict(g, R1(0xE70000, 64)));
        Assert.Equal("ok", Verdict(g, R1(0xEFFC00, 1024)));
        Assert.Equal("ok", Verdict(g, [0x52, 9, 0, 0, 0, 0, 0, 0x20]));
        foreach (var f in new[]
                 {
                     R1(0x720000, 32), R1(0xE6FFFF, 2), R1(0xEFFFFF, 2), R1(0xF00000, 1), R1(0xE70000, 4097), R1(0xE70000, 0),
                     [0x52, 5, 0, 0, 0, 0, 0, 32], [0x52, 2, 0, 0, 0, 0, 0, 32], [0x52, 10, 0, 0, 0, 0, 0, 32],
                     X1(0xE70000), X2(0xE70000, new byte[32]), [0x58, 3], [0x43, 0], [0x43, 5],
                     [0x57, 1, 0, 0x0E, 0x70], [0x44, 1, 2, 3], [0xE0, 4, 0, 0], [0x52, 1],
                 })
            Assert.StartsWith("refused", Verdict(g, f));
        var data = Enumerable.Range(0, 4096).Select(i => (byte)i).ToArray();
        var hdr = Enumerable.Range(0, 4096).Select(i => (byte)(i * 3)).ToArray();
        // a plan outside the region or with a sector twice is refused as a whole
        Assert.Throws<ValueError>(() => new CpsPackPort(new FakeCpsRadio()).Arm([new PackSector(0xE6F000, data)]));
        Assert.Throws<ValueError>(() => new CpsPackPort(new FakeCpsRadio()).Arm([new PackSector(0xF00000, data)]));
        Assert.Throws<ValueError>(() => new CpsPackPort(new FakeCpsRadio()).Arm([new PackSector(0xE70800, data)]));
        Assert.Throws<ValueError>(() => new CpsPackPort(new FakeCpsRadio()).Arm([new PackSector(0xE70000, data), new PackSector(0xE70000, data)]));
        Assert.Throws<ValueError>(() => new CpsPackPort(new FakeCpsRadio()).Arm([]));
        g.Arm([new PackSector(0xEB1000, data), new PackSector(0xEB0000, hdr)]);
        Assert.Throws<ValueError>(() => g.Arm([new PackSector(0xEB1000, data)]));      // once
        foreach (var f in new[]
                 {
                     [0x43, 6, 0], [0x43, 6, 1], [0x43, 6, 2], [0x43, 6, 5], [0x43, 6, 7], [0x43, 7], [0x43, 254], [0x43, 0x50],
                     [0x58, 4, 0, 0, 0, 0, 0, 1, 0], [0x58, 7, 0, 0, 0, 0, 0, 1, 0], [0x58, 9],
                     X1(0xEB0000),                       // the header's sector before the payload's
                     X1(0xE70000), X1(0xEF0000), X1(0x720000), X1(0xF00000), X1(0x000000),
                     X2(0xEB1000, data[..32]),            // data with no prepared sector
                     [0x58, 3],
                 })
            Assert.StartsWith("refused", Verdict(g, f));
        foreach (var f in new byte[][] { [0x43, 0], [0x43, 1], [0x43, 2, 0, 0, 1, 1, 0, 0x41, 0], [0x43, 3], [0x43, 6, 4] })
            Assert.Equal("ok", Verdict(g, f));
        Assert.Equal("ok", Verdict(g, X1(0xEB1000)));
        Assert.StartsWith("refused", Verdict(g, X1(0xEB0000)));                      // while one is pending
        Assert.StartsWith("refused", Verdict(g, X2(0xEB0FE0, data[..64])));          // runs out of the sector
        Assert.StartsWith("refused", Verdict(g, X2(0xEB1000, [.. data[..31], 0xAA])));  // other bytes than planned
        Assert.StartsWith("refused", Verdict(g, [0x58, 2, .. RamMonitor.Be32Bytes(0xEB1000), 0, 40, .. data[..32]]));   // length field
        Assert.Equal("ok", Verdict(g, X2(0xEB1000, data[..2048])));
        Assert.StartsWith("refused", Verdict(g, [0x58, 3]));                          // half a sector: no commit
        Assert.Equal("ok", Verdict(g, X2(0xEB1800, data[2048..])));
        Assert.Equal("ok", Verdict(g, [0x58, 3]));
        Assert.StartsWith("refused", Verdict(g, X1(0xEB1000)));                       // twice
        Assert.Equal("ok", Verdict(g, X1(0xEB0000)));
        Assert.Equal("ok", Verdict(g, X2(0xEB0000, hdr)));
        Assert.Equal("ok", Verdict(g, [0x58, 3]));
        Assert.StartsWith("refused", Verdict(g, X1(0xEB2000)));                       // the plan is done
        Assert.Equal(2, g.Committed);
        Assert.Throws<ValueError>(() => g.BaudRate = 921600);
    }

    [Fact]
    public void ARefusedFrameNeverReachesThePort()
    {
        var radio = new FakeCpsRadio();
        var g = new CpsPackPort(radio);
        Assert.Throws<ValueError>(() => g.Write([0x43, 6, 1]));
        Assert.Throws<ValueError>(() => g.Write(X1(0xE70000)));
        Assert.Empty(radio.Requests);
        Assert.Equal(2, g.Refusals.Count);
    }
}
