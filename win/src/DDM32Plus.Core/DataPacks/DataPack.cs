// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using System.Text;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.DataPacks;

/// <summary>A pack that cannot be built (tools/datapack.py PackError / iddb.IddbError).</summary>
public sealed class PackError(string message) : Exception(message);

/// <summary>A slot header's fields (datapack.parse).</summary>
public sealed record PackHeader(int Type, int Format, uint Seq, uint Length, uint Crc, string Date, string Name, string Tag,
    bool CrcOk);

/// <summary>One CPS sector write of a slot image: its physical data chip address and its 4096 bytes.</summary>
public sealed record PackSector(long Address, byte[] Data);

/// <summary>
/// DDM32 Plus user data packs: the port of tools/datapack.py (docs/plus.md "User data packs"). A pack is the slot image
/// the radio keeps on its data chip: a 64-byte header (DPK1, type, format, sequence, length, payload CRC-32, date,
/// name, builder tag, header CRC-32), then the payload. Two slots a type in 0xE70000-0xEFFFFF (the one data chip
/// region the firmware may write; AGENTS.md rule 2). The data always comes from files the user downloaded; nothing
/// derived from it is ever in the repository or a release. Held byte for byte to datapack.py by
/// tests/DDM32Plus.Core.Tests/DataPackTests.cs (Fixtures/datapack-results.json, make_datapack_fixtures.py).
/// </summary>
public static class DataPack
{
    public static readonly byte[] Magic = "DPK1"u8.ToArray();
    public const int RadioId = 1, Air = 2;
    public const int HeaderSize = 64;
    public const int Sector = 4096;
    public const int IdChars = 16;          // 6-bit characters a record's text, a multiple of 4
    public const int AirFreqs = 64;         // air_pack.h AIR_PACK_FREQS

    /// <summary>The builder tag this program writes (datapack.py writes "datapack.py 1").</summary>
    public const string Tag = "DDM32Plus-win 1";
    public const string PythonTag = "datapack.py 1";

    /// <summary>The region [RegionStart, RegionEnd) on the data chip: 0xE70000-0xEFFFFF.</summary>
    public const long RegionStart = 0xE70000, RegionEnd = 0xF00000;

    public static int SlotSize(int type) => type switch
    {
        RadioId => 0x40000,
        Air => 0x4000,
        _ => throw new PackError($"pack type {type}"),
    };

    /// <summary>datapack.SLOT_ADDR: RadioID 0xE70000 / 0xEB0000, air band 0xEF0000 / 0xEF4000.</summary>
    public static long SlotAddress(int type, int slot)
    {
        if (slot is not (0 or 1))
            throw new PackError($"slot {slot}");
        return type switch
        {
            RadioId => slot == 0 ? 0xE70000 : 0xEB0000,
            Air => slot == 0 ? 0xEF0000 : 0xEF4000,
            _ => throw new PackError($"pack type {type}"),
        };
    }

    /// <summary>True when [address, address + length) lies inside the data pack region.</summary>
    public static bool InRegion(long address, long length) =>
        length > 0 && address >= RegionStart && address <= RegionEnd - length;

