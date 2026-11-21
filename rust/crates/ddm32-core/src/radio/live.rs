// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads what the health and live readers print (pc/ddm32_app/parsers.py parse_health, health_summary, parse_blocks,
//! live_summary, s_meter, format_mhz, format_uptime; health_tab.verdict; live_tab.dbm_percent; the C# LiveData.cs).
//! Held to the Python results in live-results.json.

use super::debug::SCAN_FIELDS;
use std::collections::HashMap;

pub const LOW_STACK: i64 = 200;
pub const LOW_HEAP: i64 = 1024;
pub const DBM_LOW: i64 = -140;
pub const DBM_HIGH: i64 = -40;
pub const INTERVALS: [i32; 5] = [1, 2, 3, 4, 5];
pub const DEFAULT_INTERVAL: i32 = 2;

/// The radio's health from the debug-link views tasks, crash, reset (parsers.health_summary).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HealthSummary {
    pub uptime_ms: Option<i64>,
    pub heap_free: Option<i64>,
    pub heap_min_free: Option<i64>,
    pub min_task: Option<String>,
    pub min_free: Option<i64>,
    pub crash_valid: bool,
    pub crash_built: Option<String>,
    pub crash_task: Option<String>,
    pub crash_pc: Option<String>,
    pub crash_uptime_ms: Option<i64>,
    pub reset_cause: Option<String>,
    pub tasks: Vec<(String, Option<i64>)>,
    pub ok: bool,
}

