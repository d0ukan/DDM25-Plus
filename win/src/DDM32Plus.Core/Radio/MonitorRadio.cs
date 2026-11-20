// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// The host side of the pinned RAM monitor (RadioFoundry stub/stubtool.py class Radio, MIT): the part the DDM32 Plus
/// tools use, ported line by line so that the same answers from the radio give the same bytes to the radio. It loads
/// the stub through the boot ROM at power-on, talks the monitor's frames (02 op len chk 03 payload), configures it for
/// the DM-32UV, reads flash (LZ4 / raw / blank sectors, every sector checked against the radio's CRC-32), and sends
/// the generic SPI flash commands the installer's guard allows. It never raises the baud rate (protocol 7's BAUD is
/// not sent here; the guards refuse it) and has no NAND path (the DM-32UV has none).
/// Every byte goes through the IRadioPort it is given: the tools hand it their guard.
/// </summary>
public sealed class MonitorRadio
{
    public const int Block = 256, Chunk = 1024, Sector = 4096, Window = 16, Pack = 16;
    public const long LoadAddr = 0x10000;
    public const long SsiProg = 0x04000000, SsiData = 0x140A0000;
    public const long SysSoftRstn = 0x11000000, QuadEnable = 0x1100005C, LcsfcBaudr = 0x11000030;
    public const long WdtLock = 0x14010000, WdtEn = 0x14010010, WdtKey = 0x5ADA7200;
    public const long CrashSoftMark = 0x18007EF4, CrashBootSoft = 0x54464F53;
    public const long Timer2Value = 0x14000018;
    public const int LedRed = 33, LedGreen = 32;
    public const int Ok = 0x05, BadSum = 0x0A, BadOp = 0x0B, TooLong = 0x0E, Ecc = 0x0D;
    public const int WriteVersion = 7, CfgUartThis = 3;
    private static readonly (long Run, long End) Sram = (0x10000, 0x58000);

    /// <summary>memory: (SSI base, SER, chip select or null, size, name)</summary>
    private static readonly Dictionary<string, (long Base, bool Ser, int? Cs, long Size, string Name)> Mem = new()
    {
        ["progmem"] = (SsiProg, true, null, 0x100000, "program flash"),
        ["q128"] = (SsiData, true, 18, 0x1000000, "data flash"),
    };

    private static readonly Dictionary<string, string> ProtectMem = new() { ["progmem"] = "progmem", ["q128"] = "data" };

    /// <summary>The DM-32UV's board profile (boards/dm32 board.toml [flash.stub] and [[flash.protect]]).</summary>
    public sealed record Board(int Id, string Name, int Latch, int Key, bool Nand, long Progmem,
        IReadOnlyList<(string Mem, long Start, long End, string What)> Protect);

    public static readonly Board Dm32 = new(1, "DM-32UV", 255, 255, false, 1048576,
    [
        ("data", 4096, 823296, "factory calibration: the stock codeplug area's 4 KB page tagged 2, which can be any page of it (hardware-dm32.md 10.1, 11.1)"),
        ("data", 7405568, 7409664, "a decoded calibration copy an earlier firmware left, the fallback when no factory page is found (hardware-dm32.md 10.1, 17)"),
    ]);

    public static readonly Board Generic = new(0, "HR_C7000 radio", 255, 255, false, 0x100000, []);

    private readonly IRadioPort port;
    private readonly Action<string> log;
    private readonly IClock clock;
    public int Retries { get; set; }
    public int Retried { get; private set; }
    public bool Echo { get; private set; }
    public long StubBase { get; private set; } = LoadAddr;
    public PyDict? V6 { get; private set; }
    public bool SectorOp { get; private set; }
    public long Info { get; private set; }
    public Board? Configured { get; private set; }
    public Board Profile { get; set; } = Generic;
    public long ProgmemSize { get; set; } = 0x100000;
    private int linkScore;

    public MonitorRadio(IRadioPort port, Action<string> log, IClock clock, int retries = 6)
    {
        this.port = port;
        this.log = log;
        this.clock = clock;
        Retries = retries;
    }

    private void SetTimeout(double t)
    {
        if (port.Timeout != t)
            port.Timeout = t;
    }

