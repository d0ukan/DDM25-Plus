// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only decoder of the DDM32 Plus codeplug in a data-flash (q128, 16 MiB) backup image: a port of the C#
//! Codeplug/Codeplug.cs, itself a port of pc/ddm32_app/codeplug.py (its module text has the layout). Channels 1-1024,
//! 68 zone slots of 80 channels (16 with the old layout byte), 1024 contacts, 76 RX group lists, the radio name and DMR
//! ID; FM, DMR, YSF, NXDN, dPMR and P25 in its three forms. The NAC as the firmware shows it: "659 ($293)". Held to
//! codeplug-results.json.gz. Nothing here writes anything.

use crate::radio::monitor::crc32;
use std::collections::{HashMap, HashSet};
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodeplugError(pub String);

impl std::fmt::Display for CodeplugError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for CodeplugError {}

/// One channel: its number, its mode and one text per column.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ChannelRow {
    pub number: i64,
    pub mode: String,
    pub values: HashMap<String, String>,
}

impl ChannelRow {
    pub fn get(&self, key: &str) -> &str {
        self.values.get(key).map_or("", String::as_str)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ZoneRow {
    pub index: i64,
    pub slot: i64,
    pub name: String,
    pub channels: Vec<i64>,
    pub channel_names: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ContactRow {
    pub index: i64,
    pub name: String,
    pub id: i64,
    pub kind: String,
    pub ts: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ListRow {
    pub index: i64,
    pub name: String,
    pub contacts: Vec<i64>,
    pub contact_names: String,
}

/// The words the viewer writes into its rows and column titles, in one language (Turkish: the Python viewer's).
#[derive(Debug, Clone)]
pub struct CodeplugText {
    pub any: &'static str,
    pub any_lower: &'static str,
    pub yes: &'static str,
    pub default: &'static str,
    pub unknown_mode: &'static str,
    pub unknown_kind: &'static str,
    pub same_as_rx: &'static str,
    pub all_talkgroups: &'static str,
    pub contact_tg: &'static str,
    pub all_groups: &'static str,
    pub mode2_form: &'static str,
    pub bad_record: &'static str,
    pub no_tone: &'static str,
    pub tone: &'static str,
    pub tone_rx_tx: &'static str,
    pub all_tg: &'static str,
    pub contact: &'static str,
    pub rx_only: &'static str,
    pub contact_types: [&'static str; 3],
    pub channel_columns: &'static [(&'static str, &'static str)],
    pub zone_columns: &'static [(&'static str, &'static str)],
    pub contact_columns: &'static [(&'static str, &'static str)],
    pub list_columns: &'static [(&'static str, &'static str)],
}

pub const TR: CodeplugText = CodeplugText {
    any: "Herhangi",
    any_lower: "herhangi",
    yes: "Evet",
    default: "Varsayılan",
    unknown_mode: "Bilinmiyor ({0})",
    unknown_kind: "Bilinmiyor (RFKV tür {0})",
    same_as_rx: "RX ile aynı",
    all_talkgroups: "Herhangi (tüm TG)",
    contact_tg: "kişi {0}",
    all_groups: " (tüm gruplar)",
    mode2_form: "  (CH_MODE 2 biçimi)",
    bad_record: "  (geçersiz kayıt)",
    no_tone: "Ton yok",
    tone: "Ton ",
    tone_rx_tx: "Ton RX {0} / TX {1}",
    all_tg: "tüm TG",
    contact: "Kişi ",
    rx_only: "sadece RX",
    contact_types: ["Grup", "Özel", "Herkes"],
    channel_columns: &[
        ("number", "No"),
        ("name", "Ad"),
        ("freq", "Frekans"),
        ("mode", "Mod"),
        ("summary", "Ayar"),
        ("rx_mhz", "RX MHz"),
        ("tx_mhz", "TX MHz"),
        ("bw", "Bant"),
        ("power", "Güç"),
        ("rx_only", "Sadece RX"),
        ("rx_tone", "RX ton"),
        ("tx_tone", "TX ton"),
        ("cc", "CC"),
        ("ts", "TS"),
        ("nac", "NAC"),
        ("tx_nac", "TX NAC"),
        ("tg", "TG"),
        ("contact", "Kişi"),
        ("rx_list", "RX grup listesi"),
        ("code", "DG-ID / RAN / CC"),
        ("unit_id", "ID"),
        ("scan_skip", "Taramada atla"),
        ("tot_s", "TOT sn"),
        ("mode_byte", "CH_MODE"),
    ],
    zone_columns: &[("index", "No"), ("name", "Ad"), ("count", "Kanal sayısı"), ("channel_names", "Kanallar")],
    contact_columns: &[("index", "No"), ("name", "Ad"), ("id", "ID / TG"), ("type", "Tür"), ("ts", "TS")],
    list_columns: &[("index", "No"), ("name", "Ad"), ("count", "Üye sayısı"), ("contact_names", "Kişiler")],
};

pub const EN: CodeplugText = CodeplugText {
    any: "Any",
    any_lower: "any",
    yes: "Yes",
    default: "Default",
    unknown_mode: "Unknown ({0})",
    unknown_kind: "Unknown (RFKV kind {0})",
    same_as_rx: "Same as RX",
    all_talkgroups: "Any (all TGs)",
    contact_tg: "contact {0}",
    all_groups: " (all groups)",
    mode2_form: "  (CH_MODE 2 form)",
    bad_record: "  (invalid record)",
    no_tone: "No tone",
    tone: "Tone ",
    tone_rx_tx: "Tone RX {0} / TX {1}",
    all_tg: "all TGs",
    contact: "Contact ",
    rx_only: "RX only",
    contact_types: ["Group", "Private", "All call"],
    channel_columns: &[
        ("number", "No"),
        ("name", "Name"),
        ("freq", "Frequency"),
        ("mode", "Mode"),
        ("summary", "Settings"),
        ("rx_mhz", "RX MHz"),
        ("tx_mhz", "TX MHz"),
        ("bw", "Bandwidth"),
        ("power", "Power"),
        ("rx_only", "RX only"),
        ("rx_tone", "RX tone"),
        ("tx_tone", "TX tone"),
        ("cc", "CC"),
        ("ts", "TS"),
        ("nac", "NAC"),
        ("tx_nac", "TX NAC"),
        ("tg", "TG"),
        ("contact", "Contact"),
        ("rx_list", "RX group list"),
        ("code", "DG-ID / RAN / CC"),
        ("unit_id", "ID"),
        ("scan_skip", "Skip in scan"),
        ("tot_s", "TOT s"),
        ("mode_byte", "CH_MODE"),
    ],
    zone_columns: &[("index", "No"), ("name", "Name"), ("count", "Channels"), ("channel_names", "Channel list")],
    contact_columns: &[("index", "No"), ("name", "Name"), ("id", "ID / TG"), ("type", "Type"), ("ts", "TS")],
    list_columns: &[("index", "No"), ("name", "Name"), ("count", "Members"), ("contact_names", "Contacts")],
};

impl CodeplugText {
    pub fn for_lang(lang: &str) -> &'static CodeplugText {
        if lang == "en" {
            &EN
        } else {
            &TR
        }
    }

    /// The firmware's NAC form (CONFIG_NAC_DEC): "659 ($293)", "Any 3966 ($F7E)".
    pub fn nac(&self, nac: i64) -> String {
        if nac == NAC_ANY {
            format!("{} 3966 ($F7E)", self.any)
        } else {
            format!("{nac} (${nac:03X})")
        }
    }
}

fn fmt1(t: &str, a: &str) -> String {
    t.replace("{0}", a)
}

pub const IMAGE_SIZE: usize = 1 << 24;
pub const EE_BASE: usize = 0x700000;
pub const FL_BASE: usize = 0x720000;
pub const CH_PER_BANK: usize = 128;
pub const CH_BANKS: usize = 8;
pub const CH_SIZE: usize = 56;
pub const MAX_CHANNELS: usize = CH_PER_BANK * CH_BANKS;
pub const BANK0_BITMAP: usize = EE_BASE + 0x3780;
pub const BANK1_BITMAP: usize = FL_BASE + 0x7B1B0;
pub const BANK_STRIDE: usize = 16 + CH_PER_BANK * CH_SIZE;
const CH_NAME: usize = 0x00;
const CH_RX: usize = 0x10;
const CH_TX: usize = 0x14;
const CH_MODE: usize = 0x18;
const CH_POWER: usize = 0x19;
const CH_TOT: usize = 0x1B;
const CH_RX_TONE: usize = 0x20;
const CH_TX_TONE: usize = 0x22;
const CH_LFLAG1: usize = 0x26;
const CH_DMR_ID: usize = 0x27;
const CH_RX_GROUP: usize = 0x2B;
const CH_CC: usize = 0x2C;
const CH_CONTACT: usize = 0x2E;
const CH_FLAG2: usize = 0x31;
const CH_FLAG4: usize = 0x33;
const FLAG2_TS2: u8 = 0x40;
const FLAG4_ZONE_SKIP: u8 = 0x20;
const FLAG4_RX_ONLY: u8 = 0x04;
const FLAG4_25K: u8 = 0x02;
const LFLAG1_ID: u8 = 0x80;
pub const ZONE_SLOTS: usize = 68;
const ZONE_BITMAP: usize = EE_BASE + 0x8010;
const ZONE_DATA: usize = EE_BASE + 0x8030;
const ZONE_LAYOUT: usize = EE_BASE + 0x806F;
const CONTACT_BASE: usize = FL_BASE + 0x87620;
const CONTACT_SIZE: usize = 24;
const CONTACT_COUNT: usize = 1024;
const LIST_COUNTS: usize = FL_BASE + 0x8D620;
const LIST_BASE: usize = FL_BASE + 0x8D6A0;
const LIST_SIZE: usize = 80;
const LIST_COUNT: usize = 76;
const GEN_CALLSIGN: usize = EE_BASE + 0xE0;
const GEN_DMR_ID: usize = EE_BASE + 0xE8;
pub const NAC_ANY: i64 = 0xF7E;
pub const NAC_DEFAULT: i64 = 0x293;
const KV_ROOT: usize = EE_BASE + 0xE000;
const KV_MAGIC: u32 = 0x564B_4652; // "RFKV"
const KV_BANK_A: u64 = 0xAF000;
const KV_BANK_END: u64 = 0xCF000;
const KV_PAGE: u64 = 0x1000;
const KV_PAGES: usize = 33;
const KEY_CH_TAG: u16 = 0x0400;
const KEY_MODE_SET: u16 = 0x0A00;
const KIND_P25: u8 = 2;
const TAG_TX: u8 = 0x01;
const TAG_BASE_OWN: u8 = 0x02;
const TAG_FALLBACK: u8 = 0x04;
const P25_NAC_SET: i64 = 0x8000;
const MODE_P25_OLD: u8 = 2;
pub const POWER_NAMES: [&str; 9] = ["L-", "L", "L+", "L++", "M", "M+", "M++", "H-", "H"];

fn le16(b: &[u8], o: usize) -> i64 {
    b[o] as i64 | (b[o + 1] as i64) << 8
}

fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// A BCD word of digits, or None when a nibble is not a digit.
pub fn bcd(word: i64, digits: u32) -> Option<i64> {
    let mut v = 0i64;
    for i in (0..digits).rev() {
        let d = (word >> (4 * i)) & 0xF;
        if d > 9 {
            return None;
        }
        v = v * 10 + d;
    }
    Some(v)
}

fn freq_hz(r: &[u8], off: usize) -> Option<i64> {
    bcd(le32(r, off) as i64, 8).filter(|&v| v != 0).map(|v| v * 10)
}

fn bcd_id(b: &[u8], o: usize) -> i64 {
    let w = u32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) as i64;
    bcd(w, 8).filter(|&v| v <= 0xFFFFFF).unwrap_or(0)
}

/// A name: Latin-1 bytes up to the first 0x00 or 0xFF.
pub fn name(b: &[u8], o: usize, n: usize) -> String {
    b[o..o + n].iter().take_while(|&&c| c != 0 && c != 0xFF).map(|&c| c as char).collect()
}

/// "" none, "67.0" CTCSS, "D023N" / "D023I" DCS (codeplug.c tone()).
pub fn tone(v: i64) -> String {
    if v == 0 || v == 0xFFFF {
        return String::new();
    }
    if v & 0x8000 != 0 {
        let mut code = 0;
        for i in (0..3).rev() {
            let d = (v >> (4 * i)) & 0xF;
            if d > 7 {
                return String::new();
            }
            code = code * 8 + d;
        }
        return format!("D{code:03o}{}", if v & 0x4000 != 0 { "I" } else { "N" });
    }
    if v & 0x4000 != 0 {
        return String::new();
    }
    match bcd(v, 4) {
        Some(t) if (300..=3000).contains(&t) => format!("{}.{}", t / 10, t % 10),
        _ => String::new(),
    }
}

fn power_name(b: u8, t: &CodeplugText) -> String {
    if b == 0 {
        t.default.into()
    } else {
        POWER_NAMES[(b as usize - 1).min(POWER_NAMES.len() - 1)].into()
    }
}

fn mhz(hz: Option<i64>) -> String {
    hz.map_or(String::new(), |h| format!("{:.5}", h as f64 / 1e6))
}

fn kv_page_addr(off: u64) -> Option<usize> {
    if off == 0xE000 {
        return Some(KV_ROOT);
    }
    if (KV_BANK_A..KV_BANK_END).contains(&off) && off.is_multiple_of(KV_PAGE) {
        return Some(FL_BASE + off as usize - 0x20000);
    }
    None
}

/// The RFKV area, every page checked as the firmware checks it: {key: value}, the first entry of a key counting;
/// empty when there is none or it is damaged.
pub fn read_kv(image: &[u8]) -> HashMap<u16, Vec<u8>> {
    let mut out = HashMap::new();
    let mut off: u64 = 0xE000;
    let mut seen = HashSet::new();
    for _ in 0..KV_PAGES {
        let Some(a) = kv_page_addr(off) else { return HashMap::new() };
        if !seen.insert(off) {
            return HashMap::new();
        }
        let h = &image[a..a + 16];
        let (hlen, dlen) = (h[5] as u64, le16(h, 6) as u64);
        if le32(h, 0) != KV_MAGIC || h[4] != 1 || hlen < 16 || hlen + dlen > KV_PAGE {
            return HashMap::new();
        }
        let data = &image[a + hlen as usize..a + (hlen + dlen) as usize];
        if crc32(data, 0) != le32(h, 8) {
            return HashMap::new();
        }
        let mut at = 0usize;
        while at < data.len() {
            if data.len() - at < 4 || le16(data, at + 2) as usize > data.len() - at - 4 {
                return HashMap::new();
            }
            let key = le16(data, at) as u16;
            let n = le16(data, at + 2) as usize;
            out.entry(key).or_insert_with(|| data[at + 4..at + 4 + n].to_vec());
            at += 4 + n;
        }
        off = le32(h, 12) as u64;
        if off == 0 {
            return out;
        }
    }
    HashMap::new()
}

/// codeplug_tag.c codeplug_name_check over a record's 16 name bytes (0xFF padding ends the name).
pub fn name_check(raw: &[u8]) -> i64 {
    let mut h: u32 = 2166136261;
    let mut end = false;
    for &b in &raw[..16] {
        let c = if end || b == 0xFF { 0 } else { b };
        end = end || c == 0;
        h = (h ^ c as u32).wrapping_mul(16777619);
    }
    ((h ^ (h >> 16)) & 0xFFFF) as i64
}

fn p25_tag(kv: &HashMap<u16, Vec<u8>>, number: i64, r: &[u8]) -> Option<(u8, u8, Vec<u8>)> {
    let v = kv.get(&(KEY_CH_TAG + number as u16 - 1))?;
    if v.len() < 5 || v[0] == 0 || v[0] == 0xFF {
        return None;
    }
    let at = if v[4] & TAG_FALLBACK != 0 { 9 } else { 5 };
    if v.len() < at || le16(v, 2) != name_check(&r[CH_NAME..CH_NAME + 16]) {
        return None;
    }
    if !(v[4] & TAG_BASE_OWN != 0 || (r[CH_MODE] == 1 && r[CH_FLAG4] & FLAG4_RX_ONLY != 0)) {
        return None;
    }
    Some((v[0], v[4], v[at..].to_vec()))
}

fn nac_field(f: i64, fallback: i64) -> i64 {
    if f & P25_NAC_SET != 0 && f & 0x7000 == 0 {
        f & 0xFFF
    } else {
        fallback
    }
}

/// "145.50000" -> "145.500"; "446.10625" kept.
pub fn short_mhz(text: &str) -> String {
    let mut t = text.to_string();
    while t.ends_with('0') && t.find('.').is_some_and(|d| t.len() - d - 1 > 3) {
        t.pop();
    }
    t
}

/// One 56-byte channel record as a row, or None when its frequencies are malformed (not a channel).
pub fn decode_channel(
    number: i64,
    r: &[u8],
    kv: &HashMap<u16, Vec<u8>>,
    radio_id: i64,
    t: &CodeplugText,
) -> Option<ChannelRow> {
    let rx = freq_hz(r, CH_RX)?;
    let tx = freq_hz(r, CH_TX)?;
    let mb = r[CH_MODE];
    let mut mode = match mb {
        0 => "FM".to_string(),
        1 => "DMR".into(),
        2 | 0xA5 => "P25".into(),
        0xA6 => "YSF".into(),
        0xA7 => "NXDN".into(),
        0xA8 => "dPMR".into(),
        _ => fmt1(t.unknown_mode, &format!("0x{mb:02X}")),
    };
    let tag = if mb == 1 { p25_tag(kv, number, r) } else { None };
    if let Some((kind, _, _)) = &tag {
        mode = if *kind == KIND_P25 { "P25".into() } else { fmt1(t.unknown_kind, &kind.to_string()) };
    }
    let cc = r[CH_CC] as i64;
    let mut v: HashMap<String, String> =
        t.channel_columns.iter().map(|(k, _)| (k.to_string(), String::new())).collect();
    let mut set = |k: &str, s: String| {
        v.insert(k.to_string(), s);
    };
    let mut rx_only = r[CH_FLAG4] & FLAG4_RX_ONLY != 0;
    let mut any_list = false;
    let mut tx_nac: Option<String> = None;
    set("number", number.to_string());
    set("name", name(r, CH_NAME, 16));
    set("rx_mhz", mhz(Some(rx)));
    set("tx_mhz", mhz(Some(tx)));
    set("mode", mode.clone());
    set("mode_byte", format!("0x{mb:02X}"));
    set("bw", if r[CH_FLAG4] & FLAG4_25K != 0 { "25 kHz" } else { "12.5 kHz" }.into());
    set("power", power_name(r[CH_POWER], t));
    set("scan_skip", if r[CH_FLAG4] & FLAG4_ZONE_SKIP != 0 { t.yes.into() } else { String::new() });
    set("tot_s", if r[CH_TOT] != 0 { (r[CH_TOT] as i64 * 15).to_string() } else { String::new() });
    let mut unit: i64 = if r[CH_LFLAG1] & LFLAG1_ID != 0 {
        (r[CH_DMR_ID] as i64) << 16 | (r[CH_DMR_ID + 1] as i64) << 8 | r[CH_DMR_ID + 2] as i64
    } else {
        0
    };
    let nz = |x: i64| if x != 0 { x.to_string() } else { String::new() };
    if mode == "FM" {
        set("rx_tone", tone(le16(r, CH_RX_TONE)));
        set("tx_tone", tone(le16(r, CH_TX_TONE)));
    } else if mode == "DMR" {
        set("cc", if cc & 0x80 != 0 { t.any.into() } else { (cc & 0x0F).to_string() });
        set(
            "ts",
            if cc & 0x40 != 0 {
                t.any.into()
            } else if r[CH_FLAG2] & FLAG2_TS2 != 0 {
                "2".into()
            } else {
                "1".into()
            },
        );
        set("contact", nz(le16(r, CH_CONTACT)));
        any_list = cc & 0x20 != 0;
        set("rx_list", if any_list { t.all_talkgroups.into() } else { nz(r[CH_RX_GROUP] as i64) });
        set("unit_id", nz(unit));
        if cc & 0xC0 != 0 {
            rx_only = true; // any CC / TS: the firmware allows no TX
        }
    } else if let (true, Some((_, flags, p))) = (mode == "P25", &tag) {
        let squelch = p.first().map_or(0, |b| b & 3);
        let mut rx_nac = nac_field(if p.len() >= 3 { le16(p, 1) } else { 0 }, NAC_DEFAULT);
        if rx_nac == 0xF7F {
            rx_nac = NAC_DEFAULT;
        }
        let tx_field = if p.len() >= 5 { le16(p, 3) } else { 0 };
        set("mode_byte", "0x01 + RFKV".into());
        set("nac", t.nac(rx_nac));
        tx_nac = (tx_field & P25_NAC_SET != 0).then(|| t.nac(nac_field(tx_field, rx_nac)));
        set("tx_nac", tx_nac.clone().unwrap_or_else(|| t.same_as_rx.into()));
        let mut tg = if p.len() >= 8 && p[5] & 1 != 0 {
            let g = le16(p, 6);
            if g != 0 {
                g.to_string()
            } else {
                t.any.into()
            }
        } else {
            let c = le16(r, CH_CONTACT);
            if c != 0 {
                fmt1(t.contact_tg, &c.to_string())
            } else {
                "1".into()
            }
        };
        if squelch != 1 {
            tg.push_str(t.all_groups);
        }
        set("tg", tg);
        let p25_id = kv
            .get(&(KEY_MODE_SET + KIND_P25 as u16))
            .filter(|m| m.len() >= 4)
            .map(|m| le32(m, 0) as i64)
            .filter(|&x| (1..=0xFFFFFE).contains(&x))
            .unwrap_or(0);
        let id = if unit != 0 {
            unit
        } else if p25_id != 0 {
            p25_id
        } else {
            radio_id
        };
        set("unit_id", nz(id));
        rx_only = flags & TAG_TX == 0;
        set("bw", "12.5 kHz".into());
    } else if mode == "P25" && mb == MODE_P25_OLD {
        let mut rx_nac = le16(r, CH_RX_TONE);
        let mut txn = le16(r, CH_TX_TONE);
        if rx_nac > 0xFFF || rx_nac == 0xF7F {
            rx_nac = NAC_ANY;
        }
        if txn >= 0xF7E {
            txn = 0x293;
        }
        let g = le16(r, CH_CONTACT);
        if unit > 0xFFFFFE {
            unit = 0;
        }
        set("nac", t.nac(rx_nac) + t.mode2_form);
        tx_nac = (txn != rx_nac).then(|| t.nac(txn));
        set("tx_nac", tx_nac.clone().unwrap_or_else(|| t.same_as_rx.into()));
        let mut tg = if g != 0 { g.to_string() } else { t.any.into() };
        if g != 0 && r[CH_FLAG2] & FLAG2_TS2 != 0 {
            tg.push_str(t.all_groups);
        }
        set("tg", tg);
        set("unit_id", nz(unit));
        set("bw", "12.5 kHz".into());
        if g == 0 || unit == 0 {
            rx_only = true;
        }
    } else if mode == "P25" {
        let nac = (cc & 0x0F) << 8 | r[CH_RX_GROUP] as i64;
        set("nac", t.nac(nac) + if cc & 0xF0 == 0xD0 { "" } else { t.bad_record });
        set("tx_nac", t.same_as_rx.into());
        let g = le16(r, CH_CONTACT);
        set("tg", if g != 0 { g.to_string() } else { t.any.into() });
        set("unit_id", nz(unit));
    } else if mode == "YSF" {
        let dg = r[CH_RX_GROUP] & 0x7F;
        set("code", format!("DG-ID {}", if dg != 0 { dg.to_string() } else { t.any_lower.into() }));
        rx_only = true;
    } else if mode == "NXDN" {
        let ran = r[CH_RX_GROUP] & 0x3F;
        set("code", format!("RAN {}", if ran != 0 { ran.to_string() } else { t.any_lower.into() }));
        rx_only = true;
    } else if mode == "dPMR" {
        let c = (r[CH_RX_GROUP] & 0x7F) as i64;
        set("code", format!("CC {}", if c != 0 { (c - 1).to_string() } else { t.any_lower.into() }));
        rx_only = true;
    }
    set("rx_only", if rx_only { t.yes.into() } else { String::new() });
    let (rxm, txm) = (v["rx_mhz"].clone(), v["tx_mhz"].clone());
    let freq = if rxm == txm { short_mhz(&rxm) } else { format!("{} / TX {}", short_mhz(&rxm), short_mhz(&txm)) };
    v.insert("freq".into(), freq);
    let summary = summary(&v, &mode, any_list, tx_nac.as_deref(), rx_only, t);
    v.insert("summary".into(), summary);
    Some(ChannelRow { number, mode, values: v })
}

fn summary(
    v: &HashMap<String, String>,
    mode: &str,
    any_list: bool,
    tx_nac: Option<&str>,
    rx_only: bool,
    t: &CodeplugText,
) -> String {
    let mut parts: Vec<String> = vec![];
    if mode == "FM" {
        let (rt, tt) = (&v["rx_tone"], &v["tx_tone"]);
        if rt.is_empty() && tt.is_empty() {
            parts.push(t.no_tone.into());
        } else if rt == tt {
            parts.push(format!("{}{rt}", t.tone));
        } else {
            let a = if rt.is_empty() { "-" } else { rt };
            let b = if tt.is_empty() { "-" } else { tt };
            parts.push(t.tone_rx_tx.replace("{0}", a).replace("{1}", b));
        }
    } else if mode == "DMR" {
        parts.push(format!("CC {}", v["cc"]));
        parts.push(format!("TS {}", v["ts"]));
        if any_list {
            parts.push(t.all_tg.into());
        } else if !v["contact"].is_empty() {
            parts.push(format!("{}{}", t.contact, v["contact"]));
        }
    } else if mode == "P25" {
        parts.push(match tx_nac {
            None => format!("NAC {}", v["nac"]),
            Some(x) => format!("NAC {} > TX {x}", v["nac"]),
        });
        parts.push(format!("TG {}", v["tg"]));
    } else if !v["code"].is_empty() {
        parts.push(v["code"].clone());
    }
    if rx_only {
        parts.push(t.rx_only.into());
    }
    parts.join(" · ")
}

pub fn bank_bitmap(bank: usize) -> usize {
    if bank == 0 {
        BANK0_BITMAP
    } else {
        BANK1_BITMAP + (bank - 1) * BANK_STRIDE
    }
}

pub fn channel_addr(number: usize) -> usize {
    let i = number - 1;
    bank_bitmap(i / CH_PER_BANK) + 16 + i % CH_PER_BANK * CH_SIZE
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Table {
    Channels,
    Zones,
    Contacts,
    Lists,
}

/// A decoded codeplug.
#[derive(Debug, Clone)]
pub struct Codeplug {
    pub channels: Vec<ChannelRow>,
    pub zones: Vec<ZoneRow>,
    pub contacts: Vec<ContactRow>,
    pub lists: Vec<ListRow>,
    pub radio_name: String,
    pub dmr_id: i64,
    pub zone_capacity: usize,
    pub bad_records: usize,
    pub rfkv_entries: usize,
    pub text: &'static CodeplugText,
}

fn size_error(n: u64) -> CodeplugError {
    CodeplugError(format!("The data flash image must be 16 MiB (q128-pass1.bin); this one is {n} bytes"))
}

impl Codeplug {
    /// Decodes a 16 MiB data-flash image (q128-pass1.bin) with the words of one language.
    pub fn decode(image: &[u8], t: &'static CodeplugText) -> Result<Codeplug, CodeplugError> {
        if image.len() != IMAGE_SIZE {
            return Err(size_error(image.len() as u64));
        }
        let kv = read_kv(image);
        let dmr_id = bcd_id(image, GEN_DMR_ID);
        let mut used = HashSet::new();
        let mut channels = vec![];
        let mut bad = 0;
        for bank in 0..CH_BANKS {
            let bits = &image[bank_bitmap(bank)..bank_bitmap(bank) + CH_PER_BANK / 8];
            for k in 0..CH_PER_BANK {
                if bits[k / 8] & (1 << (k % 8)) == 0 {
                    continue;
                }
                let number = bank * CH_PER_BANK + k + 1;
                let rec = &image[channel_addr(number)..channel_addr(number) + CH_SIZE];
                match decode_channel(number as i64, rec, &kv, dmr_id, t) {
                    None => {
                        if rec.iter().any(|&b| b != 0xFF) {
                            bad += 1;
                        }
                    }
                    Some(row) => {
                        used.insert(number as i64);
                        channels.push(row);
                    }
                }
            }
        }
        let names: HashMap<i64, String> = channels.iter().map(|c| (c.number, c.get("name").to_string())).collect();
        let cap = if image[ZONE_LAYOUT] <= 4 { 80 } else { 16 };
        let record = 16 + 2 * cap;
        let zbits = &image[ZONE_BITMAP..ZONE_BITMAP + ZONE_SLOTS.div_ceil(8)];
        let mut zones = vec![];
        for slot in 0..ZONE_SLOTS {
            if zbits[slot / 8] & (1 << (slot % 8)) == 0 {
                continue;
            }
            let a = ZONE_DATA + slot * record;
            if image[a] == 0 || image[a] == 0xFF {
                continue;
            }
            let mut members = vec![];
            for i in 0..cap {
                let n = le16(image, a + 16 + 2 * i);
                if n == 0 {
                    break;
                }
                if used.contains(&n) {
                    members.push(n);
                }
            }
            let channel_names = members
                .iter()
                .map(|n| format!("{n} {}", names.get(n).map_or("", String::as_str)))
                .collect::<Vec<_>>()
                .join(", ");
            zones.push(ZoneRow {
                index: zones.len() as i64 + 1,
                slot: slot as i64,
                name: name(image, a, 16),
                channels: members,
                channel_names,
            });
        }
        let mut contacts = vec![];
        for i in 0..CONTACT_COUNT {
            let r = &image[CONTACT_BASE + i * CONTACT_SIZE..CONTACT_BASE + (i + 1) * CONTACT_SIZE];
            if r[0] == 0xFF || r[0x14] > 2 {
                continue;
            }
            let kind = r[0x14] as usize;
            let cid = if kind == 2 { 0xFFFFFF } else { bcd_id(r, 0x10) };
            if cid == 0 {
                continue;
            }
            let ts = if r[0x17] != 0xFF && r[0x17] & 1 == 0 {
                if r[0x17] & 2 != 0 { "2" } else { "1" }.to_string()
            } else {
                String::new()
            };
            contacts.push(ContactRow {
                index: i as i64 + 1,
                name: name(r, 0, 16),
                id: cid,
                kind: t.contact_types[kind].into(),
                ts,
            });
        }
        let cnames: HashMap<i64, String> = contacts.iter().map(|c| (c.index, c.name.clone())).collect();
        let mut lists = vec![];
        for i in 0..LIST_COUNT {
            let n = image[LIST_COUNTS + i] as usize;
            if n == 0 || n == 0xFF {
                continue;
            }
            let a = LIST_BASE + i * LIST_SIZE;
            let mut members = vec![];
            for k in 0..(n - 1).min(32) {
                let c = le16(image, a + 16 + 2 * k);
                if c == 0 || c > CONTACT_COUNT as i64 {
                    break;
                }
                members.push(c);
            }
            let contact_names = members
                .iter()
                .map(|c| cnames.get(c).cloned().unwrap_or_else(|| format!("#{c}")))
                .collect::<Vec<_>>()
                .join(", ");
            lists.push(ListRow { index: i as i64 + 1, name: name(image, a, 16), contacts: members, contact_names });
        }
        Ok(Codeplug {
            channels,
            zones,
            contacts,
            lists,
            radio_name: name(image, GEN_CALLSIGN, 8),
            dmr_id,
            zone_capacity: cap,
            bad_records: bad,
            rfkv_entries: kv.len(),
            text: t,
        })
    }

    /// The data flash image of a backup folder (q128-pass1.bin), or a 16 MiB image file itself.
    pub fn load_image(path: &Path) -> Result<Vec<u8>, CodeplugError> {
        let file = if path.is_dir() { path.join("q128-pass1.bin") } else { path.to_path_buf() };
        let len = std::fs::metadata(&file).map_err(|e| CodeplugError(e.to_string()))?.len();
        if len != IMAGE_SIZE as u64 {
            return Err(size_error(len));
        }
        std::fs::read(&file).map_err(|e| CodeplugError(e.to_string()))
    }

    pub fn mode_counts(&self) -> HashMap<String, usize> {
        let mut d = HashMap::new();
        for c in &self.channels {
            *d.entry(c.mode.clone()).or_insert(0) += 1;
        }
        d
    }

    pub fn columns(&self, table: Table) -> &'static [(&'static str, &'static str)] {
        match table {
            Table::Channels => self.text.channel_columns,
            Table::Zones => self.text.zone_columns,
            Table::Contacts => self.text.contact_columns,
            Table::Lists => self.text.list_columns,
        }
    }

    /// Every row of a table as {column key: text}.
    pub fn rows(&self, table: Table) -> Vec<HashMap<String, String>> {
        let d = |kv: Vec<(&str, String)>| kv.into_iter().map(|(k, v)| (k.to_string(), v)).collect::<HashMap<_, _>>();
        match table {
            Table::Channels => self.channels.iter().map(|c| c.values.clone()).collect(),
            Table::Zones => self
                .zones
                .iter()
                .map(|z| {
                    d(vec![
                        ("index", z.index.to_string()),
                        ("name", z.name.clone()),
                        ("count", z.channels.len().to_string()),
                        ("channel_names", z.channel_names.clone()),
                    ])
                })
                .collect(),
            Table::Contacts => self
                .contacts
                .iter()
                .map(|c| {
                    d(vec![
                        ("index", c.index.to_string()),
                        ("name", c.name.clone()),
                        ("id", c.id.to_string()),
                        ("type", c.kind.clone()),
                        ("ts", c.ts.clone()),
                    ])
                })
                .collect(),
            Table::Lists => self
                .lists
                .iter()
                .map(|l| {
                    d(vec![
                        ("index", l.index.to_string()),
                        ("name", l.name.clone()),
                        ("count", l.contacts.len().to_string()),
                        ("contact_names", l.contact_names.clone()),
                    ])
                })
                .collect(),
        }
    }

    /// The channels a zone holds, in the zone's order.
    pub fn zone_channels(&self, zone: &ZoneRow) -> Vec<&ChannelRow> {
        zone.channels.iter().filter_map(|n| self.channels.iter().find(|c| c.number == *n)).collect()
    }
}

/// A row matches a search text when any of its texts holds it, ignoring case ("" matches every row).
pub fn matches(row: &HashMap<String, String>, search: &str) -> bool {
    let s = search.trim();
    if s.is_empty() {
        return true;
    }
    let s = s.to_lowercase();
    row.values().any(|v| v.to_lowercase().contains(&s))
}

/// codeplug.to_csv: the column titles, then one line per row, "\n" line ends, quoted only when needed.
pub fn to_csv(columns: &[(&str, &str)], rows: &[HashMap<String, String>]) -> String {
    let field = |v: &str| {
        if v.contains([',', '"', '\n', '\r']) {
            format!("\"{}\"", v.replace('"', "\"\""))
        } else {
            v.to_string()
        }
    };
    let mut s = columns.iter().map(|c| field(c.1)).collect::<Vec<_>>().join(",");
    s.push('\n');
    for r in rows {
        s.push_str(&columns.iter().map(|c| field(r.get(c.0).map_or("", String::as_str))).collect::<Vec<_>>().join(","));
        s.push('\n');
    }
    s
}

/// The CSV file, UTF-8 with a BOM (Excel reads it).
pub fn write_csv(path: &Path, csv: &str) -> std::io::Result<()> {
    let mut b = vec![0xEF, 0xBB, 0xBF];
    b.extend_from_slice(csv.as_bytes());
    std::fs::write(path, b)
}
