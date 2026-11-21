// SPDX-License-Identifier: GPL-3.0-or-later
//! The radio tools, held byte for byte to the Python tools (Radio/Tools.cs):
//! - [`backup_tool`]: pc/radiotools/dm32_readonly_backup.py, the two-pass backup through the pinned RAM monitor;
//! - [`install_tool`]: install_plus.py, install a DDM32 Plus release, or put the radio back from its backup;
//! - [`restore_stock_tool`]: install_firstboot.py --restore-stock --use-saved-backup, the proven stock restore.

use super::codec::Codec;
use super::error::{ToolError, ToolResult};
use super::guards::{Authorization, InstallPort, ReadOnlyPort};
use super::monitor::{dm32_board, ram_monitor, sha, v6_py, DumpSink, MonitorRadio};
use super::plan::{self, capacity, check_chips, check_monitor, device_crcs, read_program, write_plan};
use super::port::{Clock, RawSerial};
use super::unit_image;
use crate::pyjson::{self, Py, PyDict};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// What a tool run gets from its caller: where its lines go, the clock, the only way to a serial port, the release
/// key and the codec tables. The program passes `RadioAccess::factory` or `RadioAccess::write_factory` explicitly;
/// the tests pass a replay of a recorded run or a fake radio. Without one the tool gets no port at all
/// (`port::no_port`), so a caller that forgets it can never reach a real radio, in any build.
pub struct ToolContext<'a> {
    pub print: &'a dyn Fn(&str),
    pub clock: &'a dyn Clock,
    pub serial: &'a dyn Fn(&str) -> ToolResult<Box<dyn RawSerial>>,
    /// The release key to check a release folder with; None: the embedded one.
    pub release_key_hex: Option<String>,
    pub codec: &'a Codec,
}

impl ToolContext<'_> {
    pub fn key(&self) -> Option<&str> {
        self.release_key_hex.as_deref()
    }
}

/// argparse for the tools' few options: an unknown, missing or malformed option ends the run (exit 2).
pub(crate) struct Args {
    values: HashMap<String, Option<String>>,
}

impl Args {
    pub fn new(argv: &[String], with_value: &[&str], flags: &[&str], required: &[&str]) -> ToolResult<Args> {
        let mut values = HashMap::new();
        let mut i = 0;
        while i < argv.len() {
            let a = argv[i].as_str();
            if with_value.contains(&a) {
                if i + 1 >= argv.len() {
                    return Err(ToolError::exit("2"));
                }
                values.insert(a.to_string(), Some(argv[i + 1].clone()));
                i += 2;
                continue;
            } else if flags.contains(&a) {
                values.insert(a.to_string(), None);
            } else {
                return Err(ToolError::exit("2"));
            }
            i += 1;
        }
        if required.iter().any(|r| !values.contains_key(*r)) {
            return Err(ToolError::exit("2"));
        }
        Ok(Args { values })
    }

    pub fn get(&self, name: &str) -> Option<&str> {
        self.values.get(name).and_then(|v| v.as_deref())
    }

    pub fn has(&self, name: &str) -> bool {
        self.values.contains_key(name)
    }

    /// Python int() of the option's value (exit 2 when it is no int).
    pub fn int(&self, name: &str, fallback: i64) -> ToolResult<i64> {
        match self.values.get(name) {
            None => Ok(fallback),
            Some(v) => crate::pytext::py_int(v.as_deref().unwrap_or("")).ok_or_else(|| ToolError::exit("2")),
        }
    }
}

/// Files as Python writes them on Windows, on every OS (text mode: "\n" becomes "\r\n"): the receipts and backup
/// manifests are byte for byte the Windows Python tool's.
pub mod pyfile {
    use super::*;

    pub fn crlf(text: &str) -> Vec<u8> {
        text.replace('\n', "\r\n").into_bytes()
    }

    pub fn write_text(path: &Path, text: &str) -> ToolResult<()> {
        std::fs::write(path, crlf(text))?;
        Ok(())
    }

