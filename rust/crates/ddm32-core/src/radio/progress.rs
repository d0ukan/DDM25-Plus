// SPDX-License-Identifier: GPL-3.0-or-later
//! The install and restore tools' own progress lines turned into what the page shows (Radio/Progress.cs): a phase
//! (a resource key that takes the percent) and the bar's percent.

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
        let line = line.trim();
        // ^(.+?): (\d{1,3})%(?: \((\d+)/(\d+)\))?$
        let re = regex::Regex::new(r"^(.+?): (\d{1,3})%(?: \((\d+)/(\d+)\))?$").unwrap();
        let c = re.captures(line)?;
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
