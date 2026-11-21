// SPDX-License-Identifier: GPL-3.0-or-later
//! The transport guards (dm32_readonly_backup.ReadOnlyPort, install_firstboot.InstallPort; Radio/Guards.cs): every
//! frame that goes out is checked first, and a refused frame never reaches the port.

use super::error::{ToolError, ToolResult};
use super::monitor::{self, be32, be32_bytes, le32_bytes, ram_monitor as rm, sha, sum8};
use super::port::{RadioPort, RawSerial};
use crate::pyjson::PyDict;
use std::collections::{HashMap, HashSet};

/// The backup's transport guard. Before the monitor runs only the ROM greeting reply, the exact pinned image blocks
/// and its start; then only HELLO, the timer read, the DM32 configuration, JEDEC reads, bounded flash reads and
/// sector CRCs / reads inside the identified flash. No write, erase, reboot, GPIO, baud change or RF command exists
/// in it; the baud rate is fixed.
pub struct ReadOnlyPort {
    pub raw: Box<dyn RawSerial>,
    blocks: HashSet<Vec<u8>>,
    sent: HashSet<Vec<u8>>,
    go: Vec<u8>,
    pub monitor: bool,
    pub limits: HashMap<&'static str, u64>,
    pub counts: PyDict,
}

impl ReadOnlyPort {
    pub fn new(raw: Box<dyn RawSerial>) -> ToolResult<ReadOnlyPort> {
        let image = rm::image()?;
        if sha(&image) != rm::SHA256 {
            return Err(ToolError::value("RAM image changed"));
        }
        let mut padded = vec![0xFFu8; image.len().div_ceil(256) * 256];
        padded[..image.len()].copy_from_slice(&image);
        let mut blocks = HashSet::new();
        for i in (0..padded.len()).step_by(256) {
            let mut p = be32_bytes(0x10000 + i as u64).to_vec();
            p.extend_from_slice(&be32_bytes(64));
            p.extend_from_slice(&padded[i..i + 256]);
            blocks.insert(rm::rom_cmd(0x18, &p));
        }
        Ok(ReadOnlyPort {
            raw,
            blocks,
            sent: HashSet::new(),
            go: rm::rom_cmd(0x1b, &be32_bytes(0x10000)),
            monitor: false,
            limits: HashMap::from([("progmem", 0), ("q128", 0)]),
            counts: PyDict::new(),
        })
    }