    /// open(path, 'x'): only a new file.
    pub fn create_text(path: &Path, text: &str) -> ToolResult<()> {
        use std::io::Write;
        let mut f = std::fs::OpenOptions::new().write(true).create_new(true).open(path)?;
        f.write_all(&crlf(text))?;
        Ok(())
    }

    /// Path.mkdir(parents=True, exist_ok=False)
    pub fn make_new_dir(path: &Path) -> ToolResult<()> {
        if path.exists() {
            return Err(ToolError::file_exists(format!(
                "[WinError 183] Cannot create a file when that file already exists: '{}'",
                path.display()
            )));
        }
        std::fs::create_dir_all(path)?;
        Ok(())
    }
}

fn dumps(d: &PyDict) -> String {
    pyjson::dumps2(&Py::Dict(d.clone()))
}

/// The arguments as owned strings (the tools take `&[String]`).
pub fn argv(items: &[&str]) -> Vec<String> {
    items.iter().map(|s| s.to_string()).collect()
}

// ---- backup ---------------------------------------------------------------------------------------------------------

fn file_record(path: &Path, memory: &str, size: u64, capture_pass: i64) -> ToolResult<PyDict> {
    let data = std::fs::read(path)?;
    if data.len() as u64 != size {
        return Err(ToolError::value("Incomplete capture"));
    }
    let half = (size / 2) as usize;
    let blank = data.iter().all(|&b| b == 0xFF);
    let repeated = data[..half] == data[half..];
    let sectors: Vec<Py> = data.chunks(4096).map(|c| Py::Str(sha(c))).collect();
    Ok(PyDict::new()
        .with("file", path.file_name().unwrap().to_string_lossy().into_owned())
        .with("memory", memory)
        .with("bytes", size as i64)
        .with("pass", capture_pass)
        .with("sha256", sha(&data))
        .with("sector_bytes", 4096)
        .with("sector_sha256", Py::List(sectors))
        .with("all_blank", blank)
        .with("suspicious_repeated_halves", repeated && !blank))
}

fn capture(
    radio: &mut MonitorRadio<'_, ReadOnlyPort>,
    memory: &str,
    size: u64,
    destination: &Path,
    capture_pass: i64,
    print: &dyn Fn(&str),
) -> ToolResult<PyDict> {
    let partial = crate::releases::net::partial_path(destination);
    if destination.is_file() {
        return Err(ToolError::file_exists(destination.to_string_lossy()));
    }
    let mut last: i64 = -1;
    {
        let mut out = std::fs::OpenOptions::new().read(true).write(true).create_new(true).open(&partial)?;
        let mut progress = |done: u64, total: u64| {
            let percent = (done * 100 / total) as i64;
            if percent / 5 != last {
                last = percent / 5;
                print(&format!("{memory} pass {capture_pass}: {percent}% ({done}/{total})"));
            }
        };
        radio.dump(&mut out as &mut dyn DumpSink, memory, 0, size, Some(&mut progress), true)?;
        out.sync_all()?;
    }
    let mut record = file_record(&partial, memory, size, capture_pass)?;
    if destination.exists() {
        return Err(ToolError::file_exists(destination.to_string_lossy()));
    }
    std::fs::rename(&partial, destination)?;
    record.set("file", destination.file_name().unwrap().to_string_lossy().into_owned());
    Ok(record)
}

fn compare(first: &PyDict, second: &PyDict) -> ToolResult<PyDict> {
    if first.get("memory") != second.get("memory") || first.get("bytes") != second.get("bytes") {
        return Err(ToolError::value("Capture ranges differ"));
    }
    let a = first.get("sector_sha256").and_then(Py::as_list).cloned().unwrap_or_default();
    let b = second.get("sector_sha256").and_then(Py::as_list).cloned().unwrap_or_default();
    let changed: Vec<Py> =
        a.iter().zip(&b).enumerate().filter(|(_, (x, y))| x != y).map(|(i, _)| Py::Int(i as i64 * 4096)).collect();
    let n = first.get("bytes").and_then(Py::as_i64).unwrap_or(0) as usize / 4096;
    if a.len() != n || b.len() != n {
        return Err(ToolError::value("Missing sector hashes"));
    }
    Ok(PyDict::new()
        .with("memory", first.get("memory").cloned().unwrap_or(Py::Null))
        .with("identical", changed.is_empty())
        .with("changed_sector_addresses", Py::List(changed)))
}

