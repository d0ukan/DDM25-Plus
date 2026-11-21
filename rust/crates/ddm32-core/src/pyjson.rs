// SPDX-License-Identifier: GPL-3.0-or-later
//! Python's `json.dumps` output, byte for byte (ensure_ascii; `indent=None` or a number of spaces), over an
//! ordered value type that behaves like a Python dict: the radio tools' receipts, backup manifests and printed plans
//! must be the bytes the Python tools write (win/docs/porting-plan.md).

use std::fmt::Write as _;

/// A JSON value with Python's key order (insertion order; a new value for a key keeps its place).
#[derive(Debug, Clone, PartialEq)]
pub enum Py {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
    List(Vec<Py>),
    Dict(PyDict),
}

impl Py {
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Py::Str(s) => Some(s),
            _ => None,
        }
    }
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Py::Int(i) => Some(*i),
            _ => None,
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Py::Bool(b) => Some(*b),
            _ => None,
        }
    }
    pub fn is_true(&self) -> bool {
        matches!(self, Py::Bool(true))
    }
    pub fn as_dict(&self) -> Option<&PyDict> {
        match self {
            Py::Dict(d) => Some(d),
            _ => None,
        }
    }
    pub fn as_list(&self) -> Option<&Vec<Py>> {
        match self {
            Py::List(l) => Some(l),
            _ => None,
        }
    }

    /// From a parsed serde_json value (objects keep their order).
    pub fn from_json(v: &serde_json::Value) -> Py {
        match v {
            serde_json::Value::Null => Py::Null,
            serde_json::Value::Bool(b) => Py::Bool(*b),
            serde_json::Value::Number(n) => match n.as_i64() {
                Some(i) => Py::Int(i),
                None => Py::Float(n.as_f64().unwrap_or(f64::NAN)),
            },
            serde_json::Value::String(s) => Py::Str(s.clone()),
            serde_json::Value::Array(a) => Py::List(a.iter().map(Py::from_json).collect()),
            serde_json::Value::Object(o) => {
                let mut d = PyDict::new();
                for (k, v) in o {
                    d.set(k, Py::from_json(v));
                }
                Py::Dict(d)
            }
        }
    }
}

impl From<bool> for Py {
    fn from(b: bool) -> Self {
        Py::Bool(b)
    }
}
impl From<i64> for Py {
    fn from(i: i64) -> Self {
        Py::Int(i)
    }
}
impl From<i32> for Py {
    fn from(i: i32) -> Self {
        Py::Int(i as i64)
    }
}
impl From<u32> for Py {
    fn from(i: u32) -> Self {
        Py::Int(i as i64)
    }
}
impl From<usize> for Py {
    fn from(i: usize) -> Self {
        Py::Int(i as i64)
    }
}
impl From<f64> for Py {
    fn from(f: f64) -> Self {
        Py::Float(f)
    }
}
impl From<&str> for Py {
    fn from(s: &str) -> Self {
        Py::Str(s.to_string())
    }
}
impl From<String> for Py {
    fn from(s: String) -> Self {
        Py::Str(s)
    }
}
impl From<PyDict> for Py {
    fn from(d: PyDict) -> Self {
        Py::Dict(d)
    }
}
impl<T: Into<Py>> From<Option<T>> for Py {
    fn from(o: Option<T>) -> Self {
        match o {
            Some(v) => v.into(),
            None => Py::Null,
        }
    }
}
impl<T: Into<Py>> From<Vec<T>> for Py {
    fn from(v: Vec<T>) -> Self {
        Py::List(v.into_iter().map(Into::into).collect())
    }
}

/// An ordered JSON object (a Python dict).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct PyDict {
    items: Vec<(String, Py)>,
}

impl PyDict {
    pub fn new() -> Self {
        Self::default()
    }

    /// `d[key] = value`: a new key goes last, an existing key keeps its position.
    pub fn set(&mut self, key: &str, value: impl Into<Py>) {
        let value = value.into();
        if let Some(slot) = self.items.iter_mut().find(|(k, _)| k == key) {
            slot.1 = value;
        } else {
            self.items.push((key.to_string(), value));
        }
    }

    /// Builder form of [`PyDict::set`].
    pub fn with(mut self, key: &str, value: impl Into<Py>) -> Self {
        self.set(key, value);
        self
    }

    pub fn get(&self, key: &str) -> Option<&Py> {
        self.items.iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }

    /// The value as a string (Python `d.get(k)` compared with a str), or None.
    pub fn str(&self, key: &str) -> Option<&str> {
        self.get(key).and_then(Py::as_str)
    }

