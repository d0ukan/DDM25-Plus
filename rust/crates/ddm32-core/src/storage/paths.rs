// SPDX-License-Identifier: GPL-3.0-or-later
//! Where the program keeps things (Storage/AppPaths.cs, with the per-OS data folder of the Rust program).
//!
//! The data folder ("root"):
//! - Windows: the program's own install folder (the .exe's folder), as the C# program does: the setup installs per
//!   user to `%LOCALAPPDATA%\Programs\DDM32 Plus`, so the folder is the user's own and writable, and the Rust
//!   program takes over the C# program's folder (and its `backups`) on the self-update hand-over.
//! - Linux: `$XDG_DATA_HOME/DDM32Plus`, else `~/.local/share/DDM32Plus` (an AppImage's own folder is read only).
//! - macOS: `~/Library/Application Support/DDM32Plus` (an .app bundle must not be written to).
//!
//! Inside it:
//!   settings.json   settings (language, theme, disclaimer acceptance, update check)
//!   backups/       the radio's backups and every install / restore log (private: calibration, channels).
//!                   The user never chooses this folder; program updates and the uninstaller never touch it.
//!   releases/       downloaded firmware releases and the verified manifest cache
//!   updates/     downloaded program updates, the self-update's staging and rollback copies
//!   packs/       data packs the user built
//! `DDM32_DATA` overrides the folder (tests, screenshots).

use crate::app_info;
use std::path::{Path, PathBuf};

/// A data folder of the Python or C# program found on this PC (read only: never moved or deleted).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OtherDataFolder {
    pub folder: PathBuf,
    pub backups: PathBuf,
    pub backup_folders: usize,
    pub verified_backups: usize,
    /// "python" or "net".
    pub program: &'static str,
}

/// Which OS's rules to use (a parameter so every OS's rules are tested on every OS).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Os {
    Windows,
    Linux,
    MacOs,
}