    /// <summary>The header (64 bytes): datapack.header(ptype, payload, date, name, seq, tag).</summary>
    public static byte[] Header(int type, byte[] payload, string date, string name, uint seq = 0, string tag = Tag)
    {
        if (date.Length != 8 || date.Any(c => c is < '0' or > '9'))
            throw new PackError($"date '{date}': YYYYMMDD");
        if (payload.Length > SlotSize(type) - HeaderSize)
            throw new PackError($"{payload.Length} bytes: the slot holds {SlotSize(type) - HeaderSize}");
        var h = new byte[HeaderSize];
        Magic.CopyTo(h, 0);
        h[4] = (byte)type;
        h[5] = 1;                                   // format 1
        BinaryPrimitives.WriteUInt16LittleEndian(h.AsSpan(6), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(8), seq);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(12), (uint)payload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(16), Crc32.Compute(payload));
        Encoding.ASCII.GetBytes(date).CopyTo(h, 20);
        var n = PyText.AsciiReplace(name);
        Array.Copy(n, 0, h, 28, Math.Min(16, n.Length));
        var t = PyText.AsciiReplace(tag);
        Array.Copy(t, 0, h, 44, Math.Min(16, t.Length));
        BinaryPrimitives.WriteUInt32LittleEndian(h.AsSpan(60), Crc32.Compute(h.AsSpan(0, 60)));
        return h;
    }

    /// <summary>The slot image of a new pack: the header with sequence seq, then the payload.</summary>
    public static byte[] Image(int type, byte[] payload, string date, string name, uint seq = 0, string tag = Tag) =>
        [.. Header(type, payload, date, name, seq, tag), .. payload];

    /// <summary>datapack.parse: the header's fields (null when it is no pack) and the payload.</summary>
    public static (PackHeader? Header, byte[] Payload) Parse(ReadOnlySpan<byte> image)
    {
        if (image.Length < HeaderSize || !image[..4].SequenceEqual(Magic) ||
            BinaryPrimitives.ReadUInt32LittleEndian(image[60..]) != Crc32.Compute(image[..60]))
            return (null, []);
        var length = BinaryPrimitives.ReadUInt32LittleEndian(image[12..]);
        var crc = BinaryPrimitives.ReadUInt32LittleEndian(image[16..]);
        var end = (int)Math.Min(image.Length, HeaderSize + (long)length);
        var payload = image[HeaderSize..end].ToArray();
        return (new PackHeader(image[4], image[5], BinaryPrimitives.ReadUInt32LittleEndian(image[8..]), length, crc,
            PyText.AsciiDecode(image.Slice(20, 8)), PyText.AsciiDecode(PyText.RStripNul(image.Slice(28, 16))),
            PyText.AsciiDecode(PyText.RStripNul(image.Slice(44, 16))),
            payload.Length == length && Crc32.Compute(payload) == crc), payload);
    }

    /// <summary>datapack.next_seq: one more than the valid sequences the radio's two slots hold.</summary>
    public static uint NextSeq(IEnumerable<byte[]> images)
    {
        uint max = 0;
        foreach (var img in images)
        {
            var (h, _) = Parse(img);
            if (h is { CrcOk: true } && h.Seq > max)
                max = h.Seq;
        }
        if (max == uint.MaxValue)
            throw new PackError("the sequence number is used up");
        return max + 1;
    }

    /// <summary>datapack.inactive_slot: the slot a new pack goes into, the one not in force (0 when neither holds a
    /// valid pack of the type).</summary>
    public static int InactiveSlot(int type, IReadOnlyList<byte[]> images)
    {
        (int Slot, uint Seq)? best = null;
        for (var i = 0; i < images.Count; i++)
        {
            var (h, _) = Parse(images[i]);
            if (h is { CrcOk: true } && h.Type == type && (best is null || h.Seq > best.Value.Seq))
                best = (i, h.Seq);
        }
        return best is null ? 0 : 1 - best.Value.Slot;
    }

    /// <summary>The slot (0, 1) in force, or null: the valid pack of the type with the higher sequence.</summary>
    public static int? ActiveSlot(int type, IReadOnlyList<byte[]> images)
    {
        (int Slot, uint Seq)? best = null;
        for (var i = 0; i < images.Count; i++)
        {
            var (h, _) = Parse(images[i]);
            if (h is { CrcOk: true } && h.Type == type && (best is null || h.Seq > best.Value.Seq))
                best = (i, h.Seq);
        }
        return best?.Slot;
    }

    /// <summary>datapack.cps_plan: the sector writes for a slot image, in order: the payload's sectors, then the
    /// header's sector last (its rewrite is the commit: until then the slot does not count). Every sector lies inside
    /// the slot and the region, or this refuses.</summary>
    public static List<PackSector> CpsPlan(long slotAddress, byte[] image)
    {
        var padded = new byte[(image.Length + Sector - 1) / Sector * Sector];
        Array.Fill(padded, (byte)0xFF);
        image.CopyTo(padded, 0);
        var secs = new List<PackSector>();
        for (var o = 0; o < padded.Length; o += Sector)
            secs.Add(new PackSector(slotAddress + o, padded[o..(o + Sector)]));
        if (secs.Count == 0)
            throw new PackError("empty image");
        foreach (var s in secs)
        {
            if (!InRegion(s.Address, Sector) || s.Address % Sector != 0)
                throw new PackError($"sector 0x{s.Address:X}: outside the data pack region 0xE70000-0xEFFFFF");
        }
        return [.. secs.Skip(1), secs[0]];
    }

    /// <summary>The plan for a pack of a type into one of its slots: CpsPlan after the slot size check.</summary>
    public static List<PackSector> PlanForSlot(int type, int slot, byte[] image)
    {
        if (image.Length > SlotSize(type))
            throw new PackError($"{image.Length} bytes: the slot holds {SlotSize(type)}");
        return CpsPlan(SlotAddress(type, slot), image);
    }

    // --- RadioID (format 1) ---

    private static int Code6(char c) => c switch
    {
        ' ' => 0,
        >= '0' and <= '9' => 1 + c - '0',
        >= 'A' and <= 'Z' => 11 + c - 'A',
        >= 'a' and <= 'z' => 37 + c - 'a',
        '.' => 63,
        _ => 0,
    };

    /// <summary>datapack.radioid_payload: entries (id, callsign, name) -> an 8-byte header, then records sorted by ID
    /// (3 bytes LE, the text 6 bits a character).</summary>
    public static byte[] RadioIdPayload(IEnumerable<(int Id, string Call, string Name)> entries, int chars = IdChars)
    {
        if (chars % 4 != 0 || chars < 4 || chars > 32)
            throw new PackError("text characters: 4..32, a multiple of 4");
        var list = entries.ToList();
        list.Sort((a, b) =>
        {
            var c = a.Id.CompareTo(b.Id);
            if (c == 0)
                c = PyText.CompareCodePoints(a.Call, b.Call);
            return c != 0 ? c : PyText.CompareCodePoints(a.Name, b.Name);
        });
        var rec = 3 + chars * 3 / 4;
        var out_ = new List<byte>(8 + list.Count * rec);
        var head = new byte[8];
        BinaryPrimitives.WriteUInt32LittleEndian(head, (uint)list.Count);
        head[4] = (byte)rec;
        head[5] = (byte)chars;
        out_.AddRange(head);
        var last = 0;
        foreach (var (rid, call, name) in list)
        {
            if (rid <= 0 || rid > 0xFFFFFF || rid <= last)
                throw new PackError($"ID {rid}: out of range or repeated");
            last = rid;
            var text = PyText.PadRight(PyText.Left(RadioIdCsv.Fold(PyText.Strip($"{call} {name}")), chars), chars);
            out_.Add((byte)rid);
            out_.Add((byte)(rid >> 8));
            out_.Add((byte)(rid >> 16));
            for (var k = 0; k < chars; k += 4)
            {
                var v = 0;
                for (var j = k; j < k + 4; j++)
                    v = v << 6 | Code6(text[j]);
                out_.Add((byte)(v >> 16));
                out_.Add((byte)(v >> 8));
                out_.Add((byte)v);
            }
        }
        return [.. out_];
    }

    /// <summary>datapack.radioid_from_csv: the payload, how many IDs, and what was skipped.</summary>
    public static (byte[] Payload, int Count, RadioIdSkipped Skipped) RadioIdFromCsv(byte[] csv, IEnumerable<string> countries)
    {
        var (rows, skipped) = RadioIdCsv.Read(csv, countries);
        return (RadioIdPayload(rows.Select(r => (r.Id, r.Call, r.Name))), rows.Count, skipped);
    }

    // --- Air band (format 1) ---

    /// <summary>datapack.air_payload: {code: [(type index, kHz)]} -> a 4-byte header, per airfield its code (8), its
    /// count (1) and the frequencies (channel 2, type 1).</summary>
    public static byte[] AirPayload(IReadOnlyDictionary<string, List<(int Type, int Khz)>> fields)
    {
        if (fields.Count > 0xFFFF)
            throw new PackError($"{fields.Count} airfields");
        var out_ = new List<byte> { (byte)fields.Count, (byte)(fields.Count >> 8), 0, 0 };
        foreach (var code in fields.Keys.Order(Comparer<string>.Create(PyText.CompareCodePoints)))
        {
            var freqs = fields[code].Take(AirFreqs).ToList();
            if (code.Length == 0 || code.Length > 8 || code.Any(c => c > 0x7F))
                throw new PackError($"code '{code}'");
            var c = new byte[8];
            Encoding.ASCII.GetBytes(code).CopyTo(c, 0);
            out_.AddRange(c);
            out_.Add((byte)freqs.Count);
            foreach (var (t, khz) in freqs)
            {
                var ch = (int)Math.Floor((khz - AirText.ChBaseKhz) / 5.0);     // Python's // floors
                if (ch is < 0 or > 0xFFFF || t is < 0 or > 0xFF)
                    throw new PackError($"frequency {khz} kHz");
                out_.Add((byte)ch);
                out_.Add((byte)(ch >> 8));
                out_.Add((byte)t);
            }
        }
        return [.. out_];
    }

    /// <summary>datapack.info: what a slot image holds, in English (the tool's own text).</summary>
    public static string Info(byte[] image)
    {
        var (h, payload) = Parse(image);
        if (h is null)
            return "not a data pack";
        var lines = new List<string>
        {
            $"type {h.Type} format {h.Format} sequence {h.Seq} \"{h.Name}\" date {h.Date} {h.Length} bytes, payload CRC " +
            $"{(h.CrcOk ? "ok" : "WRONG")}, built by {h.Tag}",
        };
        if (h.Type == RadioId && payload.Length >= 8)
            lines.Add($"{BinaryPrimitives.ReadUInt32LittleEndian(payload)} IDs, {payload[5]} characters a record ({payload[4]} bytes)");
        if (h.Type == Air && payload.Length >= 4)
            lines.Add($"{BinaryPrimitives.ReadUInt16LittleEndian(payload)} airfields");
        return string.Join("\n", lines);
    }

    /// <summary>How many entries a pack's payload holds (IDs or airfields), or null.</summary>
    public static int? Count(PackHeader h, byte[] payload) =>
        h.Type == RadioId && payload.Length >= 8 ? (int)Math.Min(int.MaxValue, BinaryPrimitives.ReadUInt32LittleEndian(payload))
        : h.Type == Air && payload.Length >= 4 ? BinaryPrimitives.ReadUInt16LittleEndian(payload)
        : null;
}
