// SPDX-License-Identifier: GPL-3.0-or-later
//! The pinned RAM monitor and its host side (RadioFoundry stub/stubtool.py class Radio, MIT,
//! LICENSES/RadioFoundry-MIT.txt; the C# MonitorRadio.cs): the part the DDM32 Plus tools use, ported line by line so
//! that the same answers from the radio give the same bytes to the radio. It loads the stub through the boot ROM at
//! power-on, talks the monitor's frames (02 op len chk 03 payload), configures it for the DM-32UV, reads flash (LZ4 /
//! raw / blank sectors, every sector checked against the radio's CRC-32), and sends the generic SPI flash commands
//! the installer's guard allows. It never raises the baud rate and has no NAND path (the DM-32UV has none).
//! Every byte goes through the port it is given: the tools hand it their guard.

use super::error::{ToolError, ToolResult};
use super::port::{Clock, RadioPort};
use crate::pyjson::{Py, PyDict};
use sha2::{Digest, Sha256};

// ---- helpers (zlib.crc32, hashlib.sha256, the stub's checksums) ----------------------------------------------------

/// zlib.crc32(data, crc)
pub fn crc32(data: &[u8], crc: u32) -> u32 {
    let mut h = crc32fast::Hasher::new_with_initial(crc);
    h.update(data);
    h.finalize()
}

/// hashlib.sha256(data).hexdigest()
pub fn sha(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

pub fn sum8(b: &[u8]) -> u8 {
    b.iter().fold(0u32, |s, &x| s + x as u32) as u8
}

pub fn be32(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}

pub fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

pub fn be32_bytes(v: u64) -> [u8; 4] {
    (v as u32).to_be_bytes()
}

pub fn le32_bytes(v: u64) -> [u8; 4] {
    (v as u32).to_le_bytes()
}

fn hex_spaced(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect::<Vec<_>>().join(" ")
}

fn head(b: &[u8], n: usize) -> &[u8] {
    &b[..n.min(b.len())]
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && hay.windows(needle.len()).any(|w| w == needle)
}

/// The pinned monitor host's own LZ4 block decoder (stubtool.lz4_block); None for a corrupt block.
pub fn lz4_block(src: &[u8], size: usize) -> Option<Vec<u8>> {
    let mut out: Vec<u8> = Vec::with_capacity(size);
    let mut i = 0usize;
    let n = src.len();
    while i < n {
        let tok = src[i] as usize;
        i += 1;
        let mut lit = tok >> 4;
        if lit == 15 {
            loop {
                let b = *src.get(i)? as usize;
                i += 1;
                lit += b;
                if b != 255 {
                    break;
                }
            }
        }
        let end = n.min(i + lit); // Python slices past the end without error
        if i < end {
            out.extend_from_slice(&src[i..end]);
        }
        i += lit;
        if i >= n {
            break;
        }
        let off = *src.get(i)? as usize | (*src.get(i + 1)? as usize) << 8;
        i += 2;
        let mut ml = tok & 15;
        if ml == 15 {
            loop {
                let b = *src.get(i)? as usize;
                i += 1;
                ml += b;
                if b != 255 {
                    break;
                }
            }
        }
        ml += 4;
        if off == 0 || off > out.len() {
            return None;
        }
        let start = out.len() - off;
        for k in 0..ml {
            let b = out[start + k];
            out.push(b);
        }
    }
    (out.len() == size).then_some(out)
}

// ---- the pinned image and the boot ROM's framing ---------------------------------------------------------------------

/// The pinned RAM monitor (RadioFoundry stub/stubtool.py, MIT): the universal stub image, used only when its
/// SHA-256 is the pinned value.
pub mod ram_monitor {
    use super::*;

    pub const SHA256: &str = "eae5b98615d67ba465061c84a5fb209a902333dc700671ce4ca69a05eb4aaee9";
    pub const UPSTREAM_PIN: &str = "dfbaf20c400c73e2ccf6edb80af7d1d255bee418";
    pub const HOST_TOOL_SHA256: &str = "4e3ad1fa0c85d89e9f4bc15c5d6f97212bbab341ac72d958bdf7d0e250124b9e";

    pub const ROM_ACK: [u8; 4] = [0x02, 0x05, 0x00, 0x03];
    pub const ROM_HELLO: [u8; 4] = [0x02, 0x24, 0x00, 0x03];
    pub const ROM_REPLY: [u8; 4] = [0x02, 0x14, 0x00, 0x03];
    pub const ROM_GAVE_UP: [u8; 4] = [0x00, 0x10, 0x00, 0x03];
    pub const ROM_NAK: [u8; 4] = [0x02, 0x0a, 0x00, 0x03];

    const EMBEDDED: &[u8] = include_bytes!("../../data/ram-monitor.bin");

    /// stub_univ_image(): the pinned universal stub, after its SHA-256 check.
    pub fn image() -> ToolResult<Vec<u8>> {
        if sha(EMBEDDED) != SHA256 {
            return Err(ToolError::value("Pinned RAM monitor hash mismatch"));
        }
        Ok(EMBEDDED.to_vec())
    }

    /// 02 op chk 03 payload: the ROM's own framing.
    pub fn rom_cmd(op: u8, payload: &[u8]) -> Vec<u8> {
        let mut v = vec![0x02, op, sum8(payload), 0x03];
        v.extend_from_slice(payload);
        v
    }

    /// 02 op len(2) chk 03 payload: the monitor's frames (install_firstboot.packet).
    pub fn packet(op: u8, payload: &[u8]) -> Vec<u8> {
        let mut v = vec![0x02, op, (payload.len() >> 8) as u8, payload.len() as u8, sum8(payload), 0x03];
        v.extend_from_slice(payload);
        v
    }

    /// A program flash SPI transaction (install_firstboot.spi_packet).
    pub fn spi_packet(tx: &[u8], rx: usize) -> Vec<u8> {
        let mut p =
            vec![0x04, 0x00, 0x00, 0x00, 0x01, 0x00, (tx.len() >> 8) as u8, tx.len() as u8, (rx >> 8) as u8, rx as u8];
        p.extend_from_slice(tx);
        packet(5, &p)
    }
}

use ram_monitor as rm;

// ---- the monitor host ---------------------------------------------------------------------------------------------

pub const BLOCK: usize = 256;
pub const SECTOR: u64 = 4096;
pub const WINDOW: u64 = 16;
pub const PACK: u64 = 16;
pub const LOAD_ADDR: u64 = 0x10000;
pub const SSI_PROG: u64 = 0x0400_0000;
pub const SSI_DATA: u64 = 0x140A_0000;
pub const SYS_SOFT_RSTN: u64 = 0x1100_0000;
pub const QUAD_ENABLE: u64 = 0x1100_005C;
pub const LCSFC_BAUDR: u64 = 0x1100_0030;
pub const WDT_LOCK: u64 = 0x1401_0000;
pub const WDT_EN: u64 = 0x1401_0010;
pub const WDT_KEY: u64 = 0x5ADA_7200;
pub const CRASH_SOFT_MARK: u64 = 0x1800_7EF4;
pub const CRASH_BOOT_SOFT: u64 = 0x5446_4F53;
pub const TIMER2_VALUE: u64 = 0x1400_0018;
pub const LED_RED: u8 = 33;
pub const LED_GREEN: u8 = 32;
pub const OK: u8 = 0x05;
pub const BAD_SUM: u8 = 0x0A;
pub const BAD_OP: u8 = 0x0B;
pub const TOO_LONG: u8 = 0x0E;
pub const WRITE_VERSION: u8 = 7;
pub const CFG_UART_THIS: u8 = 3;
const SRAM: (u64, u64) = (0x10000, 0x58000);

/// A memory of the radio: (SSI base, SER, chip select, size, name).
struct Mem {
    base: u64,
    ser: bool,
    cs: Option<u8>,
    size: u64,
    name: &'static str,
}

fn mem(name: &str) -> Mem {
    match name {
        "progmem" => Mem { base: SSI_PROG, ser: true, cs: None, size: 0x100000, name: "program flash" },
        _ => Mem { base: SSI_DATA, ser: true, cs: Some(18), size: 0x1000000, name: "data flash" },
    }
}

fn protect_mem(m: &str) -> &'static str {
    if m == "progmem" {
        "progmem"
    } else {
        "data"
    }
}

