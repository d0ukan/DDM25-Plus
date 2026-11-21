// SPDX-License-Identifier: GPL-3.0-or-later
//! A sortable `MAJOR.MINOR.PATCH[-PRERELEASE]` (signed_manifest.parse_version): a prerelease sorts before its
//! release; prerelease parts compare numerically when all digits, else as text, and a number sorts before text.
//! The order is exactly Python's tuple order of parse_version's key (checked against it in the tests).

use super::ManifestError;
use num_bigint::BigUint;
use std::cmp::Ordering;

#[derive(Debug, Clone, PartialEq, Eq)]
enum Part {
    Number(BigUint),
    Text(String),
}

impl Part {
    fn cmp_py(&self, other: &Part) -> Ordering {
        match (self, other) {
            (Part::Number(a), Part::Number(b)) => a.cmp(b),
            (Part::Number(_), Part::Text(_)) => Ordering::Less,
            (Part::Text(_), Part::Number(_)) => Ordering::Greater,
            // Python compares str by code point; Rust's str order is UTF-8 byte order, which is the same
            (Part::Text(a), Part::Text(b)) => a.as_str().cmp(b.as_str()),
        }
    }
}

#[derive(Debug, Clone)]
pub struct ReleaseVersion {
    pub text: String,
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
    pub prerelease: Option<String>,
    parts: Vec<Part>,
}

/// Python's `\d` for str patterns: any Unicode decimal digit (Nd). Returns its value.
pub(crate) fn decimal_value(c: char) -> Option<u32> {
    if c.is_ascii_digit() {
        return Some(c as u32 - '0' as u32);
    }
    if c.is_ascii() {
        return None;
    }
    // Unicode Nd characters come in contiguous runs of ten starting at a code point ending in 0 (mod 16 is not
    // guaranteed, but each block of ten starts at a digit zero). Use the tables of the known blocks.
    let cp = c as u32;
    for &zero in UNICODE_DIGIT_ZEROS {
        if cp >= zero && cp < zero + 10 {
            return Some(cp - zero);
        }
    }
    None
}

/// The digit zero of every Unicode Nd block (Unicode 15.1), outside ASCII.
pub(crate) const UNICODE_DIGIT_ZEROS: &[u32] = &[
    0x0660, 0x06F0, 0x07C0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66, 0x0BE6, 0x0C66, 0x0CE6, 0x0D66, 0x0DE6, 0x0E50,
    0x0ED0, 0x0F20, 0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0, 0x1C40, 0x1C50,
    0xA620, 0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10, 0x104A0, 0x10D30, 0x11066, 0x110F0, 0x11136,
    0x111D0, 0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x11730, 0x118E0, 0x11950, 0x11C50, 0x11D50, 0x11DA0,
    0x11F50, 0x16A60, 0x16AC0, 0x16B50, 0x1D7CE, 0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6, 0x1E140, 0x1E2F0, 0x1E4F0,
    0x1E950, 0x1FBF0,
];

impl ReleaseVersion {
    pub fn parse(text: &str) -> Result<ReleaseVersion, ManifestError> {
        Self::try_parse(text).ok_or_else(|| ManifestError(format!("Bad version '{text}'")))
    }

    /// Python: `re.match(r'^(\d{1,4})\.(\d{1,4})\.(\d{1,4})(?:-([0-9A-Za-z.-]{1,40}))?$', text)`; `\d` takes any
    /// Unicode digit and `$` also matches before a final "\n".
    pub fn try_parse(text: &str) -> Option<ReleaseVersion> {
        let body = text.strip_suffix('\n').unwrap_or(text);
        let chars: Vec<char> = body.chars().collect();
        let mut i = 0;
        let mut nums = [0u32; 3];
        for (k, num) in nums.iter_mut().enumerate() {
            let start = i;
            let mut v = 0u32;
            while i < chars.len() && i - start < 5 {
                match decimal_value(chars[i]) {
                    Some(d) => {
                        v = v * 10 + d;
                        i += 1;
                    }
                    None => break,
                }
            }
            let n = i - start;
            if n == 0 || n > 4 {
                return None;
            }
            *num = v;
            if k < 2 {
                if i >= chars.len() || chars[i] != '.' {
                    return None;
                }
                i += 1;
            }
        }
        let pre = if i == chars.len() {
            None
        } else {
            if chars[i] != '-' {
                return None;
            }
            let rest: String = chars[i + 1..].iter().collect();
            let len = rest.chars().count();
            if !(1..=40).contains(&len) || !rest.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-') {
                return None;
            }
            Some(rest)
        };
        let parts = match &pre {
            None => vec![],
            Some(p) => p
                .split('.')
                .map(|s| {
                    if !s.is_empty() && s.chars().all(|c| c.is_ascii_digit()) {
                        Part::Number(s.parse().unwrap())
                    } else {
                        Part::Text(s.to_string())
                    }
                })
                .collect(),
        };
        Some(ReleaseVersion {
            text: text.to_string(),
            major: nums[0],
            minor: nums[1],
            patch: nums[2],
            prerelease: pre,
            parts,
        })
    }

    pub fn is_prerelease(&self) -> bool {
        self.prerelease.is_some()
    }
}

impl Ord for ReleaseVersion {
    fn cmp(&self, other: &Self) -> Ordering {
        let c = (self.major, self.minor, self.patch).cmp(&(other.major, other.minor, other.patch));
        if c != Ordering::Equal {
            return c;
        }
        // Python: release (1,) and prerelease (0, part, part, ...)
        let tag_a = if self.is_prerelease() { 0 } else { 1 };
        let tag_b = if other.is_prerelease() { 0 } else { 1 };
        if tag_a != tag_b {
            return tag_a.cmp(&tag_b);
        }
        for (a, b) in self.parts.iter().zip(&other.parts) {
            let c = a.cmp_py(b);
            if c != Ordering::Equal {
                return c;
            }
        }
        self.parts.len().cmp(&other.parts.len())
    }
}

impl PartialOrd for ReleaseVersion {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for ReleaseVersion {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}

impl Eq for ReleaseVersion {}

impl std::fmt::Display for ReleaseVersion {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.text)
    }
}
