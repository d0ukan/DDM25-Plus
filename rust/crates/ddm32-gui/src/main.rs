// SPDX-License-Identifier: GPL-3.0-or-later
//! DDM32 Plus: the window. `--self-update ...` and `--update-check FILE` are the self-update protocol (the same as the
//! C# program's, so either program can hand over to the other); `--demo` shows example contents (no radio, no network).
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

use ddm32_core::update::self_update as su;
use ddm32_gui::AppWindow;
use slint::ComponentHandle;

fn main() -> Result<(), slint::PlatformError> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some(su::HELPER_ARG) {
        let code = su::helper_main(&args[1..], &mut |pid| Box::new(su::RealProcesses::new(pid)));
        std::process::exit(code);
    }
    let ui = AppWindow::new()?;
    let paths =
        ddm32_core::storage::AppPaths::for_this_program().unwrap_or_else(|| ddm32_core::storage::AppPaths::new("."));
    let demo = args.iter().any(|a| a == "--demo");
    ddm32_gui::setup(&ui, paths, !demo);
    if demo {
        ddm32_gui::demo(&ui);
    }
    if let Some(i) = args.iter().position(|a| a == su::CHECK_ARG) {
        // the new program is up: tell the helper (after the window exists)
        if let Some(marker) = args.get(i + 1) {
            let _ = su::write_marker(std::path::Path::new(marker), ddm32_core::app_info::VERSION);
        }
    }
    ui.run()
}