/// The DM-32UV's board profile (boards/dm32 board.toml [flash.stub] and [[flash.protect]]).
#[derive(Debug, Clone, PartialEq)]
pub struct Board {
    pub id: u8,
    pub name: &'static str,
    pub latch: u8,
    pub key: u8,
    pub nand: bool,
    pub progmem: u64,
    pub protect: Vec<(&'static str, u64, u64, &'static str)>,
}

pub fn dm32_board() -> Board {
    Board {
        id: 1,
        name: "DM-32UV",
        latch: 255,
        key: 255,
        nand: false,
        progmem: 1048576,
        protect: vec![
            ("data", 4096, 823296, "factory calibration: the stock codeplug area's 4 KB page tagged 2, which can be any page of it (hardware-dm32.md 10.1, 11.1)"),
            ("data", 7405568, 7409664, "a decoded calibration copy an earlier firmware left, the fallback when no factory page is found (hardware-dm32.md 10.1, 17)"),
        ],
    }
}

pub fn generic_board() -> Board {
    Board { id: 0, name: "HR_C7000 radio", latch: 255, key: 255, nand: false, progmem: 0x100000, protect: vec![] }
}

/// Where a dump goes: a file or memory.
pub trait DumpSink {
    fn set_len(&mut self, n: u64) -> std::io::Result<()>;
    fn write_at(&mut self, off: u64, data: &[u8]) -> std::io::Result<()>;
    fn flush_sink(&mut self) -> std::io::Result<()>;
}

impl DumpSink for Vec<u8> {
    fn set_len(&mut self, n: u64) -> std::io::Result<()> {
        self.resize(n as usize, 0);
        Ok(())
    }
    fn write_at(&mut self, off: u64, data: &[u8]) -> std::io::Result<()> {
        let o = off as usize;
        if self.len() < o + data.len() {
            self.resize(o + data.len(), 0);
        }
        self[o..o + data.len()].copy_from_slice(data);
        Ok(())
    }
    fn flush_sink(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl DumpSink for std::fs::File {
    fn set_len(&mut self, n: u64) -> std::io::Result<()> {
        std::fs::File::set_len(self, n)
    }
    fn write_at(&mut self, off: u64, data: &[u8]) -> std::io::Result<()> {
        use std::io::{Seek, SeekFrom, Write};
        self.seek(SeekFrom::Start(off))?;
        self.write_all(data)
    }
    fn flush_sink(&mut self) -> std::io::Result<()> {
        use std::io::Write;
        self.flush()?;
        self.sync_all()
    }
}

/// The stub's HELLO v6 footprint.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct V6 {
    pub load: u64,
    pub board: u64,
    pub flags: u64,
    pub run: u64,
    pub image: u64,
    pub bss_end: u64,
    pub stack_lo: u64,
    pub stack_top: u64,
    pub stack_used: u64,
}

impl V6 {
    pub fn to_py(&self) -> PyDict {
        PyDict::new()
            .with("load", self.load as i64)
            .with("board", self.board as i64)
            .with("flags", self.flags as i64)
            .with("run", self.run as i64)
            .with("image", self.image as i64)
            .with("bss_end", self.bss_end as i64)
            .with("stack_lo", self.stack_lo as i64)
            .with("stack_top", self.stack_top as i64)
            .with("stack_used", self.stack_used as i64)
    }
}

pub type Progress<'p> = Option<&'p mut dyn FnMut(u64, u64)>;

/// What `sectors` hands each record to: (crc32, method, data).
type SectorRecord<'f, R> = dyn FnMut(&mut R, u32, u8, Vec<u8>) -> ToolResult<()> + 'f;

pub struct MonitorRadio<'a, P: RadioPort + ?Sized> {
    pub port: &'a mut P,
    log: &'a dyn Fn(&str),
    clock: &'a dyn Clock,
    pub retries: u32,
    pub retried: u64,
    pub echo: bool,
    pub stub_base: u64,
    pub v6: Option<V6>,
    pub sector_op: bool,
    pub info: u64,
    pub configured: Option<Board>,
    pub profile: Board,
    pub progmem_size: u64,
    link_score: i64,
}

