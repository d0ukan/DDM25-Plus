// SPDX-License-Identifier: GPL-3.0-or-later
using System.Reflection;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// The pinned RAM monitor (RadioFoundry stub/stubtool.py, MIT, LICENSES/RadioFoundry-MIT.txt): the universal stub
/// image the tools load into the radio's RAM through its boot ROM, and the boot ROM's framing. The image is embedded
/// (Radio/Data/ram-monitor.bin, written by make_radio_fixtures.py from the pinned file) and used only when its
/// SHA-256 is the pinned value.
/// </summary>
public static class RamMonitor
{
    public const string Sha256 = "eae5b98615d67ba465061c84a5fb209a902333dc700671ce4ca69a05eb4aaee9";
    public const string UpstreamPin = "dfbaf20c400c73e2ccf6edb80af7d1d255bee418";
    public const string HostToolSha256 = "4e3ad1fa0c85d89e9f4bc15c5d6f97212bbab341ac72d958bdf7d0e250124b9e";

    public static readonly byte[] RomAck = [0x02, 0x05, 0x00, 0x03];
    public static readonly byte[] RomHello = [0x02, 0x24, 0x00, 0x03];
    public static readonly byte[] RomReply = [0x02, 0x14, 0x00, 0x03];
    public static readonly byte[] RomGaveUp = [0x00, 0x10, 0x00, 0x03];
    public static readonly byte[] RomNak = [0x02, 0x0a, 0x00, 0x03];

    private static readonly Lazy<byte[]> Image_ = new(() =>
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("DDM32Plus.Core.Radio.ram-monitor.bin")
                      ?? throw new ValueError("The RAM monitor image is missing from the program");
        var ms = new MemoryStream();
        s.CopyTo(ms);
        var image = ms.ToArray();
        if (Hash.Sha(image) != Sha256)
            throw new ValueError("Pinned RAM monitor hash mismatch");
        return image;
    });

    /// <summary>stub_univ_image(): the pinned universal stub, after its SHA-256 check.</summary>
    public static byte[] Image => (byte[])Image_.Value.Clone();

    public static byte Sum8(ReadOnlySpan<byte> b)
    {
        var s = 0;
        foreach (var x in b)
            s += x;
        return (byte)(s & 0xFF);
    }

    /// <summary>02 op chk 03 payload: the ROM's own framing.</summary>
    public static byte[] RomCmd(byte op, byte[] payload) => [0x02, op, Sum8(payload), 0x03, .. payload];

    /// <summary>02 op len(2) chk 03 payload: the monitor's frames (install_firstboot.packet).</summary>
    public static byte[] Packet(byte op, byte[]? payload = null)
    {
        payload ??= [];
        return [0x02, op, (byte)(payload.Length >> 8), (byte)(payload.Length & 0xFF), Sum8(payload), 0x03, .. payload];
    }

    /// <summary>A program flash SPI transaction (install_firstboot.spi_packet).</summary>
    public static byte[] SpiPacket(byte[] tx, int rx = 0) =>
        Packet(5, [0x04, 0x00, 0x00, 0x00, 0x01, 0x00, (byte)(tx.Length >> 8), (byte)tx.Length, (byte)(rx >> 8), (byte)rx, .. tx]);

    public static uint Be32(ReadOnlySpan<byte> b) => (uint)(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);

    public static byte[] Be32Bytes(long v) => [(byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v];

    public static byte[] Le32Bytes(long v) => [(byte)v, (byte)(v >> 8), (byte)(v >> 16), (byte)(v >> 24)];
}

/// <summary>
/// The backup's transport guard (dm32_readonly_backup.ReadOnlyPort): every frame that goes out is checked first.
/// Before the monitor runs only the ROM greeting reply, the exact pinned image blocks and its start; then only HELLO,
/// the timer read, the DM32 configuration, JEDEC reads, bounded flash reads and sector CRCs / reads inside the
/// identified flash. No write, erase, reboot, GPIO, baud change or RF command exists in it; the baud rate is fixed.
/// </summary>
public class ReadOnlyPort : IRadioPort
{
    protected readonly IRadioPort Raw;
    private readonly HashSet<string> blocks;
    private readonly HashSet<string> sent = [];
    private readonly byte[] go;
    public bool Monitor { get; set; }
    public Dictionary<string, long> Limits { get; set; } = new() { ["progmem"] = 0, ["q128"] = 0 };
    public PyDict Counts { get; } = new();