    public byte[] ReadExact(int n, double timeout)
    {
        var end = clock.Monotonic() + timeout;
        var out_ = new List<byte>(n);
        SetTimeout(0.02);
        while (out_.Count < n)
        {
            var chunk = port.Read(n - out_.Count);
            if (chunk.Length > 0)
                out_.AddRange(chunk);
            else if (clock.Monotonic() >= end)
                break;
        }
        return [.. out_];
    }

    private static string HexSpaced(ReadOnlySpan<byte> b) =>
        string.Join(' ', b.ToArray().Select(x => x.ToString("x2", CultureInfo.InvariantCulture)));

    private static bool Contains(byte[] hay, byte[] needle) => hay.AsSpan().IndexOf(needle) >= 0;

    private static byte[] Head(byte[] b, int n) => b[..Math.Min(n, b.Length)];

    // ---- boot ROM: wait for it, load the stub, start it ----------------------------------------------------------
    private (bool Ok, byte[] Got) RomAck(byte[] sent, double timeout)
    {
        var end = clock.Monotonic() + timeout;
        var buf = Array.Empty<byte>();
        while (true)
        {
            if (buf.Length >= 4 && buf.AsSpan(0, 4).SequenceEqual(RamMonitor.RomAck))
                return (true, []);
            var n = Math.Min(buf.Length, sent.Length);
            var echo = n > 0 && buf.AsSpan(0, n).SequenceEqual(sent.AsSpan(0, n));
            if (echo && buf.Length >= sent.Length)
            {
                buf = buf[sent.Length..];
                if (!Echo)
                    log("the cable echoes what the host sends; the echo is ignored");
                Echo = true;
                continue;
            }
            var tail = buf.Length >= 4 ? buf[^4..] : buf;
            if (!echo && tail.AsSpan().SequenceEqual(RamMonitor.RomAck))
            {
                log("before the boot ROM's acknowledgement: " + HexSpaced(Head(buf[..^4], 16)));
                return (true, []);
            }
            if (!echo && (tail.AsSpan().SequenceEqual(RamMonitor.RomNak) || Contains(buf, RamMonitor.RomGaveUp)))
                return (false, Head(buf, 16));
            if (clock.Monotonic() >= end)
                return (false, Head(buf, 16));
            var want = echo ? sent.Length - buf.Length : 4 - buf.Length;
            buf = [.. buf, .. ReadExact(Math.Max(1, want), Math.Max(0.0, Math.Min(0.05, end - clock.Monotonic())))];
        }
    }

    private DumpError RomError(string what, byte[] got)
    {
        var bytes_ = got.Length > 0 ? "received " + HexSpaced(got) : "nothing received";
        return new DumpError($"{what} ({bytes_}{(Echo ? "; the cable echoes" : "")}). The radio's boot ROM did not take the " +
                             "loader over the programming cable. Try a cable with an FTDI, CP210x or PL2303 chip, " +
                             "--slow-cable, or use the stock firmware file instead of reading the radio");
    }

    private static DumpError RomLate(byte[] got) =>
        new($"the radio's boot ROM stopped waiting before the answer to its greeting reached it (received {HexSpaced(Head(got, 16))}). " +
            "The cable or a USB hub carries it too slowly: run again with --slow-cable");

    /// <summary>Waits for the boot ROM's greeting (not the --slow-cable way: that is never used here).</summary>
    private bool WaitForRadio(double wait)
    {
        var end = clock.Monotonic() + wait;
        var last = Array.Empty<byte>();
        SetTimeout(0.1);
        while (clock.Monotonic() < end)
        {
            var b = port.Read(1);
            if (b.Length == 0)
                continue;
            last = [.. last, .. b];
            if (last.Length > 4)
                last = last[^4..];
            if (last.AsSpan().SequenceEqual(RamMonitor.RomHello))
                return false;
            if (last.AsSpan().SequenceEqual(RamMonitor.RomGaveUp))
                throw new DumpError("the radio started its firmware: its boot ROM's greeting did not come through. Run again with --slow-cable");
        }
        throw new DumpError($"the radio did not start its serial boot ROM within {(int)wait} s: radio off and cable in " +
                            "before the script starts, then switch it on");
    }

