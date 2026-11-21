// SPDX-License-Identifier: GPL-3.0-or-later
//! What the monitor protocol needs from a serial port (pyserial's subset the Python tools use), the clocks the tools
//! wait by, and the switch in front of every real serial port (Radio/Ports.cs, SerialRadioPort.cs).
//!
//! A real port exists only when the crate is built with the `radio` feature (OFF by default). Without it
//! [`RadioAccess::factory`] and [`RadioAccess::write_factory`] refuse every port, and no type in the crate can open
//! one. With it, every guard (ReadOnlyPort, InstallPort, CpsPackPort) still sits between the tool and the port, and
//! each radio step starts only from its own button (one click = one step).

use super::error::{ToolError, ToolResult};
use std::cell::Cell;

/// What the monitor protocol needs from a serial port.
pub trait RadioPort {
    /// Read timeout in seconds.
    fn timeout(&self) -> f64;
    fn set_timeout(&mut self, t: f64);
    fn baud_rate(&self) -> u32;
    fn set_baud_rate(&mut self, baud: u32) -> ToolResult<()>;
    /// Sends the bytes; returns how many went out (or None when the port does not say).
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>>;
    /// What arrived within the timeout, at most n bytes; empty when nothing came.
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>>;
    fn reset_input_buffer(&mut self) -> ToolResult<()>;
    fn flush(&mut self) -> ToolResult<()>;
}

/// The serial port itself: opened and closed by the tool.
pub trait RawSerial: RadioPort {
    fn open(&mut self) -> ToolResult<()>;
    fn close(&mut self) -> ToolResult<()>;
}

/// Opens the raw port for a tool; the only way a tool reaches a serial port.
pub type SerialFactory<'a> = &'a dyn Fn(&str) -> ToolResult<Box<dyn RawSerial>>;

/// The time the tools wait by: real for a radio, virtual for the byte-for-byte tests.
pub trait Clock {
    /// time.monotonic()
    fn monotonic(&self) -> f64;
    /// time.sleep()
    fn sleep(&self, seconds: f64);
    /// time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    fn utc_stamp(&self) -> String;
}

pub struct SystemClock {
    start: std::time::Instant,
}

impl Default for SystemClock {
    fn default() -> Self {
        Self { start: std::time::Instant::now() }
    }
}

impl Clock for SystemClock {
    fn monotonic(&self) -> f64 {
        self.start.elapsed().as_secs_f64()
    }
    fn sleep(&self, seconds: f64) {
        if seconds > 0.0 {
            std::thread::sleep(std::time::Duration::from_secs_f64(seconds));
        }
    }
    fn utc_stamp(&self) -> String {
        crate::releases::client::now_utc()
    }
}

/// The virtual clock of the byte-for-byte tests (the same as make_radio_fixtures.py): every reading advances 1 ms,
/// a sleep advances by its length, and every UTC stamp is the same.
pub struct VirtualClock {
    t: Cell<f64>,
    stamp: String,
}

impl VirtualClock {
    pub fn new(stamp: &str) -> Self {
        Self { t: Cell::new(1000.0), stamp: stamp.into() }
    }
    pub fn t(&self) -> f64 {
        self.t.get()
    }
}

impl Default for VirtualClock {
    fn default() -> Self {
        Self::new("2026-11-20T00:00:00Z")
    }
}

impl Clock for VirtualClock {
    fn monotonic(&self) -> f64 {
        self.t.set(self.t.get() + 0.001);
        self.t.get()
    }
    fn sleep(&self, seconds: f64) {
        self.t.set(self.t.get() + seconds.max(0.0));
    }
    fn utc_stamp(&self) -> String {
        self.stamp.clone()
    }
}

/// The switch in front of every serial port: a tool gets a port only through [`RadioAccess::factory`] (read-only
/// tools) or [`RadioAccess::write_factory`] (the tools that write). Both are compile-time switches (the `radio`
/// feature); nothing at run time can turn either on.
pub struct RadioAccess;

impl RadioAccess {
    /// A real serial port is compiled in.
    pub const ENABLED: bool = cfg!(feature = "radio");
    /// The writing tools get a port (the same switch: a radio build installs, as the C# release build does).
    pub const WRITES_ENABLED: bool = cfg!(feature = "radio");

