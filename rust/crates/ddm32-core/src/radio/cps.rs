// SPDX-License-Identifier: GPL-3.0-or-later
//! The data pack writer over the running firmware's open-firmware CPS protocol (docs/spec/open-firmware-cps.md 2;
//! docs/plus.md "Loading a pack from the PC"; Radio/CpsPack.cs): the guard, the client and the tool.

use super::error::{ToolError, ToolResult};
use super::monitor::{be32, be32_bytes, crc32};
use super::port::{Clock, RadioPort, RawSerial};
use super::tools::{Args, ToolContext};
use crate::datapacks::datapack::{self as dp, PackHeader, PackSector, AIR, HEADER_SIZE, RADIO_ID, SECTOR};
use crate::pyjson::{self, Py, PyDict};
use crate::pytext::ascii_decode;

fn refuse(why: impl std::fmt::Display) -> ToolError {
    ToolError::value(format!("refused: {why}"))
}

/// The data pack transport guard: every frame is checked before it goes out. Read-only until [`CpsPackPort::arm`]
/// gives it the plan; then only that plan's sectors, in plan order, each once, with exactly the planned bytes, all
/// inside the data pack region 0xE70000-0xEFFFFF. Allowed:
///   R 9 (radio info); R 1 (flash, physical address) only inside the region, at most 4096 bytes;
///   after arm: C 0, C 1, C 2 (screen text), C 3, C 6 4 (red LED), C 5 (close);
///   X 1 (prepare the next planned sector), X 2 (its planned bytes, inside it), X 3 (commit, once the whole sector
///   was sent).
/// Refused: everything else: W, X 4 / X 7, C 6 0 / C 6 1 (reboots), C 6 2 (save settings), C 6 5..7, D, MMDVM, an
/// address outside the region, a sector out of order or twice, other bytes than planned. The baud rate is fixed.
pub struct CpsPackPort {
    pub raw: Box<dyn RawSerial>,
    plan: Option<Vec<PackSector>>,
    next: usize,
    pending: Option<PackSector>,
    covered: Vec<bool>,
    pub committed: usize,
    pub refusals: Vec<String>,
}

impl CpsPackPort {
    pub fn new(raw: Box<dyn RawSerial>) -> Self {
        Self { raw, plan: None, next: 0, pending: None, covered: vec![], committed: 0, refusals: vec![] }
    }

    pub fn armed(&self) -> bool {
        self.plan.is_some()
    }

    /// The write plan, once: from now on its sectors (and nothing else) may be written, in its order.
    pub fn arm(&mut self, sectors: &[PackSector]) -> ToolResult<()> {
        if self.plan.is_some() {
            return Err(ToolError::value("refused: the pack plan is set once"));
        }
        if sectors.is_empty() {
            return Err(ToolError::value("refused: an empty plan"));
        }
        let mut seen = std::collections::HashSet::new();
        for s in sectors {
            if s.data.len() != SECTOR || s.address % SECTOR as u64 != 0 || !dp::in_region(s.address, SECTOR as u64) {
                return Err(ToolError::value(format!(
                    "refused: sector 0x{:X} is outside the data pack region 0xE70000-0xEFFFFF",
                    s.address
                )));
            }
            if !seen.insert(s.address) {
                return Err(ToolError::value(format!("refused: sector 0x{:X} twice in the plan", s.address)));
            }
        }
        self.plan = Some(sectors.to_vec());
        Ok(())
    }

