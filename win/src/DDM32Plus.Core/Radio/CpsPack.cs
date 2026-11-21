// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using System.Text;
using DDM32Plus.Core.DataPacks;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// The data pack transport guard: every frame of the open-firmware CPS protocol (docs/spec/open-firmware-cps.md 2;
/// docs/plus.md "Loading a pack from the PC") is checked before it goes out. Read-only until <see cref="Arm"/> gives
/// it the plan; then only that plan's sectors, in plan order, each once, with exactly the planned bytes, all inside
/// the data pack region 0xE70000-0xEFFFFF. Allowed:
///   R 9 (radio info); R 1 (flash, physical address) only inside the region, at most 4096 bytes;
///   after Arm: C 0, C 1, C 2 (screen text), C 3, C 6 4 (red LED), C 5 (close: the radio checks its slots);
///   X 1 (prepare the next planned sector), X 2 (its planned bytes, inside it), X 3 (commit, once the whole sector
///   was sent).
/// Refused: everything else: W, X 4 / X 7, C 6 0 / C 6 1 (reboots), C 6 2 (save settings), C 6 5..7, D, MMDVM, an
/// address outside the region, a sector out of order or twice, other bytes than planned. The baud rate is fixed.
/// </summary>
public sealed class CpsPackPort(IRadioPort raw) : IRadioPort
{
    private List<PackSector>? plan;
    private int next;                       // the next planned sector
    private PackSector? pending;
    private bool[] covered = [];
    public bool Armed => plan is not null;
    public int Committed { get; private set; }
    public List<string> Refusals { get; } = [];

    /// <summary>The write plan, once: from now on its sectors (and nothing else) may be written, in its order.</summary>
    public void Arm(IReadOnlyList<PackSector> sectors)
    {
        if (plan is not null)
            throw new ValueError("refused: the pack plan is set once");
        if (sectors.Count == 0)
            throw new ValueError("refused: an empty plan");
        var seen = new HashSet<long>();
        foreach (var s in sectors)
        {
            if (s.Data.Length != DataPack.Sector || s.Address % DataPack.Sector != 0 || !DataPack.InRegion(s.Address, DataPack.Sector))
                throw new ValueError($"refused: sector 0x{s.Address:X} is outside the data pack region 0xE70000-0xEFFFFF");
            if (!seen.Add(s.Address))
                throw new ValueError($"refused: sector 0x{s.Address:X} twice in the plan");
        }
        plan = [.. sectors.Select(s => new PackSector(s.Address, (byte[])s.Data.Clone()))];
    }

    public double Timeout
    {
        get => raw.Timeout;
        set => raw.Timeout = value;
    }

    public int BaudRate
    {
        get => raw.BaudRate;
        set
        {
            if (value != 115200)
                throw new ValueError("The data pack transport is fixed at 115200 baud");
            raw.BaudRate = value;
        }
    }

    public byte[] Read(int n) => raw.Read(n);
    public void ResetInputBuffer() => raw.ResetInputBuffer();
    public void Flush() => raw.Flush();

    public int? Write(byte[] data)
    {
        try
        {
            Validate(data);
        }
        catch (ValueError e)
        {
            Refusals.Add(e.Message);
            pending = null;                 // a refused frame ends the sector: it is never committed
            throw;
        }
        return raw.Write(data);
    }

    private static ValueError Refuse(string why) => new($"refused: {why}");

