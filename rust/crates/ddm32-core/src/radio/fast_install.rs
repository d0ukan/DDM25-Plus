// SPDX-License-Identifier: GPL-3.0-or-later
//! The two faster update paths (owner's decision 2026-11-20, AGENTS.md rule 4; Radio/FastInstall.cs), only for an
//! update over an installed DDM32 Plus, never for a first install from stock:
//!
//! (A) quick update, --quick --backup B: an earlier verified two-pass backup of this radio is reused. Before any write
//!     the radio must be the same radio: its chip IDs, its boot area and its codec slot are read live (only those
//!     ranges) and must equal the backup's. Every application sector below the slot is rewritten; the boot area, the
//!     slot and the data chip are never written.
//! (B) install without backup, --no-backup: no stored backup. The 1 MiB program flash is read once, live, kept in
//!     memory only (no file), and the plan and every check are made from it.
//!
//! Both keep all of install_plus's rules: only application sectors, in plan order, once; the data chip read for its
//! sector CRCs before and after and never written; the whole program read back and compared before the restart; a
//! failed write is never retried. Every run is one user-approved step.

use super::codec::Codec;
use super::error::{ToolError, ToolResult};
use super::guards::{Authorization, InstallPort};
use super::monitor::{dm32_board, ram_monitor, sha, DumpSink, MonitorRadio};
use super::plan::{self, capacity, check_chips, check_monitor, device_crcs, SIZE, SLOT, START};
use super::tools::{pyfile, run_writes, write_receipt, Args, ToolContext};
use super::unit_image::{self, detect};
use crate::pyjson::{self, Py, PyDict};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// Over an installed DDM32 Plus only (the detection of a backup or of the live read).
pub fn require_ddm32(detection: &PyDict, what: &str) -> ToolResult<()> {
    if detection.str("layout") == Some("stock") {
        return Err(ToolError::value(format!(
            "A first install from stock firmware needs a new backup (its codec slot is prepared from it): {what} holds stock firmware"
        )));
    }
    if detection.str("layout") != Some("classic") || detection.str("firmware") != Some("ddm32") {
        return Err(ToolError::value(format!(
            "This path is only for an update over an installed DDM32 Plus: {what} holds {} ({}); take a new backup and use the normal install",
            detection.str("firmware").unwrap_or(""),
            detection.str("layout").unwrap_or("")
        )));
    }
    Ok(())
}

/// (A)'s images: target = backup boot + application + backup slot; the basis marks every application sector below
/// the slot as changed (inverted), so all of them are rewritten whatever the radio holds there now.
pub fn quick_inputs(
    backup: &Path,
    plus: &Path,
    key_hex: Option<&str>,
    codec: &Codec,
) -> ToolResult<(Vec<u8>, Vec<u8>, u64)> {
    let mut images = unit_image::saved_images(backup)?;
    let program = images.remove("progmem-pass1.bin").unwrap();
    require_ddm32(&detect(&program, codec)?, "the backup")?;
    let tag = plan::slot_check(&program)?;
    let app = plan::plus_application(plus, key_hex)?;
    let target = plan::with_application(&program, &app);
    let mut basis = target.clone();
    for b in &mut basis[START..SLOT] {
        *b ^= 0xFF;
    }
    Ok((basis, target, tag))
}

/// (A)'s images and the plan it prints (files only; also what the wizard's check step shows).
pub fn quick_plan(
    backup: &Path,
    plus: &Path,
    key_hex: Option<&str>,
    codec: &Codec,
) -> ToolResult<(Vec<u8>, Vec<u8>, PyDict)> {
    let (basis, target, tag) = quick_inputs(backup, plus, key_hex, codec)?;
    unit_image::backup_jedec(backup)?;
    let mut plan = plan::make_plan("install_plus_quick", &basis, &target, Some(tag), None)?;
    plan.set("backup", "reused");
    plan.set("identity_checked_live", vec!["chip_ids", "boot_area", "codec_slot"]);
    Ok((basis, target, plan))
}

