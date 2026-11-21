// SPDX-License-Identifier: GPL-3.0-or-later
//! Offline helpers on the user's own verified backup (pc/radiotools/prepare_unit_image.py, Radio/UnitImage.cs) and
//! what a DM-32UV runs, from its backup only (radio_detect.py). Never opens a port and never writes a file. The
//! composed first-install image holds the radio's own codec: it stays in memory.

use super::codec::{Codec, Profile, PROGMEM_BASE};
use super::error::{ToolError, ToolResult};
use super::monitor::sha;
use crate::pyjson::{self, Py, PyDict};
use serde_json::Value;
use std::collections::HashMap;
use std::path::Path;

pub const START: usize = 0xC000;
pub const SIZE: usize = 0x100000;
pub const SECTOR: usize = 4096;
pub const SLOT: usize = 0xE8000;
pub const TESTED_STOCK: [&str; 1] = ["DM32.01.01.047.bin"];

fn get<'a>(o: &'a Value, key: &str) -> ToolResult<&'a Value> {
    o.as_object().and_then(|m| m.get(key)).ok_or_else(|| ToolError::key(key))
}

fn read_manifest(folder: &Path) -> ToolResult<Value> {
    let bytes = std::fs::read(folder.join("backup-manifest.json"))?;
    serde_json::from_slice(&bytes).map_err(|e| ToolError::new("JSONDecodeError", e.to_string()))
}

fn truthy(v: &Value) -> bool {
    match v {
        Value::Bool(b) => *b,
        Value::String(s) => !s.is_empty(),
        Value::Number(n) => n.as_f64().is_some_and(|f| f != 0.0),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
        Value::Null => false,
    }
}

/// Both passes of both chips from a verified two-pass backup, checked against its manifest.
pub fn saved_images(folder: &Path) -> ToolResult<HashMap<String, Vec<u8>>> {
    let m = read_manifest(folder)?;
    let names: Vec<String> =
        ["progmem", "q128"].iter().flat_map(|mem| (1..=2).map(move |n| format!("{mem}-pass{n}.bin"))).collect();
    let verified = m.get("backup_verified").is_some_and(truthy);
    let captures = m.get("captures").and_then(Value::as_array);
    let bad = || ToolError::value("A complete verified two-pass backup is required");
    let Some(captures) = captures.filter(|_| verified) else { return Err(bad()) };
    let mut files = std::collections::HashSet::new();
    for r in captures {
        files.insert(get(r, "file")?.as_str().unwrap_or("").to_string());
    }
    if files != names.iter().cloned().collect() {
        return Err(bad());
    }
    let mut images = HashMap::new();
    for record in captures {
        let name = get(record, "file")?.as_str().unwrap_or("").to_string();
        let data = std::fs::read(folder.join(&name))?;
        if Some(data.len() as u64) != get(record, "bytes")?.as_u64()
            || Some(sha(&data).as_str()) != get(record, "sha256")?.as_str()
        {
            return Err(ToolError::value("Saved image length/hash differs from its manifest"));
        }
        let sectors: Vec<Option<&str>> =
            get(record, "sector_sha256")?.as_array().map(|a| a.iter().map(Value::as_str).collect()).unwrap_or_default();
        let mine: Vec<String> = data.chunks(SECTOR).map(sha).collect();
        if sectors.len() != mine.len() || sectors.iter().zip(&mine).any(|(a, b)| *a != Some(b.as_str())) {
            return Err(ToolError::value("Saved sector hashes differ"));
        }
        images.insert(name, data);
    }
    for mem in ["progmem", "q128"] {
        if images[&format!("{mem}-pass1.bin")] != images[&format!("{mem}-pass2.bin")] {
            return Err(ToolError::value("Saved passes differ"));
        }
    }
    if images["progmem-pass1.bin"].len() != 0x100000 || images["q128-pass1.bin"].len() != 0x1000000 {
        return Err(ToolError::value("Candidate board preset requires 1 MiB program and 16 MiB data flash"));
    }
    Ok(images)
}