    /// <summary>Checks one frame; throws ValueError ("refused: ...") for anything outside the rules above.</summary>
    public void Validate(byte[] f)
    {
        if (f.Length < 2)
            throw Refuse("short frame");
        switch (f[0])
        {
            case 0x52:                                      // R
                if (f.Length != 8)
                    throw Refuse("R frame length");
                var addr = RamMonitor.Be32(f.AsSpan(2));
                var len = BinaryPrimitives.ReadUInt16BigEndian(f.AsSpan(6));
                if (f[1] == 9)
                    return;
                if (f[1] != 1)
                    throw Refuse($"R area {f[1]}");
                if (len == 0 || len > DataPack.Sector || !DataPack.InRegion(addr, len))
                    throw Refuse($"R 0x{addr:X} +{len}: outside the data pack region 0xE70000-0xEFFFFF");
                return;
            case 0x43:                                      // C
                if (plan is null)
                    throw Refuse($"C {f[1]} before the plan");
                switch (f[1])
                {
                    case 0 or 1 or 3 or 5 when f.Length == 2:
                        return;
                    case 2 when f.Length is >= 8 and <= 7 + 17:
                        return;
                    case 6 when f.Length == 3 && f[2] == 4:
                        return;
                    default:
                        throw Refuse($"C {f[1]}{(f.Length > 2 ? " " + f[2] : "")}");
                }
            case 0x58:                                      // X
                if (plan is null)
                    throw Refuse($"X {f[1]} before the plan");
                switch (f[1])
                {
                    case 1:
                    {
                        if (f.Length != 5)
                            throw Refuse("X 1 frame length");
                        var a = (long)(f[2] << 16 | f[3] << 8 | f[4]) * DataPack.Sector;
                        if (!DataPack.InRegion(a, DataPack.Sector))
                            throw Refuse($"sector 0x{a:X} is outside the data pack region 0xE70000-0xEFFFFF");
                        if (pending is not null)
                            throw Refuse($"sector 0x{a:X} while 0x{pending.Address:X} is pending");
                        if (next >= plan.Count || plan[next].Address != a)
                            throw Refuse($"sector 0x{a:X} is not the next planned sector");
                        pending = plan[next];
                        covered = new bool[DataPack.Sector];
                        next++;
                        return;
                    }
                    case 2:
                    {
                        if (pending is null)
                            throw Refuse("X 2 without a prepared sector");
                        if (f.Length < 9)
                            throw Refuse("X 2 frame length");
                        var a = RamMonitor.Be32(f.AsSpan(2));
                        int n = BinaryPrimitives.ReadUInt16BigEndian(f.AsSpan(6));
                        if (n == 0 || n != f.Length - 8)
                            throw Refuse("X 2 length field");
                        if (a < pending.Address || a + n > pending.Address + DataPack.Sector || !DataPack.InRegion(a, n))
                            throw Refuse($"X 2 0x{a:X} +{n}: outside the prepared sector");
                        var off = (int)(a - pending.Address);
                        if (!f.AsSpan(8, n).SequenceEqual(pending.Data.AsSpan(off, n)))
                            throw Refuse($"X 2 0x{a:X}: other bytes than planned");
                        Array.Fill(covered, true, off, n);
                        return;
                    }
                    case 3:
                        if (f.Length != 2)
                            throw Refuse("X 3 frame length");
                        if (pending is null)
                            throw Refuse("X 3 without a prepared sector");
                        if (covered.Any(c => !c))
                            throw Refuse($"X 3: sector 0x{pending.Address:X} was not sent whole");
                        pending = null;
                        Committed++;
                        return;
                    default:
                        throw Refuse($"X {f[1]}");
                }
            default:
                throw Refuse($"command 0x{f[0]:X2}");
        }
    }
}

/// <summary>A reply the data pack client did not expect (no answer, '-', a short or wrong reply).</summary>
public sealed class CpsError(string message) : ToolError("CpsError", message);

/// <summary>
/// The open-firmware CPS client for data packs (upstream tools/cps_client.py's Cps, the subset docs/plus.md names):
/// radio info, flash reads at physical addresses, the session opening and close, journaled sector writes (prepare,
/// data in 32-byte blocks as qdmr sends them, commit). A sector write is never repeated: a bad reply ends the run.
/// Reads change nothing and are asked again (twice at most) when a reply is spoiled.
/// </summary>
public sealed class CpsClient(IRadioPort port, IClock clock)
{
    public const int Block = 32;
    public const int ReadChunk = 1024;
    public double ReplyTimeout { get; init; } = 2.0;

    private byte[] ReadExactly(int n, double timeout)
    {
        port.Timeout = timeout;
        var out_ = new List<byte>(n);
        var end = clock.Monotonic() + timeout;
        while (out_.Count < n)
        {
            var got = port.Read(n - out_.Count);
            out_.AddRange(got);
            if (got.Length == 0 && clock.Monotonic() >= end)
                break;
        }
        return [.. out_];
    }

    public void Control(byte sub, params byte[] args)
    {
        port.Write([0x43, sub, .. args]);
        var r = ReadExactly(1, ReplyTimeout);
        if (r is not [0x2D])
            throw new CpsError($"C {sub}: reply {Convert.ToHexString(r)}");
    }