    public ReadOnlyPort(IRadioPort raw)
    {
        Raw = raw;
        var image = RamMonitor.Image;
        if (Hash.Sha(image) != RamMonitor.Sha256)
            throw new ValueError("RAM image changed");
        var padded = new byte[(image.Length + 255) / 256 * 256];
        Array.Fill(padded, (byte)0xFF);
        image.CopyTo(padded, 0);
        blocks = [];
        for (var i = 0; i < padded.Length; i += 256)
            blocks.Add(Convert.ToHexString(RamMonitor.RomCmd(0x18,
                [.. RamMonitor.Be32Bytes(0x10000 + i), .. RamMonitor.Be32Bytes(64), .. padded.AsSpan(i, 256)])));
        go = RamMonitor.RomCmd(0x1b, RamMonitor.Be32Bytes(0x10000));
    }

    public double Timeout
    {
        get => Raw.Timeout;
        set => Raw.Timeout = value;
    }

    public int BaudRate
    {
        get => Raw.BaudRate;
        set
        {
            if (value != 115200)
                throw new ValueError("Backup transport is fixed at 115200 baud");
            Raw.BaudRate = value;
        }
    }

    public byte[] Read(int n) => Raw.Read(n);
    public void ResetInputBuffer() => Raw.ResetInputBuffer();
    public void Flush() => Raw.Flush();

    public string Memory(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 6)
            throw new ValueError("Short memory selector");
        var (b, flags, cs) = (RamMonitor.Be32(payload), payload[4], payload[5]);
        if (b == 0x04000000 && flags == 1 && cs == 0) return "progmem";
        if (b == 0x140a0000 && flags == 3 && cs == 18) return "q128";
        throw new ValueError("Unsupported SPI controller/chip select");
    }

    private void Bounds(string mem, long address, long length)
    {
        if (length <= 0 || address < 0 || address + length > Limits[mem])
            throw new ValueError("Read outside identified flash capacity");
    }

    public virtual string Validate(byte[] data)
    {
        if (!Monitor)
        {
            var hex = Convert.ToHexString(data);
            if (data.AsSpan().SequenceEqual(RamMonitor.RomReply)) return "rom_greeting";
            if (blocks.Contains(hex))
            {
                sent.Add(hex);
                return "ram_download";
            }
            if (data.AsSpan().SequenceEqual(go) && sent.SetEquals(blocks))
            {
                Monitor = true;
                return "ram_execute";
            }
            throw new ValueError("Only the exact pinned RAM image may be downloaded/executed");
        }
        if (data.Length < 6 || data[0] != 2 || data[5] != 3)
            throw new ValueError("Malformed monitor frame");
        int op = data[1], n = data[2] << 8 | data[3];
        var payload = data.AsSpan(6);
        if (n != payload.Length || data[4] != RamMonitor.Sum8(payload))
            throw new ValueError("Monitor frame length/checksum mismatch");
        if (op == 1 && payload.Length == 0) return "hello";
        if (op == 2 && payload.SequenceEqual(Convert.FromHexString("140000180004"))) return "timer_read";
        if (op == 0x0d && payload.Length == 16)
        {
            uint a = RamMonitor.Be32(payload), b = RamMonitor.Be32(payload[4..]);
            if (a == b && a > 1_000_000 && a < 400_000_000 && payload[8..].SequenceEqual((byte[])[255, 255, 33, 32, 1, 0, 3, 0]))
                return "dm32_config";
        }
        if (op == 5 && payload.Length >= 10)
        {
            var mem = Memory(payload);
            int txLen = payload[6] << 8 | payload[7], rxLen = payload[8] << 8 | payload[9];
            var tx = payload[10..];
            if (txLen == tx.Length)
            {
                if (tx.Length == 1 && tx[0] == 0x9f && rxLen == 3) return "jedec_read";
                if (tx.Length == 4 && tx[0] == 3 && rxLen >= 1 && rxLen <= 1024)
                {
                    Bounds(mem, tx[1] << 16 | tx[2] << 8 | tx[3], rxLen);
                    return "flash_read";
                }
            }
        }
        if ((op == 7 || op == 9) && payload.Length == 12)
        {
            var mem = Memory(payload);
            long address = RamMonitor.Be32(payload[6..]);
            int count = op == 7 ? payload[10] << 8 | payload[11] : payload[10];
            if (address % 4096 == 0 && count >= 1 && count <= 16 && (op == 7 || payload[11] is 0 or 1))
            {
                Bounds(mem, address, count * 4096L);
                return op == 7 ? "sector_crc" : "sector_read";
            }
        }
        throw new ValueError($"Monitor operation 0x{op:x} refused by read-only transport");
    }

    public int? Write(byte[] data)
    {
        var kind = Validate([.. data]);
        var result = Raw.Write(data);
        if (result is not null && result != data.Length)
            throw new ToolError("OSError", "Short serial write");
        Counts[kind] = (Counts.ContainsKey(kind) ? (long)Counts[kind]! : 0L) + 1;
        return result;
    }
}