/// `--port P --plus R --out NEWDIR (--quick --backup B | --no-backup) [--wait 1..600] [--plan-only]`
pub fn fast_install_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    let args = Args::new(
        argv,
        &["--port", "--backup", "--plus", "--out", "--wait"],
        &["--quick", "--no-backup", "--plan-only"],
        &["--port", "--plus", "--out"],
    )?;
    let quick = args.has("--quick");
    let no_backup = args.has("--no-backup");
    if quick == no_backup
        || (quick && args.get("--backup").is_none())
        || (no_backup && args.has("--backup"))
        || (no_backup && args.has("--plan-only"))
    {
        return Err(ToolError::exit("2"));
    }
    let wait = args.int("--wait", 600)?;
    if !(1..=600).contains(&wait) {
        return Err(ToolError::exit("2"));
    }
    let out_dir = PathBuf::from(args.get("--out").unwrap());
    let plus = PathBuf::from(args.get("--plus").unwrap());
    let mut basis = vec![];
    let mut target = vec![];
    let mut plan: Option<PyDict> = None;
    let mut expected_chips: Option<HashMap<&'static str, String>> = None;
    let mut app = vec![];
    if quick {
        let backup = PathBuf::from(args.get("--backup").unwrap());
        let (b, t, p) = quick_plan(&backup, &plus, ctx.key(), ctx.codec)?;
        (basis, target) = (b, t);
        expected_chips = Some(unit_image::backup_jedec(&backup)?);
        (ctx.print)(&pyjson::dumps2(&Py::Dict(p.clone())));
        plan = Some(p);
        if args.has("--plan-only") {
            return Ok(());
        }
    } else {
        app = plan::plus_application(&plus, ctx.key())?;
    }
    ram_monitor::image()?;
    pyfile::make_new_dir(&out_dir)?;
    let mut report = PyDict::new()
        .with("mode", if quick { "install_plus_quick" } else { "install_plus_no_backup" })
        .with("backup", if quick { "reused" } else { "none" })
        .with("started_utc", ctx.clock.utc_stamp());
    if let Some(p) = &plan {
        report.update(p);
    }
    for k in [
        "writes_started",
        "installation_verified",
        "bootloader_unchanged",
        "codec_slot_verified",
        "data_sector_crcs_unchanged",
        "restart_sent",
    ] {
        report.set(k, false);
    }
    let port_name = args.get("--port").unwrap().to_string();
    let raw = (ctx.serial)(&port_name)?;
    let mut guard = if quick {
        InstallPort::new(raw, &basis, &target, false, Authorization::Identity)?
    } else {
        InstallPort::deferred(raw)?
    };
    let result =
        fast_run(&mut guard, &mut report, &out_dir, &port_name, wait, quick, target, &app, expected_chips, ctx);
    if let Err(e) = &result {
        report.set("error", e.record());
    }
    report.set("finished_utc", ctx.clock.utc_stamp());
    let _ = guard.ro.raw.close();
    let receipt = write_receipt(&mut report, &guard, &out_dir);
    result.and(receipt)
}

