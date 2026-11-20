// SPDX-License-Identifier: GPL-3.0-or-later
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// A simulated DM-32UV for the C# tools: the part of the pinned monitor's own FakeRadio (RadioFoundry
/// stub/stubtool.py, with pc/tests/rtfixtures.fake_radio's chip ID and corruption switches) the tools use: the boot
/// ROM load, HELLO (universal stub), CONFIG, peek / poke, SPI (JEDEC, read, write enable, status, sector erase, page
/// program), sector CRCs and sector reads (sent raw or blank). Its behaviour is checked against the Python FakeRadio:
/// the C# tools must send it exactly the bytes the Python tools sent to the Python one (FakeRadioTests). No serial
/// port: everything is in memory.
/// </summary>
internal sealed class FakeRadio : IRawSerial
{
    private const int Ok = 0x05, BadSum = 0x0A, BadOp = 0x0B, Sector = 4096;
    private readonly IClock clock;
    public Dictionary<string, byte[]> Images { get; }
    public List<(string Mem, int Op, long Addr)> Writes { get; } = [];
    public List<(long Addr, long Value)> Pokes { get; } = [];
    public List<int> Ops { get; } = [];
    public List<byte[]> Sent { get; } = [];
    public List<string> Errors { get; } = [];
    public bool Opened { get; private set; }
    public bool Closed { get; private set; }
    public string DataJedec { get; set; } = "ef4018";
    public long? CorruptAfterWrites { get; set; }
    public bool Silent { get; set; }                     // the radio was not switched on: no boot ROM greeting
    private readonly List<byte> out_ = [], inb = [];
    private readonly List<byte> loaded = [];
    private bool running, romTaken, wel;
    private readonly int stubLen;
    private readonly long run, end;
    private long board, flags;
    private readonly Dictionary<long, long> mem = new() { [0x14020000] = 0, [0x11000038] = 0xFFFFFE07 };

    public FakeRadio(byte[] program, byte[] data, IClock clock)
    {
        this.clock = clock;
        Images = new() { ["progmem"] = [.. program], ["q128"] = [.. data] };
        var image = RamMonitor.Image;
        stubLen = (image.Length + 255) / 256 * 256;
        run = 0x30000;
        end = BitConverter.ToUInt32(image, 0xBC);
        out_.AddRange(RamMonitor.RomHello);
    }

    public double Timeout { get; set; } = 0.02;
    public int BaudRate { get; set; } = 115200;

    public void Open()
    {
        Opened = true;
        if (Silent)
            out_.Clear();
    }

    public void Close() => Closed = true;
    public void Flush() { }
    public void ResetInputBuffer() => out_.Clear();

    public byte[] Read(int n)
    {
        var k = Math.Min(n, out_.Count);
        var r = out_.GetRange(0, k).ToArray();
        out_.RemoveRange(0, k);
        return r;
    }

    public int? Write(byte[] b)
    {
        Sent.Add([.. b]);
        if (Silent)
            return null;
        inb.AddRange(b);
        while (Step())
        {
        }
        return null;
    }

    private static byte[] Frame(int status, byte[] body) =>
        [0x02, (byte)status, (byte)(body.Length >> 8), (byte)body.Length, RamMonitor.Sum8(body), 0x03, .. body];

    private static uint Be(List<byte> b, int o) => (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);