    /// <summary>Boots the universal stub through the serial boot ROM at power-on, loaded at 0x10000.</summary>
    public void Start(int wait = 120)
    {
        var stub = RamMonitor.Image;
        CheckUnivLoad(stub, LoadAddr);
        var padded = new byte[(stub.Length + Block - 1) / Block * Block];
        Array.Fill(padded, (byte)0xFF);
        stub.CopyTo(padded, 0);
        stub = padded;
        StubBase = LoadAddr;
        var answered = WaitForRadio(wait);
        if (!answered)
            port.Write(RamMonitor.RomReply);
        clock.Sleep(answered ? 0.2 : 0.05);
        var reply = ReadExact(256, 0.02);
        if (Contains(reply, RamMonitor.RomGaveUp))
            throw RomLate(reply);
        if (Contains(reply, RamMonitor.RomReply))
        {
            Echo = true;
            log("the cable echoes what the host sends; the echo is ignored");
        }
        if (reply.Length > 0)
            log("after the boot ROM greeting: " + HexSpaced(Head(reply, 16)));
        var blocks = stub.Length / Block;
        log($"boot ROM answered; loading {stub.Length}-byte stub ({blocks} blocks)");
        for (var i = 0; i < blocks; i++)
        {
            byte[] payload = [.. RamMonitor.Be32Bytes(LoadAddr + i * Block), 0, 0, 0, 0x40, .. stub.AsSpan(i * Block, Block)];
            var got = Array.Empty<byte>();
            var ok = false;
            for (var attempt = 0; attempt < 3; attempt++)
            {
                port.ResetInputBuffer();
                var frame = RamMonitor.RomCmd(0x18, payload);
                port.Write(frame);
                (ok, got) = RomAck(frame, 1.0);
                if (ok)
                    break;
                if (Contains(got, RamMonitor.RomGaveUp))
                    throw RomLate(got);
                log($"stub block {i} not acknowledged ({(got.Length > 0 ? "received " + HexSpaced(got) : "nothing received")}), sending it again");
            }
            if (!ok)
                throw RomError($"stub block {i} was not acknowledged", got);
        }
        port.ResetInputBuffer();
        var go = RamMonitor.RomCmd(0x1B, RamMonitor.Be32Bytes(LoadAddr));
        port.Write(go);
        var (goOk, goGot) = RomAck(go, 1.0);
        if (!goOk)
            throw RomError("the start command was not acknowledged", goGot);
        var endT = clock.Monotonic() + 3;
        (int Version, int MaxPayload) info;
        while (true)
        {
            try
            {
                info = Hello();
                break;
            }
            catch (DumpError)
            {
                if (clock.Monotonic() > endT)
                    throw new DumpError("the stub was started but does not answer");
            }
        }
        log($"stub v{info.Version} running (max payload {info.MaxPayload}, SSI burst {Info >> 8 & 0xFF} / {Info & 0xFF} bytes)");
    }

    private static void CheckUnivLoad(byte[] image, long load)
    {
        if (image.Length < 0xC0 || !image.AsSpan(0x80, 4).SequenceEqual("UCFG"u8))
            throw new DumpError("this is not a universal stub image (no parameter block at offset 0x80)");
        long run = BitConverter.ToUInt32(image, 0xB8), end = BitConverter.ToUInt32(image, 0xBC);
        long n = image.Length;
        if (!(Sram.Run <= run && run + n <= end && end <= Sram.End))
            throw new DumpError($"the stub's RAM 0x{run:X}-0x{end:X} is not inside SRAM");
        if (load % 4 != 0 || !(Sram.Run <= load && load + n <= Sram.End))
            throw new DumpError($"load address 0x{load:X}: the {n}-byte stub must land 4-byte aligned inside SRAM");
        if (load != run && load < run + n && run < load + n)
            throw new DumpError($"load address 0x{load:X} overlaps the stub's run address 0x{run:X}");
    }

    // ---- stub frames ---------------------------------------------------------------------------------------------
    private int Rate() => port.BaudRate != 0 ? port.BaudRate : 115200;

