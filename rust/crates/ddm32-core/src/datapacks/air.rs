// SPDX-License-Identifier: GPL-3.0-or-later
//! The aerodromes' AD 2.18 tables ("ATS communication facilities") as page text -> the air band pack's frequency
//! lists: the port of datapack.parse_ad218 with tools/airfields.py's service words and channel rules
//! (DataPacks/AirText.cs). The user downloads their State's AIP pages on their own PC; nothing is redistributed.

use crate::pytext::{py_split, py_splitlines, py_strip};
use crate::releases::version::decimal_value;
use fancy_regex::Regex;
use std::sync::OnceLock;

/// air_freq_t.ch = (name - CH_BASE_KHZ) / 5
pub const CH_BASE_KHZ: i64 = 108000;
/// The VHF aeronautical mobile band (hi exclusive).
pub const LO_KHZ: i64 = 118000;
pub const HI_KHZ: i64 = 137000;

/// airfield.h air_type_t, in display order within an airfield.
pub const TYPES: [&str; 15] =
    ["ATIS", "DEL", "GND", "RMP", "TWR", "APP", "DEP", "AFIS", "INFO", "CTAF", "PAR", "SAR", "EMR", "OPS", "OTH"];

pub fn type_word(w: &str) -> Option<&'static str> {
    Some(match w {
        "ATIS" | "D-ATIS" | "D-ATIS-ARR" | "D-ATIS-DEP" => "ATIS",
        "DEL" | "DLV" | "CLD" | "CLR" | "CLNC" | "CLEARANCE" | "DELIVERY" => "DEL",
        "GND" | "GROUND" => "GND",
        "RMP" | "RAMP" | "APRON" => "RMP",
        "TWR" | "TOWER" => "TWR",
        "APP" | "APPROACH" | "APP/DEP" | "ACP" | "ARR" => "APP",
        "DEP" | "DEPARTURE" => "DEP",
        "AFIS" => "AFIS",
        "INFO" | "FIS" => "INFO",
        "CTAF" | "UNIC" | "UNICOM" => "CTAF",
        "PAR" => "PAR",
        "SAR" => "SAR",
        "EMR" | "EMERG" => "EMR",
        "HAVAS" | "OPS" | "POST" | "PMSV" | "PTD" | "SFA" | "TAFR" => "OPS",
        _ => return None,
    })
}

struct Res {
    freq: Regex,
    section: Regex,
    end: Regex,
    not_word: regex::Regex,
    code: Regex,
}

fn res() -> &'static Res {
    static R: OnceLock<Res> = OnceLock::new();
    R.get_or_init(|| Res {
        // Python's \s also takes U+001C..U+001F
        freq: Regex::new(r"(?<![\d.])(1[0-3]\d[.,]\d{1,3})[\s\x1c-\x1f]*(?:MHZ|MHz|Mhz)?(?![\d.])").unwrap(),
        section: Regex::new(r"AD[\s\x1c-\x1f]*[23][\s\x1c-\x1f]*\.[\s\x1c-\x1f]*18").unwrap(),
        end: Regex::new(r"AD[\s\x1c-\x1f]*[23][\s\x1c-\x1f]*\.[\s\x1c-\x1f]*19").unwrap(),
        not_word: regex::Regex::new(r"[^A-Za-z/ -]").unwrap(),
        code: Regex::new(r"(?<![A-Za-z0-9])(LT[A-Z]{2})(?![A-Za-z0-9])").unwrap(),
    })
}