impl<'a, P: RadioPort + ?Sized> MonitorRadio<'a, P> {
    pub fn new(port: &'a mut P, log: &'a dyn Fn(&str), clock: &'a dyn Clock, retries: u32) -> Self {
        Self {
            port,
            log,
            clock,
            retries,
            retried: 0,
            echo: false,
            stub_base: LOAD_ADDR,
            v6: None,
            sector_op: false,
            info: 0,
            configured: None,
            profile: generic_board(),
            progmem_size: 0x100000,
            link_score: 0,
        }
    }

    fn log(&self, s: &str) {
        (self.log)(s)
    }

    fn set_timeout(&mut self, t: f64) {
        if self.port.timeout() != t {
            self.port.set_timeout(t);
        }
    }

    pub fn read_exact(&mut self, n: usize, timeout: f64) -> ToolResult<Vec<u8>> {
        let end = self.clock.monotonic() + timeout;
        let mut out = Vec::with_capacity(n);
        self.set_timeout(0.02);
        while out.len() < n {
            let chunk = self.port.read(n - out.len())?;
            if !chunk.is_empty() {
                out.extend_from_slice(&chunk);
            } else if self.clock.monotonic() >= end {
                break;
            }
        }
        Ok(out)
    }

    // ---- boot ROM: wait for it, load the stub, start it ----------------------------------------------------------
    fn rom_ack(&mut self, sent: &[u8], timeout: f64) -> ToolResult<(bool, Vec<u8>)> {
        let end = self.clock.monotonic() + timeout;
        let mut buf: Vec<u8> = Vec::new();
        loop {
            if buf.len() >= 4 && buf[..4] == rm::ROM_ACK {
                return Ok((true, vec![]));
            }
            let n = buf.len().min(sent.len());
            let echo = n > 0 && buf[..n] == sent[..n];
            if echo && buf.len() >= sent.len() {
                buf = buf[sent.len()..].to_vec();
                if !self.echo {
                    self.log("the cable echoes what the host sends; the echo is ignored");
                }
                self.echo = true;
                continue;
            }
            let tail: Vec<u8> = if buf.len() >= 4 { buf[buf.len() - 4..].to_vec() } else { buf.clone() };
            if !echo && tail == rm::ROM_ACK {
                let before = &buf[..buf.len() - 4];
                self.log(&format!("before the boot ROM's acknowledgement: {}", hex_spaced(head(before, 16))));
                return Ok((true, vec![]));
            }
            if !echo && (tail == rm::ROM_NAK || contains(&buf, &rm::ROM_GAVE_UP)) {
                return Ok((false, head(&buf, 16).to_vec()));
            }
            if self.clock.monotonic() >= end {
                return Ok((false, head(&buf, 16).to_vec()));
            }
            let want = if echo { sent.len() - buf.len() } else { 4 - buf.len() };
            let t = 0.0f64.max(0.05f64.min(end - self.clock.monotonic()));
            let more = self.read_exact(want.max(1), t)?;
            buf.extend_from_slice(&more);
        }
    }

    fn rom_error(&self, what: &str, got: &[u8]) -> ToolError {
        let bytes =
            if got.is_empty() { "nothing received".to_string() } else { format!("received {}", hex_spaced(got)) };
        ToolError::dump(format!(
            "{what} ({bytes}{}). The radio's boot ROM did not take the loader over the programming cable. Try a cable with an FTDI, CP210x or PL2303 chip, --slow-cable, or use the stock firmware file instead of reading the radio",
            if self.echo { "; the cable echoes" } else { "" }
        ))
    }

    fn rom_late(got: &[u8]) -> ToolError {
        ToolError::dump(format!(
            "the radio's boot ROM stopped waiting before the answer to its greeting reached it (received {}). The cable or a USB hub carries it too slowly: run again with --slow-cable",
            hex_spaced(head(got, 16))
        ))
    }

    /// Waits for the boot ROM's greeting (not the --slow-cable way: that is never used here).
    fn wait_for_radio(&mut self, wait: f64) -> ToolResult<bool> {
        let end = self.clock.monotonic() + wait;
        let mut last: Vec<u8> = Vec::new();
        self.set_timeout(0.1);
        while self.clock.monotonic() < end {
            let b = self.port.read(1)?;
            if b.is_empty() {
                continue;
            }
            last.extend_from_slice(&b);
            if last.len() > 4 {
                last = last[last.len() - 4..].to_vec();
            }
            if last == rm::ROM_HELLO {
                return Ok(false);
            }
            if last == rm::ROM_GAVE_UP {
                return Err(ToolError::dump(
                    "the radio started its firmware: its boot ROM's greeting did not come through. Run again with --slow-cable",
                ));
            }
        }
        Err(ToolError::dump(format!(
            "the radio did not start its serial boot ROM within {} s: radio off and cable in before the script starts, then switch it on",
            wait as i64
        )))
    }