    public void Text(byte y, string s)
    {
        var t = Encoding.ASCII.GetBytes(s);
        port.Write([0x43, 2, 0, y, 1, 1, 0, .. t.AsSpan(0, Math.Min(16, t.Length)), 0]);
        var r = ReadExactly(1, ReplyTimeout);
        if (r is not [0x2D])
            throw new CpsError($"C 2: reply {Convert.ToHexString(r)}");
    }

    /// <summary>The write session's opening without "save settings" (C 6 2): the data chip outside the region is not
    /// touched for a pack.</summary>
    public void Opening(string line)
    {
        Control(0);
        Control(1);
        Text(0, "DDM32 Plus");
        Text(16, line);
        Control(3);
        Control(6, 4);
    }

    public void Close() => Control(5);

    private void Drain()
    {
        clock.Sleep(0.1);
        port.Timeout = 0.1;
        for (var i = 0; i < 64 && port.Read(4096).Length > 0; i++)
        {
        }
    }

    public byte[] Read(byte area, long addr, int n, int retries = 2)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                return ReadOnce(area, addr, n);
            }
            catch (CpsError) when (attempt < retries)
            {
                Drain();
            }
        }
    }

    private byte[] ReadOnce(byte area, long addr, int n)
    {
        port.Write([0x52, area, .. RamMonitor.Be32Bytes(addr), (byte)(n >> 8), (byte)n]);
        var h = ReadExactly(1, ReplyTimeout);
        if (h is not [0x52])
            throw new CpsError($"R {area} 0x{addr:X} {n}: reply {Convert.ToHexString(h)}");
        var l = ReadExactly(2, ReplyTimeout);
        if (l.Length != 2)
            throw new CpsError($"R {area} 0x{addr:X}: short reply");
        var ln = l[0] << 8 | l[1];
        if (area == 1 && ln != n)
            throw new CpsError($"R {area} 0x{addr:X} {n}: reply length {ln}");
        var d = ReadExactly(ln, ReplyTimeout + ln / 5000.0);
        if (d.Length != ln)
            throw new CpsError($"R {area} 0x{addr:X}: short reply {d.Length} of {ln}");
        return d;
    }

    /// <summary>Flash bytes [addr, addr + n) in chunks of 1024.</summary>
    public byte[] ReadFlash(long addr, int n)
    {
        var out_ = new byte[n];
        for (var o = 0; o < n; o += ReadChunk)
        {
            var k = Math.Min(ReadChunk, n - o);
            Read(1, addr + o, k).CopyTo(out_, o);
        }
        return out_;
    }

    /// <summary>The radio info (area 9): struct version, radio type (11 = DM-32UV), git, build, flash ID.</summary>
    public PyDict Info()
    {
        var d = Read(9, 0, 0x20);
        if (d.Length < 44)
            throw new CpsError($"radio info: {d.Length} bytes");
        static string Str(byte[] b, int o, int n)
        {
            var s = b.AsSpan(o, n);
            var z = s.IndexOf((byte)0);
            return PyText.AsciiDecode(z < 0 ? s : s[..z]);
        }
        var info = new PyDict
        {
            ["struct"] = (long)BinaryPrimitives.ReadUInt32LittleEndian(d),
            ["radio_type"] = (long)BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(4)),
            ["git"] = Str(d, 8, 16),
            ["build"] = Str(d, 24, 16),
            ["flash_id"] = (long)BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(40)),
        };
        return info;
    }

    private void Ack(string what, byte sub, double timeout)
    {
        var r = ReadExactly(2, timeout);
        if (r.Length != 2 || r[0] != 0x58 || r[1] != sub)
            throw new CpsError($"{what}: reply {Convert.ToHexString(r)}");
    }

    /// <summary>One journaled sector write: prepare, the 4096 bytes in 32-byte blocks, commit. Never repeated.</summary>
    public void WriteSector(long addr, byte[] data)
    {
        if (data.Length != DataPack.Sector || addr % DataPack.Sector != 0)
            throw new ValueError("a sector is 4096 bytes on a 4096-byte boundary");
        var sector = addr / DataPack.Sector;
        port.Write([0x58, 1, (byte)(sector >> 16), (byte)(sector >> 8), (byte)sector]);
        Ack($"prepare sector 0x{addr:X}", 1, ReplyTimeout);
        for (var o = 0; o < DataPack.Sector; o += Block)
        {
            port.Write([0x58, 2, .. RamMonitor.Be32Bytes(addr + o), (byte)(Block >> 8), (byte)Block, .. data.AsSpan(o, Block)]);
            Ack($"data 0x{addr + o:X}", 2, ReplyTimeout);
        }
        port.Write([0x58, 3]);
        Ack($"commit sector 0x{addr:X}", 3, ReplyTimeout * 3);     // erase, program and journal a sector
    }
}

