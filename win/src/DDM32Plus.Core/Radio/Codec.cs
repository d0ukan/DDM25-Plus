// SPDX-License-Identifier: GPL-3.0-or-later
using System.Reflection;
using System.Text.Json;

namespace DDM32Plus.Core.Radio;

/// <summary>A refusal of the codec slot writer (hrc7000_codec.ExtractError, codecmap.MapError).</summary>
public sealed class ExtractError(string message, string pyType = "ExtractError") : ToolError(pyType, message);

/// <summary>
/// The DM-32UV part of RadioFoundry's codec slot writer (tools/hrc7000_codec.py and rfoundry/codecmap.py, MIT): find
/// the radio's own DMR codec in a stock program flash image by its content, and build the version 2 codec slot the
/// Classic / DDM32 Plus firmware runs it from. The stock profiles and codec maps (hashes, addresses, the code words
/// that hold addresses; no codec byte) are embedded from the pinned files (Radio/Data/codec-dm32.json).
/// </summary>
public sealed class Codec
{
    public const long ProgmemBase = 0x03000000, ProgmemSize = 0x100000, StockHeader = 0x100;
    public const uint SlotMagic = 0x43524444, SlotVersion = 2;
    public const int RelMax = 64, HeaderLen = 128;
    public const uint TablesHeadCrc = 0x11475389;
    public static readonly uint[] CodecSig = [0x152514D8, 0x74C776CD];
    public static readonly string[] EntryNames = ["decode", "encode", "dec_init", "dec_set", "enc_init", "enc_set", "fec_init"];
    public static readonly byte[] Magic = "BFUV32-V2"u8.ToArray();

    public sealed record Window(long Addr, long Len, long Run);

    public sealed record Profile(string Name, string Board, long Tag, long Size, Window Code, Window Tables,
        string? CodeSha256, string TablesSha256, uint? HeadCrc, IReadOnlyDictionary<string, long> Entries,
        string? Kind = null, long? Hdr = null, bool NoMagic = false);

    public sealed record CodecMap(string Id, long CodeLen, long CodeRun, string? CodeSha256, long MoveAlignment,
        IReadOnlyList<(long Off, long Value)> SelfRefs, IReadOnlyList<(long Off, long Target)> IslandPtrs, bool HasIslands);

    /// <summary>The board's windows: slot (flash), codec run window and modem table window (RAM), image base.</summary>
    public sealed record Layout(long SlotLo, long SlotHi, long RunLo, long RunHi, long TablesLo, long TablesHi, long AppBase);

    public IReadOnlyList<(string Key, Profile Profile)> Profiles { get; }
    public IReadOnlyDictionary<string, CodecMap> Maps { get; }
    public Layout Board { get; }

    public Codec(IReadOnlyList<(string, Profile)> profiles, IReadOnlyDictionary<string, CodecMap> maps, Layout board)
    {
        Profiles = profiles;
        Maps = maps;
        Board = board;
    }

    private static readonly Lazy<Codec> Dm32_ = new(LoadEmbedded);

    /// <summary>The pinned DM-32UV profiles and maps.</summary>
    public static Codec Dm32 => Dm32_.Value;

