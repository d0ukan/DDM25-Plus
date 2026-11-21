// SPDX-License-Identifier: GPL-3.0-or-later
fn main() {
    // Debug builds (and so the tests) keep Slint's element debug info: the keyboard / screen-reader test walks the
    // element tree with it. Release builds stay without it.
    let debug = std::env::var("PROFILE").is_ok_and(|p| p == "debug");
    let config = slint_build::CompilerConfiguration::new().with_debug_info(debug);
    slint_build::compile_with_config("ui/app.slint", config).expect("the Slint UI compiles");
}
