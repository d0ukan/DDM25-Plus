// SPDX-License-Identifier: GPL-3.0-or-later
//! The search log (SearchLog.cs; firmware tools/plus_searchlog.py, docs/plus.md "Search log on the PC"): the radio's
//! P25, DMR and all digital searches keep 8 finds each; read every second over the debug link (GET_VIEW sections p25s =
//! 28, dmrs = 29, alls = 30, read only, not counted as a look), every new call becomes a row here and in the day's CSV
//! file. A call is new
//! when an entry appears, its call counter grows, or its talkgroup / source changes. A radio restart (counters back
//! down) is a new start, not a call.

use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};

pub const SECTION_P25: u8 = 0x1C;
pub const SECTION_DMR: u8 = 0x1D;
pub const SECTION_ALL: u8 = 0x1E;
pub const CSV_HEADER: &str = "first_heard,last_heard,mode,mhz,code,slot,group,last_source,caller,encrypted,calls";

/// A local time as seconds since the Unix epoch plus the offset the program shows (the GUI passes the local clock).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Stamp(pub i64);

/// One call a search found (no audio). mode: "P25", "DMR", "YSF" or "NXDN"; code: P25 NAC ("$1C8/456"), DMR colour code
/// ("CC1"), YSF DG-ID ("DG0") or NXDN RAN ("RAN1"); slot: DMR slots seen and RPT / DIR; group: P25 / NXDN talkgroup or
/// DMR talkgroup / called ID; caller: the YSF source callsign (empty otherwise).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchCall {
    pub time: Stamp,
    pub mode: &'static str,
    pub hz: i64,
    pub code: String,
    pub slot: String,
    pub group: i64,
    pub source: i64,
    pub encrypted: bool,
    pub calls: i64,
    pub caller: String,
}

/// What an entry looked like at the last poll: calls, group, source, caller.
/// One line of the log: every call heard on one mode + frequency + code (NAC / CC / DG-ID / RAN) + group, counted
/// (owner 2026-11-21). first / last: when it was first and last heard; source / caller / slot: the last call's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchGroup {
    pub mode: &'static str,
    pub hz: i64,
    pub code: String,
    pub group: i64,
    pub first: Stamp,
    pub last: Stamp,
    pub slot: String,
    pub source: i64,
    pub caller: String,
    pub encrypted: bool,
    /// Calls counted before the radio's counter last went down (a restart, an entry dropped and found again).
    base: i64,
    /// The radio's own counter for this group now (the highest of its entries).
    current: i64,
}

impl SearchGroup {
    pub fn calls(&self) -> i64 {
        self.base + self.current
    }
}