    pub fn contains_key(&self, key: &str) -> bool {
        self.get(key).is_some()
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn keys(&self) -> impl Iterator<Item = &str> {
        self.items.iter().map(|(k, _)| k.as_str())
    }

    pub fn iter(&self) -> impl Iterator<Item = (&str, &Py)> {
        self.items.iter().map(|(k, v)| (k.as_str(), v))
    }

    /// `dict.update(other)`.
    pub fn update(&mut self, other: &PyDict) {
        for (k, v) in &other.items {
            self.set(k, v.clone());
        }
    }
}

/// `json.dumps(value, indent=indent)` with Python's defaults (ensure_ascii, separators).
pub fn dumps(value: &Py, indent: Option<usize>) -> String {
    let mut s = String::new();
    write_value(&mut s, value, indent, 0);
    s
}

/// `json.dumps(value, indent=2)`.
pub fn dumps2(value: &Py) -> String {
    dumps(value, Some(2))
}

fn write_value(sb: &mut String, v: &Py, indent: Option<usize>, level: usize) {
    match v {
        Py::Null => sb.push_str("null"),
        Py::Bool(b) => sb.push_str(if *b { "true" } else { "false" }),
        Py::Int(i) => {
            let _ = write!(sb, "{i}");
        }
        Py::Float(f) => sb.push_str(&repr_float(*f)),
        Py::Str(s) => write_str(sb, s),
        Py::List(items) => {
            container(sb, '[', ']', items.len(), indent, level, |sb, i, ind, lvl| write_value(sb, &items[i], ind, lvl));
        }
        Py::Dict(d) => {
            container(sb, '{', '}', d.items.len(), indent, level, |sb, i, ind, lvl| {
                let (k, v) = &d.items[i];
                write_str(sb, k);
                sb.push_str(": ");
                write_value(sb, v, ind, lvl);
            });
        }
    }
}

fn container(
    sb: &mut String,
    open: char,
    close: char,
    count: usize,
    indent: Option<usize>,
    level: usize,
    mut item: impl FnMut(&mut String, usize, Option<usize>, usize),
) {
    sb.push(open);
    if count == 0 {
        sb.push(close);
        return;
    }
    for i in 0..count {
        if i > 0 {
            sb.push_str(if indent.is_none() { ", " } else { "," });
        }
        if let Some(n) = indent {
            sb.push('\n');
            sb.push_str(&" ".repeat(n * (level + 1)));
        }
        item(sb, i, indent, level + 1);
    }
    if let Some(n) = indent {
        sb.push('\n');
        sb.push_str(&" ".repeat(n * level));
    }
    sb.push(close);
}

fn write_str(sb: &mut String, s: &str) {
    sb.push('"');
    for c in s.chars() {
        match c {
            '"' => sb.push_str("\\\""),
            '\\' => sb.push_str("\\\\"),
            '\n' => sb.push_str("\\n"),
            '\r' => sb.push_str("\\r"),
            '\t' => sb.push_str("\\t"),
            '\u{8}' => sb.push_str("\\b"),
            '\u{c}' => sb.push_str("\\f"),
            c if (c as u32) < 0x20 || (c as u32) > 0x7e => {
                let mut buf = [0u16; 2];
                for unit in c.encode_utf16(&mut buf) {
                    let _ = write!(sb, "\\u{unit:04x}");
                }
            }
            c => sb.push(c),
        }
    }
    sb.push('"');
}

/// Python's `repr(float)`: the shortest round-trip digits, exponent form below 1e-4 and from 1e16.
pub fn repr_float(d: f64) -> String {
    if d.is_nan() {
        return "NaN".into();
    }
    if d.is_infinite() {
        return if d > 0.0 { "Infinity".into() } else { "-Infinity".into() };
    }
    if d == 0.0 {
        return if d.is_sign_negative() { "-0.0".into() } else { "0.0".into() };
    }
    // shortest digits and decimal exponent from Rust's {:e} (shortest round trip as well)
    let e = format!("{d:e}");
    let (mantissa, exp) = e.split_once('e').unwrap_or((&e, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    let neg = mantissa.starts_with('-');
    let digits: String = mantissa.chars().filter(|c| c.is_ascii_digit()).collect();
    let sign = if neg { "-" } else { "" };
    if !(-4..16).contains(&exp) {
        let mut m = digits[..1].to_string();
        if digits.len() > 1 {
            m.push('.');
            m.push_str(&digits[1..]);
        }
        let es = if exp < 0 { "-" } else { "+" };
        return format!("{sign}{m}e{es}{:02}", exp.abs());
    }
    let point = exp + 1; // digits before the decimal point
    let s = if point <= 0 {
        format!("0.{}{}", "0".repeat((-point) as usize), digits)
    } else if (point as usize) >= digits.len() {
        format!("{}{}.0", digits, "0".repeat(point as usize - digits.len()))
    } else {
        format!("{}.{}", &digits[..point as usize], &digits[point as usize..])
    };
    format!("{sign}{s}")
}

/// Python's `hex(n)`: `0x1a2b`.
pub fn hex(n: i64) -> String {
    if n < 0 {
        format!("-0x{:x}", -(n as i128))
    } else {
        format!("0x{n:x}")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn floats_like_python() {
        assert_eq!(repr_float(1.0), "1.0");
        assert_eq!(repr_float(0.1), "0.1");
        assert_eq!(repr_float(1e-5), "1e-05");
        assert_eq!(repr_float(1e16), "1e+16");
        assert_eq!(repr_float(123456.789), "123456.789");
        assert_eq!(repr_float(0.0001), "0.0001");
        assert_eq!(repr_float(-2.5), "-2.5");
        assert_eq!(repr_float(1234567890123456.0), "1234567890123456.0");
    }

    #[test]
    fn dumps_like_python() {
        let d =
            PyDict::new().with("a", 1).with("b", vec!["x", "ü"]).with("c", PyDict::new()).with("d", Py::List(vec![]));
        assert_eq!(dumps(&d.clone().into(), None), r#"{"a": 1, "b": ["x", "\u00fc"], "c": {}, "d": []}"#);
        assert_eq!(
            dumps2(&d.into()),
            "{\n  \"a\": 1,\n  \"b\": [\n    \"x\",\n    \"\\u00fc\"\n  ],\n  \"c\": {},\n  \"d\": []\n}"
        );
        assert_eq!(hex(0xe7000), "0xe7000");
    }
}