    /// Checks one frame; ValueError ("refused: ...") for anything outside the rules above.
    pub fn validate(&mut self, f: &[u8]) -> ToolResult<()> {
        if f.len() < 2 {
            return Err(refuse("short frame"));
        }
        match f[0] {
            0x52 => {
                if f.len() != 8 {
                    return Err(refuse("R frame length"));
                }
                let addr = be32(&f[2..]) as u64;
                let len = u16::from_be_bytes([f[6], f[7]]) as u64;
                if f[1] == 9 {
                    return Ok(());
                }
                if f[1] != 1 {
                    return Err(refuse(format!("R area {}", f[1])));
                }
                if len == 0 || len > SECTOR as u64 || !dp::in_region(addr, len) {
                    return Err(refuse(format!("R 0x{addr:X} +{len}: outside the data pack region 0xE70000-0xEFFFFF")));
                }
                Ok(())
            }
            0x43 => {
                if self.plan.is_none() {
                    return Err(refuse(format!("C {} before the plan", f[1])));
                }
                match f[1] {
                    0 | 1 | 3 | 5 if f.len() == 2 => Ok(()),
                    2 if (8..=7 + 17).contains(&f.len()) => Ok(()),
                    6 if f.len() == 3 && f[2] == 4 => Ok(()),
                    _ => Err(refuse(format!(
                        "C {}{}",
                        f[1],
                        if f.len() > 2 { format!(" {}", f[2]) } else { String::new() }
                    ))),
                }
            }
            0x58 => {
                let Some(plan) = self.plan.as_ref() else {
                    return Err(refuse(format!("X {} before the plan", f[1])));
                };
                match f[1] {
                    1 => {
                        if f.len() != 5 {
                            return Err(refuse("X 1 frame length"));
                        }
                        let a = ((f[2] as u64) << 16 | (f[3] as u64) << 8 | f[4] as u64) * SECTOR as u64;
                        if !dp::in_region(a, SECTOR as u64) {
                            return Err(refuse(format!(
                                "sector 0x{a:X} is outside the data pack region 0xE70000-0xEFFFFF"
                            )));
                        }
                        if let Some(p) = &self.pending {
                            return Err(refuse(format!("sector 0x{a:X} while 0x{:X} is pending", p.address)));
                        }
                        if self.next >= plan.len() || plan[self.next].address != a {
                            return Err(refuse(format!("sector 0x{a:X} is not the next planned sector")));
                        }
                        self.pending = Some(plan[self.next].clone());
                        self.covered = vec![false; SECTOR];
                        self.next += 1;
                        Ok(())
                    }
                    2 => {
                        let Some(p) = &self.pending else { return Err(refuse("X 2 without a prepared sector")) };
                        if f.len() < 9 {
                            return Err(refuse("X 2 frame length"));
                        }
                        let a = be32(&f[2..]) as u64;
                        let n = u16::from_be_bytes([f[6], f[7]]) as usize;
                        if n == 0 || n != f.len() - 8 {
                            return Err(refuse("X 2 length field"));
                        }
                        if a < p.address || a + n as u64 > p.address + SECTOR as u64 || !dp::in_region(a, n as u64) {
                            return Err(refuse(format!("X 2 0x{a:X} +{n}: outside the prepared sector")));
                        }
                        let off = (a - p.address) as usize;
                        if f[8..8 + n] != p.data[off..off + n] {
                            return Err(refuse(format!("X 2 0x{a:X}: other bytes than planned")));
                        }
                        for c in &mut self.covered[off..off + n] {
                            *c = true;
                        }
                        Ok(())
                    }
                    3 => {
                        if f.len() != 2 {
                            return Err(refuse("X 3 frame length"));
                        }
                        let Some(p) = &self.pending else { return Err(refuse("X 3 without a prepared sector")) };
                        if self.covered.iter().any(|c| !c) {
                            return Err(refuse(format!("X 3: sector 0x{:X} was not sent whole", p.address)));
                        }
                        self.pending = None;
                        self.committed += 1;
                        Ok(())
                    }
                    _ => Err(refuse(format!("X {}", f[1]))),
                }
            }
            other => Err(refuse(format!("command 0x{other:02X}"))),
        }
    }
}

impl RadioPort for CpsPackPort {
    fn timeout(&self) -> f64 {
        self.raw.timeout()
    }
    fn set_timeout(&mut self, t: f64) {
        self.raw.set_timeout(t)
    }
    fn baud_rate(&self) -> u32 {
        self.raw.baud_rate()
    }
    fn set_baud_rate(&mut self, baud: u32) -> ToolResult<()> {
        if baud != 115200 {
            return Err(ToolError::value("The data pack transport is fixed at 115200 baud"));
        }
        self.raw.set_baud_rate(baud)
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        if let Err(e) = self.validate(data) {
            self.refusals.push(e.message.clone());
            self.pending = None; // a refused frame ends the sector: it is never committed
            return Err(e);
        }
        self.raw.write(data)
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        self.raw.read(n)
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.raw.reset_input_buffer()
    }
    fn flush(&mut self) -> ToolResult<()> {
        self.raw.flush()
    }
}

