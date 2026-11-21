// SPDX-License-Identifier: GPL-3.0-or-later
//! RadioID.net's user list ("user.csv": RADIO_ID, CALLSIGN, FIRST_NAME, LAST_NAME, CITY, STATE, COUNTRY), read as
//! tools/iddb.py reads it for the pack builder (iddb.read, fold, short_name, pack_call), with Python's csv rules
//! (the excel dialect; DataPacks/RadioIdCsv.cs). The user downloads the file from radioid.net themselves; it is
//! never redistributed.

use super::PackError;
use crate::pytext::{py_int, py_split, py_strip, py_upper};
use unicode_normalization::UnicodeNormalization;

pub const DOWNLOAD_PAGE: &str = "https://radioid.net/database/dumps";
pub const DOWNLOAD_URL: &str = "https://radioid.net/static/user.csv";
pub const NAME_MAX: usize = 10;
pub const CALL_MAX: usize = 6;
pub const ID_MAX: i64 = 0xFFFFFF;
const ALPHABET: &str = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/// What the RadioID list reading left out (iddb.read's "skipped").
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RadioIdSkipped {
    pub callsign: usize,
    pub id: usize,
    pub duplicate: usize,
}

/// One ID taken from the list: the callsign (upper case, A-Z / 0-9, up to 6), its base-37 number, the short name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RadioIdRow {
    pub id: i64,
    pub call: String,
    pub packed: i64,
    pub name: String,
}

fn special(c: char) -> Option<&'static str> {
    Some(match c {
        'ı' => "i",
        'İ' => "I",
        'ß' => "ss",
        'ø' => "o",
        'Ø' => "O",
        'æ' => "ae",
        'Æ' => "AE",
        'đ' => "d",
        'Đ' => "D",
        'ł' => "l",
        'Ł' => "L",
        'œ' => "oe",
        'Œ' => "OE",
        'þ' => "th",
        'Þ' => "Th",
        'ð' => "d",
        'Ð' => "D",
        _ => return None,
    })
}

/// iddb.fold: ASCII only (the Turkish dotless / dotted i and a few letters first, then accents off).
pub fn fold(text: &str) -> String {
    let mut s = String::with_capacity(text.len());
    for c in text.chars() {
        match special(c) {
            Some(r) => s.push_str(r),
            None => s.push(c),
        }
    }
    s.nfkd().filter(|c| (*c as u32) < 128).collect()
}

pub fn country_key(name: &str) -> String {
    let k = py_strip(&fold(name)).to_lowercase();
    if k == "turkey" {
        "turkiye".into()
    } else {
        k
    }
}

fn is_upper(w: &str) -> bool {
    w.chars().any(|c| c.is_ascii_alphabetic()) && !w.chars().any(|c| c.is_ascii_lowercase())
}

fn is_lower(w: &str) -> bool {
    w.chars().any(|c| c.is_ascii_alphabetic()) && !w.chars().any(|c| c.is_ascii_uppercase())
}

/// iddb.short_name: the first word of the first name (else of the last name), ASCII, letters / digits / ' - . only,
/// at most 10 characters; an all-upper or all-lower name in title case.
pub fn short_name(first: &str, last: &str) -> String {
    for field in [first, last] {
        let cleaned: String =
            fold(field).chars().map(|c| if c.is_ascii_alphanumeric() || "'-.".contains(c) { c } else { ' ' }).collect();
        let words = py_split(&cleaned);
        if let Some(w0) = words.first() {
            let mut w: String = w0.chars().take(NAME_MAX).collect();
            if is_upper(&w) || is_lower(&w) {
                w = w[..1].to_uppercase() + &w[1..].to_lowercase();
            }
            return w;
        }
    }
    String::new()
}

/// iddb.pack_call: base-37 of up to 6 characters (A-Z, 0-9) padded with ' ' on the right; None when it does not fit.
pub fn pack_call(call: &str) -> Option<i64> {
    let call = py_upper(py_strip(call));
    let n = call.chars().count();
    if n == 0 || n > CALL_MAX || call.chars().any(|c| c == ' ' || !ALPHABET.contains(c)) {
        return None;
    }
    let mut v = 0i64;
    for c in call.chars().chain(std::iter::repeat_n(' ', CALL_MAX - n)) {
        v = v * 37 + ALPHABET.find(c).unwrap() as i64;
    }
    Some(v)
}

/// raw.decode('utf-8-sig', errors='replace')
pub fn decode(raw: &[u8]) -> String {
    let start = if raw.starts_with(&[0xEF, 0xBB, 0xBF]) { 3 } else { 0 };
    String::from_utf8_lossy(&raw[start..]).into_owned()
}

const NOT_A_LIST: &str = "not a RadioID.net user.csv (needs columns CALLSIGN, COUNTRY, FIRST_NAME, RADIO_ID)";