    private (int? Status, byte[] Body, string Why) Exchange(int op, byte[] payload, double timeout = 1.0)
    {
        byte[] frame = [0x02, (byte)op, (byte)(payload.Length >> 8), (byte)(payload.Length & 0xFF), RamMonitor.Sum8(payload), 0x03, .. payload];
        port.ResetInputBuffer();
        port.Write(frame);
        var hdr = ReadExact(6, timeout + frame.Length * 10.0 / Rate());
        if (hdr.Length != 6 || hdr[0] != 0x02 || hdr[5] != 0x03)
            return (null, [], "no or malformed reply header");
        var n = hdr[2] << 8 | hdr[3];
        var body = ReadExact(n, 0.5 + n * 0.0002);
        if (body.Length != n)
            return (null, [], $"reply short ({body.Length} of {n} bytes)");
        if (RamMonitor.Sum8(body) != hdr[4])
            return (null, [], "reply checksum");
        if (hdr[1] == BadSum)
            return (null, [], "the radio saw a bad request checksum");
        return (hdr[1], body, "");
    }

    public byte[] Request(int op, byte[]? payload = null, int? retries = null, double timeout = 1.0)
    {
        payload ??= [];
        var r = retries ?? Retries;
        var why = "";
        for (var attempt = 0; attempt < r + 1; attempt++)
        {
            var (status, body, w) = Exchange(op, payload, timeout);
            if (status == Ok)
            {
                linkScore = Math.Max(0, linkScore - 1);
                return body;
            }
            if (status is not null)
                throw new StatusError($"stub op 0x{op:X2} refused: status 0x{status:X2}", status.Value);
            why = w;
            Retried++;
            if (attempt < r)
            {
                log($"op 0x{op:X2}: {why}, again ({attempt + 1} of {r})");
                clock.Sleep(0.02);
            }
        }
        throw new DumpError($"op 0x{op:X2}: no clean reply after {r + 1} tries ({why})");
    }

    public (int Version, int MaxPayload) Hello(int retries = 0)
    {
        var r = Request(0x01, retries: retries);
        if (r.Length < 6 || !r.AsSpan(0, 6).SequenceEqual("C7STUB"u8))
            throw new DumpError("unexpected hello " + PyBytesRepr(r));
        Info = r.Length >= 17 ? RamMonitor.Be32(r.AsSpan(13)) : 0;
        SectorOp = r[6] >= WriteVersion;
        if (r.Length >= 43 && r[6] >= 6)
        {
            long G(int o) => RamMonitor.Be32(r.AsSpan(o));
            V6 = new PyDict
            {
                ["load"] = G(9), ["board"] = (long)r[17], ["flags"] = (long)r[18], ["run"] = G(19), ["image"] = G(23),
                ["bss_end"] = G(27), ["stack_lo"] = G(31), ["stack_top"] = G(35), ["stack_used"] = G(39),
            };
            StubBase = (long)V6["run"]!;
        }
        else
            V6 = null;
        return (r[6], r[7] << 8 | r[8]);
    }

    private static string PyBytesRepr(byte[] b)
    {
        var sb = new System.Text.StringBuilder("b'");
        foreach (var x in b)
        {
            if (x == (byte)'\\') sb.Append("\\\\");
            else if (x == (byte)'\'') sb.Append("\\'");
            else if (x == (byte)'\t') sb.Append("\\t");
            else if (x == (byte)'\n') sb.Append("\\n");
            else if (x == (byte)'\r') sb.Append("\\r");
            else if (x < 0x20 || x >= 0x7f) sb.Append("\\x").Append(x.ToString("x2", CultureInfo.InvariantCulture));
            else sb.Append((char)x);
        }
        return sb.Append('\'').ToString();
    }

    private long V(string k) => (long)V6![k]!;

    public string Footprint() =>
        $"loaded at 0x{V("load"):X}, runs at 0x{V("run"):X}-0x{V("stack_top"):X}: image {V("image")} B, .bss to 0x{V("bss_end"):X}, " +
        $"stack {V("stack_top") - V("stack_lo")} B with {V("stack_used")} B used at most";

