// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using System.Globalization;
using DDM32Plus.Core.Radio;
using System.Text;

namespace DDM32Plus.Core.Codeplug;

/// <summary>The codeplug in a backup is not a 16 MiB data-flash image (or cannot be read).</summary>
public sealed class CodeplugError(string message) : Exception(message);

/// <summary>One channel as the viewer shows it: its number, its mode ("FM", "DMR", "P25", "YSF", "NXDN", "dPMR" or an
/// "unknown" text) and one text per column (<see cref="CodeplugText.ChannelColumns"/>).</summary>
public sealed class ChannelRow
{
    public int Number { get; init; }
    public string Mode { get; init; } = "";
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);

    public string this[string key] => Values.TryGetValue(key, out var v) ? v : "";
}

public sealed record ZoneRow(int Index, int Slot, string Name, IReadOnlyList<int> Channels, string ChannelNames)
{
    public int Count => Channels.Count;
}

public sealed record ContactRow(int Index, string Name, long Id, string Type, string Ts);

public sealed record ListRow(int Index, string Name, IReadOnlyList<int> Contacts, string ContactNames)
{
    public int Count => Contacts.Count;
}

/// <summary>
/// The words the viewer writes into its rows and column titles, in one language. Turkish is the Python viewer's own
/// (pc/ddm32_app/codeplug.py), English is new. The decoder takes one of these, so a language change decodes again.
/// </summary>
public sealed class CodeplugText
{
    public required string Any { get; init; }              // "Herhangi"
    public required string AnyLower { get; init; }         // "herhangi" (DG-ID / RAN / CC)
    public required string Yes { get; init; }
    public required string Default { get; init; }          // power byte 0
    public required string UnknownMode { get; init; }      // "{0}": "0x03"
    public required string UnknownKind { get; init; }      // "{0}": RFKV kind
    public required string SameAsRx { get; init; }
    public required string AllTalkgroups { get; init; }    // RX list "Any"
    public required string ContactTg { get; init; }        // "{0}": contact number
    public required string AllGroups { get; init; }        // " (tüm gruplar)"
    public required string Mode2Form { get; init; }        // "  (CH_MODE 2 biçimi)"
    public required string BadRecord { get; init; }        // "  (geçersiz kayıt)"
    public required string NoTone { get; init; }
    public required string Tone { get; init; }             // "Ton "
    public required string ToneRxTx { get; init; }         // "{0}" rx, "{1}" tx
    public required string AllTg { get; init; }            // "tüm TG"
    public required string Contact { get; init; }          // "Kişi "
    public required string RxOnly { get; init; }           // "sadece RX"
    public required string[] ContactTypes { get; init; }   // group, private, all call
    public required (string Key, string Title)[] ChannelColumns { get; init; }
    public required (string Key, string Title)[] ZoneColumns { get; init; }
    public required (string Key, string Title)[] ContactColumns { get; init; }
    public required (string Key, string Title)[] ListColumns { get; init; }

    /// <summary>The firmware's NAC form (CONFIG_NAC_DEC): "659 ($293)", "Any 3966 ($F7E)".</summary>
    public string Nac(int nac) =>
        nac == Codeplug.NacAny ? $"{Any} 3966 ($F7E)" : string.Create(CultureInfo.InvariantCulture, $"{nac} (${nac:X3})");

    public static readonly CodeplugText Tr = new()
    {
        Any = "Herhangi", AnyLower = "herhangi", Yes = "Evet", Default = "Varsayılan", UnknownMode = "Bilinmiyor ({0})",
        UnknownKind = "Bilinmiyor (RFKV tür {0})", SameAsRx = "RX ile aynı", AllTalkgroups = "Herhangi (tüm TG)",
        ContactTg = "kişi {0}", AllGroups = " (tüm gruplar)", Mode2Form = "  (CH_MODE 2 biçimi)", BadRecord = "  (geçersiz kayıt)",
        NoTone = "Ton yok", Tone = "Ton ", ToneRxTx = "Ton RX {0} / TX {1}", AllTg = "tüm TG", Contact = "Kişi ",
        RxOnly = "sadece RX", ContactTypes = ["Grup", "Özel", "Herkes"],
        ChannelColumns =
        [
            ("number", "No"), ("name", "Ad"), ("freq", "Frekans"), ("mode", "Mod"), ("summary", "Ayar"), ("rx_mhz", "RX MHz"),
            ("tx_mhz", "TX MHz"), ("bw", "Bant"), ("power", "Güç"), ("rx_only", "Sadece RX"), ("rx_tone", "RX ton"),
            ("tx_tone", "TX ton"), ("cc", "CC"), ("ts", "TS"), ("nac", "NAC"), ("tx_nac", "TX NAC"), ("tg", "TG"),
            ("contact", "Kişi"), ("rx_list", "RX grup listesi"), ("code", "DG-ID / RAN / CC"), ("unit_id", "ID"),
            ("scan_skip", "Taramada atla"), ("tot_s", "TOT sn"), ("mode_byte", "CH_MODE"),
        ],
        ZoneColumns = [("index", "No"), ("name", "Ad"), ("count", "Kanal sayısı"), ("channel_names", "Kanallar")],
        ContactColumns = [("index", "No"), ("name", "Ad"), ("id", "ID / TG"), ("type", "Tür"), ("ts", "TS")],
        ListColumns = [("index", "No"), ("name", "Ad"), ("count", "Üye sayısı"), ("contact_names", "Kişiler")],
    };