    pub fn memory(payload: &[u8]) -> ToolResult<&'static str> {
        if payload.len() < 6 {
            return Err(ToolError::value("Short memory selector"));
        }
        let (b, flags, cs) = (be32(payload), payload[4], payload[5]);
        if b == 0x0400_0000 && flags == 1 && cs == 0 {
            return Ok("progmem");
        }
        if b == 0x140a_0000 && flags == 3 && cs == 18 {
            return Ok("q128");
        }
        Err(ToolError::value("Unsupported SPI controller/chip select"))
    }

    fn bounds(&self, m: &str, address: i64, length: i64) -> ToolResult<()> {
        if length <= 0 || address < 0 || address + length > self.limits[m] as i64 {
            return Err(ToolError::value("Read outside identified flash capacity"));
        }
        Ok(())
    }

    pub fn validate(&mut self, data: &[u8]) -> ToolResult<&'static str> {
        if !self.monitor {
            if data == rm::ROM_REPLY {
                return Ok("rom_greeting");
            }
            if self.blocks.contains(data) {
                self.sent.insert(data.to_vec());
                return Ok("ram_download");
            }
            if data == self.go.as_slice() && self.sent == self.blocks {
                self.monitor = true;
                return Ok("ram_execute");
            }
            return Err(ToolError::value("Only the exact pinned RAM image may be downloaded/executed"));
        }
        if data.len() < 6 || data[0] != 2 || data[5] != 3 {
            return Err(ToolError::value("Malformed monitor frame"));
        }
        let op = data[1];
        let n = (data[2] as usize) << 8 | data[3] as usize;
        let payload = &data[6..];
        if n != payload.len() || data[4] != sum8(payload) {
            return Err(ToolError::value("Monitor frame length/checksum mismatch"));
        }
        if op == 1 && payload.is_empty() {
            return Ok("hello");
        }
        if op == 2 && payload == [0x14, 0x00, 0x00, 0x18, 0x00, 0x04] {
            return Ok("timer_read");
        }
        if op == 0x0d && payload.len() == 16 {
            let (a, b) = (be32(payload), be32(&payload[4..]));
            if a == b && a > 1_000_000 && a < 400_000_000 && payload[8..] == [255, 255, 33, 32, 1, 0, 3, 0] {
                return Ok("dm32_config");
            }
        }
        if op == 5 && payload.len() >= 10 {
            let m = Self::memory(payload)?;
            let tx_len = (payload[6] as usize) << 8 | payload[7] as usize;
            let rx_len = (payload[8] as i64) << 8 | payload[9] as i64;
            let tx = &payload[10..];
            if tx_len == tx.len() {
                if tx.len() == 1 && tx[0] == 0x9f && rx_len == 3 {
                    return Ok("jedec_read");
                }
                if tx.len() == 4 && tx[0] == 3 && (1..=1024).contains(&rx_len) {
                    self.bounds(m, (tx[1] as i64) << 16 | (tx[2] as i64) << 8 | tx[3] as i64, rx_len)?;
                    return Ok("flash_read");
                }
            }
        }
        if (op == 7 || op == 9) && payload.len() == 12 {
            let m = Self::memory(payload)?;
            let address = be32(&payload[6..]) as i64;
            let count = if op == 7 { (payload[10] as i64) << 8 | payload[11] as i64 } else { payload[10] as i64 };
            if address % 4096 == 0 && (1..=16).contains(&count) && (op == 7 || payload[11] == 0 || payload[11] == 1) {
                self.bounds(m, address, count * 4096)?;
                return Ok(if op == 7 { "sector_crc" } else { "sector_read" });
            }
        }
        Err(ToolError::value(format!("Monitor operation 0x{op:x} refused by read-only transport")))
    }

    fn count(&mut self, kind: &str) {
        let n = self.counts.get(kind).and_then(|v| v.as_i64()).unwrap_or(0);
        self.counts.set(kind, n + 1);
    }

    fn checked_write(&mut self, kind: &str, data: &[u8]) -> ToolResult<Option<usize>> {
        let result = self.raw.write(data)?;
        if let Some(r) = result {
            if r != data.len() {
                return Err(ToolError::os("Short serial write"));
            }
        }
        self.count(kind);
        Ok(result)
    }
}

impl RadioPort for ReadOnlyPort {
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
            return Err(ToolError::value("Backup transport is fixed at 115200 baud"));
        }
        self.raw.set_baud_rate(baud)
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        let kind = self.validate(data)?;
        self.checked_write(kind, data)
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

/// How writes get enabled: Full (the live program and data chip equal the backup: install_firstboot's own rule) or
/// Identity (the quick update over DDM32 Plus: chip IDs, boot area and codec slot equal the reused backup's; every
/// application sector is rewritten, the slot never).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Authorization {
    Full,
    Identity,
}

pub const START: usize = 0xC000;
pub const SIZE: usize = 0x100000;
pub const SECTOR: usize = 4096;
pub const SLOT: usize = 0xE8000;

/// The installer's guard: on top of the read-only rules, only the changed application sectors at 0xC000 and up may
/// be erased and programmed, with their exact planned page bytes, in order and once, and only after the live
/// pre-write comparison (or, restoring from the saved backup, after choosing that mode). The boot area and the data
/// chip are read only. The restart registers go out only after the readback matched. In saved-backup mode no
/// original firmware or data is read, and only already written sectors are read back.
pub struct InstallPort {
    pub ro: ReadOnlyPort,
    original: Vec<u8>,
    target: Vec<u8>,
    pub todo: Vec<usize>,
    pub plan: Vec<Vec<u8>>,
    pub index: usize,
    pub enabled: bool,
    pub closed_writes: bool,
    pub reset_plan: Vec<Vec<u8>>,
    reset_index: usize,
    pub skip_original: bool,
    pub kind: Authorization,
    pub has_images: bool,
    sector_ends: HashMap<usize, usize>,
}

impl InstallPort {
    pub fn new(
        raw: Box<dyn RawSerial>,
        original: &[u8],
        target: &[u8],
        skip_original: bool,
        kind: Authorization,
    ) -> ToolResult<InstallPort> {
        let mut p = Self::deferred(raw)?;
        p.skip_original = skip_original;
        p.kind = kind;
        p.set_images(original, target)?;
        Ok(p)
    }