    /// <summary>CONFIG (op 0x0D) for the board, checked by the HELLO echo; writes are refused until it succeeded.</summary>
    public void Configure(Board board)
    {
        if (V6 is null)
        {
            log("stub protocol before 6: no board configuration");
            Profile = board;
            return;
        }
        var hz = TimerHz(0.3);
        if (!(1e6 < hz && hz < 4e8))
            throw new DumpError($"the stub's timer runs at {hz:F0} Hz, which can't be right: not configuring it");
        var hzi = (long)(Math.Round(hz / 1e6, MidpointRounding.ToEven) * 1e6);
        byte[] payload = [.. RamMonitor.Be32Bytes(hzi), .. RamMonitor.Be32Bytes(hzi),
            (byte)board.Latch, (byte)board.Key, LedRed, LedGreen, (byte)board.Id, (byte)(board.Nand ? 1 : 0), CfgUartThis, 0];
        Request(0x0D, payload);
        Hello(Retries);
        if (V6 is null || V("board") != board.Id || V("flags") != (board.Nand ? 1 : 0))
            throw new DumpError($"the stub did not take the {board.Name} configuration (HELLO says board " +
                                $"{(V6 is null ? "None" : V("board"))}, flags {(V6 is null ? "None" : V("flags"))}): nothing written");
        Configured = board;
        Profile = board;
        log($"stub configured for the {board.Name} (clock {(hzi / 1e6).ToString("F1", CultureInfo.InvariantCulture)} MHz); {Footprint()}");
    }

    public void CheckWritable()
    {
        if (V6 is not null && Configured is null)
            throw new DumpError("the stub's board configuration is not confirmed: refusing to write");
    }

    public long MemSize(string mem) => mem == "progmem" ? (V6 is not null ? ProgmemSize : Mem[mem].Size) : Mem[mem].Size;

    private static int MemFlags(string mem)
    {
        var (_, ser, cs, _, _) = Mem[mem];
        return (ser ? 1 : 0) | (cs is not null ? 2 : 0);
    }

    private void SendNoreply(int op, byte[] payload)
    {
        port.ResetInputBuffer();
        byte[] frame = [0x02, (byte)op, (byte)(payload.Length >> 8), (byte)(payload.Length & 0xFF), RamMonitor.Sum8(payload), 0x03, .. payload];
        port.Write(frame);
        port.Flush();
        clock.Sleep(0.05 + frame.Length * 10.0 / Rate());
    }

    /// <summary>Soft-resets the SoC (restart-cause mark, watchdog off, quad off, divider, SYS_SOFT_RSTN).</summary>
    public void ResetChip()
    {
        Poke(CrashSoftMark, RamMonitor.Le32Bytes(CrashBootSoft));
        Poke(WdtLock, RamMonitor.Le32Bytes(WdtKey));
        Poke(WdtEn, RamMonitor.Le32Bytes(0));
        Poke(WdtLock, RamMonitor.Le32Bytes(0));
        Poke(QuadEnable, RamMonitor.Le32Bytes(0));
        Poke(LcsfcBaudr, RamMonitor.Le32Bytes(4));
        SendNoreply(0x03, [.. RamMonitor.Be32Bytes(SysSoftRstn), .. RamMonitor.Le32Bytes(0)]);
    }

    public byte[] Peek(long addr, int n) => Request(0x02, [.. RamMonitor.Be32Bytes(addr), (byte)(n >> 8), (byte)n]);

    public void Poke(long addr, byte[] data) => Request(0x03, [.. RamMonitor.Be32Bytes(addr), .. data]);

    public byte[] Spi(long b, byte[] tx, int rxLen, bool ser = true, int? cs = null, int? retries = null)
    {
        var flags = (ser ? 1 : 0) | (cs is not null ? 2 : 0);
        byte[] payload = [.. RamMonitor.Be32Bytes(b), (byte)flags, (byte)(cs ?? 0), (byte)(tx.Length >> 8), (byte)tx.Length,
            (byte)(rxLen >> 8), (byte)rxLen, .. tx];
        var r = Request(0x05, payload, retries);
        if (r.Length != rxLen)
            throw new DumpError($"SPI reply is {r.Length} bytes, wanted {rxLen}");
        return r;
    }

    public byte[] Jedec(string mem)
    {
        var (b, ser, cs, _, _) = Mem[mem];
        return Spi(b, [0x9F], 3, ser, cs);
    }

