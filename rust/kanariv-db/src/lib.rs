//! Experimental Rust implementation of the kanariv SQLite schema.
//!
//! Scope (phase 3 of `rust/README.md`):
//! - the exact schema from `src/db/core.cpp`, including FTS5 and triggers;
//! - track CRUD, three-stage search, queues, manual playlists, history,
//!   bookmarks, lyrics, libraries and aggregate statistics.
//!
//! Not implemented here: engine-state persistence, smart-playlist evaluation,
//! background write batching, and the C ABI adapter that would implement
//! `kanariv::contracts::IDatabase`. The crate is a standalone library; the
//! daemon does not link it.
//!
//! The audio path is untouched: this crate stores rows only.

#![allow(clippy::missing_errors_doc, clippy::must_use_candidate)]

mod error;
mod model;
mod schema;
mod store;

pub use error::{DbError, Result};
pub use model::{
    Bookmark, HistoryEntry, Library, Lyrics, PlaybackStats, Playlist, Queue, QueueItem, SourceKind,
    SourceRef, Track,
};
pub use schema::SCHEMA;
pub use store::Store;