    /// An install without a stored backup: the images are set once, after the single live read of the program flash;
    /// until then the port is read only.
    pub fn deferred(raw: Box<dyn RawSerial>) -> ToolResult<InstallPort> {
        Ok(InstallPort {
            ro: ReadOnlyPort::new(raw)?,
            original: vec![],
            target: vec![],
            todo: vec![],
            plan: vec![],
            index: 0,
            enabled: false,
            closed_writes: false,
            reset_plan: vec![],
            reset_index: 0,
            skip_original: false,
            kind: Authorization::Full,
            has_images: false,
            sector_ends: HashMap::new(),
        })
    }

    pub fn target(&self) -> &[u8] {
        &self.target
    }

    /// Fixes the write plan, once, before any write is enabled.
    pub fn set_images(&mut self, original: &[u8], target: &[u8]) -> ToolResult<()> {
        if self.has_images || self.enabled || self.closed_writes {
            return Err(ToolError::value("The write plan is fixed once, before any write"));
        }
        if original.len() != SIZE || target.len() != SIZE || original[..START] != target[..START] {
            return Err(ToolError::value("Exact 1 MiB images with identical protected boot area required"));
        }
        let todo: Vec<usize> =
            (START..SIZE).step_by(SECTOR).filter(|&a| original[a..a + SECTOR] != target[a..a + SECTOR]).collect();
        if self.kind == Authorization::Identity && todo.iter().any(|&a| a >= SLOT) {
            return Err(ToolError::value("A quick update never writes the codec slot"));
        }
        self.original = original.to_vec();
        self.target = target.to_vec();
        for &a in &todo {
            self.plan.push(rm::spi_packet(&[0x06], 0));
            self.plan.push(rm::spi_packet(&[0x20, (a >> 16) as u8, (a >> 8) as u8, a as u8], 0));
            for q in (a..a + SECTOR).step_by(256) {
                let page = &target[q..q + 256];
                if page.iter().all(|&b| b == 0xFF) {
                    continue;
                }
                self.plan.push(rm::spi_packet(&[0x06], 0));
                let mut tx = vec![0x02, (q >> 16) as u8, (q >> 8) as u8, q as u8];
                tx.extend_from_slice(page);
                self.plan.push(rm::spi_packet(&tx, 0));
            }
            self.sector_ends.insert(a, self.plan.len());
        }
        self.todo = todo;
        self.has_images = true;
        Ok(())
    }

    /// The quick update's live check: the boot area and the codec slot read from the radio now equal the reused
    /// backup's (the chip IDs were checked before). Only then are writes enabled.
    pub fn authorize_identity(&mut self, live_boot: &[u8], live_slot: &[u8]) -> ToolResult<()> {
        if self.kind != Authorization::Identity
            || !self.has_images
            || self.enabled
            || self.closed_writes
            || live_boot != &self.target[..START]
            || live_slot != &self.target[SLOT..]
        {
            return Err(ToolError::value(
                "Live identity check failed (boot area or codec slot differ from the backup); writes remain disabled",
            ));
        }
        self.enabled = true;
        Ok(())
    }

    pub fn authorize(&mut self, program: &[u8], data: &[u32], expected_data: &[u32]) -> ToolResult<()> {
        if self.kind != Authorization::Full
            || !self.has_images
            || self.enabled
            || self.closed_writes
            || program != self.original.as_slice()
            || data != expected_data
        {
            return Err(ToolError::value("Live pre-write comparison failed; writes remain disabled"));
        }
        self.enabled = true;
        Ok(())
    }

    pub fn authorize_saved_backup(&mut self) -> ToolResult<()> {
        if !self.skip_original || self.enabled || self.closed_writes {
            return Err(ToolError::value("Saved-backup-only installation was not selected"));
        }
        self.enabled = true;
        Ok(())
    }

    pub fn finish_writes(&mut self) -> ToolResult<()> {
        if !self.enabled || self.index != self.plan.len() {
            return Err(ToolError::value("Write plan incomplete"));
        }
        self.enabled = false;
        self.closed_writes = true;
        Ok(())
    }