    public byte[] ReadFlash(string mem, long addr, int n)
    {
        var (b, ser, cs, _, _) = Mem[mem];
        return Spi(b, [0x03, (byte)(addr >> 16), (byte)(addr >> 8), (byte)addr], n, ser, cs);
    }

    private int Status(string mem)
    {
        var (b, ser, cs, _, _) = Mem[mem];
        return Spi(b, [0x05], 1, ser, cs)[0];
    }

    private int WaitReady(string mem, string what, double timeout)
    {
        var end = clock.Monotonic() + timeout;
        while (true)
        {
            var st = Status(mem);
            if ((st & 1) == 0)
                return st;
            if (clock.Monotonic() > end)
                throw new DumpError($"{what}: flash still busy after {timeout.ToString("F1", CultureInfo.InvariantCulture)} s (status 0x{st:X2})");
        }
    }

    /// <summary>Write enable, the command, busy polls (generic SPI: 06, 05, 20 / 02).</summary>
    public void WriteCmd(string mem, byte[] tx, string what, double timeout)
    {
        Guard(mem, tx);
        var (b, ser, cs, _, _) = Mem[mem];
        Spi(b, [0x06], 0, ser, cs);
        if ((Status(mem) & 2) == 0)
            throw new DumpError($"{what}: write enable did not stick (status 0x{Status(mem):X2}): chip write-protected?");
        Spi(b, tx, 0, ser, cs);
        WaitReady(mem, what, timeout);
    }

    /// <summary>The last check before a flash command goes out: never a protected region of the board.</summary>
    private void Guard(string mem, byte[] tx)
    {
        int op = tx[0];
        var pm = ProtectMem[mem];
        if (op is 0x60 or 0xC7)
        {
            var hit = Profile.Protect.Where(p => p.Mem == pm).ToList();
            if (hit.Count > 0)
                throw new DumpError($"refusing to erase the whole {ProtectName(pm)}: it holds protected regions ({Describe(hit)}). Nothing was erased");
        }
        else if (op is 0x20 or 0x52 or 0xD8 or 0x02 && tx.Length >= 4)
        {
            long a = tx[1] << 16 | tx[2] << 8 | tx[3];
            long n = op switch { 0x20 => Sector, 0x52 => 0x8000, 0xD8 => 0x10000, _ => tx.Length - 4 };
            if (op != 0x02)
                a -= a % n;
            long start = a, end = a + Math.Max(n, 1);
            var hit = Profile.Protect.Where(p => p.Mem == pm && p.Start < end && start < p.End).ToList();
            if (hit.Count > 0)
                throw new DumpError($"refusing to {(op != 0x02 ? "erase" : "program")} {ProtectName(pm)} 0x{start:X}-0x{end:X}: it overlaps a protected region ({Describe(hit)}). Nothing was written");
        }
    }

    private static string ProtectName(string pm) => pm switch
    {
        "progmem" => "program flash",
        "data" => "data flash",
        _ => pm,
    };

    private static string Describe(IEnumerable<(string Mem, long Start, long End, string What)> hit) =>
        string.Join("; ", hit.Select(p => $"{ProtectName(p.Mem)} 0x{p.Start:X}-0x{p.End:X}, {p.What}"));

    /// <summary>[(crc32, blank)] for count sectors from addr, computed by the radio.</summary>
    public List<(uint Crc, bool Blank)> Sums(string mem, long addr, int count)
    {
        var (b, _, cs, _, _) = Mem[mem];
        byte[] payload = [.. RamMonitor.Be32Bytes(b), (byte)MemFlags(mem), (byte)(cs ?? 0), .. RamMonitor.Be32Bytes(addr),
            (byte)(count >> 8), (byte)count];
        var r = Request(0x07, payload, timeout: 1.0 + count * 0.15);
        if (r.Length != count * 5)
            throw new DumpError($"sector sums reply is {r.Length} bytes, wanted {count * 5}");
        return [.. Enumerable.Range(0, count).Select(i => (RamMonitor.Be32(r.AsSpan(i * 5)), r[i * 5 + 4] == 1))];
    }

