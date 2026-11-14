//! Error model mirroring `kanariv::utils::Error`.
//!
//! The wire carries both a symbolic code and a numeric value; the CLI uses
//! the symbolic form for display and the numeric form only as a fallback.

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
            _ => Self::Unknown,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Error {
    pub code: StatusCode,
    pub message: String,
}

impl Error {
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
    pub fn io(message: impl Into<String>) -> Self {
        Self::new(StatusCode::Io, message)
    }

    /// CLI exit code contract (mirrors `exitCodeForError` in shell.cpp).
    #[must_use]
    pub fn exit_code(&self) -> i32 {
        match self.code {
            StatusCode::InvalidArg => 106,
            StatusCode::NotFound => 109,
            StatusCode::State => 105,
            _ => 1,
        }
    }

    /// Full stderr text: `Error: <Code> <message>` plus the daemon hint.
    #[must_use]
    pub fn render(&self) -> String {
        let mut text = format!("Error: {} {}", self.code.as_str(), self.message);
        if self.code == StatusCode::State && self.message == "daemon not running" {
            text.push_str("\nhint: run `kanariv start` to start the daemon");
        }
        text
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code.as_str(), self.message)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;