    public static readonly CodeplugText En = new()
    {
        Any = "Any", AnyLower = "any", Yes = "Yes", Default = "Default", UnknownMode = "Unknown ({0})",
        UnknownKind = "Unknown (RFKV kind {0})", SameAsRx = "Same as RX", AllTalkgroups = "Any (all TGs)",
        ContactTg = "contact {0}", AllGroups = " (all groups)", Mode2Form = "  (CH_MODE 2 form)", BadRecord = "  (invalid record)",
        NoTone = "No tone", Tone = "Tone ", ToneRxTx = "Tone RX {0} / TX {1}", AllTg = "all TGs", Contact = "Contact ",
        RxOnly = "RX only", ContactTypes = ["Group", "Private", "All call"],
        ChannelColumns =
        [
            ("number", "No"), ("name", "Name"), ("freq", "Frequency"), ("mode", "Mode"), ("summary", "Settings"),
            ("rx_mhz", "RX MHz"), ("tx_mhz", "TX MHz"), ("bw", "Bandwidth"), ("power", "Power"), ("rx_only", "RX only"),
            ("rx_tone", "RX tone"), ("tx_tone", "TX tone"), ("cc", "CC"), ("ts", "TS"), ("nac", "NAC"), ("tx_nac", "TX NAC"),
            ("tg", "TG"), ("contact", "Contact"), ("rx_list", "RX group list"), ("code", "DG-ID / RAN / CC"),
            ("unit_id", "ID"), ("scan_skip", "Skip in scan"), ("tot_s", "TOT s"), ("mode_byte", "CH_MODE"),
        ],
        ZoneColumns = [("index", "No"), ("name", "Name"), ("count", "Channels"), ("channel_names", "Channel list")],
        ContactColumns = [("index", "No"), ("name", "Name"), ("id", "ID / TG"), ("type", "Type"), ("ts", "TS")],
        ListColumns = [("index", "No"), ("name", "Name"), ("count", "Members"), ("contact_names", "Contacts")],
    };

    public static CodeplugText For(string lang) => lang == "en" ? En : Tr;
}

