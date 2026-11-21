// SPDX-License-Identifier: GPL-3.0-or-later
//! The radio tools' own progress lines turned into what the page shows (Radio/Progress.cs and
//! ToolRunner.BackupProgress): a phase (a resource key that takes the percent), the phase's percent and the bar's
//! overall percent.
//!
//! The backup prints `progmem pass 1: 25% (262144/1048576)` per memory and pass; [`backup_progress`] turns that into
//! one percent over both passes of both chips (the 16 MiB data chip weighs 16 times the 1 MiB program chip). The
//! install and restore print their phases ("Read current firmware: 40%", "...checked: 25% (1/4)", "Verify ...");
//! [`ProgressTracker`] names the phase and [`ProgressView`] spreads the phases over one bar that only moves forward.
//! Every other tool line (JEDEC IDs, "boot ROM answered", ...) is a [`ProgressEvent::Info`]: it never replaces the
//! percent line.

use std::sync::OnceLock;

fn re(cell: &'static OnceLock<regex::Regex>, pattern: &str) -> &'static regex::Regex {
    cell.get_or_init(|| regex::Regex::new(pattern).unwrap())
}

/// The resource key of the backup's percent line.
pub const BACKUP_KEY: &str = "Prog_Backup";

const PROGMEM: u64 = 1 << 20;
const Q128: u64 = 1 << 24;

/// Backup progress 0..100 over both passes of both chips from a backup line, else None.
pub fn backup_progress(line: &str) -> Option<i64> {
    static RE: OnceLock<regex::Regex> = OnceLock::new();
    let c = re(&RE, r"^(progmem|q128) pass ([12]): \d{1,3}% \((\d+)/(\d+)\)$").captures(line.trim())?;
    let pass: u64 = c[2].parse().ok()?;
    let done: u64 = c[3].parse().ok()?;
    let total: u64 = c[4].parse().ok()?;
    if total == 0 {
        return None;
    }
    // the line's own total scales its chip's share, so a chip of another size still fills its part exactly
    let (before, share) = if &c[1] == "q128" { (PROGMEM, Q128) } else { (0, PROGMEM) };
    let at = (pass - 1) * (PROGMEM + Q128) + before + done.min(total) * share / total;
    Some(((at * 100) / (2 * (PROGMEM + Q128))).min(100) as i64)
}

#[derive(Debug, Default, Clone)]
pub struct ProgressTracker {
    phase: Option<&'static str>,
}

impl ProgressTracker {
    pub fn new() -> Self {
        Self::default()
    }

    /// (resource key, percent of the bar) for a progress line, else None.
    pub fn feed(&mut self, line: &str) -> Option<(&'static str, i64)> {
        static RE: OnceLock<regex::Regex> = OnceLock::new();
        let c = re(&RE, r"^(.+?): (\d{1,3})%(?: \((\d+)/(\d+)\))?$").captures(line.trim())?;
        let label = c.get(1).unwrap().as_str();
        let p: i64 = c[2].parse::<i64>().unwrap().clamp(0, 100);
        let r: Option<(&'static str, i64)> = match label {
            "Read current firmware" => Some(("Prog_Compare", p / 2)),
            "Read current firmware (once, kept in memory)" => Some(("Prog_Read", p / 2)),
            "Check settings chip" => {
                Some((if self.phase == Some("Prog_Read") { "Prog_Read" } else { "Prog_Compare" }, 50 + p / 2))
            }
            "Written and sector-readback checked" | "Firmware written and sector-readback checked" => {
                Some(("Prog_Write", p))
            }
            "Read complete installed program chip" => Some(("Prog_Verify", p / 2)),
            "Recheck untouched settings chip" => Some(("Prog_Verify", 50 + p / 2)),
            "Verify newly installed application" | "Verify restored application" => Some(("Prog_Verify", p)),
            _ => None,
        };
        if let Some((k, _)) = r {
            self.phase = Some(k);
        }
        r
    }
}

/// What one tool line means for the progress area.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ProgressEvent {
    /// A percent: the phase's resource key, the phase's percent (for the text) and the bar's overall percent.
    Percent { key: &'static str, phase: i64, overall: i64 },
    /// The tool has the port open and waits for the radio to be switched on.
    Ready,
    /// Any other line worth showing (a JEDEC ID, "boot ROM answered", ...): shown beside the percent, never instead.
    Info(String),
    /// JSON and blank lines.
    Ignore,
}

/// The share of the bar each install phase fills (start, end): checks before the writes, the writes, the readback.
fn phase_span(key: &str) -> (i64, i64) {
    match key {
        "Prog_Compare" | "Prog_Read" => (0, 30),
        "Prog_Write" => (30, 70),
        "Prog_Verify" => (70, 100),
        _ => (0, 100),
    }
}

/// One run's progress: feed every tool line; the overall percent never goes back during a run.
#[derive(Debug, Default, Clone)]
pub struct ProgressView {
    tracker: ProgressTracker,
    overall: i64,
    last: Option<(&'static str, i64)>,
}

impl ProgressView {
    pub fn new() -> Self {
        Self::default()
    }

    /// The bar's overall percent so far.
    pub fn overall(&self) -> i64 {
        self.overall
    }

    /// The last (key, phase percent), for a text that must keep showing it.
    pub fn last(&self) -> Option<(&'static str, i64)> {
        self.last
    }

    pub fn feed(&mut self, line: &str) -> ProgressEvent {
        let t = line.trim();
        if let Some(p) = backup_progress(t) {
            return self.percent(BACKUP_KEY, p, p);
        }
        if let Some((key, p)) = self.tracker.feed(t) {
            let (a, b) = phase_span(key);
            return self.percent(key, p, a + (b - a) * p / 100);
        }
        if t.starts_with("READY") {
            return ProgressEvent::Ready;
        }
        if t.is_empty() || line.starts_with(['{', '}', ' ', '"', '[', ']']) {
            return ProgressEvent::Ignore;
        }
        ProgressEvent::Info(t.to_string())
    }

    fn percent(&mut self, key: &'static str, phase: i64, overall: i64) -> ProgressEvent {
        self.overall = self.overall.max(overall.clamp(0, 100));
        self.last = Some((key, phase));
        ProgressEvent::Percent { key, phase, overall: self.overall }
    }
}
