//! Wire-compatible IPC codec and transport for the kanariv daemon.
//!
//! This crate mirrors the C++ `kanariv::ipc` protocol without linking any C++
//! code: 4-byte big-endian length prefix, UTF-8 JSON payloads, and the same
//! 16 MiB frame ceiling. The audio path (decoders, engine thread) stays in
//! C++ and is never touched from here.
//!
//! * [`Command`] serializes key-for-key like `ipc::toJson(const Command&)`.
//! * [`Reply`] parses every result kind in `ipc::toJson(const Result&)`.
//! * [`frame`] is the hardened length-prefix codec.
//! * [`transport::Connection`] connects to the named pipe or Unix socket.

#![allow(
    clippy::must_use_candidate,
    clippy::missing_errors_doc,
    clippy::module_name_repetitions
)]

pub mod command;
pub mod error;
pub mod frame;
pub mod model;
pub mod reply;
pub mod transport;

pub use command::{Command, TagBatchEditEntry};
pub use error::{Result, StatusCode, WireError};
pub use frame::{decode_from, encode, MAX_FRAME};
pub use model::{
    Bookmark, ConfigValue, DeviceInfo, HistoryEntry, LibraryStats, Lyrics, PlaybackState, Playlist,
    QueueEntry, RepeatMode, SourceRef, TagBatchEditEntryResult, TopEntity, Track,
};
pub use reply::{parse_reply, Payload, QueueAddItem, QueueRemoveItem, Reply, Status};
pub use transport::Connection;

/// Builds the `IpcRequest` JSON envelope `{"id": ..., "cmd": {...}}`.
///
/// # Errors
/// Fails only if [`Command`] cannot be serialized, which would indicate a
/// codec bug rather than bad input.
pub fn encode_request(id: u32, command: &Command) -> Result<serde_json::Value> {
    let cmd = serde_json::to_value(command)
        .map_err(|err| WireError::corrupt(format!("malformed command: {err}")))?;
    Ok(serde_json::json!({ "id": id, "cmd": cmd }))
}
