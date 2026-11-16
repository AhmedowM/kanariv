//! Row value types mirroring `include/kanariv/types/record.hpp` and `db.hpp`.
//!
//! Fields use plain integers with `0` meaning "absent", matching the C++ value
//! types. Keeping the same representation makes cross-language parity checks
//! direct; [`crate::Store`] also treats SQL `NULL` as `0`/empty, as the C++
//! column helpers do.

/// Durable identity category for a track's bytes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum SourceKind {
    /// Ordinary filesystem path (legacy default).
    #[default]
    Path = 0,
    /// Stable host-issued identifier, not a filesystem path.
    External = 1,
    /// App-private copy created by the host.
    Imported = 2,
    /// Source is known but cannot currently be reopened.
    Unavailable = 3,
}

impl SourceKind {
    #[must_use]
    pub fn from_db(value: i64) -> Self {
        match value {
            1 => Self::External,
            2 => Self::Imported,
            3 => Self::Unavailable,
            _ => Self::Path,
        }
    }
}

/// Persisted source identity and availability for a track.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SourceRef {
    pub kind: SourceKind,
    pub key: String,
    pub display_name: String,
    pub available: bool,
}

impl SourceRef {
    /// Fills path keys/display names, matching `kanariv::types::normalizedSource`.
    #[must_use]
    pub fn normalized(&self, path: &str) -> Self {
        let mut out = self.clone();
        if out.kind == SourceKind::Path {
            if out.key.is_empty() {
                path.clone_into(&mut out.key);
            }
            if out.display_name.is_empty() {
                path.clone_into(&mut out.display_name);
            }
        }
        out
    }
}

/// A single audio track row.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TrackRow {
    pub id: i64,
    pub size: i64,
    pub mtime: i64,
    pub duration: f64,
    pub play_count: i64,
    pub last_played: i64,
    pub date_added: i64,
    pub last_scanned: i64,
    pub library_id: i64,
    pub path: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub genre: String,
    pub cover_art_path: String,
    pub sample_rate: u32,
    pub channels: u32,
    pub bitrate: i32,
    pub year: i32,
    pub track_num: i32,
    pub disc_num: i32,
    pub rating: i32,
    pub fingerprint: [u8; 32],
    pub dirty: bool,
    pub source: SourceRef,
    pub replay_gain_track: f64,
    pub replay_gain_album: f64,
}

impl TrackRow {
    /// Applies the C++ defaults: a zero library id means the default library.
    #[must_use]
    pub fn normalized_library_id(&self) -> i64 {
        if self.library_id == 0 {
            1
        } else {
            self.library_id
        }
    }
}

/// Queue container row (`queues` table).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Queue {
    pub id: i64,
    pub name: String,
    pub repeat_mode: i32,
    pub temp: bool,
    pub library_id: i64,
}

/// Single entry in a playback queue (`queue` table).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct QueueItem {
    pub id: i64,
    pub queue_id: i64,
    pub track_id: i64,
    pub position: i64,
    pub added: i64,
}

/// Playlist row (`playlists` table).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Playlist {
    pub id: i64,
    pub name: String,
    pub playlist_type: i32,
    pub smart_query: String,
    pub created: i64,
    pub modified: i64,
    pub library_id: i64,
}

/// Library (scan root) row (`libraries` table).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Library {
    pub id: i64,
    pub path: String,
    pub name: String,
    pub date_added: i64,
    pub last_scanned: i64,
    pub auto_scan: bool,
    pub recursive: bool,
    pub extensions: String,
}

/// Playback history entry, including the track snapshot produced by JOINs.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct HistoryEntry {
    pub id: i64,
    pub track_id: i64,
    pub started_at: i64,
    pub completed_at: i64,
    pub position_ms: i64,
    pub completion_pct: f64,
    pub queue_id: i64,
    pub title: String,
    pub artist: String,
    pub path: String,
    pub duration: f64,
}

/// Bookmark / saved position (`bookmarks` table).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Bookmark {
    pub id: i64,
    pub track_id: i64,
    pub position_ms: i64,
    pub note: String,
    pub created: i64,
}

/// Lyrics row (`lyrics` table).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Lyrics {
    pub id: i64,
    pub track_id: i64,
    pub lrc_text: String,
    pub is_synced: bool,
    pub source: String,
    pub fetched_at: i64,
}

/// Aggregate database statistics, mirroring `types::PlaybackStats`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PlaybackStats {
    pub num_tracks: i64,
    pub num_playlists: i64,
    pub num_queue_items: i64,
    pub num_history: i64,
    pub num_bookmarks: i64,
    pub num_libraries: i64,
    pub total_duration_ms: i64,
}