#[allow(clippy::too_many_arguments)]
fn fast_run(
    guard: &mut InstallPort,
    report: &mut PyDict,
    out_dir: &Path,
    port_name: &str,
    wait: i64,
    quick: bool,
    mut target: Vec<u8>,
    app: &[u8],
    expected_chips: Option<HashMap<&'static str, String>>,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    guard.ro.raw.open()?;
    let mut radio = MonitorRadio::new(guard, ctx.print, ctx.clock, 3);
    (ctx.print)(&format!(
        "READY on {port_name}: turn the radio OFF, then ON normally, no buttons held. Waiting {wait}s."
    ));
    radio.start(wait as u32)?;
    radio.configure(&dm32_board())?;
    check_monitor(&radio)?;
    radio.check_writable()?;
    let before_data: Vec<u32>;
    if quick {
        // the same radio: chip IDs, boot area and codec slot as in the backup (only those ranges are read)
        check_chips(&mut radio, expected_chips.as_ref().unwrap())?;
        let boot = read_range(&mut radio, 0, START, None, ctx.print)?;
        let slot = read_range(&mut radio, SLOT, SIZE, None, ctx.print)?;
        let boot_ok = boot == target[..START];
        let slot_ok = slot == target[SLOT..];
        report.set("live_boot_area_matches_backup", boot_ok);
        report.set("live_codec_slot_matches_backup", slot_ok);
        write_receipt(report, radio.port, out_dir)?;
        if !boot_ok || !slot_ok {
            return Err(ToolError::value(
                "This radio does not match the backup (boot area or codec slot differ): take a new backup",
            ));
        }
        (ctx.print)("Same radio: chip IDs, boot area and codec slot equal the backup's.");
        let q = radio.port.ro.limits["q128"] as usize;
        before_data = device_crcs(&mut radio, "q128", q, "Check settings chip", ctx.print)?;
        radio.port.authorize_identity(&boot, &slot)?;
    } else {
        for memory in ["progmem", "q128"] {
            let jedec = radio.jedec(memory)?;
            let cap = capacity(&jedec, memory)?;
            radio.port.ro.limits.insert(memory, cap);
            report.set(&format!("{memory}_jedec"), hex::encode(&jedec));
        }
        if radio.port.ro.limits["progmem"] != SIZE as u64 {
            return Err(ToolError::value("Expected a 1 MiB program flash"));
        }
        // the single live read: kept in memory, never saved
        let live = read_range(&mut radio, 0, SIZE, Some("Read current firmware (once, kept in memory)"), ctx.print)?;
        report.set("live_program_sha256", sha(&live));
        let detection = detect(&live, ctx.codec)?;
        report.set("detection", detection.clone());
        require_ddm32(&detection, "the radio")?;
        let tag_live = plan::slot_check(&live)?;
        target = plan::with_application(&live, app);
        let mut p = plan::make_plan("install_plus_no_backup", &live, &target, Some(tag_live), None)?;
        p.set("backup", "none");
        p.set("live_read_once", true);
        (ctx.print)(&pyjson::dumps2(&Py::Dict(p.clone())));
        report.update(&p);
        radio.port.set_images(&live, &target)?;
        let q = radio.port.ro.limits["q128"] as usize;
        before_data = device_crcs(&mut radio, "q128", q, "Check settings chip", ctx.print)?;
        write_receipt(report, radio.port, out_dir)?;
        radio.port.authorize(&live, &before_data, &before_data)?;
    }
    report.set("writes_started", true);
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)(&format!(
        "INSTALLING: {} application sectors below the codec slot. Boot, slot and data writes blocked.",
        radio.port.todo.len()
    ));
    run_writes(&mut radio, report, out_dir, "Written and sector-readback checked", ctx)?;
    let after = read_range(&mut radio, 0, SIZE, Some("Read complete installed program chip"), ctx.print)?;
    let after_data =
        device_crcs(&mut radio, "q128", before_data.len() * 4096, "Recheck untouched settings chip", ctx.print)?;
    if after[SLOT..] != target[SLOT..] || plan::slot_check(&after)? != plan::slot_check(&target)? {
        return Err(ToolError::value("Codec slot changed; restart disabled"));
    }
    radio.port.authorize_reset(&after, &after_data, &before_data)?;
    for k in ["installation_verified", "bootloader_unchanged", "codec_slot_verified", "data_sector_crcs_unchanged"] {
        report.set(k, true);
    }
    report.set("installed_program_sha256", sha(&after));
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)("VERIFIED: application, untouched boot area, codec slot and settings chip all read back. Restarting.");
    radio.retries = 0;
    radio.reset_chip()?;
    report.set("restart_sent", true);
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)("PASS: DDM32 Plus installed and verified. Check the boot screen on the radio. No RF command was sent.");
    Ok(())
}

/// [start, end) of the program flash into memory (every sector checked against the radio's CRC).
fn read_range(
    radio: &mut MonitorRadio<'_, InstallPort>,
    start: usize,
    end: usize,
    label: Option<&str>,
    print: &dyn Fn(&str),
) -> ToolResult<Vec<u8>> {
    let mut ms: Vec<u8> = Vec::new();
    let mut last: i64 = -1;
    let mut progress = |done: u64, total: u64| {
        if let Some(l) = label {
            let p = ((done - start as u64) * 100 / (total - start as u64)) as i64;
            if p / 10 != last {
                print(&format!("{l}: {p}%"));
                last = p / 10;
            }
        }
    };
    radio.dump(&mut ms as &mut dyn DumpSink, "progmem", start as u64, end as u64, Some(&mut progress), true)?;
    if ms.len() != end - start {
        return Err(ToolError::value("Incomplete program readback"));
    }
    Ok(ms)
}