/// `--port COMx --out NEWDIR --installed-firmware TEXT [--wait 1..120]`
pub fn backup_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    let args = Args::new(
        argv,
        &["--port", "--out", "--installed-firmware", "--wait"],
        &[],
        &["--port", "--out", "--installed-firmware"],
    )?;
    let wait = args.int("--wait", 120)?;
    if !(1..=120).contains(&wait) {
        return Err(ToolError::exit("2"));
    }
    let out_dir = PathBuf::from(args.get("--out").unwrap());
    ram_monitor::image()?;
    pyfile::make_new_dir(&out_dir)?;
    let mut report = PyDict::new()
        .with("upstream_commit", ram_monitor::UPSTREAM_PIN)
        .with("host_tool_sha256", ram_monitor::HOST_TOOL_SHA256)
        .with("ram_monitor_sha256", ram_monitor::SHA256)
        .with("installed_firmware_user_reported", args.get("--installed-firmware").unwrap())
        .with("started_utc", ctx.clock.utc_stamp())
        .with("flash_written", false)
        .with("rf_command_sent", false)
        .with("backup_verified", false)
        .with("captures", Py::List(vec![]));
    let port_name = args.get("--port").unwrap().to_string();
    let raw = (ctx.serial)(&port_name)?;
    let mut guard = ReadOnlyPort::new(raw)?;
    let manifest = out_dir.join("backup-manifest.partial.json");
    let result = backup_run(&mut guard, &mut report, &out_dir, &manifest, &port_name, wait, ctx);
    if let Err(e) = &result {
        report.set("backup_verified", false);
        report.set("error", e.record());
        report.set("outgoing_commands", guard.counts.clone());
        let _ = pyfile::write_text(&manifest, &(dumps(&report) + "\n"));
    }
    let _ = guard.raw.close();
    result
}