    private static Codec LoadEmbedded()
    {
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("DDM32Plus.Core.Radio.codec-dm32.json")
                      ?? throw new CodecError("The codec tables are missing from the program");
        using var doc = JsonDocument.Parse(s);
        var root = doc.RootElement;
        var profiles = new List<(string, Profile)>();
        foreach (var p in root.GetProperty("profiles").EnumerateObject())
        {
            var v = p.Value;
            Window W(string k)
            {
                var a = v.GetProperty(k);
                return new Window(a[0].GetInt64(), a[1].GetInt64(), a[2].GetInt64());
            }
            profiles.Add((p.Name, new Profile(v.GetProperty("name").GetString()!, v.GetProperty("board").GetString()!,
                v.GetProperty("tag").GetInt64(), v.GetProperty("size").GetInt64(), W("code"), W("tables"),
                v.TryGetProperty("code_sha256", out var cs) ? cs.GetString() : null,
                v.GetProperty("tables_sha256").GetString()!,
                v.TryGetProperty("tables_head_crc", out var hc) ? hc.GetUInt32() : null,
                v.GetProperty("entries").EnumerateObject().ToDictionary(e => e.Name, e => e.Value.GetInt64()),
                v.TryGetProperty("kind", out var kind) ? kind.GetString() : null,
                v.TryGetProperty("hdr", out var hdr) ? hdr.GetInt64() : null,
                v.TryGetProperty("magic", out var magic) && magic.ValueKind == JsonValueKind.Null)));
        }
        var maps = new Dictionary<string, CodecMap>();
        foreach (var m in root.GetProperty("maps").EnumerateObject())
        {
            var c = m.Value.GetProperty("code");
            maps[m.Name] = new CodecMap(m.Name, c.GetProperty("len").GetInt64(), c.GetProperty("run").GetInt64(),
                c.TryGetProperty("sha256", out var sh) ? sh.GetString() : null,
                c.TryGetProperty("move_alignment", out var al) ? al.GetInt64() : 4,
                [.. m.Value.GetProperty("self_refs").EnumerateArray().Select(r =>
                    r.GetProperty("kind").GetString() == "word"
                        ? (r.GetProperty("off").GetInt64(), r.GetProperty("value").GetInt64())
                        : throw new ExtractError($"{m.Name}: the code can't be moved by adding to words", "MapError"))],
                [.. m.Value.GetProperty("island_ptrs").EnumerateArray().Select(r => (r.GetProperty("off").GetInt64(), r.GetProperty("target").GetInt64()))],
                m.Value.GetProperty("islands").GetArrayLength() > 0);
        }
        var b = root.GetProperty("board");
        var board = new Layout(b.GetProperty("slot")[0].GetInt64(), b.GetProperty("slot")[1].GetInt64(),
            b.GetProperty("run")[0].GetInt64(), b.GetProperty("run")[1].GetInt64(),
            b.GetProperty("tables")[0].GetInt64(), b.GetProperty("tables")[1].GetInt64(), b.GetProperty("app_base").GetInt64());
        return new Codec(profiles, maps, board);
    }

    private static uint Word(ReadOnlySpan<byte> b, long off) => BitConverter.ToUInt32(b.Slice((int)off, 4));

    /// <summary>The checked header of a codec slot (magic, version, windows, CRC), else null.</summary>
    public PyDict? ParseSlot(ReadOnlySpan<byte> slot)
    {
        if (slot.Length < HeaderLen)
            return null;
        var f = new long[19];
        for (var i = 0; i < 19; i++)
            f[i] = Word(slot, i * 4);
        long magic = f[0], version = f[1], codeOff = f[2], codeLen = f[3], run = f[4], tabOff = f[5], tabLen = f[6], tabRam = f[7];
        if (magic != SlotMagic || version != SlotVersion)
            return null;
        if (codeOff < HeaderLen || codeOff + codeLen > slot.Length || tabOff + tabLen > slot.Length || codeLen == 0)
            return null;
        var code = slot.Slice((int)codeOff, (int)codeLen);
        var tables = slot.Slice((int)tabOff, (int)tabLen);
        var crc = Crc32.Compute(tables, Crc32.Compute(code));
        long relOff = f[17], relCount = f[18];
        if (relCount > RelMax || relOff % 4 != 0 || relOff + 4 * relCount > slot.Length)
            return null;
        var rels = new List<long>();
        for (var i = 0; i < relCount; i++)
            rels.Add(Word(slot, relOff + 4 * i));
        crc = Crc32.Compute(slot.Slice((int)relOff, (int)(4 * relCount)), crc);
        var end = Math.Max(tabOff + tabLen, relOff + 4 * relCount);
        if (code.Length < 8 || crc != f[16] || Word(code, 0) != CodecSig[0] || Word(code, 4) != CodecSig[1])
            return null;
        return new PyDict
        {
            ["version"] = version, ["run"] = run, ["tables_ram"] = tabRam, ["tag"] = f[15], ["crc"] = f[16], ["length"] = end,
            ["relocations"] = rels, ["code_sha256"] = Hash.Sha(code), ["tables_sha256"] = Hash.Sha(tables),
        };
    }