    /// Boots the universal stub through the serial boot ROM at power-on, loaded at 0x10000.
    pub fn start(&mut self, wait: u32) -> ToolResult<()> {
        let stub = rm::image()?;
        check_univ_load(&stub, LOAD_ADDR)?;
        let mut padded = vec![0xFFu8; stub.len().div_ceil(BLOCK) * BLOCK];
        padded[..stub.len()].copy_from_slice(&stub);
        let stub = padded;
        self.stub_base = LOAD_ADDR;
        let answered = self.wait_for_radio(wait as f64)?;
        if !answered {
            self.port.write(&rm::ROM_REPLY)?;
        }
        self.clock.sleep(if answered { 0.2 } else { 0.05 });
        let reply = self.read_exact(256, 0.02)?;
        if contains(&reply, &rm::ROM_GAVE_UP) {
            return Err(Self::rom_late(&reply));
        }
        if contains(&reply, &rm::ROM_REPLY) {
            self.echo = true;
            self.log("the cable echoes what the host sends; the echo is ignored");
        }
        if !reply.is_empty() {
            self.log(&format!("after the boot ROM greeting: {}", hex_spaced(head(&reply, 16))));
        }
        let blocks = stub.len() / BLOCK;
        self.log(&format!("boot ROM answered; loading {}-byte stub ({blocks} blocks)", stub.len()));
        for i in 0..blocks {
            let mut payload = be32_bytes(LOAD_ADDR + (i * BLOCK) as u64).to_vec();
            payload.extend_from_slice(&[0, 0, 0, 0x40]);
            payload.extend_from_slice(&stub[i * BLOCK..(i + 1) * BLOCK]);
            let mut got = vec![];
            let mut ok = false;
            for _attempt in 0..3 {
                self.port.reset_input_buffer()?;
                let frame = rm::rom_cmd(0x18, &payload);
                self.port.write(&frame)?;
                (ok, got) = self.rom_ack(&frame, 1.0)?;
                if ok {
                    break;
                }
                if contains(&got, &rm::ROM_GAVE_UP) {
                    return Err(Self::rom_late(&got));
                }
                let what = if got.is_empty() {
                    "nothing received".to_string()
                } else {
                    format!("received {}", hex_spaced(&got))
                };
                self.log(&format!("stub block {i} not acknowledged ({what}), sending it again"));
            }
            if !ok {
                return Err(self.rom_error(&format!("stub block {i} was not acknowledged"), &got));
            }
        }
        self.port.reset_input_buffer()?;
        let go = rm::rom_cmd(0x1B, &be32_bytes(LOAD_ADDR));
        self.port.write(&go)?;
        let (go_ok, go_got) = self.rom_ack(&go, 1.0)?;
        if !go_ok {
            return Err(self.rom_error("the start command was not acknowledged", &go_got));
        }
        let end_t = self.clock.monotonic() + 3.0;
        let info = loop {
            match self.hello(0) {
                Ok(i) => break i,
                Err(e) if e.is_dump() => {
                    if self.clock.monotonic() > end_t {
                        return Err(ToolError::dump("the stub was started but does not answer"));
                    }
                }
                Err(e) => return Err(e),
            }
        };
        self.log(&format!(
            "stub v{} running (max payload {}, SSI burst {} / {} bytes)",
            info.0,
            info.1,
            self.info >> 8 & 0xFF,
            self.info & 0xFF
        ));
        Ok(())
    }

    // ---- stub frames ---------------------------------------------------------------------------------------------
    fn rate(&self) -> f64 {
        let b = self.port.baud_rate();
        if b != 0 {
            b as f64
        } else {
            115200.0
        }
    }

    fn exchange(&mut self, op: u8, payload: &[u8], timeout: f64) -> ToolResult<(Option<u8>, Vec<u8>, String)> {
        let frame = rm::packet(op, payload);
        self.port.reset_input_buffer()?;
        self.port.write(&frame)?;
        let t = timeout + frame.len() as f64 * 10.0 / self.rate();
        let hdr = self.read_exact(6, t)?;
        if hdr.len() != 6 || hdr[0] != 0x02 || hdr[5] != 0x03 {
            return Ok((None, vec![], "no or malformed reply header".into()));
        }
        let n = (hdr[2] as usize) << 8 | hdr[3] as usize;
        let body = self.read_exact(n, 0.5 + n as f64 * 0.0002)?;
        if body.len() != n {
            return Ok((None, vec![], format!("reply short ({} of {n} bytes)", body.len())));
        }
        if sum8(&body) != hdr[4] {
            return Ok((None, vec![], "reply checksum".into()));
        }
        if hdr[1] == BAD_SUM {
            return Ok((None, vec![], "the radio saw a bad request checksum".into()));
        }
        Ok((Some(hdr[1]), body, String::new()))
    }

    pub fn request(&mut self, op: u8, payload: &[u8], retries: Option<u32>, timeout: f64) -> ToolResult<Vec<u8>> {
        let r = retries.unwrap_or(self.retries);
        let mut why = String::new();
        for attempt in 0..r + 1 {
            let (status, body, w) = self.exchange(op, payload, timeout)?;
            if status == Some(OK) {
                self.link_score = (self.link_score - 1).max(0);
                return Ok(body);
            }
            if let Some(s) = status {
                return Err(ToolError::status(format!("stub op 0x{op:02X} refused: status 0x{s:02X}"), s));
            }
            why = w;
            self.retried += 1;
            if attempt < r {
                self.log(&format!("op 0x{op:02X}: {why}, again ({} of {r})", attempt + 1));
                self.clock.sleep(0.02);
            }
        }
        Err(ToolError::dump(format!("op 0x{op:02X}: no clean reply after {} tries ({why})", r + 1)))
    }