fn backup_run(
    guard: &mut ReadOnlyPort,
    report: &mut PyDict,
    out_dir: &Path,
    manifest: &Path,
    port_name: &str,
    wait: i64,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    guard.raw.open()?;
    (ctx.print)(&format!(
        "READY: {port_name} open; switch radio OFF then ON normally, cable seated, no keys held. Waiting {wait}s."
    ));
    let mut radio = MonitorRadio::new(guard, ctx.print, ctx.clock, 3);
    radio.start(wait as u32)?;
    if radio.v6.is_none() {
        return Err(ToolError::value("Expected pinned universal monitor protocol"));
    }
    radio.configure(&dm32_board())?;
    report.set("ram_monitor_footprint", v6_py(&radio.v6));
    let mut chips = PyDict::new();
    report.set("chips", chips.clone());
    for memory in ["progmem", "q128"] {
        let jedec = radio.jedec(memory)?;
        let size = capacity(&jedec, memory)?;
        radio.port.limits.insert(memory, size);
        chips.set(memory, PyDict::new().with("jedec", hex::encode(&jedec)).with("bytes", size as i64));
        report.set("chips", chips.clone());
        (ctx.print)(&format!("{memory}: JEDEC {}, {size} bytes", hex::encode(&jedec)));
    }
    radio.progmem_size = radio.port.limits["progmem"];
    pyfile::write_text(manifest, &(dumps(report) + "\n"))?;
    let mut captures: Vec<PyDict> = Vec::new();
    for capture_pass in [1, 2] {
        for memory in ["progmem", "q128"] {
            let size = radio.port.limits[memory];
            let rec = capture(
                &mut radio,
                memory,
                size,
                &out_dir.join(format!("{memory}-pass{capture_pass}.bin")),
                capture_pass,
                ctx.print,
            )?;
            captures.push(rec);
            report.set("captures", Py::List(captures.iter().cloned().map(Py::Dict).collect()));
            pyfile::write_text(manifest, &(dumps(report) + "\n"))?;
        }
    }
    let comparison = [compare(&captures[0], &captures[2])?, compare(&captures[1], &captures[3])?];
    report.set("comparison", Py::List(comparison.iter().cloned().map(Py::Dict).collect()));
    let verified = comparison.iter().all(|c| c.get("identical").is_some_and(Py::is_true))
        && !captures.iter().any(|c| {
            c.get("all_blank").is_some_and(Py::is_true) || c.get("suspicious_repeated_halves").is_some_and(Py::is_true)
        });
    report.set("backup_verified", verified);
    if !verified {
        return Err(ToolError::value(
            "Captures changed, are blank, or suggest address aliasing; preserve and investigate",
        ));
    }
    let data = std::fs::read(out_dir.join("q128-pass1.bin"))?;
    let codeplug = &data[0x1000..0xc9000];
    {
        use std::io::Write;
        let mut f = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(out_dir.join("stock-codeplug-pages.raw.bin"))?;
        f.write_all(codeplug)?;
    }
    report.set(
        "codeplug_raw",
        PyDict::new()
            .with("file", "stock-codeplug-pages.raw.bin")
            .with("start", 0x1000)
            .with("end_exclusive", 0xc9000)
            .with("bytes", codeplug.len())
            .with("sha256", sha(codeplug))
            .with("vendor_cps_export", false),
    );
    report.set("completed_utc", ctx.clock.utc_stamp());
    report.set("outgoing_commands", radio.port.counts.clone());
    pyfile::create_text(&out_dir.join("backup-manifest.json"), &(dumps(report) + "\n"))?;
    (ctx.print)("VERIFIED: both complete flash ranges read twice and matching. Power cycle the radio to start its firmware normally.");
    Ok(())
}

// ---- install / restore over the Classic layout ----------------------------------------------------------------------

/// The receipt of an install or restore run: the report plus the guard's counts.
pub(crate) fn write_receipt(report: &mut PyDict, guard: &InstallPort, out_dir: &Path) -> ToolResult<()> {
    report.set("outgoing_operations", guard.ro.counts.clone());
    report.set("planned_write_frames", guard.plan.len());
    report.set("sent_write_frames", guard.index);
    pyfile::write_text(&out_dir.join("install-receipt.json"), &(dumps(report) + "\n"))
}

/// The progress lines of the writes ("...checked: 25% (1/4)"), a receipt at each step of 5 %.
pub(crate) fn write_progress_line(label: &str, n: usize, total: usize, last: &mut i64) -> Option<String> {
    let p = (n * 100 / total) as i64;
    if p / 5 != *last {
        *last = p / 5;
        Some(format!("{label}: {p}% ({n}/{total})"))
    } else {
        None
    }
}

