// SPDX-License-Identifier: GPL-3.0-or-later
//! The few Python string rules the ported code depends on, so it gives the Python tools' bytes: str.isspace,
//! strip / split / splitlines, int(), upper() of a callsign, ASCII encode / decode with 'replace'.
//! (Python's str order is by code point; Rust's `str` order is the same.)

use crate::releases::version::decimal_value;

/// Python's `str.isspace` for one character: Unicode White_Space plus U+001C..U+001F.
pub fn py_isspace(c: char) -> bool {
    c.is_whitespace() || ('\u{1c}'..='\u{1f}').contains(&c)
}

/// `str.strip()`
pub fn py_strip(s: &str) -> &str {
    s.trim_matches(py_isspace)
}

/// `str.split()` with no argument: runs of white space separate, none at the ends.
pub fn py_split(s: &str) -> Vec<String> {
    s.split(py_isspace).filter(|w| !w.is_empty()).map(str::to_string).collect()
}

/// `str.splitlines()`: \n, \r, \r\n, \v, \f, \x1c, \x1d, \x1e, \x85, U+2028, U+2029.
pub fn py_splitlines(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut it = s.chars().peekable();
    let mut pending = false;
    while let Some(c) = it.next() {
        if matches!(
            c,
            '\n' | '\r' | '\u{b}' | '\u{c}' | '\u{1c}' | '\u{1d}' | '\u{1e}' | '\u{85}' | '\u{2028}' | '\u{2029}'
        ) {
            out.push(std::mem::take(&mut cur));
            if c == '\r' && it.peek() == Some(&'\n') {
                it.next();
            }
            pending = false;
        } else {
            cur.push(c);
            pending = true;
        }
    }
    if pending {
        out.push(cur);
    }
    out
}

/// `int(s)` of a decimal string: surrounding white space, an optional sign, Unicode decimal digits, single
/// underscores between digits. None where Python raises ValueError; values beyond i64 saturate.
pub fn py_int(s: &str) -> Option<i64> {
    let s = py_strip(s);
    let mut chars = s.chars().peekable();
    let mut neg = false;
    if let Some(&c) = chars.peek() {
        if c == '+' || c == '-' {
            neg = c == '-';
            chars.next();
        }
    }
    let mut v: i64 = 0;
    let mut any = false;
    let mut last_underscore = true;
    for c in chars {
        if c == '_' {
            if last_underscore {
                return None;
            }
            last_underscore = true;
            continue;
        }
        let d = decimal_value(c)? as i64;
        any = true;
        last_underscore = false;
        v = v.saturating_mul(10).saturating_add(d);
    }
    if !any || last_underscore {
        return None;
    }
    Some(if neg { -v } else { v })
}

/// `str.upper()` as far as it decides whether a callsign is A-Z / 0-9 (the full mappings that end in ASCII).
pub fn py_upper(s: &str) -> String {
    // Rust's to_uppercase uses the full Unicode mappings, as Python does (ß -> SS, ligatures, ı -> I, ſ -> S)
    s.to_uppercase()
}

/// `s.encode('ascii', 'replace')`: one '?' a code point outside ASCII.
pub fn ascii_replace(s: &str) -> Vec<u8> {
    s.chars().map(|c| if (c as u32) < 0x80 { c as u8 } else { b'?' }).collect()
}

/// `b.decode('ascii', 'replace')`: U+FFFD a byte outside ASCII.
pub fn ascii_decode(b: &[u8]) -> String {
    b.iter().map(|&x| if x < 0x80 { x as char } else { '\u{FFFD}' }).collect()
}

pub fn rstrip_nul(b: &[u8]) -> &[u8] {
    let n = b.iter().rposition(|&x| x != 0).map_or(0, |i| i + 1);
    &b[..n]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn python_rules() {
        assert_eq!(py_strip("\u{1c} a b \u{a0}"), "a b");
        assert_eq!(py_split(" a  b\tc "), vec!["a", "b", "c"]);
        assert_eq!(py_splitlines("a\r\nb\rc\n"), vec!["a", "b", "c"]);
        assert_eq!(py_splitlines("a\n\nb"), vec!["a", "", "b"]);
        assert_eq!(py_int(" 1_000 "), Some(1000));
        assert_eq!(py_int("1__0"), None);
        assert_eq!(py_int("_1"), None);
        assert_eq!(py_int("-5"), Some(-5));
        assert_eq!(py_int("١٢"), Some(12));
        assert_eq!(py_upper("straße"), "STRASSE");
        assert_eq!(ascii_replace("aü"), b"a?");
    }
}