fn cps_error(m: impl Into<String>) -> ToolError {
    ToolError::new("CpsError", m)
}

fn hexu(b: &[u8]) -> String {
    hex::encode_upper(b)
}

/// The open-firmware CPS client for data packs (upstream tools/cps_client.py's Cps, the subset docs/plus.md names).
/// A sector write is never repeated: a bad reply ends the run. Reads change nothing and are asked again (twice at
/// most) when a reply is spoiled.
pub struct CpsClient<'a> {
    pub port: &'a mut CpsPackPort,
    clock: &'a dyn Clock,
    pub reply_timeout: f64,
}

pub const BLOCK: usize = 32;
pub const READ_CHUNK: usize = 1024;

impl<'a> CpsClient<'a> {
    pub fn new(port: &'a mut CpsPackPort, clock: &'a dyn Clock) -> Self {
        Self { port, clock, reply_timeout: 2.0 }
    }

    fn read_exactly(&mut self, n: usize, timeout: f64) -> ToolResult<Vec<u8>> {
        self.port.set_timeout(timeout);
        let mut out = Vec::with_capacity(n);
        let end = self.clock.monotonic() + timeout;
        while out.len() < n {
            let got = self.port.read(n - out.len())?;
            let empty = got.is_empty();
            out.extend_from_slice(&got);
            if empty && self.clock.monotonic() >= end {
                break;
            }
        }
        Ok(out)
    }

    pub fn control(&mut self, sub: u8, args: &[u8]) -> ToolResult<()> {
        let mut f = vec![0x43, sub];
        f.extend_from_slice(args);
        self.port.write(&f)?;
        let r = self.read_exactly(1, self.reply_timeout)?;
        if r != [0x2D] {
            return Err(cps_error(format!("C {sub}: reply {}", hexu(&r))));
        }
        Ok(())
    }

    pub fn text(&mut self, y: u8, s: &str) -> ToolResult<()> {
        let t: Vec<u8> = s.chars().map(|c| if (c as u32) < 0x80 { c as u8 } else { b'?' }).collect();
        let mut f = vec![0x43, 2, 0, y, 1, 1, 0];
        f.extend_from_slice(&t[..t.len().min(16)]);
        f.push(0);
        self.port.write(&f)?;
        let r = self.read_exactly(1, self.reply_timeout)?;
        if r != [0x2D] {
            return Err(cps_error(format!("C 2: reply {}", hexu(&r))));
        }
        Ok(())
    }

    /// The write session's opening without "save settings" (C 6 2): the data chip outside the region is not touched.
    pub fn opening(&mut self, line: &str) -> ToolResult<()> {
        self.control(0, &[])?;
        self.control(1, &[])?;
        self.text(0, "DDM32 Plus")?;
        self.text(16, line)?;
        self.control(3, &[])?;
        self.control(6, &[4])
    }

    pub fn close(&mut self) -> ToolResult<()> {
        self.control(5, &[])
    }

    fn drain(&mut self) -> ToolResult<()> {
        self.clock.sleep(0.1);
        self.port.set_timeout(0.1);
        for _ in 0..64 {
            if self.port.read(4096)?.is_empty() {
                break;
            }
        }
        Ok(())
    }

    pub fn read(&mut self, area: u8, addr: u64, n: usize, retries: u32) -> ToolResult<Vec<u8>> {
        let mut attempt = 0;
        loop {
            match self.read_once(area, addr, n) {
                Err(e) if e.py_type == "CpsError" && attempt < retries => {
                    self.drain()?;
                    attempt += 1;
                }
                r => return r,
            }
        }
    }