/// <summary>
/// The installer's guard (install_firstboot.InstallPort): on top of the read-only rules, only the changed
/// application sectors at 0xC000 and up may be erased and programmed, with their exact planned page bytes, in order
/// and once, and only after the live pre-write comparison (or, restoring from the saved backup, after choosing that
/// mode). The boot area and the data chip are read only. The restart registers go out only after the readback
/// matched. In saved-backup mode no original firmware or data is read, and only already written sectors are read back.
/// </summary>
public sealed class InstallPort : ReadOnlyPort
{
    public const int Start = 0xC000, Size = 0x100000, Sector = 4096, Slot = 0xE8000;

    /// <summary>How writes get enabled: Full (the live program and data chip equal the backup: install_firstboot's
    /// own rule) or Identity (the quick update over DDM32 Plus: chip IDs, boot area and codec slot equal the reused
    /// backup's; every application sector is rewritten, the slot never).</summary>
    public enum Authorization { Full, Identity }

    private byte[] original = [], target = [];
    public List<int> Todo { get; } = [];
    public List<byte[]> Plan { get; } = [];
    public int Index { get; private set; }
    public bool Enabled { get; private set; }
    public bool ClosedWrites { get; private set; }
    public List<byte[]> ResetPlan { get; } = [];
    private int resetIndex;
    public bool SkipOriginal { get; }
    public Authorization Kind { get; }
    public bool HasImages { get; private set; }
    private readonly Dictionary<int, int> sectorEnds = [];
    public byte[] Target => target;

    public InstallPort(IRadioPort raw, byte[] original, byte[] target, bool skipOriginal = false,
        Authorization kind = Authorization.Full) : base(raw)
    {
        SkipOriginal = skipOriginal;
        Kind = kind;
        SetImages(original, target);
    }

    /// <summary>An install without a stored backup: the images are set once, after the single live read of the
    /// program flash; until then the port is read only.</summary>
    public InstallPort(IRadioPort raw) : base(raw) => Kind = Authorization.Full;

    /// <summary>Fixes the write plan, once, before any write is enabled.</summary>
    public void SetImages(byte[] original, byte[] target)
    {
        if (HasImages || Enabled || ClosedWrites)
            throw new ValueError("The write plan is fixed once, before any write");
        if (original.Length != Size || target.Length != Size || !original.AsSpan(0, Start).SequenceEqual(target.AsSpan(0, Start)))
            throw new ValueError("Exact 1 MiB images with identical protected boot area required");
        var todo = new List<int>();
        for (var a = Start; a < Size; a += Sector)
            if (!original.AsSpan(a, Sector).SequenceEqual(target.AsSpan(a, Sector)))
                todo.Add(a);
        if (Kind == Authorization.Identity && todo.Any(a => a >= Slot))
            throw new ValueError("A quick update never writes the codec slot");
        this.original = [.. original];
        this.target = [.. target];
        Todo.AddRange(todo);
        foreach (var a in Todo)
        {
            Plan.Add(RamMonitor.SpiPacket([0x06]));
            Plan.Add(RamMonitor.SpiPacket([0x20, (byte)(a >> 16), (byte)(a >> 8), (byte)a]));
            for (var q = a; q < a + Sector; q += 256)
            {
                var page = target.AsSpan(q, 256);
                if (!page.ContainsAnyExcept((byte)0xFF))
                    continue;
                Plan.Add(RamMonitor.SpiPacket([0x06]));
                Plan.Add(RamMonitor.SpiPacket([0x02, (byte)(q >> 16), (byte)(q >> 8), (byte)q, .. page]));
            }
            sectorEnds[a] = Plan.Count;
        }
        HasImages = true;
    }

    /// <summary>The quick update's live check: the boot area and the codec slot read from the radio now equal the
    /// reused backup's (the chip IDs were checked before). Only then are writes enabled.</summary>
    public void AuthorizeIdentity(byte[] liveBoot, byte[] liveSlot)
    {
        if (Kind != Authorization.Identity || !HasImages || Enabled || ClosedWrites ||
            !liveBoot.AsSpan().SequenceEqual(target.AsSpan(0, Start)) || !liveSlot.AsSpan().SequenceEqual(target.AsSpan(Slot)))
            throw new ValueError("Live identity check failed (boot area or codec slot differ from the backup); writes remain disabled");
        Enabled = true;
    }