    /// <summary>A known codec anywhere in a program flash dump, found by its content (hrc7000_codec.locate).</summary>
    public Profile? Locate(byte[] dump, IEnumerable<(string Key, Profile Profile)>? profiles = null, string? board = null)
    {
        var list = (profiles ?? Profiles).Select(x => x.Profile)
            .Where(p => (board is null || board == p.Board) && p.CodeSha256 is not null).ToList();
        if (dump.Length != ProgmemSize || list.Count == 0)
            return null;
        byte[] sig = [.. BitConverter.GetBytes(CodecSig[0]), .. BitConverter.GetBytes(CodecSig[1])];
        Dictionary<uint, List<int>>? heads = null;
        var at = dump.AsSpan().IndexOf(sig);
        while (at >= 0)
        {
            // stock versions with the same codec bytes are told apart by where they put it: those first (stable)
            foreach (var p in list.OrderBy(p => p.Code.Addr != ProgmemBase + at))
            {
                var clen = (int)p.Code.Len;
                var tlen = (int)p.Tables.Len;
                if (at % 4 != 0 || Hash.Sha(Slice(dump, at, clen)) != p.CodeSha256)
                    continue;
                var want = p.HeadCrc ?? TablesHeadCrc;
                if (heads is null)
                {
                    heads = [];
                    for (var i = 0; i < dump.Length - 64; i += 4)
                    {
                        var c = Crc32.Compute(dump.AsSpan(i, 64));
                        if (!heads.TryGetValue(c, out var l))
                            heads[c] = l = [];
                        l.Add(i);
                    }
                }
                foreach (var t in heads.GetValueOrDefault(want) ?? [])
                    if (Hash.Sha(Slice(dump, t, tlen)) == p.TablesSha256)
                        return p with
                        {
                            Kind = "progmem", Size = ProgmemSize, Code = new Window(ProgmemBase + at, clen, p.Code.Run),
                            Tables = new Window(ProgmemBase + t, tlen, p.Tables.Run),
                        };
            }
            var next = dump.AsSpan(at + 1).IndexOf(sig);
            at = next < 0 ? -1 : at + 1 + next;
        }
        return null;
    }

    /// <summary>dump[a:a+n] as Python slices it (shorter at the end).</summary>
    private static ReadOnlySpan<byte> Slice(byte[] b, long a, long n) =>
        a >= b.Length ? [] : b.AsSpan((int)a, (int)Math.Min(n, b.Length - a));

    /// <summary>The profile a stock file matches (whole-file SHA-256, or a program flash dump by its windows).</summary>
    public Profile? Identify(byte[] stock, string? board = null)
    {
        var mine = Profiles.Where(x => board is null || board == x.Profile.Board).ToList();
        var digest = Hash.Sha(stock);
        var prof = mine.FirstOrDefault(x => x.Key == digest).Profile;
        if (prof is not null && prof.Kind != "progmem")
            return prof;
        return Locate(stock, mine, board);
    }

    private static List<byte[]> Carve(byte[] stock, Profile prof)
    {
        var windows = new List<byte[]>();
        foreach (var (key, w) in new[] { ("code", prof.Code), ("tables", prof.Tables) })
        {
            var hdr = prof.Kind == "progmem" ? 0 : prof.Hdr ?? StockHeader;
            var off = prof.Kind == "progmem" ? w.Addr - ProgmemBase : w.Addr - 0x0300C000 + hdr;
            if (off < hdr || off + w.Len > stock.Length)
                throw new ExtractError($"{key} window outside the file");
            windows.Add(stock.AsSpan((int)off, (int)w.Len).ToArray());
        }
        return windows;
    }

    /// <summary>(profile, code, tables) of a stock image, checked.</summary>
    public (Profile Profile, byte[] Code, byte[] Tables) Extract(byte[] stock, string? board = null)
    {
        var mine = Profiles.Where(x => board is null || board == x.Profile.Board).ToList();
        var digest = Hash.Sha(stock);
        var prof = Identify(stock, board) ?? throw new ExtractError(
            $"unsupported stock file (sha256 {digest}); supported: " +
            string.Join(", ", mine.Select(x => $"{x.Profile.Name} ({x.Key[..8]}...)")));
        if (prof.Kind != "progmem" && !prof.NoMagic && !stock.AsSpan().StartsWith(Magic))
            throw new ExtractError("not a BFUV32-V2 firmware file");
        var w = Carve(stock, prof);
        if (Word(w[0], 0) != CodecSig[0] || Word(w[0], 4) != CodecSig[1])
            throw new ExtractError("codec signature words not found at the code window");
        return (prof, w[0], w[1]);
    }