impl Os {
    pub fn current() -> Os {
        if cfg!(windows) {
            Os::Windows
        } else if cfg!(target_os = "macos") {
            Os::MacOs
        } else {
            Os::Linux
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppPaths {
    pub root: PathBuf,
}

/// Environment lookup (tests pass a fake).
pub type Env<'a> = &'a dyn Fn(&str) -> Option<String>;

pub fn real_env(name: &str) -> Option<String> {
    std::env::var(name).ok()
}

fn non_empty(v: Option<String>) -> Option<String> {
    v.filter(|s| !s.trim().is_empty())
}

impl AppPaths {
    pub fn new(root: impl AsRef<Path>) -> AppPaths {
        let r = root.as_ref();
        AppPaths { root: std::path::absolute(r).unwrap_or_else(|_| r.to_path_buf()) }
    }

    pub fn backups(&self) -> PathBuf {
        self.root.join(app_info::BACKUPS_FOLDER)
    }
    pub fn releases(&self) -> PathBuf {
        self.root.join(app_info::RELEASES_FOLDER)
    }
    pub fn updates(&self) -> PathBuf {
        self.root.join(app_info::UPDATES_FOLDER)
    }
    pub fn packs(&self) -> PathBuf {
        self.root.join(app_info::PACKS_FOLDER)
    }
    pub fn settings_file(&self) -> PathBuf {
        self.root.join(app_info::SETTINGS_FILE)
    }

    /// The data folder for this OS (see the module text) unless DDM32_DATA is set. `exe_dir` is the program's folder.
    pub fn detect(exe_dir: &Path, os: Os, env: Env<'_>) -> Option<AppPaths> {
        if let Some(over) = non_empty(env("DDM32_DATA")) {
            return Some(AppPaths::new(over));
        }
        match os {
            Os::Windows => Some(AppPaths::new(exe_dir)),
            Os::Linux => {
                if let Some(x) = non_empty(env("XDG_DATA_HOME")).filter(|x| Path::new(x).is_absolute()) {
                    return Some(AppPaths::new(Path::new(&x).join(app_info::EXE_NAME)));
                }
                let home = non_empty(env("HOME"))?;
                Some(AppPaths::new(Path::new(&home).join(".local").join("share").join(app_info::EXE_NAME)))
            }
            Os::MacOs => {
                let home = non_empty(env("HOME"))?;
                Some(AppPaths::new(
                    Path::new(&home).join("Library").join("Application Support").join(app_info::EXE_NAME),
                ))
            }
        }
    }

    /// The real data folder of this program on this OS.
    pub fn for_this_program() -> Option<AppPaths> {
        let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
        AppPaths::detect(&exe_dir, Os::current(), &real_env)
    }

    /// Creates the data folders (backups, releases, updates).
    /// Moves the old Turkish folders (up to C# 2.0.0-beta.2) to the English names first: a rename on the same disk,
    /// nothing copied or deleted. A folder whose new name already exists, or that cannot be moved, stays; the next
    /// start tries again, and an old backups folder that stays is still searched ([`AppPaths::own_backup_roots`]).
    pub fn ensure(&self) -> std::io::Result<&AppPaths> {
        self.migrate_legacy_folders();
        for d in [self.backups(), self.releases(), self.updates()] {
            std::fs::create_dir_all(d)?;
        }
        Ok(self)
    }

    /// The old folder names moved this time.
    pub fn migrate_legacy_folders(&self) -> Vec<&'static str> {
        let mut moved = Vec::new();
        for (old, new) in app_info::LEGACY_FOLDERS {
            let from = self.root.join(old);
            let to = self.root.join(new);
            if from.is_dir() && !to.exists() && std::fs::rename(&from, &to).is_ok() {
                moved.push(old);
            }
        }
        moved
    }

    /// The old `yedekler` when it is still there (a move that could not be done, or both names present).
    pub fn legacy_backups(&self) -> Option<PathBuf> {
        let l = self.root.join(app_info::LEGACY_BACKUPS_FOLDER);
        l.is_dir().then_some(l)
    }

    /// This program's backup roots: `backups`, then the old `yedekler` if it is still there.
    pub fn own_backup_roots(&self) -> Vec<PathBuf> {
        let mut v = vec![self.backups()];
        v.extend(self.legacy_backups());
        v
    }

    /// The other programs' data folders on Windows (read only): the Python program's `%LOCALAPPDATA%\DDM32Plus` and
    /// `DDM25Plus` (and under %APPDATA%), and the C# program's install folder `%LOCALAPPDATA%\Programs\DDM32 Plus`
    /// when this program lives elsewhere. This program's own folder is never reported. Empty on Linux and macOS
    /// (the older programs ran on Windows only).
    pub fn find_other_data(&self, os: Os, env: Env<'_>) -> Vec<OtherDataFolder> {
        let mut found: Vec<OtherDataFolder> = Vec::new();
        if os != Os::Windows {
            return found;
        }
        let mut bases: Vec<String> = Vec::new();
        for b in [non_empty(env("LOCALAPPDATA")), non_empty(env("APPDATA"))].into_iter().flatten() {
            if !bases.contains(&b) {
                bases.push(b);
            }
        }
        let mut candidates: Vec<(PathBuf, &'static str)> = Vec::new();
        for b in &bases {
            for name in [app_info::PYTHON_DATA_NAME, app_info::LEGACY_DATA_NAME] {
                candidates.push((Path::new(b).join(name), "python"));
            }
        }
        if let Some(local) = non_empty(env("LOCALAPPDATA")) {
            let mut p = PathBuf::from(local);
            for part in app_info::NET_INSTALL_SUBDIR {
                p.push(part);
            }
            candidates.push((p, "net"));
        }
        for (folder, program) in candidates {
            let folder = std::path::absolute(&folder).unwrap_or(folder);
            if !folder.is_dir() || same_path(&folder, &self.root) || found.iter().any(|f| same_path(&f.folder, &folder))
            {
                continue;
            }
            let backups = if program == "net" {
                // the C# program: backups, or yedekler up to 2.0.0-beta.2
                let new = folder.join(app_info::BACKUPS_FOLDER);
                if new.is_dir() {
                    new
                } else {
                    folder.join(app_info::LEGACY_BACKUPS_FOLDER)
                }
            } else {
                folder.join(app_info::PYTHON_BACKUPS_FOLDER)
            };
            if program == "net" && !backups.is_dir() {
                continue;
            }
            let (all, verified) = count_backups(&backups);
            found.push(OtherDataFolder { folder, backups, backup_folders: all, verified_backups: verified, program });
        }
        found
    }

    /// What an uninstaller may remove from the data folder (the setup's [UninstallDelete]): everything but the
    /// user's backups. `backups` is never in this list.
    pub fn removable_on_uninstall(&self) -> Vec<PathBuf> {
        vec![self.releases(), self.updates(), self.packs(), self.settings_file()]
    }
}

/// parent/name, or name-b, name-c ... the first that does not exist (the tools refuse an existing --out).
pub fn unique_folder(parent: &Path, name: &str) -> PathBuf {
    let first = parent.join(name);
    if !first.exists() {
        return first;
    }
    for c in 'b'..='z' {
        let p = parent.join(format!("{name}-{c}"));
        if !p.exists() {
            return p;
        }
    }
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    parent.join(format!("{name}-{secs}"))
}

/// (backup folders, verified ones): folders with a backup-manifest.json, verified when it says so.
pub fn count_backups(backups: &Path) -> (usize, usize) {
    let Ok(rd) = std::fs::read_dir(backups) else { return (0, 0) };
    let (mut all, mut verified) = (0, 0);
    for e in rd.filter_map(Result::ok) {
        let d = e.path();
        if !d.is_dir() || !d.join("backup-manifest.json").is_file() {
            continue;
        }
        all += 1;
        if is_verified_backup(&d) {
            verified += 1;
        }
    }
    (all, verified)
}

/// backup-manifest.json says backup_verified: true (file read only).
pub fn is_verified_backup(folder: &Path) -> bool {
    std::fs::read(folder.join("backup-manifest.json"))
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .is_some_and(|v| v.get("backup_verified") == Some(&serde_json::Value::Bool(true)))
}

/// Two paths name the same folder (case-insensitive, as on Windows; trailing separators ignored).
pub fn same_path(a: &Path, b: &Path) -> bool {
    let norm = |p: &Path| {
        let s = std::path::absolute(p).unwrap_or_else(|_| p.to_path_buf()).to_string_lossy().to_lowercase();
        s.trim_end_matches(['/', '\\']).to_string()
    };
    norm(a) == norm(b)
}