    /// The serial port factory for the read-only tools.
    pub fn factory(name: &str) -> ToolResult<Box<dyn RawSerial>> {
        open_real(name)
    }

    /// The serial port factory for the tools that write (install, quick update, install without backup, restore,
    /// data packs).
    pub fn write_factory(name: &str) -> ToolResult<Box<dyn RawSerial>> {
        if !Self::WRITES_ENABLED {
            return Err(ToolError::radio_disabled());
        }
        open_real(name)
    }
}

#[cfg(feature = "radio")]
fn open_real(name: &str) -> ToolResult<Box<dyn RawSerial>> {
    Ok(Box::new(serial::SerialRadioPort::new(name)))
}

#[cfg(not(feature = "radio"))]
fn open_real(_name: &str) -> ToolResult<Box<dyn RawSerial>> {
    Err(ToolError::radio_disabled())
}

/// A tool run whose caller gave no port factory: every port is refused (in every build).
pub fn no_port(_name: &str) -> ToolResult<Box<dyn RawSerial>> {
    Err(ToolError::no_port_given())
}

#[cfg(feature = "radio")]
pub mod serial {
    //! A real serial port with pyserial's behaviour as the Python tools use it: 115200 8N1, DTR and RTS low,
    //! read(n) returns what arrived within the timeout. Created only by [`super::RadioAccess`].
    use super::*;
    use std::io::{Read, Write};
    use std::time::{Duration, Instant};

    pub struct SerialRadioPort {
        name: String,
        baud: u32,
        timeout: f64,
        port: Option<Box<dyn serialport::SerialPort>>,
    }

    impl SerialRadioPort {
        pub(crate) fn new(name: &str) -> Self {
            Self { name: name.into(), baud: 115200, timeout: 0.02, port: None }
        }

        fn port(&mut self) -> ToolResult<&mut Box<dyn serialport::SerialPort>> {
            self.port.as_mut().ok_or_else(|| ToolError::os("the serial port is not open"))
        }
    }

    fn os(e: impl std::fmt::Display) -> ToolError {
        ToolError::os(e.to_string())
    }

    impl RadioPort for SerialRadioPort {
        fn timeout(&self) -> f64 {
            self.timeout
        }
        fn set_timeout(&mut self, t: f64) {
            self.timeout = t;
        }
        fn baud_rate(&self) -> u32 {
            self.baud
        }
        fn set_baud_rate(&mut self, baud: u32) -> ToolResult<()> {
            self.baud = baud;
            if let Some(p) = self.port.as_mut() {
                p.set_baud_rate(baud).map_err(os)?;
            }
            Ok(())
        }
        fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
            let p = self.port()?;
            p.write_all(data).map_err(os)?;
            Ok(Some(data.len()))
        }
        fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
            let timeout = self.timeout;
            let p = self.port()?;
            let mut buf = vec![0u8; n];
            let mut got = 0;
            let end = Instant::now() + Duration::from_secs_f64(timeout);
            while got < n {
                let avail = p.bytes_to_read().map_err(os)? as usize;
                if avail > 0 {
                    let k = avail.min(n - got);
                    match p.read(&mut buf[got..got + k]) {
                        Ok(r) => got += r,
                        Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                        Err(e) => return Err(os(e)),
                    }
                    continue;
                }
                if Instant::now() >= end {
                    break;
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            buf.truncate(got);
            Ok(buf)
        }
        fn reset_input_buffer(&mut self) -> ToolResult<()> {
            self.port()?.clear(serialport::ClearBuffer::Input).map_err(os)
        }
        fn flush(&mut self) -> ToolResult<()> {
            self.port()?.flush().map_err(os)
        }
    }

    impl RawSerial for SerialRadioPort {
        fn open(&mut self) -> ToolResult<()> {
            let mut p = serialport::new(&self.name, self.baud)
                .data_bits(serialport::DataBits::Eight)
                .parity(serialport::Parity::None)
                .stop_bits(serialport::StopBits::One)
                .flow_control(serialport::FlowControl::None)
                .timeout(Duration::from_millis(20))
                .open()
                .map_err(os)?;
            p.write_data_terminal_ready(false).map_err(os)?;
            p.write_request_to_send(false).map_err(os)?;
            self.port = Some(p);
            Ok(())
        }
        fn close(&mut self) -> ToolResult<()> {
            self.port = None;
            Ok(())
        }
    }
}