/// iddb.read(path, countries): the rows sorted by ID and what was skipped. No country given: Turkey.
pub fn read(raw: &[u8], countries: &[String]) -> Result<(Vec<RadioIdRow>, RadioIdSkipped), PackError> {
    let mut want: std::collections::HashSet<String> = countries.iter().map(|c| country_key(c)).collect();
    if want.is_empty() {
        want.insert(country_key("Turkey"));
    }
    let text = decode(raw);
    let mut records = csv_rows(&text).into_iter();
    let names = records.next().ok_or_else(|| PackError(NOT_A_LIST.into()))?;
    if !["CALLSIGN", "COUNTRY", "FIRST_NAME", "RADIO_ID"].iter().all(|n| names.iter().any(|x| x == n)) {
        return Err(PackError(NOT_A_LIST.into()));
    }
    let mut rows: std::collections::BTreeMap<i64, RadioIdRow> = std::collections::BTreeMap::new();
    let mut sk = RadioIdSkipped::default();
    for fields in records {
        if fields.is_empty() {
            continue; // csv.DictReader skips empty rows
        }
        let mut r: std::collections::HashMap<&str, &str> = std::collections::HashMap::new();
        for (i, f) in fields.iter().enumerate().take(names.len()) {
            r.insert(&names[i], f); // a repeated column name: the last one wins
        }
        let get = |k: &str| r.get(k).copied().unwrap_or("");
        let call = py_upper(py_strip(get("CALLSIGN")));
        if !want.contains(&country_key(get("COUNTRY"))) {
            continue;
        }
        let Some(rid) = py_int(get("RADIO_ID")) else {
            sk.id += 1;
            continue;
        };
        if rid <= 0 || rid > ID_MAX {
            sk.id += 1;
            continue;
        }
        let Some(packed) = pack_call(&call) else {
            sk.callsign += 1;
            continue;
        };
        if rows.contains_key(&rid) {
            sk.duplicate += 1;
            continue;
        }
        rows.insert(rid, RadioIdRow { id: rid, call, packed, name: short_name(get("FIRST_NAME"), get("LAST_NAME")) });
    }
    Ok((rows.into_values().collect(), sk))
}

/// Python's csv.reader with the default (excel) dialect, strict off.
pub fn csv_rows(text: &str) -> Vec<Vec<String>> {
    #[derive(PartialEq, Clone, Copy)]
    enum S {
        StartRecord,
        StartField,
        InField,
        InQuoted,
        QuoteInQuoted,
        EatCrnl,
    }
    let mut out = Vec::new();
    let mut fields: Vec<String> = Vec::new();
    let mut field = String::new();
    let mut state = S::StartRecord;
    for c in text.chars() {
        let mut again = true;
        while again {
            again = false;
            match state {
                S::StartRecord => {
                    if c == '\n' || c == '\r' {
                        state = S::EatCrnl;
                        out.push(vec![]);
                    } else {
                        state = S::StartField;
                        again = true;
                    }
                }
                S::StartField => {
                    if c == '\n' || c == '\r' {
                        fields.push(String::new());
                        out.push(std::mem::take(&mut fields));
                        state = S::EatCrnl;
                    } else if c == '"' {
                        state = S::InQuoted;
                    } else if c == ',' {
                        fields.push(String::new());
                    } else {
                        field.push(c);
                        state = S::InField;
                    }
                }
                S::InField => {
                    if c == '\n' || c == '\r' {
                        fields.push(std::mem::take(&mut field));
                        out.push(std::mem::take(&mut fields));
                        state = S::EatCrnl;
                    } else if c == ',' {
                        fields.push(std::mem::take(&mut field));
                        state = S::StartField;
                    } else {
                        field.push(c);
                    }
                }
                S::InQuoted => {
                    if c == '"' {
                        state = S::QuoteInQuoted;
                    } else {
                        field.push(c);
                    }
                }
                S::QuoteInQuoted => {
                    if c == '"' {
                        field.push('"');
                        state = S::InQuoted;
                    } else if c == ',' {
                        fields.push(std::mem::take(&mut field));
                        state = S::StartField;
                    } else if c == '\n' || c == '\r' {
                        fields.push(std::mem::take(&mut field));
                        out.push(std::mem::take(&mut fields));
                        state = S::EatCrnl;
                    } else {
                        field.push(c);
                        state = S::InField;
                    }
                }
                S::EatCrnl => {
                    if c != '\n' && c != '\r' {
                        state = S::StartRecord;
                        again = true;
                    }
                }
            }
        }
    }
    if matches!(state, S::StartField | S::InField | S::InQuoted | S::QuoteInQuoted)
        && (state != S::StartField || !fields.is_empty())
    {
        fields.push(field);
        out.push(fields);
    }
    out
}