/// `--port COMx --backup B (--plus R [--from-stock] | --restore) --out NEWDIR [--wait 1..600] [--plan-only]`
pub fn install_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    let args = Args::new(
        argv,
        &["--port", "--backup", "--plus", "--out", "--wait"],
        &["--from-stock", "--restore", "--plan-only"],
        &["--port", "--backup", "--out"],
    )?;
    let restore = args.has("--restore");
    let plus = args.get("--plus").filter(|p| !p.is_empty()).map(PathBuf::from);
    let from_stock = args.has("--from-stock");
    if restore == plus.is_some() || (from_stock && plus.is_none()) {
        return Err(ToolError::exit("2"));
    }
    let wait = args.int("--wait", 600)?;
    if !(1..=600).contains(&wait) {
        return Err(ToolError::exit("2"));
    }
    let backup = PathBuf::from(args.get("--backup").unwrap());
    let out_dir = PathBuf::from(args.get("--out").unwrap());
    let (mode, inputs, tag, prof) = if restore {
        ("restore", plan::restore_inputs(&backup, true)?, None, None)
    } else if from_stock {
        let (i, p) = plan::stock_inputs(&backup, plus.as_ref().unwrap(), ctx.key(), ctx.codec)?;
        ("install_plus_from_stock", i, Some(p.tag), Some(p))
    } else {
        let (i, t) = plan::install_inputs(&backup, plus.as_ref().unwrap(), ctx.key())?;
        ("install_plus", i, Some(t), None)
    };
    let expected_chips = unit_image::backup_jedec(&backup)?;
    let plan = plan::make_plan(mode, &inputs.original, &inputs.target, tag, prof.as_ref())?;
    (ctx.print)(&dumps(&plan));
    if args.has("--plan-only") {
        return Ok(());
    }
    ram_monitor::image()?;
    let expected_data = plan::crc_list(&inputs.data);
    pyfile::make_new_dir(&out_dir)?;
    let mut report = plan.clone();
    report.set("started_utc", ctx.clock.utc_stamp());
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
    let mut guard = InstallPort::new(raw, &inputs.original, &inputs.target, restore, Authorization::Full)?;
    let result = install_run(
        &mut guard,
        &mut report,
        &out_dir,
        &port_name,
        wait,
        restore,
        &inputs,
        &expected_data,
        &expected_chips,
        tag,
        prof.is_some(),
        ctx,
    );
    if let Err(e) = &result {
        report.set("error", e.record());
    }
    report.set("finished_utc", ctx.clock.utc_stamp());
    let _ = guard.ro.raw.close();
    let receipt = write_receipt(&mut report, &guard, &out_dir);
    result.and(receipt)
}

#[allow(clippy::too_many_arguments)]
fn install_run(
    guard: &mut InstallPort,
    report: &mut PyDict,
    out_dir: &Path,
    port_name: &str,
    wait: i64,
    restore: bool,
    inputs: &plan::Inputs,
    expected_data: &[u32],
    expected_chips: &HashMap<&'static str, String>,
    tag: Option<u64>,
    from_stock: bool,
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
    check_chips(&mut radio, expected_chips)?;
    let target = &inputs.target;
    if restore {
        radio.port.authorize_saved_backup()?;
        report.set("writes_started", true);
        write_receipt(report, radio.port, out_dir)?;
        (ctx.print)(&format!(
            "RESTORING: {} sectors from the backup. Boot and data writes blocked.",
            radio.port.todo.len()
        ));
        write_plan(&mut radio, &mut |_, _| {})?;
        let after = read_program(
            &mut radio,
            &out_dir.join("application-after.bin"),
            "Verify restored application",
            ctx.print,
            plan::START,
        )?;
        radio.port.authorize_reset_application(&after)?;
        report.set("installation_verified", true);
        report.set("codec_slot_verified", after[plan::SLOT - plan::START..] == target[plan::SLOT..]);
        report.set("installed_application_sha256", sha(&after));
        write_receipt(report, radio.port, out_dir)?;
        (ctx.print)("VERIFIED: application and codec slot read back as in the backup. Restarting.");
        radio.retries = 0;
        radio.reset_chip()?;
        report.set("restart_sent", true);
        write_receipt(report, radio.port, out_dir)?;
        (ctx.print)("PASS: restored. Check the boot screen on the radio.");
        return Ok(());
    }
    let before = read_program(&mut radio, &out_dir.join("program-before.bin"), "Read current firmware", ctx.print, 0)?;
    let before_data = device_crcs(&mut radio, "q128", inputs.data.len(), "Check settings chip", ctx.print)?;
    report.set("precheck_program_matches_backup", before == inputs.original);
    let changed: Vec<Py> = before_data
        .iter()
        .zip(expected_data)
        .enumerate()
        .filter(|(_, (a, b))| a != b)
        .map(|(i, _)| Py::Int(i as i64 * plan::SECTOR as i64))
        .collect();
    report.set("precheck_changed_data_sectors", Py::List(changed));
    write_receipt(report, radio.port, out_dir)?;
    radio.port.authorize(&before, &before_data, expected_data)?;
    report.set("writes_started", true);
    write_receipt(report, radio.port, out_dir)?;
    let what =
        if from_stock { "application and codec slot sectors" } else { "application sectors below the codec slot" };
    (ctx.print)(&format!("INSTALLING: {} {what}. Boot and data writes blocked.", radio.port.todo.len()));
    let started = ctx.clock.monotonic();
    run_writes(&mut radio, report, out_dir, "Written and sector-readback checked", ctx)?;
    report.set("write_seconds", ctx.clock.monotonic() - started);
    let after_program = read_program(
        &mut radio,
        &out_dir.join("program-after.bin"),
        "Read complete installed program chip",
        ctx.print,
        0,
    )?;
    let after_data = device_crcs(&mut radio, "q128", inputs.data.len(), "Recheck untouched settings chip", ctx.print)?;
    if !from_stock {
        if after_program[plan::SLOT..] != inputs.original[plan::SLOT..] {
            return Err(ToolError::value("Codec slot changed; restart disabled"));
        }
    } else if after_program[plan::SLOT..] != target[plan::SLOT..] || Some(plan::slot_check(&after_program)?) != tag {
        return Err(ToolError::value("Prepared codec slot did not read back; restart disabled"));
    }
    radio.port.authorize_reset(&after_program, &after_data, expected_data)?;
    for k in ["installation_verified", "bootloader_unchanged", "codec_slot_verified", "data_sector_crcs_unchanged"] {
        report.set(k, true);
    }
    report.set("installed_program_sha256", sha(&after_program));
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)("VERIFIED: application, untouched boot area, codec slot and settings chip all read back. Restarting.");
    radio.retries = 0;
    radio.reset_chip()?;
    report.set("restart_sent", true);
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)("PASS: DDM32 Plus installed and verified. Check the boot screen on the radio. No RF command was sent.");
    Ok(())
}