/// health_tab's traffic light.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    Grey,
    Green,
    Amber,
    Red,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct LiveSignal {
    pub ok: bool,
    pub rssi_dbm: Option<i64>,
    pub noise: Option<i64>,
    pub state: Option<String>,
    pub mode: Option<String>,
    pub zone: Option<String>,
    pub channel: Option<String>,
    pub channel_number: Option<i64>,
    pub vfo: bool,
    pub rx_hz: Option<i64>,
    pub tx_hz: Option<i64>,
    pub squelch: Option<i64>,
    pub monitor: bool,
    pub uptime_ms: Option<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct LiveNative {
    pub ok: bool,
    pub status: Option<i64>,
    pub state: Option<String>,
    pub reason: Option<String>,
    pub frequency_hz: Option<i64>,
    pub rx_count: Option<i64>,
    pub rx_seen: Option<i64>,
    pub p25_sync: Option<i64>,
    pub faults: Option<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct LiveSummary {
    pub signal: LiveSignal,
    pub native: LiveNative,
    pub scan: Option<Vec<(String, Option<i64>)>>,
    pub scan_status: Option<i64>,
    pub health: Option<HealthSummary>,
}

/// One '--- NAME (ID) status CODE' block and its key=value lines.
#[derive(Debug, Clone, Default)]
pub struct LiveBlock {
    pub status: i64,
    pub values: HashMap<String, String>,
    pub lines: Vec<String>,
}

fn re(p: &str) -> regex::Regex {
    regex::Regex::new(p).unwrap()
}

/// ("poll" or "end", number) of a poll marker line, or None.
pub fn poll_marker(line: &str) -> Option<(String, i64)> {
    let c = re(r"^=== (poll|end) (\d+)").captures(line.trim())?;
    Some((c[1].to_string(), c[2].parse().ok()?))
}

/// Python int(text, 0): an optional sign, 0x / 0o / 0b, no leading zeros, underscores between digits.
pub fn py_int0(text: &str) -> Option<i64> {
    let mut t = text.trim();
    let mut neg = false;
    if let Some(r) = t.strip_prefix('-') {
        neg = true;
        t = r;
    } else if let Some(r) = t.strip_prefix('+') {
        t = r;
    }
    if t.is_empty() || t.starts_with('_') || t.ends_with('_') || t.contains("__") {
        return None;
    }
    let mut base = 10;
    let lower = t.to_ascii_lowercase();
    let body: String = if t.len() > 2 && (lower.starts_with("0x") || lower.starts_with("0o") || lower.starts_with("0b"))
    {
        base = match &lower[1..2] {
            "x" => 16,
            "o" => 8,
            _ => 2,
        };
        t[2..].trim_start_matches('_').to_string()
    } else {
        if t.len() > 1 && t.starts_with('0') && t.replace('_', "").chars().any(|c| c != '0') {
            return None; // "012": no leading zeros in base 0
        }
        t.to_string()
    };
    let body = body.replace('_', "");
    if body.is_empty() {
        return None;
    }
    let mut v: i64 = 0;
    for c in body.chars() {
        let d = c.to_digit(36)? as i64;
        if d >= base {
            return None;
        }
        v = v.checked_mul(base)?.checked_add(d)?;
    }
    Some(if neg { -v } else { v })
}

fn kv(line: &str) -> Option<(&str, &str)> {
    let eq = line.find('=')?;
    if eq == 0 || line[..eq].contains(' ') {
        return None;
    }
    Some((&line[..eq], &line[eq + 1..]))
}

type Parsed = (Vec<(String, i64)>, Vec<(String, Option<i64>)>, Vec<(String, Option<i64>)>, HashMap<String, String>);

/// The health views' key=value lines: (status by view, uptime by view, tasks, every value).
pub fn parse_health(lines: &[String]) -> Parsed {
    let view_re = re(r"^--- view (\w+) \((\d+)\) status (-?\d+)");
    let mut status: Vec<(String, i64)> = vec![];
    let mut uptime: Vec<(String, Option<i64>)> = vec![];
    let mut tasks: Vec<(String, Option<i64>)> = vec![];
    let mut values = HashMap::new();
    let mut view: Option<String> = None;
    for raw in lines {
        let line = raw.trim();
        if let Some(c) = view_re.captures(line) {
            let v = c[1].to_string();
            let st: i64 = c[3].parse().unwrap_or(0);
            status.retain(|s| s.0 != v);
            status.push((v.clone(), st));
            view = Some(v);
            continue;
        }
        let Some((key, value)) = kv(line) else { continue };
        if key == "uptime_ms" {
            if let Some(v) = &view {
                uptime.retain(|u| &u.0 != v);
                uptime.push((v.clone(), py_int0(value)));
                continue;
            }
        }
        values.insert(key.to_string(), value.to_string());
        if let Some(name) = key.strip_prefix("tasks.free.") {
            match tasks.iter_mut().find(|t| t.0 == name) {
                Some(t) => t.1 = py_int0(value),
                None => tasks.push((name.to_string(), py_int0(value))),
            }
        }
    }
    (status, uptime, tasks, values)
}

fn num(v: &HashMap<String, String>, k: &str) -> Option<i64> {
    v.get(k).and_then(|s| py_int0(s))
}

fn st(v: &HashMap<String, String>, k: &str) -> Option<String> {
    v.get(k).cloned()
}

pub fn health_summary(lines: &[String]) -> HealthSummary {
    let (status, uptime, tasks, v) = parse_health(lines);
    let up = uptime.iter().find(|u| u.0 == "tasks").or(uptime.first()).and_then(|u| u.1);
    let mut sorted: Vec<(usize, (String, Option<i64>))> = tasks.into_iter().enumerate().collect();
    sorted.sort_by_key(|(i, t)| (t.1.is_none(), t.1.unwrap_or(0), *i));
    HealthSummary {
        uptime_ms: up,
        heap_free: num(&v, "tasks.heap_free"),
        heap_min_free: num(&v, "tasks.heap_min_free"),
        min_task: st(&v, "tasks.min_task"),
        min_free: num(&v, "tasks.min_free"),
        crash_valid: v.get("crash.valid").map(String::as_str) == Some("1"),
        crash_built: st(&v, "crash.built"),
        crash_task: st(&v, "crash.task"),
        crash_pc: st(&v, "crash.pc"),
        crash_uptime_ms: num(&v, "crash.uptime_ms"),
        reset_cause: st(&v, "reset.cause"),
        tasks: sorted.into_iter().map(|(_, t)| t).collect(),
        ok: !status.is_empty() && status.iter().all(|s| s.1 == 0),
    }
}

/// Red: a crash record; amber: heap low-water under 1 KB or a stack under 200 B; else green.
pub fn verdict(s: &HealthSummary) -> Verdict {
    if s.crash_valid {
        return Verdict::Red;
    }
    let low_task = s.tasks.iter().any(|t| t.1.is_some_and(|f| f < LOW_STACK));
    if s.heap_min_free.is_some_and(|h| h < LOW_HEAP) || low_task {
        Verdict::Amber
    } else {
        Verdict::Green
    }
}

/// The blocks of one poll (or of the lines before the first poll): {name: block}.
pub fn parse_blocks(lines: &[String]) -> HashMap<String, LiveBlock> {
    let block_re = re(r"^--- (.+?) \((\S+)\) status (-?\d+)");
    let poll_re = re(r"^=== (poll|end) (\d+)");
    let mut out: HashMap<String, LiveBlock> = HashMap::new();
    let mut cur: Option<String> = None;
    for raw in lines {
        let line = raw.trim();
        if let Some(c) = block_re.captures(line) {
            let name = c[1].to_string();
            let mut b = LiveBlock { status: c[3].parse().unwrap_or(0), ..Default::default() };
            b.lines.push(line.to_string());
            out.insert(name.clone(), b);
            cur = Some(name);
            continue;
        }
        let Some(name) = &cur else { continue };
        if poll_re.is_match(line) {
            continue;
        }
        let b = out.get_mut(name).unwrap();
        b.lines.push(line.to_string());
        if let Some((k, v)) = kv(line) {
            b.values.insert(k.to_string(), v.to_string());
        }
    }
    out
}

pub fn live_summary(blocks: &HashMap<String, LiveBlock>, once: &HashMap<String, LiveBlock>) -> LiveSummary {
    let mut all = once.clone();
    for (k, v) in blocks {
        all.insert(k.clone(), v.clone());
    }
    let empty = HashMap::new();
    let ok = |name: &str| all.get(name).filter(|b| b.status == 0).map_or(&empty, |b| &b.values);
    let cv = ok("view core");
    let rssi = num(cv, "rssi_dbm");
    let signal = LiveSignal {
        ok: !cv.is_empty(),
        rssi_dbm: rssi.filter(|&r| r != 0),
        noise: num(cv, "noise"),
        state: st(cv, "state"),
        mode: st(cv, "mode"),
        zone: st(cv, "zone_name"),
        channel: st(cv, "channel_name"),
        channel_number: num(cv, "channel_number"),
        vfo: cv.get("vfo").map(String::as_str) == Some("1"),
        rx_hz: num(cv, "rx_hz"),
        tx_hz: num(cv, "tx_hz"),
        squelch: num(cv, "squelch"),
        monitor: cv.get("monitor").map(String::as_str) == Some("1"),
        uptime_ms: num(cv, "uptime_ms"),
    };
    let nv = ok("native");
    let native = LiveNative {
        ok: !nv.is_empty(),
        status: all.get("native").map(|b| b.status),
        state: st(nv, "state_name"),
        reason: st(nv, "reason_name"),
        frequency_hz: num(nv, "frequency_hz"),
        rx_count: num(nv, "rx_count"),
        rx_seen: num(nv, "rx_seen"),
        p25_sync: num(nv, "p25_sync"),
        faults: num(nv, "faults"),
    };
    let sv = ok("scan");
    let scan = (!sv.is_empty() && !sv.contains_key("raw"))
        .then(|| SCAN_FIELDS.iter().map(|k| (k.to_string(), num(sv, k))).collect());
    let health_lines: Vec<String> = ["view tasks", "view crash", "view reset"]
        .iter()
        .filter_map(|n| all.get(*n))
        .flat_map(|b| b.lines.clone())
        .collect();
    let health = (!health_lines.is_empty()).then(|| health_summary(&health_lines));
    LiveSummary { signal, native, scan, scan_status: all.get("scan").map(|b| b.status), health }
}

/// VHF/UHF S-meter (IARU R.1: S9 = -93 dBm, 6 dB a unit): "S7", "S9+20", or "" without a reading.
pub fn s_meter(dbm: Option<i64>) -> String {
    match dbm {
        None | Some(0) => String::new(),
        Some(d) if d > -93 => format!("S9+{}", d + 93),
        Some(d) => {
            let s = 9 - ((-93.0 - d as f64) / 6.0).round_ties_even() as i64;
            format!("S{}", s.clamp(0, 9))
        }
    }
}

/// 434250000 -> "434,25000 MHz" (Turkish) or "434.25000 MHz"; "-" without a frequency.
pub fn format_mhz(hz: Option<i64>, decimal_comma: bool) -> String {
    match hz {
        None | Some(0) => "-".into(),
        Some(h) => {
            let s = format!("{:.5} MHz", h as f64 / 1e6);
            if decimal_comma {
                s.replace('.', ",")
            } else {
                s
            }
        }
    }
}

/// h:mm:ss, "-" without a value.
pub fn format_uptime(ms: Option<i64>) -> String {
    match ms {
        None => "-".into(),
        Some(ms) => {
            let s = ms / 1000;
            format!("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60)
        }
    }
}

/// The signal bar: -140 .. -40 dBm as 0..100.
pub fn dbm_percent(dbm: Option<i64>) -> i64 {
    match dbm {
        None => 0,
        Some(d) => (((d.clamp(DBM_LOW, DBM_HIGH) - DBM_LOW) * 100) as f64 / (DBM_HIGH - DBM_LOW) as f64)
            .round_ties_even() as i64,
    }
}

/// "1.200 B" (Turkish thousands dot) or "1,200 B"; "-" without a value.
pub fn bytes(v: Option<i64>, turkish: bool) -> String {
    let Some(v) = v else { return "-".into() };
    let digits = v.unsigned_abs().to_string();
    let mut out = String::new();
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i) % 3 == 0 {
            out.push(if turkish { '.' } else { ',' });
        }
        out.push(c);
    }
    format!("{}{out} B", if v < 0 { "-" } else { "" })
}