    private static bool Same(IList<uint> a, IList<uint> b) => a.Count == b.Count && a.SequenceEqual(b);

    public void Authorize(byte[] program, IList<uint> data, IList<uint> expectedData)
    {
        if (Kind != Authorization.Full || !HasImages || Enabled || ClosedWrites || !program.AsSpan().SequenceEqual(original) || !Same(data, expectedData))
            throw new ValueError("Live pre-write comparison failed; writes remain disabled");
        Enabled = true;
    }

    public void AuthorizeSavedBackup()
    {
        if (!SkipOriginal || Enabled || ClosedWrites)
            throw new ValueError("Saved-backup-only installation was not selected");
        Enabled = true;
    }

    public void FinishWrites()
    {
        if (!Enabled || Index != Plan.Count)
            throw new ValueError("Write plan incomplete");
        Enabled = false;
        ClosedWrites = true;
    }

    public void AuthorizeReset(byte[] program, IList<uint> data, IList<uint> expectedData)
    {
        if (!ClosedWrites || !program.AsSpan().SequenceEqual(target) || !Same(data, expectedData))
            throw new ValueError("Post-write verification failed; reset remains disabled");
        AllowReset();
    }

    public void AuthorizeResetApplication(byte[] application)
    {
        if (!SkipOriginal || !ClosedWrites || !application.AsSpan().SequenceEqual(target.AsSpan(Start)))
            throw new ValueError("Application readback failed; reset remains disabled");
        AllowReset();
    }

    private void AllowReset()
    {
        if (ResetPlan.Count > 0)
            throw new ValueError("Restart was already authorized");
        (long A, long V)[] values =
        [
            (MonitorRadio.CrashSoftMark, MonitorRadio.CrashBootSoft), (MonitorRadio.WdtLock, MonitorRadio.WdtKey),
            (MonitorRadio.WdtEn, 0), (MonitorRadio.WdtLock, 0), (MonitorRadio.QuadEnable, 0),
            (MonitorRadio.LcsfcBaudr, 4), (MonitorRadio.SysSoftRstn, 0),
        ];
        foreach (var (a, v) in values)
            ResetPlan.Add(RamMonitor.Packet(3, [.. RamMonitor.Be32Bytes(a), .. RamMonitor.Le32Bytes(v)]));
    }

    public override string Validate(byte[] data)
    {
        if (Monitor)
        {
            if (SkipOriginal)
            {
                int op = data.Length > 1 ? data[1] : -1;
                var payload = data.Length > 6 ? data.AsSpan(6) : [];
                if (op is 7 or 9)
                {
                    if (!ClosedWrites || payload.Length != 12 || Memory(payload) != "progmem" ||
                        RamMonitor.Be32(payload[6..]) < Start)
                        throw new ValueError("Original firmware/data reads are disabled in saved-backup mode");
                }
                if (op == 5 && payload.Length >= 14 && payload[10] == 3)
                {
                    long address = payload[11] << 16 | payload[12] << 8 | payload[13];
                    long n = payload[8] << 8 | payload[9];
                    var first = address / Sector * Sector;
                    var end = (address + n + Sector - 1) / Sector * Sector;
                    var allWritten = true;
                    for (var a = first; a < end; a += Sector)
                        if (!(sectorEnds.TryGetValue((int)a, out var e) && Index >= e))
                            allWritten = false;
                    if (Memory(payload) != "progmem" || address < Start || (!ClosedWrites && !(Enabled && allWritten)))
                        throw new ValueError("Only already-written application bytes may be read back");
                }
            }
            if (data.AsSpan().SequenceEqual(RamMonitor.SpiPacket([0x05], 1)))
                return "program_status_read";
            if (Enabled && Index < Plan.Count && data.AsSpan().SequenceEqual(Plan[Index]))
            {
                Index++;
                return data[16] switch
                {
                    6 => "program_write_enable",
                    0x20 => "program_sector_erase",
                    _ => "program_page",
                };
            }
            if (resetIndex < ResetPlan.Count && data.AsSpan().SequenceEqual(ResetPlan[resetIndex]))
            {
                resetIndex++;
                return "verified_restart_register";
            }
        }
        return base.Validate(data);
    }
}
