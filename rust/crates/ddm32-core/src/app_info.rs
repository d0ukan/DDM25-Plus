// SPDX-License-Identifier: GPL-3.0-or-later
//! The program's name, version and the fixed folder names (one place for all of them; AppInfo.cs).

pub const NAME: &str = "DDM32 Plus";
pub const EXE_NAME: &str = "DDM32Plus";

/// The program file inside the install folder and inside a program zip (`DDM32Plus/DDM32Plus.exe` on Windows).
pub const EXE_FILE: &str = if cfg!(windows) { "DDM32Plus.exe" } else { "DDM32Plus" };

/// The program file name the Windows program zips carry (the C# program's update format).
pub const WINDOWS_EXE_FILE: &str = "DDM32Plus.exe";

/// This (Rust) program's version; the workspace Cargo.toml carries the same value (checked by a test).
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// The C# (.NET) program this one follows; its last version, for the self-update hand-over.
pub const NET_VERSION: &str = "2.0.0-beta.1";

pub const AUTHOR: &str = "d0ukan · TA2DOU";
pub const REPO_URL: &str = "https://github.com/d0ukan/DDM32-Plus";

/// The Python program's data folder names under %LOCALAPPDATA% (read only, never moved or deleted).
pub const PYTHON_DATA_NAME: &str = "DDM32Plus";
pub const LEGACY_DATA_NAME: &str = "DDM25Plus";

/// The C# program's per-user install folder under %LOCALAPPDATA% (its `yedekler` is read, never moved).
pub const NET_INSTALL_SUBDIR: [&str; 2] = ["Programs", "DDM32 Plus"];

// Folders and files inside the program's data folder
pub const BACKUPS_FOLDER: &str = "yedekler";
pub const RELEASES_FOLDER: &str = "surumler";
pub const UPDATES_FOLDER: &str = "guncelleme";
pub const PACKS_FOLDER: &str = "paketler";
pub const SETTINGS_FILE: &str = "settings.json";

/// Names an update must never replace and an uninstaller must never remove blindly: the user's data.
pub const RESERVED_NAMES: [&str; 7] =
    [BACKUPS_FOLDER, RELEASES_FOLDER, UPDATES_FOLDER, PACKS_FOLDER, SETTINGS_FILE, "unins000.exe", "unins000.dat"];

/// Developer-only features (local release folder, a chosen backup folder) exist only with the `developer` feature.
pub const DEVELOPER: bool = cfg!(feature = "developer");
