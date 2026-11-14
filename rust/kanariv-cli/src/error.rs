//! CLI error type on top of the shared wire error model.
//!
//! `StatusCode` and wire parsing live in `kanariv-ipc`; this module adds the
//! CLI's exit-code contract and stderr rendering.

use std::fmt;

pub use kanariv_ipc::StatusCode;

use kanariv_ipc::WireError;

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

impl From<WireError> for Error {
    fn from(err: WireError) -> Self {
        Self::new(err.code, err.message)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code.as_str(), self.message)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;
