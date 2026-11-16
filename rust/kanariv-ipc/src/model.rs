//! Shared wire records, mirroring `kanariv::types` and `utils/record_json.cpp`.

use std::fmt;

use serde::{Deserialize, Serialize};

/// Playback state as it appears on the wire ("Stopped", "Ready", ...).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
pub enum PlaybackState {
    #[default]
    Stopped,
    Ready,
    Playing,
    Paused,
    /// Numeric value outside the C++ enum range (renders as `"Unknown"`).
    Unknown,
}

impl PlaybackState {
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Stopped => "Stopped",
            Self::Ready => "Ready",
            Self::Playing => "Playing",
            Self::Paused => "Paused",
            Self::Unknown => "Unknown",
        }
    }

    #[must_use]
    pub fn from_wire(text: &str) -> Option<Self> {
        match text {
            "Stopped" => Some(Self::Stopped),
            "Ready" => Some(Self::Ready),
            "Playing" => Some(Self::Playing),
            "Paused" => Some(Self::Paused),
            _ => None,
        }
    }

    #[must_use]
    pub fn from_value(value: i64) -> Self {
        match value {
            0 => Self::Stopped,
            1 => Self::Ready,
            2 => Self::Playing,
            3 => Self::Paused,
            _ => Self::Unknown,
        }
    }
}

impl fmt::Display for PlaybackState {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Queue advancement mode as it appears on the wire ("Off", "All", "One").
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
pub enum RepeatMode {
    #[default]
    Off,
    All,
    One,
    /// Numeric value outside the C++ enum range (renders as `"Unknown"`).
    Unknown,
}

impl RepeatMode {
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Off => "Off",
            Self::All => "All",
            Self::One => "One",
            Self::Unknown => "Unknown",
        }
    }

    #[must_use]
    pub fn from_wire(text: &str) -> Option<Self> {
        match text {
            "Off" => Some(Self::Off),
            "All" => Some(Self::All),
            "One" => Some(Self::One),
            _ => None,
        }
    }

    #[must_use]
    pub fn from_value(value: i64) -> Self {
        match value {
            0 => Self::Off,
            1 => Self::All,
            2 => Self::One,
            _ => Self::Unknown,
        }
    }
}

impl fmt::Display for RepeatMode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Durable source identity attached to a track.
#[derive(Clone, Debug, PartialEq, Deserialize)]
#[serde(default)]
pub struct SourceRef {
    pub kind: String,
    pub key: String,
    pub display_name: String,
    pub available: bool,
}

impl Default for SourceRef {
    fn default() -> Self {
        Self {
            kind: "path".to_string(),
            key: String::new(),
            display_name: String::new(),
            available: true,
        }
    }
}

/// A library track row (`utils::trackToJson` / `trackFromJson`).
///
/// `fingerprint` is kept as the wire's lowercase hex string; the C++ codec
/// decodes it into 32 bytes. Missing fields fall back to the C++ defaults.
#[derive(Clone, Debug, PartialEq, Deserialize)]
#[serde(default)]
pub struct Track {
    pub id: i64,
    pub size: i64,
    pub mtime: i64,
    pub duration: f64,
    pub sample_rate: u32,
    pub channels: u32,
    pub bitrate: i32,
    pub year: i32,
    pub track_num: i32,
    pub disc_num: i32,
    pub rating: i32,
    pub play_count: i64,
    pub last_played: i64,
    pub date_added: i64,
    pub last_scanned: i64,
    pub dirty: bool,
    pub library_id: i64,
    pub fingerprint: String,
    pub path: String,
    pub source: SourceRef,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub genre: String,
    pub cover_art_path: String,
}

impl Default for Track {
    fn default() -> Self {
        Self {
            id: 0,
            size: 0,
            mtime: 0,
            duration: 0.0,
            sample_rate: 0,
            channels: 0,
            bitrate: 0,
            year: 0,
            track_num: 0,
            disc_num: 0,
            rating: 0,
            play_count: 0,
            last_played: 0,
            date_added: 0,
            last_scanned: 0,
            dirty: false,
            library_id: 1,
            fingerprint: String::new(),
            path: String::new(),
            source: SourceRef::default(),
            title: String::new(),
            artist: String::new(),
            album: String::new(),
            album_artist: String::new(),
            genre: String::new(),
            cover_art_path: String::new(),
        }
    }
}

/// Playlist row (`ipc::detail::playlistToJson`).
#[derive(Clone, Debug, Deserialize)]
#[serde(default)]
pub struct Playlist {
    pub id: i64,
    pub name: String,
    #[serde(rename = "type")]
    pub kind: i32,
    pub smart_query: String,
    pub created: i64,
    pub modified: i64,
    pub library_id: i64,
}

impl Default for Playlist {
    fn default() -> Self {
        Self {
            id: 0,
            name: String::new(),
            kind: 0,
            smart_query: String::new(),
            created: 0,
            modified: 0,
            library_id: 1,
        }
    }
}

/// One entry of the `Queues` result.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct QueueEntry {
    pub id: i64,
    pub name: String,
    pub tracks: u64,
    pub active: bool,
    pub temp: bool,
}

/// Top-N report row (`types::TopEntity`).
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct TopEntity {
    pub name: String,
    pub track_count: u64,
    pub total_plays: i64,
}

/// `types::LibraryStats`; each `top_*` is `Some` only when requested.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct LibraryStats {
    pub tracks: u64,
    pub queues: u64,
    pub playlists: u64,
    pub total_duration_ms: i64,
    pub total_play_time_ms: i64,
    pub top_tracks: Option<Vec<Track>>,
    pub top_albums: Option<Vec<TopEntity>>,
    pub top_genres: Option<Vec<TopEntity>>,
    pub top_artists: Option<Vec<TopEntity>>,
}

/// Playback history row (`types::HistoryEntry`).
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
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

/// Bookmark row (`types::Bookmark`).
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct Bookmark {
    pub id: i64,
    pub track_id: i64,
    pub position_ms: i64,
    pub note: String,
    pub created: i64,
}

/// Audio device row (`types::DeviceInfo`).
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct DeviceInfo {
    pub id: String,
    pub name: String,
    #[serde(rename = "isDefault")]
    pub is_default: bool,
}

/// Lyrics row (`types::Lyrics`).
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct Lyrics {
    pub id: i64,
    pub track_id: i64,
    pub lrc_text: String,
    pub is_synced: bool,
    pub source: String,
    pub fetched_at: i64,
}

/// One `ConfigValue` row.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct ConfigValue {
    pub key: String,
    pub value: String,
}

/// Per-entry outcome of a `TagBatchEdit`.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct TagBatchEditEntryResult {
    pub id: Option<i64>,
    pub path: Option<String>,
    pub field: String,
    pub ok: bool,
    pub error: String,
}