    fn read_once(&mut self, area: u8, addr: u64, n: usize) -> ToolResult<Vec<u8>> {
        let mut f = vec![0x52, area];
        f.extend_from_slice(&be32_bytes(addr));
        f.extend_from_slice(&[(n >> 8) as u8, n as u8]);
        self.port.write(&f)?;
        let h = self.read_exactly(1, self.reply_timeout)?;
        if h != [0x52] {
            return Err(cps_error(format!("R {area} 0x{addr:X} {n}: reply {}", hexu(&h))));
        }
        let l = self.read_exactly(2, self.reply_timeout)?;
        if l.len() != 2 {
            return Err(cps_error(format!("R {area} 0x{addr:X}: short reply")));
        }
        let ln = (l[0] as usize) << 8 | l[1] as usize;
        if area == 1 && ln != n {
            return Err(cps_error(format!("R {area} 0x{addr:X} {n}: reply length {ln}")));
        }
        let d = self.read_exactly(ln, self.reply_timeout + ln as f64 / 5000.0)?;
        if d.len() != ln {
            return Err(cps_error(format!("R {area} 0x{addr:X}: short reply {} of {ln}", d.len())));
        }
        Ok(d)
    }

    /// Flash bytes [addr, addr + n) in chunks of 1024.
    pub fn read_flash(&mut self, addr: u64, n: usize) -> ToolResult<Vec<u8>> {
        let mut out = Vec::with_capacity(n);
        let mut o = 0;
        while o < n {
            let k = READ_CHUNK.min(n - o);
            out.extend(self.read(1, addr + o as u64, k, 2)?);
            o += k;
        }
        Ok(out)
    }

    /// The radio info (area 9): struct version, radio type (11 = DM-32UV), git, build, flash ID.
    pub fn info(&mut self) -> ToolResult<PyDict> {
        let d = self.read(9, 0, 0x20, 2)?;
        if d.len() < 44 {
            return Err(cps_error(format!("radio info: {} bytes", d.len())));
        }
        let s = |o: usize, n: usize| {
            let b = &d[o..o + n];
            let z = b.iter().position(|&x| x == 0).unwrap_or(b.len());
            ascii_decode(&b[..z])
        };
        let le = |o: usize| u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as i64;
        Ok(PyDict::new()
            .with("struct", le(0))
            .with("radio_type", le(4))
            .with("git", s(8, 16))
            .with("build", s(24, 16))
            .with("flash_id", le(40)))
    }

    fn ack(&mut self, what: &str, sub: u8, timeout: f64) -> ToolResult<()> {
        let r = self.read_exactly(2, timeout)?;
        if r.len() != 2 || r[0] != 0x58 || r[1] != sub {
            return Err(cps_error(format!("{what}: reply {}", hexu(&r))));
        }
        Ok(())
    }

    /// One journaled sector write: prepare, the 4096 bytes in 32-byte blocks, commit. Never repeated.
    pub fn write_sector(&mut self, addr: u64, data: &[u8]) -> ToolResult<()> {
        if data.len() != SECTOR || !addr.is_multiple_of(SECTOR as u64) {
            return Err(ToolError::value("a sector is 4096 bytes on a 4096-byte boundary"));
        }
        let sector = addr / SECTOR as u64;
        self.port.write(&[0x58, 1, (sector >> 16) as u8, (sector >> 8) as u8, sector as u8])?;
        self.ack(&format!("prepare sector 0x{addr:X}"), 1, self.reply_timeout)?;
        for o in (0..SECTOR).step_by(BLOCK) {
            let mut f = vec![0x58, 2];
            f.extend_from_slice(&be32_bytes(addr + o as u64));
            f.extend_from_slice(&[(BLOCK >> 8) as u8, BLOCK as u8]);
            f.extend_from_slice(&data[o..o + BLOCK]);
            self.port.write(&f)?;
            self.ack(&format!("data 0x{:X}", addr + o as u64), 2, self.reply_timeout)?;
        }
        self.port.write(&[0x58, 3])?;
        self.ack(&format!("commit sector 0x{addr:X}"), 3, self.reply_timeout * 3.0)
        // erase, program, journal
    }
}

pub const RADIO_TYPE_DM32: i64 = 11;

/// One slot as the radio holds it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SlotState {
    pub ptype: u8,
    pub slot: usize,
    pub address: u64,
    pub header: Option<PackHeader>,
    pub count: Option<usize>,
    pub in_force: bool,
}