    pub fn hello(&mut self, retries: u32) -> ToolResult<(u8, u32)> {
        let r = self.request(0x01, &[], Some(retries), 1.0)?;
        if r.len() < 6 || &r[..6] != b"C7STUB" {
            return Err(ToolError::dump(format!("unexpected hello {}", py_bytes_repr(&r))));
        }
        self.info = if r.len() >= 17 { be32(&r[13..]) as u64 } else { 0 };
        self.sector_op = r[6] >= WRITE_VERSION;
        if r.len() >= 43 && r[6] >= 6 {
            let g = |o: usize| be32(&r[o..]) as u64;
            let v6 = V6 {
                load: g(9),
                board: r[17] as u64,
                flags: r[18] as u64,
                run: g(19),
                image: g(23),
                bss_end: g(27),
                stack_lo: g(31),
                stack_top: g(35),
                stack_used: g(39),
            };
            self.stub_base = v6.run;
            self.v6 = Some(v6);
        } else {
            self.v6 = None;
        }
        Ok((r[6], (r[7] as u32) << 8 | r[8] as u32))
    }

    pub fn footprint(&self) -> String {
        let v = self.v6.as_ref().expect("v6");
        format!(
            "loaded at 0x{:X}, runs at 0x{:X}-0x{:X}: image {} B, .bss to 0x{:X}, stack {} B with {} B used at most",
            v.load,
            v.run,
            v.stack_top,
            v.image,
            v.bss_end,
            v.stack_top as i64 - v.stack_lo as i64,
            v.stack_used
        )
    }

    /// CONFIG (op 0x0D) for the board, checked by the HELLO echo; writes are refused until it succeeded.
    pub fn configure(&mut self, board: &Board) -> ToolResult<()> {
        if self.v6.is_none() {
            self.log("stub protocol before 6: no board configuration");
            self.profile = board.clone();
            return Ok(());
        }
        let hz = self.timer_hz(0.3)?;
        if !(1e6 < hz && hz < 4e8) {
            return Err(ToolError::dump(format!(
                "the stub's timer runs at {hz:.0} Hz, which can't be right: not configuring it"
            )));
        }
        let hzi = ((hz / 1e6).round_ties_even() * 1e6) as u64;
        let mut payload = be32_bytes(hzi).to_vec();
        payload.extend_from_slice(&be32_bytes(hzi));
        payload.extend_from_slice(&[
            board.latch,
            board.key,
            LED_RED,
            LED_GREEN,
            board.id,
            board.nand as u8,
            CFG_UART_THIS,
            0,
        ]);
        self.request(0x0D, &payload, None, 1.0)?;
        self.hello(self.retries)?;
        let ok = self.v6.as_ref().is_some_and(|v| v.board == board.id as u64 && v.flags == board.nand as u64);
        if !ok {
            let (b, f) = match &self.v6 {
                None => ("None".to_string(), "None".to_string()),
                Some(v) => (v.board.to_string(), v.flags.to_string()),
            };
            return Err(ToolError::dump(format!(
                "the stub did not take the {} configuration (HELLO says board {b}, flags {f}): nothing written",
                board.name
            )));
        }
        self.configured = Some(board.clone());
        self.profile = board.clone();
        self.log(&format!(
            "stub configured for the {} (clock {:.1} MHz); {}",
            board.name,
            hzi as f64 / 1e6,
            self.footprint()
        ));
        Ok(())
    }

    pub fn check_writable(&self) -> ToolResult<()> {
        if self.v6.is_some() && self.configured.is_none() {
            return Err(ToolError::dump("the stub's board configuration is not confirmed: refusing to write"));
        }
        Ok(())
    }

    pub fn mem_size(&self, m: &str) -> u64 {
        if m == "progmem" && self.v6.is_some() {
            self.progmem_size
        } else {
            mem(m).size
        }
    }

    fn mem_flags(m: &str) -> u8 {
        let x = mem(m);
        (x.ser as u8) | if x.cs.is_some() { 2 } else { 0 }
    }

    fn send_noreply(&mut self, op: u8, payload: &[u8]) -> ToolResult<()> {
        self.port.reset_input_buffer()?;
        let frame = rm::packet(op, payload);
        self.port.write(&frame)?;
        self.port.flush()?;
        self.clock.sleep(0.05 + frame.len() as f64 * 10.0 / self.rate());
        Ok(())
    }

    /// Soft-resets the SoC (restart-cause mark, watchdog off, quad off, divider, SYS_SOFT_RSTN).
    pub fn reset_chip(&mut self) -> ToolResult<()> {
        self.poke(CRASH_SOFT_MARK, &le32_bytes(CRASH_BOOT_SOFT))?;
        self.poke(WDT_LOCK, &le32_bytes(WDT_KEY))?;
        self.poke(WDT_EN, &le32_bytes(0))?;
        self.poke(WDT_LOCK, &le32_bytes(0))?;
        self.poke(QUAD_ENABLE, &le32_bytes(0))?;
        self.poke(LCSFC_BAUDR, &le32_bytes(4))?;
        let mut p = be32_bytes(SYS_SOFT_RSTN).to_vec();
        p.extend_from_slice(&le32_bytes(0));
        self.send_noreply(0x03, &p)
    }

    pub fn peek(&mut self, addr: u64, n: usize) -> ToolResult<Vec<u8>> {
        let mut p = be32_bytes(addr).to_vec();
        p.extend_from_slice(&[(n >> 8) as u8, n as u8]);
        self.request(0x02, &p, None, 1.0)
    }

    pub fn poke(&mut self, addr: u64, data: &[u8]) -> ToolResult<()> {
        let mut p = be32_bytes(addr).to_vec();
        p.extend_from_slice(data);
        self.request(0x03, &p, None, 1.0).map(|_| ())
    }