    /// <summary>Tick rate of timer 2 (it counts down), against this computer's clock.</summary>
    public double TimerHz(double span = 0.5)
    {
        var t0 = clock.Monotonic();
        long v0 = BitConverter.ToUInt32(Peek(Timer2Value, 4));
        clock.Sleep(span);
        var t1 = clock.Monotonic();
        long v1 = BitConverter.ToUInt32(Peek(Timer2Value, 4));
        return ((v0 - v1) & 0xFFFFFFFF) / (t1 - t0);
    }

    private (int Status, byte[] Body) ReadFrame(double timeout)
    {
        var hdr = ReadExact(6, timeout);
        if (hdr.Length != 6 || hdr[0] != 0x02 || hdr[5] != 0x03)
            throw new FrameError("no or malformed frame header");
        var n = hdr[2] << 8 | hdr[3];
        var body = ReadExact(n, 0.5 + n * 0.0002);
        if (body.Length != n)
            throw new FrameError($"frame short ({body.Length} of {n} bytes)");
        if (RamMonitor.Sum8(body) != hdr[4])
            throw new FrameError("frame checksum");
        return (hdr[1], body);
    }

    /// <summary>Reads and drops what the stub still streams, until the line was quiet for `quiet` seconds.</summary>
    public void Drain(double quiet = 0.4)
    {
        SetTimeout(0.05);
        var last = clock.Monotonic();
        while (clock.Monotonic() - last < quiet)
            if (port.Read(4096).Length > 0)
                last = clock.Monotonic();
        port.ResetInputBuffer();
    }

    /// <summary>(crc32, method, data) for count sectors from addr; method 1 LZ4, 0 raw, 2 blank.</summary>
    private IEnumerable<(uint Crc, int Method, byte[] Data)> Sectors(string mem, long addr, int count, bool compress = true)
    {
        var (b, _, cs, _, _) = Mem[mem];
        byte[] payload = [.. RamMonitor.Be32Bytes(b), (byte)MemFlags(mem), (byte)(cs ?? 0), .. RamMonitor.Be32Bytes(addr),
            (byte)count, (byte)(compress ? 1 : 0)];
        port.ResetInputBuffer();
        port.Write([0x02, 0x09, 0, (byte)payload.Length, RamMonitor.Sum8(payload), 0x03, .. payload]);
        for (var i = 0; i < count; i++)
        {
            var (status, r) = ReadFrame(i == 0 ? 3.0 : 1.5);
            if (status != Ok)
            {
                if (status is BadOp or TooLong)
                    throw new DumpError($"stub op 0x09 refused: status 0x{status:X2}");
                throw new FrameError($"stub op 0x09: status 0x{status:X2}");
            }
            if (r.Length < 7 || r.Length != 7 + (r[5] << 8 | r[6]))
                throw new FrameError("sector record malformed");
            yield return (RamMonitor.Be32(r), r[4], r[7..]);
        }
    }

    /// <summary>The sector's 4096 bytes when they match the radio's CRC-32, else null.</summary>
    private static byte[]? Unpack(uint crc, int method, byte[] data)
    {
        byte[] sec;
        try
        {
            sec = method switch
            {
                2 => Enumerable.Repeat((byte)0xFF, Sector).ToArray(),
                1 => Lz4.Block(data, Sector),
                _ => data,
            };
        }
        catch (Exception e) when (e is DumpError or IndexOutOfRangeException or ArgumentOutOfRangeException)
        {
            return null;
        }
        return sec.Length == Sector && Crc32.Compute(sec) == crc ? sec : null;
    }

    public void CheckId(string mem)
    {
        var id = Jedec(mem);
        log($"{Mem[mem].Name} JEDEC ID {Convert.ToHexStringLower(id)}");
        if (id.All(x => x == 0xFF) || id.All(x => x == 0))
            throw new DumpError($"{Mem[mem].Name} does not answer the ID command ({Convert.ToHexStringLower(id)}): chip select or pin mux wrong, nothing was dumped");
    }

    private IEnumerable<(long W, int Count)> Windows(string mem, long start, long end)
    {
        var size = MemSize(mem);
        if (!(0 <= start && start < end && end <= size))
            throw new DumpError($"{mem} range must lie within 0..0x{size:X}");
        for (var w = start / Sector * Sector; w < end; w += Window * Sector)
            yield return (w, (int)Math.Min(Window, (end - w + Sector - 1) / Sector));
    }