/// The JEDEC IDs the backup recorded ({"progmem": "c84014", "q128": "ef4018"}).
pub fn backup_jedec(folder: &Path) -> ToolResult<HashMap<&'static str, String>> {
    let m = read_manifest(folder)?;
    let chips = m.get("chips").filter(|c| c.is_object());
    let mut out = HashMap::new();
    for memory in ["progmem", "q128"] {
        let jedec = chips
            .and_then(|c| c.get(memory))
            .filter(|c| c.is_object())
            .and_then(|c| c.get("jedec"))
            .map(|j| match j {
                Value::String(s) => s.to_lowercase(),
                other => other.to_string().to_lowercase(),
            })
            .unwrap_or_default();
        if jedec.len() != 6 || !jedec.chars().all(|c| c.is_ascii_hexdigit()) {
            return Err(ToolError::value(
                "The backup does not record its flash chip IDs; take a new backup with this program",
            ));
        }
        out.insert(memory, jedec);
    }
    Ok(out)
}

/// The codec tool after checking that its DM32 windows are the DDM32 Plus layout.
pub fn check_codec(codec: &Codec) -> ToolResult<&Codec> {
    let b = &codec.board;
    if b.slot_lo != 0x030E_8000
        || b.slot_hi != 0x0310_0000
        || b.run_lo != 0x43A00
        || b.run_hi != 0x58000
        || b.app_base != 0x0300_C000
        || b.tables_lo != 0x1800_0E48
        || b.tables_hi > 0x1800_3E38
    {
        return Err(ToolError::codec("The codec tool's DM32 windows differ from the DDM32 Plus layout"));
    }
    Ok(codec)
}

pub fn has_slot(codec: &Codec, program: &[u8]) -> bool {
    codec.parse_slot(&program[SLOT..SIZE]).is_some()
}

/// The stock profile of a program image (no slot, the codec where that stock build keeps it), or None.
pub fn stock_profile(codec: &Codec, program: &[u8]) -> Option<Profile> {
    if program.len() != SIZE || has_slot(codec, program) {
        return None;
    }
    let prof = codec.identify(program, Some("dm32"))?;
    if prof.board != "dm32" {
        return None;
    }
    let code_at = prof.code.addr as i64 - PROGMEM_BASE as i64;
    let tables_at = prof.tables.addr as i64 - PROGMEM_BASE as i64;
    let (s, e) = (START as i64, SLOT as i64);
    if !(s <= code_at && code_at < e && s <= tables_at && tables_at < e) {
        return None;
    }
    Some(prof)
}

/// '047' from 'DM32.01.01.047.bin'; the profile name otherwise.
pub fn stock_version(prof: &Profile) -> String {
    let base = prof.name.replace(".bin", "");
    let parts: Vec<&str> = base.split('.').collect();
    match parts.last() {
        Some(last) if parts.len() >= 4 && !last.is_empty() && last.chars().all(|c| c.is_ascii_digit()) => {
            last.to_string()
        }
        _ => prof.name.clone(),
    }
}

/// (target image, stock profile) for a first install from stock: boot area unchanged, the application from 0xC000,
/// the radio's own codec in the slot. CodecError for anything that is not a known stock layout.
pub fn compose_from_stock(program: &[u8], application: &[u8], codec: &Codec) -> ToolResult<(Vec<u8>, Profile)> {
    check_codec(codec)?;
    if program.len() != SIZE {
        return Err(ToolError::codec("Expected a 1 MiB program flash image"));
    }
    if has_slot(codec, program) {
        return Err(ToolError::codec(
            "A prepared codec slot is already present: this radio has the Classic layout, install over it instead",
        ));
    }
    stock_profile(codec, program).ok_or_else(|| {
        ToolError::codec("Unknown stock firmware: its codec was not found where a supported stock version keeps it")
    })?;
    if application.is_empty() || application.len() > SLOT - START {
        return Err(ToolError::codec("The application must end before the codec slot"));
    }
    let wrap = |e: ToolError| ToolError::codec(format!("Codec preparation failed: {}", e.message));
    let (prof, code, tables) = codec.extract(program, Some("dm32")).map_err(wrap)?;
    let raw = codec.merge(application, &prof, &code, &tables).map_err(wrap)?;
    if raw.len() > SIZE - START || raw[..application.len()] != *application {
        return Err(ToolError::codec("Merged image does not fit the application area"));
    }
    let slot = codec.parse_slot(&raw[SLOT - START..]);
    let ok = slot.as_ref().is_some_and(|s| {
        s.get("tag").and_then(Py::as_i64) == Some(prof.tag as i64)
            && s.str("code_sha256") == Some(sha(&code).as_str())
            && s.str("tables_sha256") == Some(sha(&tables).as_str())
    });
    if !ok {
        return Err(ToolError::codec("Codec slot did not validate"));
    }
    let mut target = program[..START].to_vec();
    target.extend_from_slice(&raw);
    target.resize(SIZE, 0xFF);
    if target.len() != SIZE || target[..START] != program[..START] {
        return Err(ToolError::codec("Composed image does not keep the boot area"));
    }
    Ok((target, prof))
}