type GroupKey = (&'static str, i64, String, i64);

/// The search log (SearchLog.cs): ONE line per mode + frequency + code + group with a call counter. The radio's entries
/// for the same group (its list may hold a frequency twice; the all digital search may list what the P25 search found)
/// count once, by their highest call counter; a counter that goes down starts a new base, so no call is lost and none
/// is counted twice. The day's CSV holds the same lines, rewritten on a change.
#[derive(Debug, Default)]
pub struct SearchLogBook {
    groups: HashMap<GroupKey, SearchGroup>,
    pub p25_on: Option<bool>,
    pub dmr_on: Option<bool>,
    pub all_on: Option<bool>,
    pub p25_at_hz: Option<i64>,
    pub dmr_at_hz: Option<i64>,
    pub all_at_hz: Option<i64>,
}

fn fields(text: &str, n: usize) -> Option<Vec<i64>> {
    let parts: Vec<&str> = text.split(',').collect();
    if parts.len() < n {
        return None;
    }
    parts[..n].iter().map(|p| p.trim().parse::<i64>().ok()).collect()
}

/// p25s.fI=HZ,NAC,TG,SRC,CALLS,AGE_S
pub fn parse_p25(text: &str, now: Stamp) -> Option<SearchCall> {
    let v = fields(text, 6)?;
    if v[0] <= 0 {
        return None;
    }
    let age = v[5].clamp(0, 86400);
    Some(SearchCall {
        time: Stamp(now.0 - age),
        mode: "P25",
        hz: v[0],
        code: nac(v[1]),
        slot: String::new(),
        group: v[2],
        source: v[3],
        encrypted: false,
        calls: v[4],
        caller: String::new(),
    })
}

/// dmrs.fI=HZ,CC,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S
pub fn parse_dmr(text: &str, now: Stamp) -> Option<SearchCall> {
    let v = fields(text, 10)?;
    if v[0] <= 0 {
        return None;
    }
    let age = v[9].clamp(0, 86400);
    let code = if (0..=15).contains(&v[1]) { format!("CC{}", v[1]) } else { "CC?".to_string() };
    Some(SearchCall {
        time: Stamp(now.0 - age),
        mode: "DMR",
        hz: v[0],
        code,
        slot: slots(v[2], v[3]),
        group: v[6],
        source: v[7],
        encrypted: v[5] != 0,
        calls: v[8],
        caller: String::new(),
    })
}

/// DMR slots seen and repeater / direct: "TS2 RPT", "TS1+2", "DIR".
fn slots(ts_mask: i64, sync_mask: i64) -> String {
    let slot = match ts_mask & 3 {
        1 => "TS1",
        2 => "TS2",
        3 => "TS1+2",
        _ => "",
    };
    let tag = if sync_mask & 1 != 0 {
        "RPT"
    } else if sync_mask & 2 != 0 {
        "DIR"
    } else {
        ""
    };
    format!("{slot} {tag}").trim().to_string()
}

/// alls.fI=HZ,MODE,CODE,TSMASK,SYNCMASK,GROUP,ENC,DST,SRC,CALLS,AGE_S,CALL (MODE P25 / DMR / YSF / NXDN; CODE the NAC,
/// colour code, DG-ID or RAN, 65535 not known; CALL the YSF callsign, the last field).
pub fn parse_all(text: &str, now: Stamp) -> Option<SearchCall> {
    let parts: Vec<&str> = text.splitn(12, ',').collect();
    if parts.len() < 11 {
        return None;
    }
    let mode: &'static str = match parts[1].trim() {
        "P25" => "P25",
        "DMR" => "DMR",
        "YSF" => "YSF",
        "NXDN" => "NXDN",
        _ => return None,
    };
    let nums: Vec<&str> = std::iter::once(parts[0]).chain(parts[2..11].iter().copied()).collect();
    let v = fields(&nums.join(","), 10)?;
    if v[0] <= 0 {
        return None;
    }
    let age = v[9].clamp(0, 86400);
    let known = (0..65535).contains(&v[1]);
    let code = match mode {
        "P25" if known => nac(v[1]),
        "P25" => "$?".to_string(),
        "DMR" if (0..=15).contains(&v[1]) => format!("CC{}", v[1]),
        "DMR" => "CC?".to_string(),
        "YSF" if known => format!("DG{}", v[1]),
        "YSF" => "DG?".to_string(),
        _ if known => format!("RAN{}", v[1]),
        _ => "RAN?".to_string(),
    };
    Some(SearchCall {
        time: Stamp(now.0 - age),
        mode,
        hz: v[0],
        code,
        slot: if mode == "DMR" { slots(v[2], v[3]) } else { String::new() },
        group: v[6],
        source: v[7],
        encrypted: v[5] != 0,
        calls: v[8],
        caller: parts.get(11).map(|c| c.trim().to_string()).unwrap_or_default(),
    })
}

impl SearchLogBook {
    pub fn new() -> Self {
        Self::default()
    }

    /// Every line, the most recently heard first.
    pub fn groups(&self) -> Vec<SearchGroup> {
        let mut v: Vec<SearchGroup> = self.groups.values().cloned().collect();
        v.sort_by(|a, b| b.last.cmp(&a.last).then(a.hz.cmp(&b.hz)).then(a.group.cmp(&b.group)));
        v
    }

    /// Distinct frequencies with a call.
    pub fn frequencies(&self) -> usize {
        let mut v: Vec<(&str, i64)> = self.groups.values().map(|g| (g.mode, g.hz)).collect();
        v.sort();
        v.dedup();
        v.len()
    }

    /// Every call counted.
    pub fn total_calls(&self) -> i64 {
        self.groups.values().map(SearchGroup::calls).sum()
    }

