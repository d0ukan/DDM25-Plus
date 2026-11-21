// SPDX-License-Identifier: GPL-3.0-or-later
//! The data folder, settings.json and the backups' fixed home (`backups`).
pub mod backups;
pub mod paths;
pub mod settings;

pub use paths::{AppPaths, Os};
pub use settings::{Settings, Theme};

/// Today's local date as YYYYMMDD (backup folder names, data pack dates).
pub fn today() -> String {
    chrono::Local::now().format("%Y%m%d").to_string()
}

/// The local time as `HH:MM:SS` and `YYYY-MM-DD HH:MM:SS` (the self-update helper's log).
pub fn local_clock() -> (String, String) {
    let now = chrono::Local::now();
    (now.format("%H:%M:%S").to_string(), now.format("%Y-%m-%d %H:%M:%S").to_string())
}