    pub fn authorize_reset(&mut self, program: &[u8], data: &[u32], expected_data: &[u32]) -> ToolResult<()> {
        if !self.closed_writes || program != self.target.as_slice() || data != expected_data {
            return Err(ToolError::value("Post-write verification failed; reset remains disabled"));
        }
        self.allow_reset()
    }

    pub fn authorize_reset_application(&mut self, application: &[u8]) -> ToolResult<()> {
        if !self.skip_original || !self.closed_writes || application != &self.target[START..] {
            return Err(ToolError::value("Application readback failed; reset remains disabled"));
        }
        self.allow_reset()
    }

    fn allow_reset(&mut self) -> ToolResult<()> {
        if !self.reset_plan.is_empty() {
            return Err(ToolError::value("Restart was already authorized"));
        }
        let values = [
            (monitor::CRASH_SOFT_MARK, monitor::CRASH_BOOT_SOFT),
            (monitor::WDT_LOCK, monitor::WDT_KEY),
            (monitor::WDT_EN, 0),
            (monitor::WDT_LOCK, 0),
            (monitor::QUAD_ENABLE, 0),
            (monitor::LCSFC_BAUDR, 4),
            (monitor::SYS_SOFT_RSTN, 0),
        ];
        for (a, v) in values {
            let mut p = be32_bytes(a).to_vec();
            p.extend_from_slice(&le32_bytes(v));
            self.reset_plan.push(rm::packet(3, &p));
        }
        Ok(())
    }

    pub fn validate(&mut self, data: &[u8]) -> ToolResult<&'static str> {
        if self.ro.monitor {
            if self.skip_original {
                let op: i32 = if data.len() > 1 { data[1] as i32 } else { -1 };
                let payload: &[u8] = if data.len() > 6 { &data[6..] } else { &[] };
                if (op == 7 || op == 9)
                    && (!self.closed_writes
                        || payload.len() != 12
                        || ReadOnlyPort::memory(payload)? != "progmem"
                        || (be32(&payload[6..]) as usize) < START)
                {
                    return Err(ToolError::value("Original firmware/data reads are disabled in saved-backup mode"));
                }
                if op == 5 && payload.len() >= 14 && payload[10] == 3 {
                    let address = (payload[11] as usize) << 16 | (payload[12] as usize) << 8 | payload[13] as usize;
                    let n = (payload[8] as usize) << 8 | payload[9] as usize;
                    let first = address / SECTOR * SECTOR;
                    let end = (address + n).div_ceil(SECTOR) * SECTOR;
                    let mut all_written = true;
                    let mut a = first;
                    while a < end {
                        if !self.sector_ends.get(&a).is_some_and(|&e| self.index >= e) {
                            all_written = false;
                        }
                        a += SECTOR;
                    }
                    if ReadOnlyPort::memory(payload)? != "progmem"
                        || address < START
                        || (!self.closed_writes && !(self.enabled && all_written))
                    {
                        return Err(ToolError::value("Only already-written application bytes may be read back"));
                    }
                }
            }
            if data == rm::spi_packet(&[0x05], 1).as_slice() {
                return Ok("program_status_read");
            }
            if self.enabled && self.index < self.plan.len() && data == self.plan[self.index].as_slice() {
                self.index += 1;
                return Ok(match data[16] {
                    6 => "program_write_enable",
                    0x20 => "program_sector_erase",
                    _ => "program_page",
                });
            }
            if self.reset_index < self.reset_plan.len() && data == self.reset_plan[self.reset_index].as_slice() {
                self.reset_index += 1;
                return Ok("verified_restart_register");
            }
        }
        self.ro.validate(data)
    }
}

impl RadioPort for InstallPort {
    fn timeout(&self) -> f64 {
        self.ro.timeout()
    }
    fn set_timeout(&mut self, t: f64) {
        self.ro.set_timeout(t)
    }
    fn baud_rate(&self) -> u32 {
        self.ro.baud_rate()
    }
    fn set_baud_rate(&mut self, baud: u32) -> ToolResult<()> {
        self.ro.set_baud_rate(baud)
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        let kind = self.validate(data)?;
        self.ro.checked_write(kind, data)
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        self.ro.read(n)
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.ro.reset_input_buffer()
    }
    fn flush(&mut self) -> ToolResult<()> {
        self.ro.flush()
    }
}