const DDM32_MARKERS: [&[u8]; 4] = [b"DDM32 Plus", b"DDM32", b"DDM25 Plus", b"DDM25"];
const CLASSIC_MARKERS: [&[u8]; 3] = [b"open25-classic", b"Open25", b"open25-"];

pub fn application_end(program: &[u8]) -> usize {
    let app = &program[START..SLOT];
    let n = app.iter().rposition(|&b| b != 0xFF).map_or(0, |i| i + 1);
    START + n
}

fn any_marker(app: &[u8], markers: &[&[u8]]) -> bool {
    markers.iter().any(|m| app.windows(m.len()).any(|w| w == *m))
}

/// radio_detect: what a 1 MiB program image holds.
pub fn detect(program: &[u8], codec: &Codec) -> ToolResult<PyDict> {
    check_codec(codec)?;
    if program.len() != SIZE {
        return Err(ToolError::value("Expected a 1 MiB program flash image"));
    }
    let app = &program[START..SLOT];
    let mut out = PyDict::new()
        .with("layout", "unknown")
        .with("firmware", "unknown")
        .with("codec_tag", Py::Null)
        .with("stock_profile", Py::Null)
        .with("stock_version", Py::Null)
        .with("stock_tested", false)
        .with("install_mode", Py::Null)
        .with("supported", false)
        .with("reason", "")
        .with("application_end", pyjson::hex(application_end(program) as i64));
    if let Some(slot) = codec.parse_slot(&program[SLOT..SIZE]) {
        out.set("layout", "classic");
        out.set("codec_tag", slot.get("tag").cloned().unwrap_or(Py::Null));
        out.set("install_mode", "over_classic");
        out.set("supported", true);
        let fw = if any_marker(app, &DDM32_MARKERS) {
            "ddm32"
        } else if any_marker(app, &CLASSIC_MARKERS) {
            "classic"
        } else {
            "unknown"
        };
        out.set("firmware", fw);
        return Ok(out);
    }
    if let Some(prof) = stock_profile(codec, program) {
        out.set("layout", "stock");
        out.set("firmware", "stock");
        out.set("codec_tag", prof.tag as i64);
        out.set("stock_profile", prof.name.clone());
        out.set("stock_version", stock_version(&prof));
        out.set("stock_tested", TESTED_STOCK.contains(&prof.name.as_str()));
        out.set("install_mode", "from_stock");
        out.set("supported", true);
        return Ok(out);
    }
    let all: Vec<&[u8]> = DDM32_MARKERS.iter().chain(CLASSIC_MARKERS.iter()).copied().collect();
    if any_marker(app, &all) {
        out.set("firmware", "classic");
        out.set("reason", "Classic-type firmware without a valid codec slot (damaged slot?)");
    } else {
        out.set(
            "reason",
            "Unknown firmware: no codec slot and no known stock codec layout (for example stock .050 or newer). Install Classic with the upstream Open25-DM32 Flasher first, then DDM32 Plus over it.",
        );
    }
    Ok(out)
}

/// radio_detect.py --backup FOLDER: the printed JSON.
pub fn detect_tool_output(backup: &Path, codec: &Codec) -> ToolResult<String> {
    let images = saved_images(backup)?;
    Ok(pyjson::dumps2(&Py::Dict(detect(&images["progmem-pass1.bin"], codec)?)) + "\n")
}