    /// One poll's key=value lines (all three sections); true when a line was added or changed.
    pub fn add<S: AsRef<str>>(&mut self, lines: &[S], now: Stamp) -> bool {
        let mut kv: HashMap<&str, &str> = HashMap::new();
        for l in lines {
            let l = l.as_ref().trim();
            if let Some((k, v)) = l.split_once('=') {
                if !k.is_empty() {
                    kv.insert(k, v);
                }
            }
        }
        let flag = |k: &str| kv.get(k).map(|v| *v == "1");
        let num = |k: &str| kv.get(k).and_then(|v| v.parse::<i64>().ok());
        self.p25_on = flag("p25s.on");
        self.dmr_on = flag("dmrs.on");
        self.all_on = flag("alls.on");
        self.p25_at_hz = num("p25s.at");
        self.dmr_at_hz = num("dmrs.at");
        self.all_at_hz = num("alls.at");
        // every entry of the three lists, grouped; a group's counter is its entries' highest
        let mut heard: HashMap<GroupKey, (i64, SearchCall)> = HashMap::new();
        for i in 0..8 {
            let found = [
                kv.get(format!("p25s.f{i}").as_str()).and_then(|t| parse_p25(t, now)),
                kv.get(format!("dmrs.f{i}").as_str()).and_then(|t| parse_dmr(t, now)),
                kv.get(format!("alls.f{i}").as_str()).and_then(|t| parse_all(t, now)),
            ];
            for c in found.into_iter().flatten() {
                if c.calls == 0 && c.group == 0 && c.source == 0 && c.caller.is_empty() {
                    continue;
                }
                let key = (c.mode, c.hz, c.code.clone(), c.group);
                let calls = c.calls.max(1);
                match heard.get_mut(&key) {
                    None => {
                        heard.insert(key, (calls, c));
                    }
                    Some(h) => {
                        h.0 = h.0.max(calls);
                        if c.time >= h.1.time {
                            h.1 = c;
                        }
                    }
                }
            }
        }
        let mut changed = false;
        for (key, (calls, c)) in heard {
            match self.groups.get_mut(&key) {
                None => {
                    self.groups.insert(
                        key,
                        SearchGroup {
                            mode: c.mode,
                            hz: c.hz,
                            code: c.code.clone(),
                            group: c.group,
                            first: c.time,
                            last: c.time,
                            slot: c.slot.clone(),
                            source: c.source,
                            caller: c.caller.clone(),
                            encrypted: c.encrypted,
                            base: 0,
                            current: calls,
                        },
                    );
                    changed = true;
                }
                Some(g) => {
                    if calls < g.current {
                        g.base += g.current; // the radio's counter went down: what it counted stays
                        g.current = calls;
                        g.last = g.last.max(c.time);
                        changed = true;
                    } else if calls > g.current || c.source != g.source || c.caller != g.caller {
                        g.current = calls;
                        g.last = g.last.max(c.time);
                        g.source = c.source;
                        g.caller = c.caller.clone();
                        if !c.slot.is_empty() {
                            g.slot = c.slot.clone();
                        }
                        g.encrypted |= c.encrypted;
                        changed = true;
                    }
                }
            }
        }
        changed
    }
}

/// The local clock as a Stamp (seconds of local time since 1970-01-01 00:00 local).
pub fn local_stamp() -> Stamp {
    Stamp(chrono::Local::now().naive_local().and_utc().timestamp())
}

/// A P25 NAC in both forms the radio shows: "$659/1625" (hex, decimal; owner 2026-11-21).
pub fn nac(v: i64) -> String {
    format!("${v:03X}/{v}")
}

pub fn mhz(hz: i64) -> String {
    format!("{:.4}", hz as f64 / 1e6)
}

/// "2026-11-21 22:00:00" from a local Stamp.
pub fn format_time(t: Stamp) -> String {
    let days = t.0.div_euclid(86400);
    let secs = t.0.rem_euclid(86400);
    let (y, m, d) = civil(days);
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}", secs / 3600, secs / 60 % 60, secs % 60)
}

/// Days since 1970-01-01 to (year, month, day) (H. Hinnant's civil_from_days).
fn civil(z: i64) -> (i64, i64, i64) {
    let z = z + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y + 1 } else { y }, m, d)
}

pub fn csv_row(g: &SearchGroup) -> String {
    format!(
        "{},{},{},{},{},{},{},{},{},{},{}",
        format_time(g.first),
        format_time(g.last),
        g.mode,
        mhz(g.hz),
        g.code,
        g.slot,
        g.group,
        g.source,
        g.caller.replace(',', " "),
        if g.encrypted { 1 } else { 0 },
        g.calls()
    )
}

/// Writes folder/search-YYYYMMDD.csv with every line of the log (rewritten whole through a temporary file, so a crash
/// never leaves half a file). The day is the day the log started.
pub fn write(folder: &Path, lines: &[SearchGroup], day: Stamp) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(folder)?;
    let date = format_time(day);
    let path = folder.join(format!("search-{}.csv", date[..10].replace('-', "")));
    let mut text = String::from(CSV_HEADER);
    text.push('\n');
    for g in lines {
        text.push_str(&csv_row(g));
        text.push('\n');
    }
    let tmp = path.with_extension("csv.tmp");
    {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(text.as_bytes())?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, &path)?;
    Ok(path)
}
