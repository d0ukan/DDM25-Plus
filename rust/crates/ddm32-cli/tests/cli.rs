// SPDX-License-Identifier: GPL-3.0-or-later
//! The command line without the `radio` feature (the default build): it runs, and no radio tool can reach a port.
use std::process::Command;

fn ddm32(args: &[&str]) -> (i32, String, String) {
    let o = Command::new(env!("CARGO_BIN_EXE_ddm32"))
        .args(args)
        .env("DDM32_DATA", std::env::temp_dir().join("ddm32-cli-test"))
        .output()
        .unwrap();
    (
        o.status.code().unwrap_or(-1),
        String::from_utf8_lossy(&o.stdout).into(),
        String::from_utf8_lossy(&o.stderr).into(),
    )
}

#[test]
fn version_names_the_build_switches() {
    let (code, out, _) = ddm32(&["version"]);
    assert_eq!(0, code);
    assert!(out.contains(env!("CARGO_PKG_VERSION")));
    assert!(out.contains("radio access: off"));
}

#[test]
fn no_radio_tool_reaches_a_port_without_the_radio_feature() {
    // a port name that does not exist; the build has no serial code at all
    let out = std::env::temp_dir().join(format!("ddm32-cli-backup-{}", std::process::id()));
    let (code, _, err) = ddm32(&[
        "tool",
        "backup",
        "--port",
        "NOPORT",
        "--out",
        &out.to_string_lossy(),
        "--installed-firmware",
        "x",
        "--wait",
        "1",
    ]);
    assert_eq!(1, code);
    assert!(err.contains("RadioDisabled"), "{err}");
    let _ = std::fs::remove_dir_all(&out);
    let (code, _, err) = ddm32(&["tool", "datapack", "read", "--port", "NOPORT"]);
    assert_eq!(1, code);
    assert!(err.contains("RadioDisabled"), "{err}");
}

#[test]
fn usage_and_unknown_commands() {
    assert_eq!(2, ddm32(&[]).0);
    assert_eq!(1, ddm32(&["nonsense"]).0);
    assert_eq!(2, ddm32(&["tool", "backup", "--bogus"]).0);
}
