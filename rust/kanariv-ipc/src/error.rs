//! Error codes and parse errors mirroring `kanariv::utils::Error`.
//!
//! The wire carries both a symbolic code and a numeric value. Parsing prefers
//! the symbolic form and falls back to the numeric one.

use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StatusCode {
    Ok,
    InvalidArg,
    NotFound,
    Unsupported,
    Io,
    Device,
    State,
    NoMem,
    Internal,
    AlreadyExists,
    Busy,
    Corrupt,
    NoSpace,
    Timeout,
    /// Rust-side catch-all for codes newer than this crate.
    Unknown,
}

impl StatusCode {
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ok => "Ok",
            Self::InvalidArg => "InvalidArg",
            Self::NotFound => "NotFound",
            Self::Unsupported => "Unsupported",
            Self::Io => "Io",
            Self::Device => "Device",
            Self::State => "State",
            Self::NoMem => "NoMem",
            Self::Internal => "Internal",
            Self::AlreadyExists => "AlreadyExists",
            Self::Busy => "Busy",
            Self::Corrupt => "Corrupt",
            Self::NoSpace => "NoSpace",
            Self::Timeout => "Timeout",
            Self::Unknown => "Unknown",
        }
    }

    #[must_use]
    pub fn from_wire(name: &str) -> Self {
        match name {
            "Ok" => Self::Ok,
            "InvalidArg" => Self::InvalidArg,
            "NotFound" => Self::NotFound,
            "Unsupported" => Self::Unsupported,
            "Io" => Self::Io,
            "Device" => Self::Device,
            "State" => Self::State,
            "NoMem" => Self::NoMem,
            "Internal" => Self::Internal,
            "AlreadyExists" => Self::AlreadyExists,
            "Busy" => Self::Busy,
            "Corrupt" => Self::Corrupt,
            "NoSpace" => Self::NoSpace,
            "Timeout" => Self::Timeout,
            _ => Self::Unknown,
        }
    }

    #[must_use]
    pub fn from_value(value: u8) -> Self {
        match value {
            0 => Self::Ok,
            1 => Self::InvalidArg,
            2 => Self::NotFound,
            3 => Self::Unsupported,
            4 => Self::Io,
            5 => Self::Device,
            6 => Self::State,
            7 => Self::NoMem,
            8 => Self::Internal,
            9 => Self::AlreadyExists,
            10 => Self::Busy,
            11 => Self::Corrupt,
            12 => Self::NoSpace,
            13 => Self::Timeout,
            _ => Self::Unknown,
        }
    }
}

impl fmt::Display for StatusCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// A wire error: the daemon reply's `error` object or a codec failure.
#[derive(Clone, Debug)]
pub struct WireError {
    pub code: StatusCode,
    pub message: String,
}

impl WireError {
    #[must_use]
    pub fn new(code: StatusCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }

    #[must_use]
    pub fn invalid_arg(message: impl Into<String>) -> Self {
        Self::new(StatusCode::InvalidArg, message)
    }

    #[must_use]
    pub fn corrupt(message: impl Into<String>) -> Self {
        Self::new(StatusCode::Corrupt, message)
    }

    #[must_use]
    pub fn io(message: impl Into<String>) -> Self {
        Self::new(StatusCode::Io, message)
    }
}

impl fmt::Display for WireError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code.as_str(), self.message)
    }
}

impl std::error::Error for WireError {}

pub type Result<T> = std::result::Result<T, WireError>;