/// A slot's image as far as the checks need it: the header, and the payload when the header is valid.
pub fn read_slot(c: &mut CpsClient<'_>, ptype: u8, slot: usize) -> ToolResult<Vec<u8>> {
    let addr = dp::slot_address(ptype, slot).map_err(|e| ToolError::value(e.0))?;
    let mut head = c.read_flash(addr, HEADER_SIZE)?;
    if &head[..4] == dp::MAGIC && u32::from_le_bytes([head[60], head[61], head[62], head[63]]) == crc32(&head[..60], 0)
    {
        let len = u32::from_le_bytes([head[12], head[13], head[14], head[15]]) as usize;
        if len > 0 && len <= dp::slot_size(ptype).unwrap_or(0) - HEADER_SIZE {
            head.extend(c.read_flash(addr + HEADER_SIZE as u64, len)?);
        }
    }
    Ok(head)
}

pub fn describe(ptype: u8, images: &[Vec<u8>]) -> Vec<SlotState> {
    let active = dp::active_slot(ptype, images);
    images
        .iter()
        .enumerate()
        .map(|(i, img)| {
            let (h, payload) = dp::parse(img);
            let count = h.as_ref().filter(|h| h.crc_ok).and_then(|h| dp::count(h, &payload));
            SlotState {
                ptype,
                slot: i,
                address: dp::slot_address(ptype, i).unwrap(),
                header: h,
                count,
                in_force: active == Some(i),
            }
        })
        .collect()
}

pub fn slot_json(s: &SlotState) -> PyDict {
    let h = s.header.as_ref();
    PyDict::new()
        .with("type", if s.ptype == RADIO_ID { "radioid" } else { "air" })
        .with("slot", s.slot)
        .with("address", pyjson::hex(s.address as i64))
        .with("valid", h.is_some())
        .with("crc_ok", h.is_some_and(|h| h.crc_ok))
        .with("in_force", s.in_force)
        .with("seq", h.map(|h| h.seq as i64))
        .with("name", h.map(|h| h.name.clone()))
        .with("date", h.map(|h| h.date.clone()))
        .with("bytes", h.map(|h| h.length as i64))
        .with("count", s.count)
        .with("tag", h.map(|h| h.tag.clone()))
}

fn line(d: PyDict) -> String {
    pyjson::dumps(&Py::Dict(d), None)
}

/// The data pack tool (like the other radio tools: argv, printed lines, the port only from ctx.serial):
///   read --port COMx                         the radio info and both slots of each pack type (read only)
///   write --port COMx --pack FILE.dpk        the pack into the slot not in force (docs/plus.md, steps 1-5)
pub fn data_pack_tool(argv: &[String], ctx: &ToolContext<'_>) -> ToolResult<()> {
    if argv.is_empty() || !(argv[0] == "read" || argv[0] == "write") {
        return Err(ToolError::exit("2"));
    }
    let a = Args::new(&argv[1..], &["--port", "--pack", "--line"], &[], &["--port"])?;
    let port_name = a.get("--port").ok_or_else(|| ToolError::exit("2"))?.to_string();
    let mut pack: Option<(Vec<u8>, PackHeader)> = None;
    if argv[0] == "write" {
        let path = std::path::PathBuf::from(a.get("--pack").ok_or_else(|| ToolError::exit("2"))?);
        let image = std::fs::read(&path)?;
        let (h, _) = dp::parse(&image);
        let whole = h.as_ref().is_some_and(|h| {
            h.crc_ok
                && h.format == 1
                && (h.ptype == RADIO_ID || h.ptype == AIR)
                && image.len() == HEADER_SIZE + h.length as usize
        });
        if !whole {
            return Err(ToolError::value(format!(
                "{}: not a whole data pack",
                path.file_name().unwrap_or_default().to_string_lossy()
            )));
        }
        pack = Some((image, h.unwrap()));
    }
    let mut raw = (ctx.serial)(&port_name)?;
    raw.open()?;
    raw.set_baud_rate(115200)?;
    raw.reset_input_buffer()?;
    let mut guard = CpsPackPort::new(raw);
    let result = data_pack_run(&mut guard, &argv[0], pack, a.get("--line").unwrap_or("Data pack"), ctx);
    let _ = guard.raw.close();
    result
}