    private bool Step()
    {
        if (!running)
        {
            if (inb.Count < 4)
                return false;
            if (inb.Take(4).SequenceEqual(RamMonitor.RomReply))
            {
                inb.RemoveRange(0, 4);
                if (romTaken)
                    out_.AddRange(RamMonitor.RomNak);
                romTaken = true;
                return true;
            }
            int op = inb[1], chk = inb[2];
            int n = op == 0x18 ? 4 + 8 + 256 : op == 0x1B ? 4 + 4 : -1;
            if (n < 0)
            {
                Errors.Add("unexpected " + Convert.ToHexString(inb.Take(8).ToArray()));
                inb.Clear();
                return false;
            }
            if (inb.Count < n)
                return false;
            var payload = inb.GetRange(4, n - 4).ToArray();
            inb.RemoveRange(0, n);
            if (RamMonitor.Sum8(payload) != chk)
            {
                Errors.Add("rom checksum");
                return true;
            }
            if (op == 0x18)
            {
                if (RamMonitor.Be32(payload) != 0x10000 + loaded.Count || !payload.AsSpan(4, 4).SequenceEqual((byte[])[0, 0, 0, 0x40]))
                    Errors.Add("load block address");
                loaded.AddRange(payload[8..]);
            }
            else
            {
                if (RamMonitor.Be32(payload) != 0x10000 || loaded.Count != stubLen)
                    Errors.Add("start command");
                running = true;
            }
            out_.AddRange(RamMonitor.RomAck);
            return true;
        }
        if (inb.Count < 6)
            return false;
        if (inb[0] != 0x02)
        {
            inb.RemoveAt(0);
            return true;
        }
        int op2 = inb[1], ln = inb[2] << 8 | inb[3], chk2 = inb[4];
        if (inb.Count < 6 + ln)
            return false;
        var p = inb.GetRange(6, ln);
        inb.RemoveRange(0, 6 + ln);
        if (RamMonitor.Sum8(p.ToArray()) != chk2)
        {
            out_.AddRange(Frame(BadSum, []));
            return true;
        }
        Ops.Add(op2);
        if (op2 is 7 or 9 && Writes.Count > 0 && CorruptAfterWrites is { } at)
        {
            Images["progmem"][at] ^= 0x01;
            CorruptAfterWrites = null;
        }
        var status = Ok;
        var body = new List<byte>();
        switch (op2)
        {
            case 0x01:
                body.AddRange("C7STUB"u8.ToArray());
                body.AddRange([6, 1032 >> 8, 1032 & 255]);
                body.AddRange(RamMonitor.Be32Bytes(0x10000));
                body.AddRange(RamMonitor.Be32Bytes(0x0808));
                body.AddRange([(byte)board, (byte)flags]);
                body.AddRange(RamMonitor.Be32Bytes(run));
                body.AddRange(RamMonitor.Be32Bytes(loaded.Count));
                body.AddRange(RamMonitor.Be32Bytes(end - 0x1004));
                body.AddRange(RamMonitor.Be32Bytes(end - 0x1000));
                body.AddRange(RamMonitor.Be32Bytes(end));
                body.AddRange(RamMonitor.Be32Bytes(600));
                break;
            case 0x0D:
                if (p.Count != 16 || p[14] > 3)
                    status = BadOp;
                else
                    (board, flags) = (p[12], p[13]);
                break;
            case 0x02:
            {
                long a = Be(p, 0);
                int c = p[4] << 8 | p[5];
                if (a == MonitorRadio.Timer2Value)
                    mem[a] = -(long)(clock.Monotonic() * 24e6) & 0xFFFFFFFF;
                if (run <= a && a < run + loaded.Count)
                    body.AddRange(loaded.Skip((int)(a - run)).Take(c));
                else
                    for (var i = 0; i < c; i += 4)
                        body.AddRange(RamMonitor.Le32Bytes(mem.GetValueOrDefault(a + i)));
                break;
            }
            case 0x03:
            {
                long a = Be(p, 0);
                for (var i = 0; i + 8 <= p.Count; i += 4)
                {
                    long v = (long)((uint)p[4 + i] | (uint)p[5 + i] << 8 | (uint)p[6 + i] << 16 | (uint)p[7 + i] << 24);
                    mem[a + i] = v;
                    Pokes.Add((a + i, v));
                }
                break;
            }
            case 0x07:
            {
                var (m, sel) = Select(p);
                long addr = Be(p, 6);
                int cnt = p[10] << 8 | p[11];
                for (var i = 0; i < cnt; i++)
                {
                    var sec = sel ? Images[m].AsSpan((int)(addr + i * Sector), Sector) : Enumerable.Repeat((byte)0xFF, Sector).ToArray();
                    body.AddRange(RamMonitor.Be32Bytes(Crc32.Compute(sec)));
                    body.Add((byte)(sec.ContainsAnyExcept((byte)0xFF) ? 0 : 1));
                }
                break;
            }
            case 0x09:
            {
                var (m, sel) = Select(p);
                long addr = Be(p, 6);
                int cnt = p[10];
                for (var i = 0; i < cnt; i++)
                {
                    var sec = sel ? Images[m].AsSpan((int)(addr + i * Sector), Sector).ToArray() : Enumerable.Repeat((byte)0xFF, Sector).ToArray();
                    byte[] rec = !sec.AsSpan().ContainsAnyExcept((byte)0xFF)
                        ? [.. RamMonitor.Be32Bytes(Crc32.Compute(sec)), 2, 0, 0]
                        : [.. RamMonitor.Be32Bytes(Crc32.Compute(sec)), 0, Sector >> 8, Sector & 255, .. sec];
                    out_.AddRange(Frame(Ok, rec));
                }
                return true;
            }
            case 0x05:
            {
                var (m, sel) = Select(p);
                int txn = p[6] << 8 | p[7], rxn = p[8] << 8 | p[9];
                var tx = p.GetRange(10, txn).ToArray();
                if (!sel)
                    body.AddRange(Enumerable.Repeat((byte)0xFF, rxn));
                else if (tx[0] == 0x03)
                    body.AddRange(Images[m].AsSpan(tx[1] << 16 | tx[2] << 8 | tx[3], rxn).ToArray());
                else if (tx[0] == 0x06)
                    wel = true;
                else if (tx[0] == 0x05)
                    body.Add((byte)(wel ? 2 : 0));
                else if (tx[0] is 0x20 or 0x02)
                {
                    var a = tx[1] << 16 | tx[2] << 8 | tx[3];
                    if (wel)
                    {
                        var img = Images[m];
                        if (tx[0] == 0x20)
                            Array.Fill(img, (byte)0xFF, a, Sector);
                        else
                            for (var i = 4; i < tx.Length; i++)
                                img[a + i - 4] &= tx[i];
                        Writes.Add((m, tx[0], a));
                    }
                    wel = false;
                }
                else if (tx[0] == 0x9F)
                    body.AddRange(m == "progmem" ? [0xC8, 0x40, 0x14] : Convert.FromHexString(DataJedec));
                else
                    Errors.Add("unknown flash command " + Convert.ToHexString(tx));
                break;
            }
            default:
                status = BadOp;
                break;
        }
        out_.AddRange(Frame(status, [.. body]));
        return true;
    }

    private static (string Mem, bool Selected) Select(List<byte> p)
    {
        var m = Be(p, 0) == MonitorRadio.SsiProg ? "progmem" : "q128";
        var sel = (p[4] & 1) != 0 && (m == "progmem" || ((p[4] & 2) != 0 && p[5] == 18));
        return (m, sel);
    }
}