/// <summary>
/// Read-only decoder of the DDM32 Plus codeplug in a data-flash (q128, 16 MiB) backup image: a port of
/// pc/ddm32_app/codeplug.py (its module text has the layout). Channels 1-1024 (bank 0 in the EEPROM image, banks 1-7 in
/// the flash image, 56 bytes each), 68 zone slots of 80 channels (16 with the old layout byte), 1024 contacts, 76 RX
/// group lists, the radio name and DMR ID; FM, DMR, YSF, NXDN, dPMR, and P25 in Classic r13's form (a receive-only DMR
/// record and its RFKV tag), the r4-r7 form (0xA5) and the multi-mode derivative's (CH_MODE 2). The NAC is written as
/// the firmware shows it now: decimal first, "659 ($293)". Held to the Python results in CodeplugTests.cs. Nothing here
/// writes anything.
/// </summary>
public sealed class Codeplug
{
    public const int ImageSize = 1 << 24;
    public const int EeBase = 0x700000, FlBase = 0x720000;
    public const int ChPerBank = 128, ChBanks = 8, ChSize = 56, MaxChannels = ChPerBank * ChBanks;
    public const int Bank0Bitmap = EeBase + 0x3780, Bank1Bitmap = FlBase + 0x7B1B0, BankStride = 16 + ChPerBank * ChSize;
    public const int ChName = 0x00, ChRx = 0x10, ChTx = 0x14, ChMode = 0x18, ChPower = 0x19, ChTot = 0x1B;
    public const int ChRxTone = 0x20, ChTxTone = 0x22, ChLflag1 = 0x26, ChDmrId = 0x27;
    public const int ChRxGroup = 0x2B, ChCc = 0x2C, ChContact = 0x2E, ChFlag2 = 0x31, ChFlag4 = 0x33;
    public const int Flag2Ts2 = 0x40, Flag4ZoneSkip = 0x20, Flag4RxOnly = 0x04, Flag425K = 0x02, Lflag1Id = 0x80;
    public const int ZoneSlots = 68, ZoneBitmap = EeBase + 0x8010, ZoneData = EeBase + 0x8030, ZoneLayout = EeBase + 0x806F;
    public const int ContactBase = FlBase + 0x87620, ContactSize = 24, ContactCount = 1024;
    public const int ListCounts = FlBase + 0x8D620, ListBase = FlBase + 0x8D6A0, ListSize = 80, ListCount = 76;
    public const int GenCallsign = EeBase + 0xE0, GenDmrId = EeBase + 0xE8;
    public const int NacAny = 0xF7E, NacDefault = 0x293;
    public const int KvRoot = EeBase + 0xE000;
    public const uint KvMagic = 0x564B4652;            // "RFKV"
    public const int KvBankA = 0xAF000, KvBankEnd = 0xCF000, KvPage = 0x1000, KvPages = 33;
    public const int KeyChTag = 0x0400, KeyModeSet = 0x0A00, KindP25 = 2;
    public const int TagTx = 0x01, TagBaseOwn = 0x02, TagFallback = 0x04, P25NacSet = 0x8000;
    public const int ModeP25Old = 2, ModeP25R7 = 0xA5;
    public static readonly string[] PowerNames = ["L-", "L", "L+", "L++", "M", "M+", "M++", "H-", "H"];
    public static readonly string[] SimpleChannelKeys = ["number", "name", "freq", "mode", "summary"];
    public static readonly string[] DerivedKeys = ["freq", "summary"];

    public List<ChannelRow> Channels { get; } = [];
    public List<ZoneRow> Zones { get; } = [];
    public List<ContactRow> Contacts { get; } = [];
    public List<ListRow> Lists { get; } = [];
    public string RadioName { get; private set; } = "";
    public long DmrId { get; private set; }
    public int ZoneCapacity { get; private set; } = 80;
    public int BadRecords { get; private set; }
    public int RfkvEntries { get; private set; }
    public CodeplugText Text { get; private set; } = CodeplugText.Tr;