    pub fn spi(
        &mut self,
        b: u64,
        tx: &[u8],
        rx_len: usize,
        ser: bool,
        cs: Option<u8>,
        retries: Option<u32>,
    ) -> ToolResult<Vec<u8>> {
        let flags = (ser as u8) | if cs.is_some() { 2 } else { 0 };
        let mut payload = be32_bytes(b).to_vec();
        payload.extend_from_slice(&[
            flags,
            cs.unwrap_or(0),
            (tx.len() >> 8) as u8,
            tx.len() as u8,
            (rx_len >> 8) as u8,
            rx_len as u8,
        ]);
        payload.extend_from_slice(tx);
        let r = self.request(0x05, &payload, retries, 1.0)?;
        if r.len() != rx_len {
            return Err(ToolError::dump(format!("SPI reply is {} bytes, wanted {rx_len}", r.len())));
        }
        Ok(r)
    }

    pub fn jedec(&mut self, m: &str) -> ToolResult<Vec<u8>> {
        let x = mem(m);
        self.spi(x.base, &[0x9F], 3, x.ser, x.cs, None)
    }

    pub fn read_flash(&mut self, m: &str, addr: u64, n: usize) -> ToolResult<Vec<u8>> {
        let x = mem(m);
        self.spi(x.base, &[0x03, (addr >> 16) as u8, (addr >> 8) as u8, addr as u8], n, x.ser, x.cs, None)
    }

    fn status_reg(&mut self, m: &str) -> ToolResult<u8> {
        let x = mem(m);
        Ok(self.spi(x.base, &[0x05], 1, x.ser, x.cs, None)?[0])
    }

    fn wait_ready(&mut self, m: &str, what: &str, timeout: f64) -> ToolResult<u8> {
        let end = self.clock.monotonic() + timeout;
        loop {
            let st = self.status_reg(m)?;
            if st & 1 == 0 {
                return Ok(st);
            }
            if self.clock.monotonic() > end {
                return Err(ToolError::dump(format!(
                    "{what}: flash still busy after {timeout:.1} s (status 0x{st:02X})"
                )));
            }
        }
    }

    /// Write enable, the command, busy polls (generic SPI: 06, 05, 20 / 02).
    pub fn write_cmd(&mut self, m: &str, tx: &[u8], what: &str, timeout: f64) -> ToolResult<()> {
        self.guard(m, tx)?;
        let x = mem(m);
        self.spi(x.base, &[0x06], 0, x.ser, x.cs, None)?;
        if self.status_reg(m)? & 2 == 0 {
            let st = self.status_reg(m)?;
            return Err(ToolError::dump(format!(
                "{what}: write enable did not stick (status 0x{st:02X}): chip write-protected?"
            )));
        }
        self.spi(x.base, tx, 0, x.ser, x.cs, None)?;
        self.wait_ready(m, what, timeout)?;
        Ok(())
    }

    /// The last check before a flash command goes out: never a protected region of the board.
    fn guard(&self, m: &str, tx: &[u8]) -> ToolResult<()> {
        let op = tx[0];
        let pm = protect_mem(m);
        if op == 0x60 || op == 0xC7 {
            let hit: Vec<_> = self.profile.protect.iter().filter(|p| p.0 == pm).collect();
            if !hit.is_empty() {
                return Err(ToolError::dump(format!(
                    "refusing to erase the whole {}: it holds protected regions ({}). Nothing was erased",
                    protect_name(pm),
                    describe(&hit)
                )));
            }
        } else if matches!(op, 0x20 | 0x52 | 0xD8 | 0x02) && tx.len() >= 4 {
            let mut a = (tx[1] as u64) << 16 | (tx[2] as u64) << 8 | tx[3] as u64;
            let n = match op {
                0x20 => SECTOR,
                0x52 => 0x8000,
                0xD8 => 0x10000,
                _ => (tx.len() - 4) as u64,
            };
            if op != 0x02 {
                a -= a % n;
            }
            let (start, end) = (a, a + n.max(1));
            let hit: Vec<_> = self.profile.protect.iter().filter(|p| p.0 == pm && p.1 < end && start < p.2).collect();
            if !hit.is_empty() {
                return Err(ToolError::dump(format!(
                    "refusing to {} {} 0x{start:X}-0x{end:X}: it overlaps a protected region ({}). Nothing was written",
                    if op != 0x02 { "erase" } else { "program" },
                    protect_name(pm),
                    describe(&hit)
                )));
            }
        }
        Ok(())
    }

    /// [(crc32, blank)] for count sectors from addr, computed by the radio.
    pub fn sums(&mut self, m: &str, addr: u64, count: usize) -> ToolResult<Vec<(u32, bool)>> {
        let x = mem(m);
        let mut payload = be32_bytes(x.base).to_vec();
        payload.extend_from_slice(&[Self::mem_flags(m), x.cs.unwrap_or(0)]);
        payload.extend_from_slice(&be32_bytes(addr));
        payload.extend_from_slice(&[(count >> 8) as u8, count as u8]);
        let r = self.request(0x07, &payload, None, 1.0 + count as f64 * 0.15)?;
        if r.len() != count * 5 {
            return Err(ToolError::dump(format!("sector sums reply is {} bytes, wanted {}", r.len(), count * 5)));
        }
        Ok((0..count).map(|i| (be32(&r[i * 5..]), r[i * 5 + 4] == 1)).collect())
    }

    /// Tick rate of timer 2 (it counts down), against this computer's clock.
    pub fn timer_hz(&mut self, span: f64) -> ToolResult<f64> {
        let t0 = self.clock.monotonic();
        let v0 = le32(&self.peek(TIMER2_VALUE, 4)?) as i64;
        self.clock.sleep(span);
        let t1 = self.clock.monotonic();
        let v1 = le32(&self.peek(TIMER2_VALUE, 4)?) as i64;
        Ok(((v0 - v1) & 0xFFFF_FFFF) as f64 / (t1 - t0))
    }

