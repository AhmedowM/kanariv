//! Command outcome data, mirroring `kanariv::app::Outcome`.
//!
//! Handlers return this instead of printing: frontends render the payload
//! (bare/JSON), print `line`, or stay silent (`quiet`).

use crate::ipc::Payload;

#[derive(Debug)]
pub struct Outcome {
    pub payload: Option<Payload>,
    /// Human-readable line, when applicable.
    pub line: Option<String>,
    /// Render `line` on stderr instead of stdout.
    pub to_stderr: bool,
    /// Runtime failure (exit code 1); the line goes to stderr.
    pub failed: bool,
    /// Suppresses human rendering of the payload.
    pub quiet: bool,
}

impl Outcome {
    #[must_use]
    pub fn ok(payload: Option<Payload>, line: Option<String>) -> Self {
        Self {
            payload,
            line,
            to_stderr: false,
            failed: false,
            quiet: false,
        }
    }

    #[must_use]
    pub fn confirm(payload: Payload, line: Option<String>) -> Self {
        Self::ok(Some(payload), line)
    }

    #[must_use]
    pub fn warn(message: impl Into<String>) -> Self {
        Self {
            payload: None,
            line: Some(message.into()),
            to_stderr: true,
            failed: false,
            quiet: false,
        }
    }

    #[must_use]
    pub fn fail(message: impl Into<String>) -> Self {
        Self {
            payload: None,
            line: Some(message.into()),
            to_stderr: true,
            failed: true,
            quiet: false,
        }
    }
}
