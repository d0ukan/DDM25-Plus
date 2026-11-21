// SPDX-License-Identifier: GPL-3.0-or-later
//! The install and restore logic of pc/radiotools/install_plus.py and install_firstboot.py that reads only files
//! (the codec slot check, the release folder check, the target images and the write plan; Radio/Tools.cs
//! InstallPlan), and the radio steps the tools share (Steps).

use super::codec::{Codec, Profile};
use super::error::{ToolError, ToolResult};
use super::guards::{InstallPort, ReadOnlyPort};
use super::monitor::{crc32, le32, sha, DumpSink, MonitorRadio};
use super::port::RadioPort;
use super::unit_image::{self, stock_version, TESTED_STOCK};
use crate::pyjson::{self, Py, PyDict};
use crate::releases::signed_manifest;
use std::collections::HashMap;
use std::path::Path;

pub const START: usize = 0xC000;
pub const SIZE: usize = 0x100000;
pub const SECTOR: usize = 4096;
pub const SLOT: usize = 0xE8000;
pub const SLOT_MAGIC: u32 = 0x4352_4444;
pub const SLOT_V2: u32 = 2;

pub const CODEC_SYMBOLS: [(&str, u64); 6] = [
    ("__codec_slot", 0x030e8000),
    ("__codec_slot_end", 0x03100000),
    ("__codec_run_lo", 0x43a00),
    ("__codec_run_hi", 0x58000),
    ("__codec_tab_lo", 0x18000e48),
    ("__codec_tab_hi", 0x18003e38),
];

/// The prepared codec slot: header, version and its own CRC. Returns its stock tag.
pub fn slot_check(program: &[u8]) -> ToolResult<u64> {
    let s = &program[SLOT..SIZE];
    let w = |off: usize| le32(&s[off..]) as u64;
    if w(0) != SLOT_MAGIC as u64 || w(4) != SLOT_V2 as u64 {
        return Err(ToolError::value(
            "No prepared codec slot (v2) at 0xE8000: not the Classic layout (stock firmware: first install with --from-stock)",
        ));
    }
    let (code_off, code_len, tab_off, tab_len) = (w(8), w(12), w(20), w(24));
    let (rel_off, rel_count, crc) = (w(0x44), w(0x48), w(0x40));
    for (off, n) in [(code_off, code_len), (tab_off, tab_len), (rel_off, 4 * rel_count)] {
        if off + n > s.len() as u64 {
            return Err(ToolError::value("Codec slot parts run past the slot"));
        }
    }
    let part = |o: u64, n: u64| &s[o as usize..(o + n) as usize];
    let got = crc32(part(rel_off, 4 * rel_count), crc32(part(tab_off, tab_len), crc32(part(code_off, code_len), 0)));
    if got as u64 != crc {
        return Err(ToolError::value("Codec slot CRC mismatch"));
    }
    Ok(w(0x3C))
}

fn json_true(m: &serde_json::Value, k: &str) -> bool {
    match m.get(k) {
        None | Some(serde_json::Value::Null) | Some(serde_json::Value::Bool(false)) => false,
        Some(serde_json::Value::Number(n)) => n.as_f64() != Some(0.0),
        Some(serde_json::Value::String(s)) => !s.is_empty(),
        _ => true,
    }
}