    private static void Put(Stream out_, byte[] full, long lo, long start, long end, long sa, byte[] sec)
    {
        sec.CopyTo(full, sa - lo);
        long a = Math.Max(start, sa), b = Math.Min(end, sa + Sector);
        out_.Seek(a - start, SeekOrigin.Begin);
        out_.Write(sec, (int)(a - sa), (int)(b - a));
    }

    /// <summary>Reads [start, end) of mem into out: every sector checked against the radio's CRC-32, read again when
    /// it does not match (uncompressed from the second try), and checked once more against the sector sums at the
    /// end.</summary>
    public void Dump(Stream out_, string mem, long start, long end, Action<long, long>? progress = null, bool compress = true)
    {
        CheckId(mem);
        var size = MemSize(mem);
        if (!(0 <= start && start < end && end <= size))
            throw new DumpError($"{mem} range must lie within 0..0x{size:X}");
        out_.Seek(0, SeekOrigin.Begin);
        out_.SetLength(end - start);
        long total = 0, fails = 0, skipped = 0;
        var sa = start / Sector * Sector;
        var lo = sa;
        var full = new byte[(end + Sector - 1) / Sector * Sector - lo];
        var want = compress;
        while (sa < end)
        {
            var count = (int)Math.Min(Pack, (end - sa + Sector - 1) / Sector);
            try
            {
                foreach (var (crc, method, data) in Sectors(mem, sa, count, want))
                {
                    var sec = Unpack(crc, method, data) ?? throw new FrameError($"0x{sa:X}: sector does not match the radio's CRC");
                    if (method == 2)
                        skipped++;
                    Put(out_, full, lo, start, end, sa, sec);
                    sa += Sector;
                    total++;
                    fails = 0;
                    linkScore = Math.Max(0, linkScore - 1);
                    want = compress;
                    if (progress is not null && total % 64 == 0)
                        progress(sa, end);
                }
            }
            catch (FrameError e)
            {
                fails++;
                Retried++;
                Drain();
                if (fails > Retries)
                    throw new DumpError($"0x{sa:X}: no clean read after {fails} tries ({e.Message})");
                log($"0x{sa:X}: {e.Message}, resuming here ({fails} of {Retries})");
                want = compress && fails < 2;
            }
        }
        for (var attempt = 0; attempt < Retries + 1; attempt++)
        {
            var bad = Verify(mem, full, lo);
            if (bad.Count == 0)
                break;
            if (attempt == Retries)
                throw new DumpError($"0x{bad[0]:X}: the read does not match the radio's sector CRC after {attempt + 1} tries");
            log($"{bad.Count} sector(s) read wrong (first 0x{bad[0]:X}), reading them again");
            Retried += bad.Count;
            foreach (var s in bad)
            {
                try
                {
                    foreach (var (crc, method, data) in Sectors(mem, s, 1, false))
                    {
                        var sec = Unpack(crc, method, data);
                        if (sec is not null)
                            Put(out_, full, lo, start, end, s, sec);
                    }
                }
                catch (FrameError e)
                {
                    log($"0x{s:X}: {e.Message}");
                    Drain();
                }
            }
        }
        out_.Flush();
        if (total == skipped)
            log("warning: every sector was blank (0xFF)");
    }

    /// <summary>Sector addresses where the flash differs from data, by the radio's CRC-32s.</summary>
    public List<long> Verify(string mem, byte[] data, long start = 0)
    {
        if (start % Sector != 0 || data.Length % Sector != 0)
            throw new DumpError("verify needs whole 4 KB sectors");
        CheckId(mem);
        var bad = new List<long>();
        foreach (var (w, count) in Windows(mem, start, start + data.Length))
        {
            var sums = Sums(mem, w, count);
            for (var i = 0; i < sums.Count; i++)
            {
                var off = w + i * Sector - start;
                if (Crc32.Compute(data.AsSpan((int)off, Sector)) != sums[i].Crc)
                    bad.Add(w + i * Sector);
            }
        }
        return bad;
    }
}