/// `--port COMx --backup B --out NEWDIR --restore-stock --use-saved-backup [--wait 1..600] [--plan-only]`
pub fn restore_stock_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    let args = Args::new(
        argv,
        &["--port", "--backup", "--out", "--wait"],
        &["--restore-stock", "--use-saved-backup", "--plan-only"],
        &["--port", "--backup", "--out"],
    )?;
    let wait = args.int("--wait", 120)?;
    if !(1..=600).contains(&wait) {
        return Err(ToolError::exit("2"));
    }
    if !args.has("--restore-stock") || !args.has("--use-saved-backup") {
        return Err(ToolError::exit("2"));
    }
    let backup = PathBuf::from(args.get("--backup").unwrap());
    let out_dir = PathBuf::from(args.get("--out").unwrap());
    let inputs = plan::restore_inputs(&backup, false)?;
    let expected_chips = unit_image::backup_jedec(&backup)?;
    let plan = plan::restore_stock_plan(&inputs.original, &inputs.target);
    (ctx.print)(&dumps(&plan));
    if args.has("--plan-only") {
        return Ok(());
    }
    ram_monitor::image()?;
    pyfile::make_new_dir(&out_dir)?;
    let mut report = PyDict::new()
        .with("started_utc", ctx.clock.utc_stamp())
        .with("mode", "restore_stock")
        .with("target_application_sha256", sha(&inputs.target[plan::START..]))
        .with("writes_started", false)
        .with("installation_verified", false)
        .with("bootloader_unchanged", false)
        .with("data_sector_crcs_unchanged", false)
        .with("restart_sent", false)
        .with("preinstall_reads_requested", false)
        .with("write_basis", "saved original stock backup");
    let port_name = args.get("--port").unwrap().to_string();
    let raw = (ctx.serial)(&port_name)?;
    let mut guard = InstallPort::new(raw, &inputs.original, &inputs.target, true, Authorization::Full)?;
    report.set("changed_application_sectors", Py::List(guard.todo.iter().map(|&a| Py::Int(a as i64)).collect()));
    let result = restore_stock_run(&mut guard, &mut report, &out_dir, &port_name, wait, &expected_chips, ctx);
    if let Err(e) = &result {
        report.set("error", e.record());
    }
    report.set("finished_utc", ctx.clock.utc_stamp());
    let _ = guard.ro.raw.close();
    let receipt = write_receipt(&mut report, &guard, &out_dir);
    result.and(receipt)
}