/// airfields.channel_khz: a frequency ("121.58", "132", "118.675") -> the channel name in kHz, or the refusal reason
/// ("not a number", "grid", "band").
pub fn channel_khz(text: &str) -> (Option<i64>, Option<&'static str>) {
    let t = py_strip(text);
    let mut digits = String::new();
    let mut dot: Option<usize> = None;
    if t.starts_with(['+', '-']) {
        return (None, Some("not a number"));
    }
    for c in t.chars() {
        if c == '.' && dot.is_none() {
            dot = Some(digits.len());
            continue;
        }
        match decimal_value(c) {
            Some(d) => digits.push(char::from_digit(d, 10).unwrap()),
            None => return (None, Some("not a number")),
        }
    }
    if digits.is_empty() {
        return (None, Some("not a number"));
    }
    let (int_part, frac) = match dot {
        None => (digits.as_str(), ""),
        Some(d) => (&digits[..d], &digits[d..]),
    };
    let frac_sig = frac.trim_end_matches('0');
    if frac_sig.len() > 3 {
        return (None, Some("grid"));
    }
    if int_part.trim_start_matches('0').len() > 6 {
        return (None, Some("band"));
    }
    let ip: i64 = if int_part.is_empty() { 0 } else { int_part.parse().unwrap() };
    let fp: i64 = if frac_sig.is_empty() { 0 } else { format!("{frac_sig:0<3}").parse().unwrap() };
    let khz = ip * 1000 + fp;
    if !(LO_KHZ..HI_KHZ).contains(&khz) {
        return (None, Some("band"));
    }
    if khz % 5 != 0 || khz % 25 == 20 {
        return (None, Some("grid"));
    }
    (Some(khz), None)
}

fn type_index(name: &str) -> usize {
    TYPES.iter().position(|t| *t == name).unwrap()
}

/// datapack.parse_ad218: the AD 2.18 (AD 3.18) table's lines -> [(type index, kHz)] in the table's order: each line
/// with an air band frequency, its service named by the line's own words (or the last named one above it).
pub fn parse_ad218(text: &str) -> Vec<(i64, i64)> {
    let r = res();
    let mut text = text.to_string();
    if let Ok(Some(m)) = r.section.find(&text) {
        text = text[m.end()..].to_string();
        if let Ok(Some(e)) = r.end.find(&text) {
            text = text[..e.start()].to_string();
        }
    }
    let mut out: Vec<(i64, i64)> = Vec::new();
    let mut service: Option<&'static str> = None;
    let oth = type_index("OTH") as i64;
    for line in py_splitlines(&text) {
        let cleaned = r.not_word.replace_all(&line, " ").to_uppercase();
        let words = py_split(&cleaned);
        if let Some(s) = words.iter().find_map(|w| type_word(w)) {
            service = Some(s);
        }
        for cap in r.freq.captures_iter(&line).flatten() {
            let f = cap.get(1).unwrap().as_str().replace(',', ".");
            let (khz, why) = channel_khz(&f);
            let (Some(khz), None) = (khz, why) else { continue };
            let t = service.map_or(oth, |s| type_index(s) as i64);
            if !out.contains(&(t, khz)) {
                out.push((t, khz));
            }
        }
    }
    out
}

/// The aerodrome code (LTxx) in a file name, else in the page text before its AD 2.18 heading, or None.
pub fn code_of(file_name: &str, text: Option<&str>) -> Option<String> {
    let r = res();
    let base = file_name.rsplit(['/', '\\']).next().unwrap_or(file_name);
    let stem = match base.rfind('.') {
        Some(i) if i > 0 => &base[..i],
        _ => base,
    };
    let last = |s: &str| r.code.captures_iter(s).flatten().last().map(|c| c[1].to_string());
    if let Some(c) = last(stem) {
        return Some(c);
    }
    let text = text?;
    let before = match r.section.find(text) {
        Ok(Some(m)) => &text[..m.start()],
        _ => text,
    };
    last(before)
}

pub fn type_name(t: i64) -> &'static str {
    TYPES.get(t as usize).copied().unwrap_or("?")
}

/// A channel's name as the radio shows it: 118.100, 132.005.
pub fn mhz(khz: i64) -> String {
    format!("{}.{:03}", khz / 1000, khz % 1000)
}