    public Dictionary<string, int> ModeCounts()
    {
        var d = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var c in Channels)
            d[c.Mode] = d.GetValueOrDefault(c.Mode) + 1;
        return d;
    }

    // --- bytes --------------------------------------------------------------------------------------------------------
    private static int Le16(ReadOnlySpan<byte> b, int o) => b[o] | (b[o + 1] << 8);

    private static uint Le32(ReadOnlySpan<byte> b, int o) => BinaryPrimitives.ReadUInt32LittleEndian(b[o..]);

    /// <summary>A BCD word of digits, or null when a nibble is not a digit.</summary>
    public static long? Bcd(long word, int digits)
    {
        long v = 0;
        for (var i = digits - 1; i >= 0; i--)
        {
            var d = (word >> (4 * i)) & 0xF;
            if (d > 9)
                return null;
            v = v * 10 + d;
        }
        return v;
    }

    private static long? FreqHz(ReadOnlySpan<byte> r, int off)
    {
        var v = Bcd(Le32(r, off), 8);
        return v is null or 0 ? null : v * 10;
    }

    private static long BcdId(ReadOnlySpan<byte> b, int o)
    {
        var v = Bcd(BinaryPrimitives.ReadUInt32BigEndian(b[o..]), 8);
        return v is null || v > 0xFFFFFF ? 0 : v.Value;
    }

    /// <summary>A name: Latin-1 bytes up to the first 0x00 or 0xFF.</summary>
    public static string Name(ReadOnlySpan<byte> b, int o, int n)
    {
        var s = b.Slice(o, n);
        var end = 0;
        while (end < s.Length && s[end] is not (0x00 or 0xFF))
            end++;
        return Encoding.Latin1.GetString(s[..end]);
    }

    /// <summary>"" none, "67.0" CTCSS, "D023N" / "D023I" DCS (codeplug.c tone()).</summary>
    public static string Tone(int v)
    {
        if (v is 0 or 0xFFFF)
            return "";
        if ((v & 0x8000) != 0)
        {
            var code = 0;
            for (var i = 2; i >= 0; i--)
            {
                var d = (v >> (4 * i)) & 0xF;
                if (d > 7)
                    return "";
                code = code * 8 + d;
            }
            return "D" + Convert.ToString(code, 8).PadLeft(3, '0') + ((v & 0x4000) != 0 ? "I" : "N");
        }
        if ((v & 0x4000) != 0)
            return "";
        var tenths = Bcd(v, 4);
        if (tenths is null || tenths < 300 || tenths > 3000)
            return "";
        return string.Create(CultureInfo.InvariantCulture, $"{tenths / 10}.{tenths % 10}");
    }

    private static string PowerName(int b, CodeplugText t) =>
        b == 0 ? t.Default : PowerNames[Math.Min(b - 1, PowerNames.Length - 1)];

    private static string Mhz(long? hz) => hz is null ? "" : (hz.Value / 1e6).ToString("0.00000", CultureInfo.InvariantCulture);

    private static string Hex2(int b) => string.Create(CultureInfo.InvariantCulture, $"0x{b:X2}");

    private static string S(long v) => v.ToString(CultureInfo.InvariantCulture);

    // --- the RFKV codeplug settings area --------------------------------------------------------------------------------
    private static int? KvPageAddr(long off)
    {
        if (off == 0xE000)
            return KvRoot;
        if (off >= KvBankA && off < KvBankEnd && off % KvPage == 0)
            return (int)(FlBase + off - 0x20000);
        return null;
    }

    /// <summary>The RFKV area, every page checked as the firmware checks it: {key: value}, the first entry of a key
    /// counting; empty when there is none or it is damaged.</summary>
    public static Dictionary<int, byte[]> ReadKv(ReadOnlySpan<byte> image)
    {
        var out_ = new Dictionary<int, byte[]>();
        long off = 0xE000;
        var seen = new HashSet<long>();
        for (var p = 0; p < KvPages; p++)
        {
            if (KvPageAddr(off) is not { } a || !seen.Add(off))
                return [];
            var h = image.Slice(a, 16);
            if (Le32(h, 0) != KvMagic || h[4] != 1 || h[5] < 16 || h[5] + Le16(h, 6) > KvPage)
                return [];
            var data = image.Slice(a + h[5], Le16(h, 6));
            if (Crc32.Compute(data) != Le32(h, 8))
                return [];
            var at = 0;
            while (at < data.Length)
            {
                if (data.Length - at < 4 || Le16(data, at + 2) > data.Length - at - 4)
                    return [];
                int key = Le16(data, at), n = Le16(data, at + 2);
                out_.TryAdd(key, data.Slice(at + 4, n).ToArray());
                at += 4 + n;
            }
            off = Le32(h, 12);
            if (off == 0)
                return out_;
        }
        return [];
    }

    /// <summary>codeplug_tag.c codeplug_name_check over a record's 16 name bytes (0xFF padding ends the name).</summary>
    public static int NameCheck(ReadOnlySpan<byte> rawName)
    {
        uint h = 2166136261;
        var end = false;
        foreach (var b in rawName[..16])
        {
            var c = end || b == 0xFF ? (byte)0 : b;
            end = end || c == 0;
            h = unchecked((h ^ c) * 16777619);
        }
        return (int)((h ^ (h >> 16)) & 0xFFFF);
    }

    private static (int Kind, int Flags, byte[] P)? P25Tag(Dictionary<int, byte[]> kv, int number, ReadOnlySpan<byte> r)
    {
        if (!kv.TryGetValue(KeyChTag + number - 1, out var v) || v.Length < 5 || v[0] is 0 or 0xFF)
            return null;
        var at = (v[4] & TagFallback) != 0 ? 9 : 5;
        if (v.Length < at || Le16(v, 2) != NameCheck(r.Slice(ChName, 16)))
            return null;
        if (!((v[4] & TagBaseOwn) != 0 || (r[ChMode] == 1 && (r[ChFlag4] & Flag4RxOnly) != 0)))
            return null;
        return (v[0], v[4], v[at..]);
    }

    private static int NacField(int f, int fallback) =>
        (f & P25NacSet) != 0 && (f & 0x7000) == 0 ? f & 0xFFF : fallback;

    // --- channels ------------------------------------------------------------------------------------------------------
    /// <summary>One 56-byte channel record as a row, or null when its frequencies are malformed (not a channel).</summary>
    public static ChannelRow? DecodeChannel(int number, ReadOnlySpan<byte> r, Dictionary<int, byte[]>? kv, long radioId, CodeplugText t)
    {
        kv ??= [];
        var rx = FreqHz(r, ChRx);
        var tx = FreqHz(r, ChTx);
        if (rx is null || tx is null)
            return null;
        int mb = r[ChMode];
        var mode = mb switch
        {
            0 => "FM", 1 => "DMR", 2 or 0xA5 => "P25", 0xA6 => "YSF", 0xA7 => "NXDN", 0xA8 => "dPMR",
            _ => string.Format(CultureInfo.InvariantCulture, t.UnknownMode, Hex2(mb)),
        };
        var tag = mb == 1 ? P25Tag(kv, number, r) : null;
        if (tag is { } tg0)
            mode = tg0.Kind == KindP25 ? "P25" : string.Format(CultureInfo.InvariantCulture, t.UnknownKind, tg0.Kind);
        int cc = r[ChCc];
        var row = new ChannelRow { Number = number, Mode = mode };
        var v = row.Values;
        foreach (var (k, _) in t.ChannelColumns)
            v[k] = "";
        bool rxOnly = (r[ChFlag4] & Flag4RxOnly) != 0, anyList = false;
        string? txNac = null;               // null: none or "Same as RX"
        v["number"] = S(number);
        v["name"] = Name(r, ChName, 16);
        v["rx_mhz"] = Mhz(rx);
        v["tx_mhz"] = Mhz(tx);
        v["mode"] = mode;
        v["mode_byte"] = Hex2(mb);
        v["bw"] = (r[ChFlag4] & Flag425K) != 0 ? "25 kHz" : "12.5 kHz";
        v["power"] = PowerName(r[ChPower], t);
        v["scan_skip"] = (r[ChFlag4] & Flag4ZoneSkip) != 0 ? t.Yes : "";
        v["tot_s"] = r[ChTot] != 0 ? S(r[ChTot] * 15) : "";
        long unit = (r[ChLflag1] & Lflag1Id) != 0 ? (r[ChDmrId] << 16) | (r[ChDmrId + 1] << 8) | r[ChDmrId + 2] : 0;
        if (mode == "FM")
        {
            v["rx_tone"] = Tone(Le16(r, ChRxTone));
            v["tx_tone"] = Tone(Le16(r, ChTxTone));
        }
        else if (mode == "DMR")
        {
            v["cc"] = (cc & 0x80) != 0 ? t.Any : S(cc & 0x0F);
            v["ts"] = (cc & 0x40) != 0 ? t.Any : (r[ChFlag2] & Flag2Ts2) != 0 ? "2" : "1";
            v["contact"] = Le16(r, ChContact) is var c and not 0 ? S(c) : "";
            anyList = (cc & 0x20) != 0;
            v["rx_list"] = anyList ? t.AllTalkgroups : r[ChRxGroup] != 0 ? S(r[ChRxGroup]) : "";
            v["unit_id"] = unit != 0 ? S(unit) : "";
            if ((cc & 0xC0) != 0)
                rxOnly = true;              // any CC / TS: the firmware allows no TX
        }
        else if (mode == "P25" && tag is { } tg)
        {
            var p = tg.P;
            var squelch = p.Length > 0 ? p[0] & 3 : 0;
            var rxNac = NacField(p.Length >= 3 ? Le16(p, 1) : 0, NacDefault);
            if (rxNac == 0xF7F)
                rxNac = NacDefault;
            var txField = p.Length >= 5 ? Le16(p, 3) : 0;
            v["mode_byte"] = "0x01 + RFKV";
            v["nac"] = t.Nac(rxNac);
            txNac = (txField & P25NacSet) == 0 ? null : t.Nac(NacField(txField, rxNac));
            v["tx_nac"] = txNac ?? t.SameAsRx;
            if (p.Length >= 8 && (p[5] & 1) != 0)
                v["tg"] = Le16(p, 6) is var g and not 0 ? S(g) : t.Any;
            else
                v["tg"] = Le16(r, ChContact) is var c and not 0 ? string.Format(CultureInfo.InvariantCulture, t.ContactTg, c) : "1";
            if (squelch != 1)
                v["tg"] += t.AllGroups;
            long p25Id = 0;
            if (kv.TryGetValue(KeyModeSet + KindP25, out var mset) && mset.Length >= 4 && Le32(mset, 0) is >= 1 and <= 0xFFFFFE)
                p25Id = Le32(mset, 0);
            var id = unit != 0 ? unit : p25Id != 0 ? p25Id : radioId;
            v["unit_id"] = id != 0 ? S(id) : "";
            rxOnly = (tg.Flags & TagTx) == 0;
            v["bw"] = "12.5 kHz";
        }
        else if (mode == "P25" && mb == ModeP25Old)
        {
            int rxNac = Le16(r, ChRxTone), txn = Le16(r, ChTxTone);
            if (rxNac > 0xFFF || rxNac == 0xF7F)
                rxNac = NacAny;
            if (txn >= 0xF7E)
                txn = 0x293;
            var g = Le16(r, ChContact);
            unit = unit <= 0xFFFFFE ? unit : 0;
            v["nac"] = t.Nac(rxNac) + t.Mode2Form;
            txNac = txn == rxNac ? null : t.Nac(txn);
            v["tx_nac"] = txNac ?? t.SameAsRx;
            v["tg"] = g != 0 ? S(g) : t.Any;
            if (g != 0 && (r[ChFlag2] & Flag2Ts2) != 0)
                v["tg"] += t.AllGroups;
            v["unit_id"] = unit != 0 ? S(unit) : "";
            v["bw"] = "12.5 kHz";
            if (g == 0 || unit == 0)
                rxOnly = true;
        }
        else if (mode == "P25")
        {
            var nac = ((cc & 0x0F) << 8) | r[ChRxGroup];
            v["nac"] = t.Nac(nac) + ((cc & 0xF0) == 0xD0 ? "" : t.BadRecord);
            v["tx_nac"] = t.SameAsRx;
            var g = Le16(r, ChContact);
            v["tg"] = g != 0 ? S(g) : t.Any;
            v["unit_id"] = unit != 0 ? S(unit) : "";
        }
        else if (mode == "YSF")
        {
            var dg = r[ChRxGroup] & 0x7F;
            v["code"] = "DG-ID " + (dg != 0 ? S(dg) : t.AnyLower);
            rxOnly = true;
        }
        else if (mode == "NXDN")
        {
            var ran = r[ChRxGroup] & 0x3F;
            v["code"] = "RAN " + (ran != 0 ? S(ran) : t.AnyLower);
            rxOnly = true;
        }
        else if (mode == "dPMR")
        {
            var c = r[ChRxGroup] & 0x7F;
            v["code"] = "CC " + (c != 0 ? S(c - 1) : t.AnyLower);
            rxOnly = true;
        }
        v["rx_only"] = rxOnly ? t.Yes : "";
        v["freq"] = v["rx_mhz"] == v["tx_mhz"] ? ShortMhz(v["rx_mhz"]) : $"{ShortMhz(v["rx_mhz"])} / TX {ShortMhz(v["tx_mhz"])}";
        v["summary"] = Summary(v, mode, anyList, txNac, rxOnly, t);
        return row;
    }

    /// <summary>"145.50000" -> "145.500"; "446.10625" kept.</summary>
    public static string ShortMhz(string text)
    {
        while (text.EndsWith('0') && text.Length - text.IndexOf('.', StringComparison.Ordinal) - 1 > 3)
            text = text[..^1];
        return text;
    }

    private static string Summary(Dictionary<string, string> v, string mode, bool anyList, string? txNac, bool rxOnly, CodeplugText t)
    {
        var parts = new List<string>();
        if (mode == "FM")
        {
            var (rt, tt) = (v["rx_tone"], v["tx_tone"]);
            if (rt.Length == 0 && tt.Length == 0)
                parts.Add(t.NoTone);
            else if (rt == tt)
                parts.Add(t.Tone + rt);
            else
                parts.Add(string.Format(CultureInfo.InvariantCulture, t.ToneRxTx, rt.Length > 0 ? rt : "-", tt.Length > 0 ? tt : "-"));
        }
        else if (mode == "DMR")
        {
            parts.Add("CC " + v["cc"]);
            parts.Add("TS " + v["ts"]);
            if (anyList)
                parts.Add(t.AllTg);
            else if (v["contact"].Length > 0)
                parts.Add(t.Contact + v["contact"]);
        }
        else if (mode == "P25")
        {
            parts.Add("NAC " + (txNac is null ? v["nac"] : v["nac"] + " > TX " + txNac));
            parts.Add("TG " + v["tg"]);
        }
        else if (v["code"].Length > 0)
            parts.Add(v["code"]);
        if (rxOnly)
            parts.Add(t.RxOnly);
        return string.Join(" · ", parts);
    }

    public static int BankBitmap(int bank) => bank == 0 ? Bank0Bitmap : Bank1Bitmap + (bank - 1) * BankStride;

    public static int ChannelAddr(int number)
    {
        var i = number - 1;
        return BankBitmap(i / ChPerBank) + 16 + i % ChPerBank * ChSize;
    }

    // --- the whole image -----------------------------------------------------------------------------------------------
    /// <summary>Decodes a 16 MiB data-flash image (q128-pass1.bin) with the words of one language.</summary>
    public static Codeplug Decode(ReadOnlySpan<byte> image, CodeplugText? text = null)
    {
        var t = text ?? CodeplugText.Tr;
        if (image.Length != ImageSize)
            throw new CodeplugError(string.Create(CultureInfo.InvariantCulture,
                $"The data flash image must be 16 MiB (q128-pass1.bin); this one is {image.Length} bytes"));
        var cp = new Codeplug { Text = t };
        var used = new HashSet<int>();
        var kv = ReadKv(image);
        cp.DmrId = BcdId(image, GenDmrId);
        cp.RfkvEntries = kv.Count;
        for (var bank = 0; bank < ChBanks; bank++)
        {
            var bits = image.Slice(BankBitmap(bank), ChPerBank / 8);
            for (var k = 0; k < ChPerBank; k++)
            {
                if ((bits[k / 8] & (1 << (k % 8))) == 0)
                    continue;
                var number = bank * ChPerBank + k + 1;
                var rec = image.Slice(ChannelAddr(number), ChSize);
                var row = DecodeChannel(number, rec, kv, cp.DmrId, t);
                if (row is null)
                {
                    if (rec.ContainsAnyExcept((byte)0xFF))
                        cp.BadRecords++;
                    continue;
                }
                used.Add(number);
                cp.Channels.Add(row);
            }
        }
        var names = cp.Channels.ToDictionary(c => c.Number, c => c["name"]);
        var cap = image[ZoneLayout] <= 4 ? 80 : 16;
        cp.ZoneCapacity = cap;
        var record = 16 + 2 * cap;
        var zbits = image.Slice(ZoneBitmap, (ZoneSlots + 7) / 8);
        for (var slot = 0; slot < ZoneSlots; slot++)
        {
            if ((zbits[slot / 8] & (1 << (slot % 8))) == 0)
                continue;
            var a = ZoneData + slot * record;
            if (image[a] is 0x00 or 0xFF)
                continue;
            var members = new List<int>();
            for (var i = 0; i < cap; i++)
            {
                var n = Le16(image, a + 16 + 2 * i);
                if (n == 0)
                    break;
                if (used.Contains(n))
                    members.Add(n);
            }
            cp.Zones.Add(new ZoneRow(cp.Zones.Count + 1, slot, Name(image, a, 16), members,
                string.Join(", ", members.Select(n => $"{S(n)} {names.GetValueOrDefault(n, "")}"))));
        }
        for (var i = 0; i < ContactCount; i++)
        {
            var r = image.Slice(ContactBase + i * ContactSize, ContactSize);
            if (r[0] == 0xFF || r[0x14] > 2)
                continue;
            int kind = r[0x14];
            var cid = kind == 2 ? 0xFFFFFF : BcdId(r, 0x10);
            if (cid == 0)
                continue;
            var ts = "";
            if (r[0x17] != 0xFF && (r[0x17] & 1) == 0)
                ts = (r[0x17] & 2) != 0 ? "2" : "1";
            cp.Contacts.Add(new ContactRow(i + 1, Name(r, 0, 16), cid, t.ContactTypes[kind], ts));
        }
        var contactNames = cp.Contacts.ToDictionary(c => c.Index, c => c.Name);
        for (var i = 0; i < ListCount; i++)
        {
            var n = image[ListCounts + i];
            if (n is 0 or 0xFF)
                continue;
            var a = ListBase + i * ListSize;
            var members = new List<int>();
            for (var k = 0; k < Math.Min(n - 1, 32); k++)
            {
                var c = Le16(image, a + 16 + 2 * k);
                if (c == 0 || c > ContactCount)
                    break;
                members.Add(c);
            }
            cp.Lists.Add(new ListRow(i + 1, Name(image, a, 16), members,
                string.Join(", ", members.Select(c => contactNames.TryGetValue(c, out var nm) ? nm : "#" + S(c)))));
        }
        cp.RadioName = Name(image, GenCallsign, 8);
        return cp;
    }

    /// <summary>The data flash image of a backup folder (q128-pass1.bin), or a 16 MiB image file itself.</summary>
    public static byte[] LoadImage(string path)
    {
        var file = Directory.Exists(path) ? Path.Combine(path, "q128-pass1.bin") : path;
        var len = new FileInfo(file).Length;
        if (len != ImageSize)
            throw new CodeplugError(string.Create(CultureInfo.InvariantCulture,
                $"The data flash image must be 16 MiB (q128-pass1.bin); this one is {len} bytes"));
        return File.ReadAllBytes(file);
    }

    // --- tables --------------------------------------------------------------------------------------------------------
    public enum Table
    {
        Channels,
        Zones,
        Contacts,
        Lists,
    }

    public (string Key, string Title)[] Columns(Table table) => table switch
    {
        Table.Channels => Text.ChannelColumns,
        Table.Zones => Text.ZoneColumns,
        Table.Contacts => Text.ContactColumns,
        _ => Text.ListColumns,
    };

    /// <summary>Every row of a table as {column key: text}.</summary>
    public IEnumerable<IReadOnlyDictionary<string, string>> Rows(Table table) => table switch
    {
        Table.Channels => Channels.Select(c => (IReadOnlyDictionary<string, string>)c.Values),
        Table.Zones => Zones.Select(z => Dict(("index", S(z.Index)), ("name", z.Name), ("count", S(z.Count)), ("channel_names", z.ChannelNames))),
        Table.Contacts => Contacts.Select(c => Dict(("index", S(c.Index)), ("name", c.Name), ("id", S(c.Id)), ("type", c.Type), ("ts", c.Ts))),
        _ => Lists.Select(l => Dict(("index", S(l.Index)), ("name", l.Name), ("count", S(l.Count)), ("contact_names", l.ContactNames))),
    };

    private static IReadOnlyDictionary<string, string> Dict(params (string K, string V)[] kv) =>
        kv.ToDictionary(x => x.K, x => x.V, StringComparer.Ordinal);

    /// <summary>A row matches a search text when any of its texts holds it, ignoring case ("" matches every row).</summary>
    public static bool Matches(IReadOnlyDictionary<string, string> row, string? search)
    {
        if (string.IsNullOrWhiteSpace(search))
            return true;
        var s = search.Trim();
        return row.Values.Any(v => v.Contains(s, StringComparison.CurrentCultureIgnoreCase) ||
                                   v.Contains(s, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>The channels a zone holds, in the zone's order (members that are channels).</summary>
    public IEnumerable<ChannelRow> ZoneChannels(ZoneRow zone)
    {
        var by = Channels.ToDictionary(c => c.Number);
        return zone.Channels.Where(by.ContainsKey).Select(n => by[n]);
    }

    /// <summary>codeplug.to_csv: the column titles, then one line per row, "\n" line ends, quoted only when needed
    /// (Python's csv.writer). The file is written UTF-8 with a BOM, as the Python viewer did (Excel reads it).</summary>
    public static string ToCsv((string Key, string Title)[] columns, IEnumerable<IReadOnlyDictionary<string, string>> rows)
    {
        var sb = new StringBuilder();
        sb.Append(string.Join(',', columns.Select(c => CsvField(c.Title)))).Append('\n');
        foreach (var r in rows)
            sb.Append(string.Join(',', columns.Select(c => CsvField(r.TryGetValue(c.Key, out var v) ? v : "")))).Append('\n');
        return sb.ToString();
    }

    private static string CsvField(string v) =>
        v.IndexOfAny([',', '"', '\n', '\r']) >= 0 ? "\"" + v.Replace("\"", "\"\"", StringComparison.Ordinal) + "\"" : v;

    public static void WriteCsv(string path, string csv) => File.WriteAllText(path, csv, new UTF8Encoding(true));
}
