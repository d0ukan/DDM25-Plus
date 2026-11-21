// SPDX-License-Identifier: GPL-3.0-or-later
//! The DM-32UV part of RadioFoundry's codec slot writer (tools/hrc7000_codec.py and rfoundry/codecmap.py, MIT;
//! Radio/Codec.cs): find the radio's own DMR codec in a stock program flash image by its content, and build the
//! version 2 codec slot the Classic / DDM32 Plus firmware runs it from. The stock profiles and codec maps (hashes,
//! addresses, the code words that hold addresses; no codec byte) are embedded from the pinned files
//! (data/codec-dm32.json).

use super::error::{ToolError, ToolResult};
use super::monitor::{crc32, le32, sha};
use crate::pyjson::{Py, PyDict};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::OnceLock;

pub const PROGMEM_BASE: u64 = 0x0300_0000;
pub const PROGMEM_SIZE: usize = 0x100000;
pub const STOCK_HEADER: u64 = 0x100;
pub const SLOT_MAGIC: u32 = 0x4352_4444;
pub const SLOT_VERSION: u32 = 2;
pub const REL_MAX: u64 = 64;
pub const HEADER_LEN: usize = 128;
pub const TABLES_HEAD_CRC: u32 = 0x1147_5389;
pub const CODEC_SIG: [u32; 2] = [0x1525_14D8, 0x74C7_76CD];
pub const ENTRY_NAMES: [&str; 7] = ["decode", "encode", "dec_init", "dec_set", "enc_init", "enc_set", "fec_init"];
pub const MAGIC: &[u8] = b"BFUV32-V2";

fn extract_error(m: impl Into<String>) -> ToolError {
    ToolError::new("ExtractError", m)
}