fn data_pack_run(
    guard: &mut CpsPackPort,
    cmd: &str,
    pack: Option<(Vec<u8>, PackHeader)>,
    line_text: &str,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    let mut c = CpsClient::new(guard, ctx.clock);
    let info = c.info()?;
    (ctx.print)(&line(PyDict::new().with("radio", info.clone())));
    if info.get("radio_type").and_then(Py::as_i64) != Some(RADIO_TYPE_DM32) {
        let t = info.get("radio_type").and_then(Py::as_i64).map_or("None".into(), |t| t.to_string());
        return Err(ToolError::new("RadioError", format!("radio type {t}: not a DM-32UV running DDM32 Plus")));
    }
    if cmd == "read" {
        for ptype in [RADIO_ID, AIR] {
            let images = vec![read_slot(&mut c, ptype, 0)?, read_slot(&mut c, ptype, 1)?];
            for s in describe(ptype, &images) {
                (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
            }
        }
        (ctx.print)(&line(PyDict::new().with("result", "READ")));
        return Ok(());
    }
    let (image, ph) = pack.unwrap();
    write_pack(&mut c, &image, &ph, line_text, ctx)
}

fn write_pack(
    c: &mut CpsClient<'_>,
    image: &[u8],
    ph: &PackHeader,
    line_text: &str,
    ctx: &ToolContext<'_>,
) -> ToolResult<()> {
    let ptype = ph.ptype;
    let pe = |e: crate::datapacks::PackError| ToolError::new("PackError", e.0);
    let before = vec![read_slot(c, ptype, 0)?, read_slot(c, ptype, 1)?];
    for s in describe(ptype, &before) {
        (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
    }
    let slot = dp::inactive_slot(ptype, &before);
    let seq = dp::next_seq(&before).map_err(pe)?;
    let payload = &image[HEADER_SIZE..];
    let target = dp::image(ptype, payload, &ph.date, &ph.name, seq, &ph.tag).map_err(pe)?;
    let plan = dp::plan_for_slot(ptype, slot, &target).map_err(pe)?;
    (ctx.print)(&line(
        PyDict::new().with(
            "plan",
            PyDict::new()
                .with("type", if ptype == RADIO_ID { "radioid" } else { "air" })
                .with("slot", slot)
                .with("address", pyjson::hex(dp::slot_address(ptype, slot).map_err(pe)? as i64))
                .with("seq", seq as i64)
                .with("bytes", target.len())
                .with("sectors", Py::List(plan.iter().map(|p| Py::Str(pyjson::hex(p.address as i64))).collect())),
        ),
    ));
    c.port.arm(&plan)?;
    c.opening(line_text)?;
    let r = (|| -> ToolResult<()> {
        for (i, p) in plan.iter().enumerate() {
            c.write_sector(p.address, &p.data)?;
            let back = c.read_flash(p.address, SECTOR)?;
            if back != p.data {
                return Err(ToolError::new(
                    "VerifyError",
                    format!("sector 0x{:X}: read back differs (not written again)", p.address),
                ));
            }
            (ctx.print)(&format!("pack: sector {}/{} 0x{:X} written and read back", i + 1, plan.len(), p.address));
        }
        c.close()?;
        Ok(())
    })();
    if let Err(e) = r {
        // leave the CPS screen; the pack in force is unchanged (its header was not written)
        let _ = c.close();
        return Err(e);
    }
    let after = vec![read_slot(c, ptype, 0)?, read_slot(c, ptype, 1)?];
    for s in describe(ptype, &after) {
        (ctx.print)(&line(PyDict::new().with("slot", slot_json(&s))));
    }
    if after[slot] != target || dp::active_slot(ptype, &after) != Some(slot) {
        return Err(ToolError::new("VerifyError", "the new pack is not the one in force after the write"));
    }
    (ctx.print)(&line(
        PyDict::new()
            .with("result", "PASS")
            .with("slot", slot)
            .with("seq", seq as i64)
            .with("sectors", plan.len())
            .with("name", ph.name.clone())
            .with("date", ph.date.clone()),
    ));
    Ok(())
}