    /// <summary>The codec slot: header, code, tables (4-byte aligned), the relocation list from the codec map.</summary>
    public byte[] SlotBytes(Profile prof, byte[] code, byte[] tables)
    {
        var lay = Board;
        const int codeOff = HeaderLen;
        var tabOff = codeOff + ((code.Length + 3) & ~3);
        var run = prof.Code.Run;
        var tabRam = prof.Tables.Run;
        if (!(lay.TablesLo <= tabRam && tabRam + tables.Length <= lay.TablesHi))
            throw new ExtractError("table address outside the reserved modem SRAM window");
        var sha = Hash.Sha(code);
        var m = Maps.Values.FirstOrDefault(x => x.CodeSha256 == sha) ?? throw new ExtractError(
            $"no codec map for this build (code sha256 {sha[..8]}...): its code can't be moved to the board's codec window");
        if (m.CodeRun != run || m.CodeLen != code.Length)
            throw new ExtractError($"the codec map of {prof.Name} doesn't match its profile");
        if (code.Length > lay.RunHi - lay.RunLo)
            throw new ExtractError($"the codec ({code.Length} bytes) is bigger than the board's codec window");
        if ((lay.RunLo - run) % 4 != 0)
            throw new ExtractError("the codec window would change the code's word alignment");
        MoveCodeCheck(code, m, lay.RunLo);
        if (m.HasIslands)
            throw new ExtractError($"{m.Id}: the code can't be moved by adding to words", "MapError");
        var rels = m.SelfRefs.Select(r => r.Off).Order().ToList();
        var relOff = tabOff + ((tables.Length + 3) & ~3);
        var relBytes = rels.SelectMany(r => BitConverter.GetBytes((uint)r)).ToArray();
        var crc = Crc32.Compute(relBytes, Crc32.Compute(tables, Crc32.Compute(code)));
        var words = new List<uint> { SlotMagic, SlotVersion, codeOff, (uint)code.Length, (uint)run, (uint)tabOff,
            (uint)tables.Length, (uint)tabRam };
        words.AddRange(EntryNames.Select(k => (uint)prof.Entries[k]));
        words.AddRange([(uint)prof.Tag, crc, (uint)relOff, (uint)rels.Count]);
        var body = new List<byte>(words.SelectMany(BitConverter.GetBytes));
        body.AddRange(new byte[HeaderLen - body.Count]);
        body.AddRange(code);
        body.AddRange(Enumerable.Repeat((byte)0xFF, tabOff - codeOff - code.Length));
        body.AddRange(tables);
        body.AddRange(Enumerable.Repeat((byte)0xFF, relOff - body.Count));
        body.AddRange(relBytes);
        if (body.Count > lay.SlotHi - lay.SlotLo)
            throw new ExtractError("codec does not fit the slot");
        return [.. body];
    }

    /// <summary>codecmap.move_code's checks: the map's own code, every listed site holding the listed value.</summary>
    private static void MoveCodeCheck(byte[] code, CodecMap m, long newRun)
    {
        if (code.Length != m.CodeLen)
            throw new ExtractError($"{m.Id}: code is {code.Length} bytes, the map says {m.CodeLen}", "MapError");
        if (m.CodeSha256 is not null && Hash.Sha(code) != m.CodeSha256)
            throw new ExtractError($"{m.Id}: the code is not the build this map describes", "MapError");
        if ((newRun - m.CodeRun) % m.MoveAlignment != 0)
            throw new ExtractError($"{m.Id}: a move by 0x{newRun - m.CodeRun:X} breaks the code's alignment", "MapError");
        foreach (var (off, want) in m.SelfRefs.Concat(m.IslandPtrs))
        {
            var got = Word(code, off);
            if (got != want)
                throw new ExtractError($"{m.Id}: site +0x{off:X} holds 0x{got:X8}, the map says 0x{want:X8}", "MapError");
        }
    }

    /// <summary>image, 0xFF up to the slot, the slot.</summary>
    public byte[] Merge(byte[] image, Profile prof, byte[] code, byte[] tables)
    {
        var slotOff = Board.SlotLo - Board.AppBase;
        if (image.Length > slotOff)
            throw new ExtractError($"the application image ({image.Length} bytes) runs into the codec slot at +0x{slotOff:X}");
        return [.. image, .. Enumerable.Repeat((byte)0xFF, (int)(slotOff - image.Length)), .. SlotBytes(prof, code, tables)];
    }

    /// <summary>A codec for tests: one synthetic stock profile and its map (pc/tests/rtfixtures.synthetic_stock_codec).</summary>
    public static Codec Synthetic(byte[] code, byte[] tables, IReadOnlyList<(long Off, long Value)> refs, long codeAt,
        long tablesAt, string name = "DM32.01.01.047.bin", long tag = 47)
    {
        var entries = new Dictionary<string, long>
        {
            ["decode"] = 0x0, ["encode"] = 0x260, ["dec_init"] = 0xB4, ["dec_set"] = 0x84, ["enc_init"] = 0x314,
            ["enc_set"] = 0x2E4, ["fec_init"] = 0x1BF8,
        };
        var prof = new Profile(name, "dm32", tag, 869748, new Window(codeAt, code.Length, 0x16CDC),
            new Window(tablesAt, tables.Length, 0x18000EA8), Hash.Sha(code), Hash.Sha(tables),
            Crc32.Compute(tables.AsSpan(0, 64)), entries);
        var map = new CodecMap($"test-dm32-{tag}", code.Length, 0x16CDC, Hash.Sha(code), 4, [.. refs], [], false);
        return new Codec([(new string('f', 64), prof)], new Dictionary<string, CodecMap> { [map.Id] = map }, Dm32.Board);
    }
}
