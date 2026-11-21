// SPDX-License-Identifier: GPL-3.0-or-later
//! An error of a radio tool, named like the Python exception the same failure raises in pc/radiotools (DumpError,
//! ValueError, CodecError, ...): receipts and backup manifests record "Type: message", and the tools must write the
//! same bytes as the Python tools (win/docs/porting-plan.md).

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolError {
    /// The Python exception's name.
    pub py_type: String,
    pub message: String,
    /// StatusError: the stub's status byte.
    pub status: Option<u8>,
}

pub type ToolResult<T> = Result<T, ToolError>;

impl ToolError {
    pub fn new(py_type: &str, message: impl Into<String>) -> Self {
        ToolError { py_type: py_type.into(), message: message.into(), status: None }
    }
    /// ValueError
    pub fn value(message: impl Into<String>) -> Self {
        Self::new("ValueError", message)
    }
    /// prepare_unit_image.CodecError (a ValueError)
    pub fn codec(message: impl Into<String>) -> Self {
        Self::new("CodecError", message)
    }
    /// The pinned monitor host's DumpError
    pub fn dump(message: impl Into<String>) -> Self {
        Self::new("DumpError", message)
    }
    /// A damaged, short or missing frame, or a sector that failed its CRC: worth asking again (a DumpError).
    pub fn frame(message: impl Into<String>) -> Self {
        Self::new("FrameError", message)
    }
    /// The stub answered with a status other than OK (a DumpError).
    pub fn status(message: impl Into<String>, status: u8) -> Self {
        ToolError { py_type: "StatusError".into(), message: message.into(), status: Some(status) }
    }
    /// FileExistsError: an output that exists already is never replaced.
    pub fn file_exists(path: impl Into<String>) -> Self {
        Self::new("FileExistsError", path)
    }
    /// argparse's error or a SystemExit with a message: the tool stops before doing anything.
    pub fn exit(code: impl Into<String>) -> Self {
        Self::new("SystemExit", code)
    }
    pub fn os(message: impl Into<String>) -> Self {
        Self::new("OSError", message)
    }
    pub fn key(key: &str) -> Self {
        Self::new("KeyError", format!("'{key}'"))
    }
    /// No serial port factory was given to this tool run (never reaches a radio, in any build).
    pub fn no_port_given() -> Self {
        Self::new("NoPortGiven", "No serial port factory was given to this tool run")
    }
    /// Radio access is compiled out of this build.
    pub fn radio_disabled() -> Self {
        Self::new(
            "RadioDisabled",
            "Radio access is switched off in this build: no serial port is opened (feature `radio` off)",
        )
    }

    /// DumpError or one of its kinds (FrameError, StatusError): what `except DumpError` catches.
    pub fn is_dump(&self) -> bool {
        matches!(self.py_type.as_str(), "DumpError" | "FrameError" | "StatusError")
    }
    pub fn is_frame(&self) -> bool {
        self.py_type == "FrameError"
    }
    /// ValueError or a subclass (CodecError, ManifestError).
    pub fn is_value(&self) -> bool {
        matches!(self.py_type.as_str(), "ValueError" | "CodecError" | "ManifestError")
    }

    /// `f'{type(exc).__name__}: {exc}'`
    pub fn record(&self) -> String {
        format!("{}: {}", self.py_type, self.message)
    }
}

impl fmt::Display for ToolError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for ToolError {}

impl From<std::io::Error> for ToolError {
    fn from(e: std::io::Error) -> Self {
        let t = match e.kind() {
            std::io::ErrorKind::NotFound => "FileNotFoundError",
            std::io::ErrorKind::AlreadyExists => "FileExistsError",
            std::io::ErrorKind::PermissionDenied => "PermissionError",
            _ => "OSError",
        };
        ToolError::new(t, e.to_string())
    }
}