fn map_error(m: impl Into<String>) -> ToolError {
    ToolError::new("MapError", m)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Window {
    pub addr: u64,
    pub len: u64,
    pub run: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Profile {
    pub name: String,
    pub board: String,
    pub tag: u64,
    pub size: u64,
    pub code: Window,
    pub tables: Window,
    pub code_sha256: Option<String>,
    pub tables_sha256: String,
    pub head_crc: Option<u32>,
    pub entries: HashMap<String, u64>,
    pub kind: Option<String>,
    pub hdr: Option<u64>,
    pub no_magic: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodecMap {
    pub id: String,
    pub code_len: u64,
    pub code_run: u64,
    pub code_sha256: Option<String>,
    pub move_alignment: u64,
    pub self_refs: Vec<(u64, u64)>,
    pub island_ptrs: Vec<(u64, u64)>,
    pub has_islands: bool,
    /// A self reference that is not a plain word: the code can't be moved (codecmap.MapError when used).
    pub unmovable: bool,
}

/// The board's windows: slot (flash), codec run window and modem table window (RAM), image base.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Layout {
    pub slot_lo: u64,
    pub slot_hi: u64,
    pub run_lo: u64,
    pub run_hi: u64,
    pub tables_lo: u64,
    pub tables_hi: u64,
    pub app_base: u64,
}

#[derive(Debug, Clone)]
pub struct Codec {
    pub profiles: Vec<(String, Profile)>,
    pub maps: Vec<CodecMap>,
    pub board: Layout,
}

const EMBEDDED: &str = include_str!("../../data/codec-dm32.json");

fn u(v: &Value) -> u64 {
    v.as_u64().expect("codec table number")
}

impl Codec {
    /// The pinned DM-32UV profiles and maps.
    pub fn dm32() -> &'static Codec {
        static C: OnceLock<Codec> = OnceLock::new();
        C.get_or_init(|| Codec::from_json(EMBEDDED))
    }

    fn from_json(text: &str) -> Codec {
        let root: Value = serde_json::from_str(text).expect("embedded codec tables");
        let mut profiles = Vec::new();
        for (key, v) in root["profiles"].as_object().unwrap() {
            let w = |k: &str| Window { addr: u(&v[k][0]), len: u(&v[k][1]), run: u(&v[k][2]) };
            profiles.push((
                key.clone(),
                Profile {
                    name: v["name"].as_str().unwrap().into(),
                    board: v["board"].as_str().unwrap().into(),
                    tag: u(&v["tag"]),
                    size: u(&v["size"]),
                    code: w("code"),
                    tables: w("tables"),
                    code_sha256: v.get("code_sha256").and_then(Value::as_str).map(str::to_string),
                    tables_sha256: v["tables_sha256"].as_str().unwrap().into(),
                    head_crc: v.get("tables_head_crc").map(|x| u(x) as u32),
                    entries: v["entries"].as_object().unwrap().iter().map(|(k, x)| (k.clone(), u(x))).collect(),
                    kind: v.get("kind").and_then(Value::as_str).map(str::to_string),
                    hdr: v.get("hdr").map(u),
                    no_magic: v.get("magic").is_some_and(Value::is_null),
                },
            ));
        }
        let mut maps = Vec::new();
        for (id, m) in root["maps"].as_object().unwrap() {
            let c = &m["code"];
            let refs = m["self_refs"].as_array().unwrap();
            maps.push(CodecMap {
                id: id.clone(),
                code_len: u(&c["len"]),
                code_run: u(&c["run"]),
                code_sha256: c.get("sha256").and_then(Value::as_str).map(str::to_string),
                move_alignment: c.get("move_alignment").map_or(4, u),
                self_refs: refs
                    .iter()
                    .filter(|r| r["kind"] == "word")
                    .map(|r| (u(&r["off"]), u(&r["value"])))
                    .collect(),
                unmovable: refs.iter().any(|r| r["kind"] != "word"),
                island_ptrs: m["island_ptrs"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|r| (u(&r["off"]), u(&r["target"])))
                    .collect(),
                has_islands: !m["islands"].as_array().unwrap().is_empty(),
            });
        }
        let b = &root["board"];
        let board = Layout {
            slot_lo: u(&b["slot"][0]),
            slot_hi: u(&b["slot"][1]),
            run_lo: u(&b["run"][0]),
            run_hi: u(&b["run"][1]),
            tables_lo: u(&b["tables"][0]),
            tables_hi: u(&b["tables"][1]),
            app_base: u(&b["app_base"]),
        };
        Codec { profiles, maps, board }
    }

    /// The checked header of a codec slot (magic, version, windows, CRC), else None.
    pub fn parse_slot(&self, slot: &[u8]) -> Option<PyDict> {
        if slot.len() < HEADER_LEN {
            return None;
        }
        let f: Vec<u64> = (0..19).map(|i| le32(&slot[i * 4..]) as u64).collect();
        let (magic, version, code_off, code_len, run, tab_off, tab_len, tab_ram) =
            (f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        if magic != SLOT_MAGIC as u64 || version != SLOT_VERSION as u64 {
            return None;
        }
        let n = slot.len() as u64;
        if code_off < HEADER_LEN as u64 || code_off + code_len > n || tab_off + tab_len > n || code_len == 0 {
            return None;
        }
        let code = &slot[code_off as usize..(code_off + code_len) as usize];
        let tables = &slot[tab_off as usize..(tab_off + tab_len) as usize];
        let mut crc = crc32(tables, crc32(code, 0));
        let (rel_off, rel_count) = (f[17], f[18]);
        if rel_count > REL_MAX || rel_off % 4 != 0 || rel_off + 4 * rel_count > n {
            return None;
        }
        let rels: Vec<i64> = (0..rel_count).map(|i| le32(&slot[(rel_off + 4 * i) as usize..]) as i64).collect();
        crc = crc32(&slot[rel_off as usize..(rel_off + 4 * rel_count) as usize], crc);
        let end = (tab_off + tab_len).max(rel_off + 4 * rel_count);
        if code.len() < 8 || crc as u64 != f[16] || le32(code) != CODEC_SIG[0] || le32(&code[4..]) != CODEC_SIG[1] {
            return None;
        }
        Some(
            PyDict::new()
                .with("version", version as i64)
                .with("run", run as i64)
                .with("tables_ram", tab_ram as i64)
                .with("tag", f[15] as i64)
                .with("crc", f[16] as i64)
                .with("length", end as i64)
                .with("relocations", Py::List(rels.into_iter().map(Py::Int).collect()))
                .with("code_sha256", sha(code))
                .with("tables_sha256", sha(tables)),
        )
    }

    /// A known codec anywhere in a program flash dump, found by its content (hrc7000_codec.locate).
    pub fn locate(&self, dump: &[u8], profiles: &[&Profile], board: Option<&str>) -> Option<Profile> {
        let list: Vec<&Profile> = profiles
            .iter()
            .copied()
            .filter(|p| board.is_none_or(|b| b == p.board) && p.code_sha256.is_some())
            .collect();
        if dump.len() != PROGMEM_SIZE || list.is_empty() {
            return None;
        }
        let mut sig = CODEC_SIG[0].to_le_bytes().to_vec();
        sig.extend_from_slice(&CODEC_SIG[1].to_le_bytes());
        let mut heads: Option<HashMap<u32, Vec<usize>>> = None;
        let mut at = find(dump, &sig, 0);
        while let Some(pos) = at {
            // stock versions with the same codec bytes are told apart by where they put it: those first (stable)
            let mut ordered = list.clone();
            ordered.sort_by_key(|p| p.code.addr != PROGMEM_BASE + pos as u64);
            for p in ordered {
                let clen = p.code.len as usize;
                let tlen = p.tables.len as usize;
                if pos % 4 != 0 || Some(sha(py_slice(dump, pos, clen))) != p.code_sha256 {
                    continue;
                }
                let want = p.head_crc.unwrap_or(TABLES_HEAD_CRC);
                let h = heads.get_or_insert_with(|| {
                    let mut m: HashMap<u32, Vec<usize>> = HashMap::new();
                    let mut i = 0;
                    while i < dump.len() - 64 {
                        m.entry(crc32(&dump[i..i + 64], 0)).or_default().push(i);
                        i += 4;
                    }
                    m
                });
                if let Some(cands) = h.get(&want) {
                    for &t in cands {
                        if sha(py_slice(dump, t, tlen)) == p.tables_sha256 {
                            let mut q = p.clone();
                            q.kind = Some("progmem".into());
                            q.size = PROGMEM_SIZE as u64;
                            q.code = Window { addr: PROGMEM_BASE + pos as u64, len: clen as u64, run: p.code.run };
                            q.tables = Window { addr: PROGMEM_BASE + t as u64, len: tlen as u64, run: p.tables.run };
                            return Some(q);
                        }
                    }
                }
            }
            at = find(dump, &sig, pos + 1);
        }
        None
    }

    fn mine(&self, board: Option<&str>) -> Vec<&(String, Profile)> {
        self.profiles.iter().filter(|(_, p)| board.is_none_or(|b| b == p.board)).collect()
    }

    /// The profile a stock file matches (whole-file SHA-256, or a program flash dump by its windows).
    pub fn identify(&self, stock: &[u8], board: Option<&str>) -> Option<Profile> {
        let mine = self.mine(board);
        let digest = sha(stock);
        if let Some((_, p)) = mine.iter().find(|(k, _)| *k == digest) {
            if p.kind.as_deref() != Some("progmem") {
                return Some(p.clone());
            }
        }
        let ps: Vec<&Profile> = mine.iter().map(|(_, p)| p).collect();
        self.locate(stock, &ps, board)
    }

    fn carve(stock: &[u8], prof: &Profile) -> ToolResult<[Vec<u8>; 2]> {
        let mut out: Vec<Vec<u8>> = Vec::new();
        for (key, w) in [("code", &prof.code), ("tables", &prof.tables)] {
            let progmem = prof.kind.as_deref() == Some("progmem");
            let hdr = if progmem { 0 } else { prof.hdr.unwrap_or(STOCK_HEADER) as i64 };
            let off = if progmem { w.addr as i64 - PROGMEM_BASE as i64 } else { w.addr as i64 - 0x0300_C000 + hdr };
            if off < hdr || off + w.len as i64 > stock.len() as i64 {
                return Err(extract_error(format!("{key} window outside the file")));
            }
            out.push(stock[off as usize..(off as u64 + w.len) as usize].to_vec());
        }
        let tables = out.pop().unwrap();
        let code = out.pop().unwrap();
        Ok([code, tables])
    }

    /// (profile, code, tables) of a stock image, checked.
    pub fn extract(&self, stock: &[u8], board: Option<&str>) -> ToolResult<(Profile, Vec<u8>, Vec<u8>)> {
        let mine = self.mine(board);
        let digest = sha(stock);
        let prof = self.identify(stock, board).ok_or_else(|| {
            extract_error(format!(
                "unsupported stock file (sha256 {digest}); supported: {}",
                mine.iter().map(|(k, p)| format!("{} ({}...)", p.name, &k[..8])).collect::<Vec<_>>().join(", ")
            ))
        })?;
        if prof.kind.as_deref() != Some("progmem") && !prof.no_magic && !stock.starts_with(MAGIC) {
            return Err(extract_error("not a BFUV32-V2 firmware file"));
        }
        let [code, tables] = Self::carve(stock, &prof)?;
        if le32(&code) != CODEC_SIG[0] || le32(&code[4..]) != CODEC_SIG[1] {
            return Err(extract_error("codec signature words not found at the code window"));
        }
        Ok((prof, code, tables))
    }

    /// The codec slot: header, code, tables (4-byte aligned), the relocation list from the codec map.
    pub fn slot_bytes(&self, prof: &Profile, code: &[u8], tables: &[u8]) -> ToolResult<Vec<u8>> {
        let lay = &self.board;
        let code_off = HEADER_LEN;
        let tab_off = code_off + ((code.len() + 3) & !3);
        let run = prof.code.run;
        let tab_ram = prof.tables.run;
        if !(lay.tables_lo <= tab_ram && tab_ram + tables.len() as u64 <= lay.tables_hi) {
            return Err(extract_error("table address outside the reserved modem SRAM window"));
        }
        let digest = sha(code);
        let m = self.maps.iter().find(|x| x.code_sha256.as_deref() == Some(digest.as_str())).ok_or_else(|| {
            extract_error(format!(
                "no codec map for this build (code sha256 {}...): its code can't be moved to the board's codec window",
                &digest[..8]
            ))
        })?;
        if m.unmovable {
            return Err(map_error(format!("{}: the code can't be moved by adding to words", m.id)));
        }
        if m.code_run != run || m.code_len != code.len() as u64 {
            return Err(extract_error(format!("the codec map of {} doesn't match its profile", prof.name)));
        }
        if code.len() as u64 > lay.run_hi - lay.run_lo {
            return Err(extract_error(format!(
                "the codec ({} bytes) is bigger than the board's codec window",
                code.len()
            )));
        }
        if (lay.run_lo as i64 - run as i64) % 4 != 0 {
            return Err(extract_error("the codec window would change the code's word alignment"));
        }
        move_code_check(code, m, lay.run_lo)?;
        if m.has_islands {
            return Err(map_error(format!("{}: the code can't be moved by adding to words", m.id)));
        }
        let mut rels: Vec<u64> = m.self_refs.iter().map(|r| r.0).collect();
        rels.sort();
        let rel_off = tab_off + ((tables.len() + 3) & !3);
        let rel_bytes: Vec<u8> = rels.iter().flat_map(|&r| (r as u32).to_le_bytes()).collect();
        let crc = crc32(&rel_bytes, crc32(tables, crc32(code, 0)));
        let mut words: Vec<u32> = vec![
            SLOT_MAGIC,
            SLOT_VERSION,
            code_off as u32,
            code.len() as u32,
            run as u32,
            tab_off as u32,
            tables.len() as u32,
            tab_ram as u32,
        ];
        for k in ENTRY_NAMES {
            words.push(*prof.entries.get(k).ok_or_else(|| ToolError::key(k))? as u32);
        }
        words.extend_from_slice(&[prof.tag as u32, crc, rel_off as u32, rels.len() as u32]);
        let mut body: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        body.resize(HEADER_LEN, 0);
        body.extend_from_slice(code);
        body.resize(tab_off, 0xFF);
        body.extend_from_slice(tables);
        body.resize(rel_off, 0xFF);
        body.extend_from_slice(&rel_bytes);
        if body.len() as u64 > lay.slot_hi - lay.slot_lo {
            return Err(extract_error("codec does not fit the slot"));
        }
        Ok(body)
    }

    /// image, 0xFF up to the slot, the slot.
    pub fn merge(&self, image: &[u8], prof: &Profile, code: &[u8], tables: &[u8]) -> ToolResult<Vec<u8>> {
        let slot_off = (self.board.slot_lo - self.board.app_base) as usize;
        if image.len() > slot_off {
            return Err(extract_error(format!(
                "the application image ({} bytes) runs into the codec slot at +0x{slot_off:X}",
                image.len()
            )));
        }
        let mut out = image.to_vec();
        out.resize(slot_off, 0xFF);
        out.extend_from_slice(&self.slot_bytes(prof, code, tables)?);
        Ok(out)
    }

    /// A codec for tests: one synthetic stock profile and its map (pc/tests/rtfixtures.synthetic_stock_codec).
    pub fn synthetic(
        code: &[u8],
        tables: &[u8],
        refs: &[(u64, u64)],
        code_at: u64,
        tables_at: u64,
        name: &str,
        tag: u64,
    ) -> Codec {
        let entries: HashMap<String, u64> = [
            ("decode", 0x0),
            ("encode", 0x260),
            ("dec_init", 0xB4),
            ("dec_set", 0x84),
            ("enc_init", 0x314),
            ("enc_set", 0x2E4),
            ("fec_init", 0x1BF8),
        ]
        .into_iter()
        .map(|(k, v)| (k.to_string(), v))
        .collect();
        let prof = Profile {
            name: name.into(),
            board: "dm32".into(),
            tag,
            size: 869748,
            code: Window { addr: code_at, len: code.len() as u64, run: 0x16CDC },
            tables: Window { addr: tables_at, len: tables.len() as u64, run: 0x1800_0EA8 },
            code_sha256: Some(sha(code)),
            tables_sha256: sha(tables),
            head_crc: Some(crc32(&tables[..64], 0)),
            entries,
            kind: None,
            hdr: None,
            no_magic: false,
        };
        let map = CodecMap {
            id: format!("test-dm32-{tag}"),
            code_len: code.len() as u64,
            code_run: 0x16CDC,
            code_sha256: Some(sha(code)),
            move_alignment: 4,
            self_refs: refs.to_vec(),
            island_ptrs: vec![],
            has_islands: false,
            unmovable: false,
        };
        Codec { profiles: vec![("f".repeat(64), prof)], maps: vec![map], board: Codec::dm32().board.clone() }
    }
}

/// codecmap.move_code's checks: the map's own code, every listed site holding the listed value.
fn move_code_check(code: &[u8], m: &CodecMap, new_run: u64) -> ToolResult<()> {
    if code.len() as u64 != m.code_len {
        return Err(map_error(format!("{}: code is {} bytes, the map says {}", m.id, code.len(), m.code_len)));
    }
    if m.code_sha256.as_ref().is_some_and(|s| *s != sha(code)) {
        return Err(map_error(format!("{}: the code is not the build this map describes", m.id)));
    }
    let delta = new_run as i64 - m.code_run as i64;
    if delta % m.move_alignment as i64 != 0 {
        return Err(map_error(format!("{}: a move by 0x{delta:X} breaks the code's alignment", m.id)));
    }
    for &(off, want) in m.self_refs.iter().chain(&m.island_ptrs) {
        let got = le32(&code[off as usize..]) as u64;
        if got != want {
            return Err(map_error(format!("{}: site +0x{off:X} holds 0x{got:08X}, the map says 0x{want:08X}", m.id)));
        }
    }
    Ok(())
}

/// dump[a:a+n] as Python slices it (shorter at the end).
fn py_slice(b: &[u8], a: usize, n: usize) -> &[u8] {
    if a >= b.len() {
        &[]
    } else {
        &b[a..b.len().min(a + n)]
    }
}

fn find(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if from >= hay.len() {
        return None;
    }
    hay[from..].windows(needle.len()).position(|w| w == needle).map(|p| p + from)
}