    fn read_frame(&mut self, timeout: f64) -> ToolResult<(u8, Vec<u8>)> {
        let hdr = self.read_exact(6, timeout)?;
        if hdr.len() != 6 || hdr[0] != 0x02 || hdr[5] != 0x03 {
            return Err(ToolError::frame("no or malformed frame header"));
        }
        let n = (hdr[2] as usize) << 8 | hdr[3] as usize;
        let body = self.read_exact(n, 0.5 + n as f64 * 0.0002)?;
        if body.len() != n {
            return Err(ToolError::frame(format!("frame short ({} of {n} bytes)", body.len())));
        }
        if sum8(&body) != hdr[4] {
            return Err(ToolError::frame("frame checksum"));
        }
        Ok((hdr[1], body))
    }

    /// Reads and drops what the stub still streams, until the line was quiet for `quiet` seconds.
    pub fn drain(&mut self, quiet: f64) -> ToolResult<()> {
        self.set_timeout(0.05);
        let mut last = self.clock.monotonic();
        while self.clock.monotonic() - last < quiet {
            if !self.port.read(4096)?.is_empty() {
                last = self.clock.monotonic();
            }
        }
        self.port.reset_input_buffer()
    }

    /// Asks for count sectors from addr; each record is (crc32, method, data): method 1 LZ4, 0 raw, 2 blank.
    /// The records are read one by one through `on_record` (a generator in Python: a refused record stops the reads).
    fn sectors(
        &mut self,
        m: &str,
        addr: u64,
        count: usize,
        compress: bool,
        on_record: &mut SectorRecord<'_, Self>,
    ) -> ToolResult<()> {
        let x = mem(m);
        let mut payload = be32_bytes(x.base).to_vec();
        payload.extend_from_slice(&[Self::mem_flags(m), x.cs.unwrap_or(0)]);
        payload.extend_from_slice(&be32_bytes(addr));
        payload.extend_from_slice(&[count as u8, compress as u8]);
        self.port.reset_input_buffer()?;
        let mut frame = vec![0x02, 0x09, 0, payload.len() as u8, sum8(&payload), 0x03];
        frame.extend_from_slice(&payload);
        self.port.write(&frame)?;
        for i in 0..count {
            let (status, r) = self.read_frame(if i == 0 { 3.0 } else { 1.5 })?;
            if status != OK {
                if status == BAD_OP || status == TOO_LONG {
                    return Err(ToolError::dump(format!("stub op 0x09 refused: status 0x{status:02X}")));
                }
                return Err(ToolError::frame(format!("stub op 0x09: status 0x{status:02X}")));
            }
            if r.len() < 7 || r.len() != 7 + ((r[5] as usize) << 8 | r[6] as usize) {
                return Err(ToolError::frame("sector record malformed"));
            }
            on_record(self, be32(&r), r[4], r[7..].to_vec())?;
        }
        Ok(())
    }

    /// The sector's 4096 bytes when they match the radio's CRC-32, else None.
    fn unpack(crc: u32, method: u8, data: Vec<u8>) -> Option<Vec<u8>> {
        let sec = match method {
            2 => vec![0xFFu8; SECTOR as usize],
            1 => lz4_block(&data, SECTOR as usize)?,
            _ => data,
        };
        (sec.len() == SECTOR as usize && crc32(&sec, 0) == crc).then_some(sec)
    }

    pub fn check_id(&mut self, m: &str) -> ToolResult<()> {
        let id = self.jedec(m)?;
        let x = mem(m);
        self.log(&format!("{} JEDEC ID {}", x.name, hex::encode(&id)));
        if id.iter().all(|&b| b == 0xFF) || id.iter().all(|&b| b == 0) {
            return Err(ToolError::dump(format!(
                "{} does not answer the ID command ({}): chip select or pin mux wrong, nothing was dumped",
                x.name,
                hex::encode(&id)
            )));
        }
        Ok(())
    }

    fn windows(&self, m: &str, start: u64, end: u64) -> ToolResult<Vec<(u64, usize)>> {
        let size = self.mem_size(m);
        if !(start < end && end <= size) {
            return Err(ToolError::dump(format!("{m} range must lie within 0..0x{size:X}")));
        }
        let mut out = vec![];
        let mut w = start / SECTOR * SECTOR;
        while w < end {
            out.push((w, WINDOW.min((end - w).div_ceil(SECTOR)) as usize));
            w += WINDOW * SECTOR;
        }
        Ok(out)
    }