/// The application of a DDM32 Plus build folder after its build manifest and map checks.
pub fn build_application(folder: &Path) -> ToolResult<Vec<u8>> {
    let app = std::fs::read(folder.join("rfoundry_dm32.bin"))?;
    let m: serde_json::Value = serde_json::from_slice(&std::fs::read(folder.join("rfoundry_dm32.json"))?)
        .map_err(|e| ToolError::new("JSONDecodeError", e.to_string()))?;
    if !json_true(&m, "plus_enabled") || !json_true(&m, "forced_rebuild_identical") {
        return Err(ToolError::value("Expected a reproducible DDM32 Plus build manifest"));
    }
    let mut outputs: HashMap<String, Option<String>> = HashMap::new();
    if let Some(o) = m.get("outputs").and_then(|o| o.as_object()) {
        for (k, v) in o {
            let name = k.replace('\\', "/").rsplit('/').next().unwrap_or("").to_string();
            outputs.insert(name, v.as_str().map(str::to_string));
        }
    }
    let out = |n: &str| outputs.get(n).cloned().flatten();
    if out("rfoundry_dm32.bin") != Some(sha(&app)) {
        return Err(ToolError::value("Application differs from its build manifest"));
    }
    let mapping = std::fs::read(folder.join("rfoundry_dm32.map"))?;
    if out("rfoundry_dm32.map") != Some(sha(&mapping)) {
        return Err(ToolError::value("Linker map differs from its build manifest"));
    }
    let text = std::str::from_utf8(&mapping)
        .map_err(|_| ToolError::new("UnicodeDecodeError", "the linker map is not UTF-8"))?;
    let re = regex::Regex::new(r"(0x[0-9a-fA-F]+)\s+(__codec_\w+)\s*=").unwrap();
    let mut symbols: HashMap<String, u64> = HashMap::new();
    for c in re.captures_iter(text) {
        if let Ok(v) = u64::from_str_radix(&c[1][2..], 16) {
            symbols.insert(c[2].to_string(), v);
        }
    }
    if CODEC_SYMBOLS.iter().any(|(k, v)| symbols.get(*k) != Some(v)) {
        return Err(ToolError::value("Linker layout differs from the reserved codec location"));
    }
    if app.is_empty() || app.len() > SLOT - START {
        return Err(ToolError::value("The application must end before the codec slot"));
    }
    Ok(app)
}

/// The release's application after its build checks and the signed release gate.
pub fn plus_application(folder: &Path, key_hex: Option<&str>) -> ToolResult<Vec<u8>> {
    let app = build_application(folder)?;
    signed_manifest::release_gate(&app, folder, key_hex).map_err(|e| ToolError::new("ManifestError", e.0))?;
    Ok(app)
}

/// boot area + application + 0xFF up to the slot + slot of `base`.
pub fn with_application(base: &[u8], app: &[u8]) -> Vec<u8> {
    let mut t = base[..START].to_vec();
    t.extend_from_slice(app);
    t.resize(SLOT, 0xFF);
    t.extend_from_slice(&base[SLOT..]);
    t
}

pub struct Inputs {
    pub original: Vec<u8>,
    pub target: Vec<u8>,
    pub data: Vec<u8>,
}

pub fn install_inputs(backup: &Path, plus: &Path, key_hex: Option<&str>) -> ToolResult<(Inputs, u64)> {
    let mut images = unit_image::saved_images(backup)?;
    let original = images.remove("progmem-pass1.bin").unwrap();
    let tag = slot_check(&original)?;
    let app = plus_application(plus, key_hex)?;
    let target = with_application(&original, &app);
    Ok((Inputs { original, target, data: images.remove("q128-pass1.bin").unwrap() }, tag))
}

pub fn stock_inputs(backup: &Path, plus: &Path, key_hex: Option<&str>, codec: &Codec) -> ToolResult<(Inputs, Profile)> {
    let mut images = unit_image::saved_images(backup)?;
    let original = images.remove("progmem-pass1.bin").unwrap();
    let app = plus_application(plus, key_hex)?;
    let (target, prof) = unit_image::compose_from_stock(&original, &app, codec)?;
    if slot_check(&target)? != prof.tag || target[..START] != original[..START] {
        return Err(ToolError::value("Prepared codec slot did not validate"));
    }
    Ok((Inputs { original, target, data: images.remove("q128-pass1.bin").unwrap() }, prof))
}

/// Every application and slot sector rewritten from the backup (a synthetic inverted basis).
pub fn restore_inputs(backup: &Path, check_slot: bool) -> ToolResult<Inputs> {
    let mut images = unit_image::saved_images(backup)?;
    let target = images.remove("progmem-pass1.bin").unwrap();
    if check_slot {
        slot_check(&target)?;
    }
    let mut basis = target.clone();
    for b in &mut basis[START..] {
        *b ^= 0xFF;
    }
    Ok(Inputs { original: basis, target, data: images.remove("q128-pass1.bin").unwrap() })
}

pub fn todo(original: &[u8], target: &[u8]) -> Vec<usize> {
    (START..SIZE).step_by(SECTOR).filter(|&a| original[a..a + SECTOR] != target[a..a + SECTOR]).collect()
}