/// <summary>
/// The data pack tool (like the other radio tools: argv, printed lines, the port only from ctx.Serial):
///   read --port COMx                         the radio info and both slots of each pack type (read only)
///   write --port COMx --pack FILE.dpk        the pack into the slot not in force (docs/plus.md, steps 1-5)
/// Prints JSON objects (PyJson) and progress lines "pack: sector i/n"; raises ToolError on any failure. The radio
/// must be switched on and run DDM32 Plus (a build with data packs); the CPS speaks to the running firmware, not the
/// boot ROM.
/// </summary>
public static class DataPackTool
{
    public const long RadioTypeDm32 = 11;

    public sealed record SlotState(int Type, int Slot, long Address, PackHeader? Header, int? Count, bool InForce);

    private static IRawSerial Open(ToolContext ctx, string portName)
    {
        var raw = ctx.Serial(portName);
        raw.Open();
        raw.BaudRate = 115200;
        raw.ResetInputBuffer();
        return raw;
    }

    /// <summary>A slot's image as far as the checks need it: the header, and the payload when the header is valid
    /// (datapack.inactive_slot / next_seq look at the payload CRC).</summary>
    public static byte[] ReadSlot(CpsClient c, int type, int slot)
    {
        var addr = DataPack.SlotAddress(type, slot);
        var head = c.ReadFlash(addr, DataPack.HeaderSize);
        if (head.AsSpan(0, 4).SequenceEqual(DataPack.Magic) &&
            BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(60)) == Crc32.Compute(head.AsSpan(0, 60)))
        {
            var len = BinaryPrimitives.ReadUInt32LittleEndian(head.AsSpan(12));
            if (len <= DataPack.SlotSize(type) - DataPack.HeaderSize && len > 0)
                return [.. head, .. c.ReadFlash(addr + DataPack.HeaderSize, (int)len)];
        }
        return head;
    }

    public static List<SlotState> Describe(int type, IReadOnlyList<byte[]> images)
    {
        var active = DataPack.ActiveSlot(type, images);
        var out_ = new List<SlotState>();
        for (var i = 0; i < images.Count; i++)
        {
            var (h, payload) = DataPack.Parse(images[i]);
            out_.Add(new SlotState(type, i, DataPack.SlotAddress(type, i), h, h is { CrcOk: true } ? DataPack.Count(h, payload) : null,
                active == i));
        }
        return out_;
    }

    public static PyDict SlotJson(SlotState s) => new()
    {
        ["type"] = s.Type == DataPack.RadioId ? "radioid" : "air",
        ["slot"] = (long)s.Slot,
        ["address"] = PyJson.Hex(s.Address),
        ["valid"] = s.Header is not null,
        ["crc_ok"] = s.Header?.CrcOk ?? false,
        ["in_force"] = s.InForce,
        ["seq"] = s.Header is null ? null : (long)s.Header.Seq,
        ["name"] = s.Header?.Name,
        ["date"] = s.Header?.Date,
        ["bytes"] = s.Header is null ? null : (long)s.Header.Length,
        ["count"] = s.Count is null ? null : (long)s.Count.Value,
        ["tag"] = s.Header?.Tag,
    };

    private static void CheckRadio(PyDict info)
    {
        if (!Equals(info["radio_type"], RadioTypeDm32))
            throw new ToolError("RadioError", $"radio type {info["radio_type"]}: not a DM-32UV running DDM32 Plus");
    }

    public static void Run(IReadOnlyList<string> argv, ToolContext ctx)
    {
        if (argv.Count == 0 || argv[0] is not ("read" or "write"))
            throw new ToolExit("2");
        var a = new Args([.. argv.Skip(1)], ["--port", "--pack", "--line"], [], ["--port"]);
        var portName = a.Get("--port") ?? throw new ToolExit("2");
        byte[]? image = null;
        PackHeader? packHeader = null;
        if (argv[0] == "write")
        {
            var path = a.Get("--pack") ?? throw new ToolExit("2");
            image = File.ReadAllBytes(path);
            (packHeader, _) = DataPack.Parse(image);
            if (packHeader is not { CrcOk: true } || packHeader.Format != 1 ||
                packHeader.Type is not (DataPack.RadioId or DataPack.Air) ||
                image.Length != DataPack.HeaderSize + packHeader.Length)
                throw new ValueError($"{Path.GetFileName(path)}: not a whole data pack");
        }
        var raw = Open(ctx, portName);
        try
        {
            var guard = new CpsPackPort(raw);
            var c = new CpsClient(guard, ctx.Clock);
            var info = c.Info();
            ctx.Print(PyJson.Dumps(new PyDict { ["radio"] = info }, null));
            CheckRadio(info);
            if (argv[0] == "read")
            {
                foreach (var type in new[] { DataPack.RadioId, DataPack.Air })
                {
                    var images = new[] { ReadSlot(c, type, 0), ReadSlot(c, type, 1) };
                    foreach (var s in Describe(type, images))
                        ctx.Print(PyJson.Dumps(new PyDict { ["slot"] = SlotJson(s) }, null));
                }
                ctx.Print(PyJson.Dumps(new PyDict { ["result"] = "READ" }, null));
                return;
            }
            WritePack(c, guard, image!, packHeader!, a.Get("--line") ?? "Data pack", ctx);
        }
        finally
        {
            raw.Close();
        }
    }

    private static void WritePack(CpsClient c, CpsPackPort guard, byte[] image, PackHeader ph, string line, ToolContext ctx)
    {
        var type = ph.Type;
        var before = new[] { ReadSlot(c, type, 0), ReadSlot(c, type, 1) };
        foreach (var s in Describe(type, before))
            ctx.Print(PyJson.Dumps(new PyDict { ["slot"] = SlotJson(s) }, null));
        var slot = DataPack.InactiveSlot(type, before);
        var seq = DataPack.NextSeq(before);
        var payload = image[DataPack.HeaderSize..];
        var target = DataPack.Image(type, payload, ph.Date, ph.Name, seq, ph.Tag);
        var plan = DataPack.PlanForSlot(type, slot, target);
        ctx.Print(PyJson.Dumps(new PyDict
        {
            ["plan"] = new PyDict
            {
                ["type"] = type == DataPack.RadioId ? "radioid" : "air",
                ["slot"] = (long)slot,
                ["address"] = PyJson.Hex(DataPack.SlotAddress(type, slot)),
                ["seq"] = (long)seq,
                ["bytes"] = (long)target.Length,
                ["sectors"] = plan.Select(p => (object?)PyJson.Hex(p.Address)).ToList(),
            },
        }, null));
        guard.Arm(plan);
        c.Opening(line);
        var closed = false;
        try
        {
            for (var i = 0; i < plan.Count; i++)
            {
                var (addr, data) = (plan[i].Address, plan[i].Data);
                c.WriteSector(addr, data);
                var back = c.ReadFlash(addr, DataPack.Sector);
                if (!back.AsSpan().SequenceEqual(data))
                    throw new ToolError("VerifyError", $"sector 0x{addr:X}: read back differs (not written again)");
                ctx.Print($"pack: sector {i + 1}/{plan.Count} 0x{addr:X} written and read back");
            }
            c.Close();
            closed = true;
            var after = new[] { ReadSlot(c, type, 0), ReadSlot(c, type, 1) };
            var states = Describe(type, after);
            foreach (var s in states)
                ctx.Print(PyJson.Dumps(new PyDict { ["slot"] = SlotJson(s) }, null));
            if (!after[slot].AsSpan().SequenceEqual(target) || DataPack.ActiveSlot(type, after) != slot)
                throw new ToolError("VerifyError", "the new pack is not the one in force after the write");
            ctx.Print(PyJson.Dumps(new PyDict
            {
                ["result"] = "PASS", ["slot"] = (long)slot, ["seq"] = (long)seq, ["sectors"] = (long)plan.Count,
                ["name"] = ph.Name, ["date"] = ph.Date,
            }, null));
        }
        finally
        {
            if (!closed)
            {
                try
                {
                    c.Close();          // leave the CPS screen; the pack in force is unchanged (its header was not written)
                }
                catch (Exception e) when (e is ToolError or IOException or TimeoutException or InvalidOperationException)
                {
                }
            }
        }
    }
}