    /// Reads [start, end) of m into out: every sector checked against the radio's CRC-32, read again when it does not
    /// match (uncompressed from the second try), and checked once more against the sector sums at the end.
    pub fn dump(
        &mut self,
        out: &mut dyn DumpSink,
        m: &str,
        start: u64,
        end: u64,
        mut progress: Progress<'_>,
        compress: bool,
    ) -> ToolResult<()> {
        self.check_id(m)?;
        let size = self.mem_size(m);
        if !(start < end && end <= size) {
            return Err(ToolError::dump(format!("{m} range must lie within 0..0x{size:X}")));
        }
        out.set_len(end - start)?;
        let (mut total, mut fails, mut skipped) = (0u64, 0u32, 0u64);
        let mut sa = start / SECTOR * SECTOR;
        let lo = sa;
        let mut full = vec![0u8; (end.div_ceil(SECTOR) * SECTOR - lo) as usize];
        let mut want = compress;
        while sa < end {
            let count = PACK.min((end - sa).div_ceil(SECTOR)) as usize;
            let mut cursor = sa;
            let r = self.sectors(m, sa, count, want, &mut |me: &mut Self, crc, method, data| {
                let sec = Self::unpack(crc, method, data)
                    .ok_or_else(|| ToolError::frame(format!("0x{cursor:X}: sector does not match the radio's CRC")))?;
                if method == 2 {
                    skipped += 1;
                }
                put(out, &mut full, lo, start, end, cursor, &sec)?;
                cursor += SECTOR;
                total += 1;
                fails = 0;
                me.link_score = (me.link_score - 1).max(0);
                want = compress;
                if total % 64 == 0 {
                    if let Some(p) = progress.as_mut() {
                        p(cursor, end);
                    }
                }
                Ok(())
            });
            sa = cursor;
            if let Err(e) = r {
                if !e.is_frame() {
                    return Err(e);
                }
                fails += 1;
                self.retried += 1;
                self.drain(0.4)?;
                if fails > self.retries {
                    return Err(ToolError::dump(format!(
                        "0x{sa:X}: no clean read after {fails} tries ({})",
                        e.message
                    )));
                }
                self.log(&format!("0x{sa:X}: {}, resuming here ({fails} of {})", e.message, self.retries));
                want = compress && fails < 2;
            }
        }
        for attempt in 0..self.retries + 1 {
            let bad = self.verify(m, &full, lo)?;
            if bad.is_empty() {
                break;
            }
            if attempt == self.retries {
                return Err(ToolError::dump(format!(
                    "0x{:X}: the read does not match the radio's sector CRC after {} tries",
                    bad[0],
                    attempt + 1
                )));
            }
            self.log(&format!("{} sector(s) read wrong (first 0x{:X}), reading them again", bad.len(), bad[0]));
            self.retried += bad.len() as u64;
            for &s in &bad {
                let r = self.sectors(m, s, 1, false, &mut |_me: &mut Self, crc, method, data| {
                    if let Some(sec) = Self::unpack(crc, method, data) {
                        put(out, &mut full, lo, start, end, s, &sec)?;
                    }
                    Ok(())
                });
                if let Err(e) = r {
                    if !e.is_frame() {
                        return Err(e);
                    }
                    self.log(&format!("0x{s:X}: {}", e.message));
                    self.drain(0.4)?;
                }
            }
        }
        out.flush_sink()?;
        if total == skipped {
            self.log("warning: every sector was blank (0xFF)");
        }
        Ok(())
    }

    /// Sector addresses where the flash differs from data, by the radio's CRC-32s.
    pub fn verify(&mut self, m: &str, data: &[u8], start: u64) -> ToolResult<Vec<u64>> {
        if !start.is_multiple_of(SECTOR) || !(data.len() as u64).is_multiple_of(SECTOR) {
            return Err(ToolError::dump("verify needs whole 4 KB sectors"));
        }
        self.check_id(m)?;
        let mut bad = vec![];
        for (w, count) in self.windows(m, start, start + data.len() as u64)? {
            let sums = self.sums(m, w, count)?;
            for (i, s) in sums.iter().enumerate() {
                let off = (w + i as u64 * SECTOR - start) as usize;
                if crc32(&data[off..off + SECTOR as usize], 0) != s.0 {
                    bad.push(w + i as u64 * SECTOR);
                }
            }
        }
        Ok(bad)
    }
}

fn put(out: &mut dyn DumpSink, full: &mut [u8], lo: u64, start: u64, end: u64, sa: u64, sec: &[u8]) -> ToolResult<()> {
    let o = (sa - lo) as usize;
    full[o..o + sec.len()].copy_from_slice(sec);
    let a = start.max(sa);
    let b = end.min(sa + SECTOR);
    out.write_at(a - start, &sec[(a - sa) as usize..(b - sa) as usize])?;
    Ok(())
}

fn protect_name(pm: &str) -> &str {
    match pm {
        "progmem" => "program flash",
        "data" => "data flash",
        other => other,
    }
}

fn describe(hit: &[&(&str, u64, u64, &str)]) -> String {
    hit.iter().map(|p| format!("{} 0x{:X}-0x{:X}, {}", protect_name(p.0), p.1, p.2, p.3)).collect::<Vec<_>>().join("; ")
}

fn check_univ_load(image: &[u8], load: u64) -> ToolResult<()> {
    if image.len() < 0xC0 || &image[0x80..0x84] != b"UCFG" {
        return Err(ToolError::dump("this is not a universal stub image (no parameter block at offset 0x80)"));
    }
    let run = le32(&image[0xB8..]) as u64;
    let end = le32(&image[0xBC..]) as u64;
    let n = image.len() as u64;
    if !(SRAM.0 <= run && run + n <= end && end <= SRAM.1) {
        return Err(ToolError::dump(format!("the stub's RAM 0x{run:X}-0x{end:X} is not inside SRAM")));
    }
    if !load.is_multiple_of(4) || !(SRAM.0 <= load && load + n <= SRAM.1) {
        return Err(ToolError::dump(format!(
            "load address 0x{load:X}: the {n}-byte stub must land 4-byte aligned inside SRAM"
        )));
    }
    if load != run && load < run + n && run < load + n {
        return Err(ToolError::dump(format!("load address 0x{load:X} overlaps the stub's run address 0x{run:X}")));
    }
    Ok(())
}

/// Python's repr() of bytes.
pub fn py_bytes_repr(b: &[u8]) -> String {
    let mut s = String::from("b'");
    for &x in b {
        match x {
            b'\\' => s.push_str("\\\\"),
            b'\'' => s.push_str("\\'"),
            b'\t' => s.push_str("\\t"),
            b'\n' => s.push_str("\\n"),
            b'\r' => s.push_str("\\r"),
            x if !(0x20..0x7f).contains(&x) => s.push_str(&format!("\\x{x:02x}")),
            x => s.push(x as char),
        }
    }
    s.push('\'');
    s
}

/// The V6 footprint as the receipt's Python dict.
pub fn v6_py(v: &Option<V6>) -> Py {
    match v {
        Some(v) => Py::Dict(v.to_py()),
        None => Py::Null,
    }
}