fn restore_stock_run(
    guard: &mut InstallPort,
    report: &mut PyDict,
    out_dir: &Path,
    port_name: &str,
    wait: i64,
    expected_chips: &HashMap<&'static str, String>,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    guard.ro.raw.open()?;
    let mut radio = MonitorRadio::new(guard, ctx.print, ctx.clock, 3);
    (ctx.print)(&format!("READY on {port_name}: turn ON normally, no buttons held. Waiting {wait}s."));
    radio.start(wait as u32)?;
    radio.configure(&dm32_board())?;
    check_monitor(&radio)?;
    radio.check_writable()?;
    check_chips(&mut radio, expected_chips)?;
    (ctx.print)("Using the verified saved backup. No pre-write firmware or settings-chip reads.");
    radio.port.authorize_saved_backup()?;
    report.set("writes_started", true);
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)(&format!(
        "INSTALLING: {} changed application sectors. Boot/data writes blocked.",
        radio.port.todo.len()
    ));
    let started = ctx.clock.monotonic();
    run_writes(&mut radio, report, out_dir, "Firmware written and sector-readback checked", ctx)?;
    report.set("write_seconds", ctx.clock.monotonic() - started);
    let after = read_program(
        &mut radio,
        &out_dir.join("application-after.bin"),
        "Verify newly installed application",
        ctx.print,
        plan::START,
    )?;
    radio.port.authorize_reset_application(&after)?;
    report.set("installed_application_sha256", sha(&after));
    report.set("installation_verified", true);
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)("VERIFIED: installed application readback matches. Restarting.");
    radio.retries = 0;
    radio.reset_chip()?;
    report.set("restart_sent", true);
    write_receipt(report, radio.port, out_dir)?;
    (ctx.print)(
        "PASS: stock application restored and verified byte for byte. Confirm the normal stock screen on the radio.",
    );
    Ok(())
}

/// The planned writes with a progress line and a receipt at each step of 5 % (the tools' Progress callback).
pub(crate) fn run_writes(
    radio: &mut MonitorRadio<'_, InstallPort>,
    report: &mut PyDict,
    out_dir: &Path,
    label: &str,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    // The receipt needs the guard while the radio holds it: the writes run sector by sector here, as write_plan does.
    radio.retries = 0;
    let todo = radio.port.todo.clone();
    let target = radio.port.target().to_vec();
    let mut last: i64 = -1;
    for (i, &a) in todo.iter().enumerate() {
        radio.write_cmd(
            "progmem",
            &[0x20, (a >> 16) as u8, (a >> 8) as u8, a as u8],
            &format!("erase 0x{a:x}"),
            3.0,
        )?;
        for q in (a..a + plan::SECTOR).step_by(256) {
            let page = &target[q..q + 256];
            if page.iter().any(|&b| b != 0xFF) {
                let mut tx = vec![0x02, (q >> 16) as u8, (q >> 8) as u8, q as u8];
                tx.extend_from_slice(page);
                radio.write_cmd("progmem", &tx, &format!("program 0x{q:x}"), 1.0)?;
            }
        }
        let mut got = Vec::with_capacity(plan::SECTOR);
        for q in (a..a + plan::SECTOR).step_by(1024) {
            got.extend(radio.read_flash("progmem", q as u64, 1024)?);
        }
        if got[..] != target[a..a + plan::SECTOR] {
            return Err(ToolError::value(format!("Sector readback mismatch at 0x{a:x}; stopped")));
        }
        if let Some(line) = write_progress_line(label, i + 1, todo.len(), &mut last) {
            (ctx.print)(&line);
            write_receipt(report, radio.port, out_dir)?;
        }
    }
    radio.port.finish_writes()?;
    radio.retries = 3;
    Ok(())
}