/// The write plan printed before anything else (install_plus.make_plan).
pub fn make_plan(
    mode: &str,
    original: &[u8],
    target: &[u8],
    tag: Option<u64>,
    prof: Option<&Profile>,
) -> ToolResult<PyDict> {
    let todo = todo(original, target);
    if matches!(mode, "install_plus" | "install_plus_quick" | "install_plus_no_backup")
        && todo.iter().any(|&a| a >= SLOT)
    {
        return Err(ToolError::exit("Refusing: the plan would touch the codec slot"));
    }
    if todo.iter().any(|&a| a < START) || original[..START] != target[..START] {
        return Err(ToolError::exit("Refusing: the plan would touch the boot area"));
    }
    let mut plan = PyDict::new()
        .with("mode", mode)
        .with("changed_sectors", todo.len())
        .with("first", todo.first().map(|&a| pyjson::hex(a as i64)))
        .with("last", todo.last().map(|&a| pyjson::hex(a as i64)))
        .with("codec_slot_stock_tag", tag.map(|t| t as i64))
        .with("target_application_sha256", sha(&target[START..]))
        .with("boot_area_unchanged", original[..START] == target[..START])
        .with("codec_slot_unchanged", original[SLOT..] == target[SLOT..])
        .with("codec_slot_rewritten_from_backup", mode == "restore")
        .with("codec_slot_prepared_from_backup", mode == "install_plus_from_stock")
        .with("data_chip_written", false);
    if let Some(p) = prof {
        plan.set("stock_profile", p.name.clone());
        plan.set("stock_version", stock_version(p));
        plan.set("stock_version_tested", TESTED_STOCK.contains(&p.name.as_str()));
        plan.set("slot_sectors", todo.iter().filter(|&&a| a >= SLOT).count());
    }
    Ok(plan)
}

/// The plan of install_firstboot --restore-stock (printed by the tool and the restore page).
pub fn restore_stock_plan(original: &[u8], target: &[u8]) -> PyDict {
    let todo = todo(original, target);
    PyDict::new()
        .with("mode", "restore_stock")
        .with("changed_sectors", todo.len())
        .with("first", todo.first().map(|&a| pyjson::hex(a as i64)))
        .with("last", todo.last().map(|&a| pyjson::hex(a as i64)))
        .with("target_application_sha256", sha(&target[START..]))
        .with("boot_area_unchanged", original[..START] == target[..START])
        .with("codec_slot_unchanged", false)
        .with("codec_slot_rewritten_from_backup", true)
        .with("data_chip_written", false)
}

pub fn crc_list(data: &[u8]) -> Vec<u32> {
    data.chunks(SECTOR).map(|c| crc32(c, 0)).collect()
}

// ---- the radio steps shared by the tools (install_firstboot's helpers) ----------------------------------------------

pub fn device_crcs<P: RadioPort + ?Sized>(
    radio: &mut MonitorRadio<'_, P>,
    memory: &str,
    size: usize,
    label: &str,
    print: &dyn Fn(&str),
) -> ToolResult<Vec<u32>> {
    let mut out = Vec::new();
    let mut last: i64 = -1;
    let mut a = 0usize;
    while a < size {
        let n = 16.min((size - a) / SECTOR);
        out.extend(radio.sums(memory, a as u64, n)?.into_iter().map(|s| s.0));
        let percent = ((a + n * SECTOR) as u64 * 100 / size as u64) as i64;
        if percent / 10 != last {
            print(&format!("{label}: {percent}%"));
            last = percent / 10;
        }
        a += 16 * SECTOR;
    }
    Ok(out)
}

pub fn read_program<P: RadioPort + ?Sized>(
    radio: &mut MonitorRadio<'_, P>,
    path: &Path,
    label: &str,
    print: &dyn Fn(&str),
    start: usize,
) -> ToolResult<Vec<u8>> {
    if path.exists() {
        return Err(ToolError::file_exists(path.to_string_lossy()));
    }
    let partial = crate::releases::net::partial_path(path);
    let mut last: i64 = -1;
    {
        let mut out = std::fs::OpenOptions::new().read(true).write(true).create_new(true).open(&partial)?;
        let mut progress = |done: u64, total: u64| {
            let p = (done * 100 / total) as i64;
            if p / 10 != last {
                print(&format!("{label}: {p}%"));
                last = p / 10;
            }
        };
        radio.dump(&mut out as &mut dyn DumpSink, "progmem", start as u64, SIZE as u64, Some(&mut progress), true)?;
        out.sync_all()?;
    }
    let data = std::fs::read(&partial)?;
    if data.len() != SIZE - start {
        return Err(ToolError::value("Incomplete program readback"));
    }
    if path.exists() {
        return Err(ToolError::file_exists(path.to_string_lossy()));
    }
    std::fs::rename(&partial, path)?;
    Ok(data)
}

/// The planned writes, one sector at a time, each read back; never retried (radio.retries = 0).
pub fn write_plan(radio: &mut MonitorRadio<'_, InstallPort>, progress: &mut dyn FnMut(usize, usize)) -> ToolResult<()> {
    radio.retries = 0;
    let todo = radio.port.todo.clone();
    let target = radio.port.target().to_vec();
    for (i, &a) in todo.iter().enumerate() {
        radio.write_cmd(
            "progmem",
            &[0x20, (a >> 16) as u8, (a >> 8) as u8, a as u8],
            &format!("erase 0x{a:x}"),
            3.0,
        )?;
        for q in (a..a + SECTOR).step_by(256) {
            let page = &target[q..q + 256];
            if page.iter().any(|&b| b != 0xFF) {
                let mut tx = vec![0x02, (q >> 16) as u8, (q >> 8) as u8, q as u8];
                tx.extend_from_slice(page);
                radio.write_cmd("progmem", &tx, &format!("program 0x{q:x}"), 1.0)?;
            }
        }
        let mut got = Vec::with_capacity(SECTOR);
        for q in (a..a + SECTOR).step_by(1024) {
            got.extend(radio.read_flash("progmem", q as u64, 1024)?);
        }
        if got[..] != target[a..a + SECTOR] {
            return Err(ToolError::value(format!("Sector readback mismatch at 0x{a:x}; stopped")));
        }
        progress(i + 1, todo.len());
    }
    radio.port.finish_writes()?;
    radio.retries = 3;
    Ok(())
}

pub fn capacity(jedec: &[u8], memory: &str) -> ToolResult<u64> {
    if jedec.len() != 3 || jedec[0] == 0 || jedec[0] == 255 || jedec[1] == 0 || jedec[1] == 255 {
        return Err(ToolError::value("Missing/malformed flash JEDEC ID"));
    }
    let allowed: &[u8] = match memory {
        "progmem" => &[20, 21],
        "q128" => &[24],
        _ => &[],
    };
    if !allowed.contains(&jedec[2]) {
        return Err(ToolError::value(format!(
            "Unsupported {memory} JEDEC density {}; do not assume a capacity",
            hex::encode(jedec)
        )));
    }
    Ok(1u64 << jedec[2])
}

/// The attached chips must be the backup's; sets the guard's read limits.
pub fn check_chips(
    radio: &mut MonitorRadio<'_, InstallPort>,
    expected: &HashMap<&'static str, String>,
) -> ToolResult<()> {
    for memory in ["progmem", "q128"] {
        let got = radio.jedec(memory)?;
        if hex::encode(&got) != expected[memory] {
            return Err(ToolError::value("Attached flash IDs differ from the saved unit"));
        }
        let cap = capacity(&got, memory)?;
        radio.port.ro.limits.insert(memory, cap);
    }
    Ok(())
}

pub fn check_monitor<P: RadioPort + ?Sized>(radio: &MonitorRadio<'_, P>) -> ToolResult<()> {
    let ok = radio.v6.as_ref().is_some_and(|v| v.board == 1 && v.flags == 0) && radio.configured.is_some();
    if !ok {
        return Err(ToolError::value("Monitor configuration/identity did not validate"));
    }
    Ok(())
}

/// The readonly guard's limits from the chip IDs (the backup tool's own loop uses it).
pub fn set_limit(guard: &mut ReadOnlyPort, memory: &'static str, cap: u64) {
    guard.limits.insert(memory, cap);
}

/// A PyDict of the plan for printing (Python's json.dumps(plan, indent=2)).
pub fn plan_text(plan: &PyDict) -> String {
    pyjson::dumps2(&Py::Dict(plan.clone()))
}
