//! Typed reply model mirroring `kanariv::ipc::toJson(const Result&)`.
//!
//! Parsing is deliberately tolerant in the same places as the C++ codec:
//! unknown fields are ignored, missing fields take their defaults, and a
//! non-array container yields an empty list. A malformed item or an unknown
//! `type` still fails (or becomes [`Reply::Other`] for the latter).

use serde::de::DeserializeOwned;
use serde_json::Value;

use crate::error::{Result, StatusCode, WireError};
use crate::model::{
    Bookmark, ConfigValue, DeviceInfo, HistoryEntry, LibraryStats, Lyrics, PlaybackState, Playlist,
    QueueEntry, RepeatMode, TagBatchEditEntryResult, TopEntity, Track,
};

/// Current playback status (`ipc::Status`).
#[derive(Clone, Debug)]
pub struct Status {
    pub state: PlaybackState,
    pub pos: f64,
    pub dur: f64,
    pub vol: f32,
    pub muted: bool,
    pub shuffle: bool,
    pub repeat: RepeatMode,
    pub track_id: i64,
    pub title: String,
    pub artist: String,
    pub path: String,
    pub q_size: u64,
    pub q_idx: u64,
    pub version: String,
}

impl Default for Status {
    fn default() -> Self {
        Self {
            state: PlaybackState::Stopped,
            pos: 0.0,
            dur: 0.0,
            vol: 1.0,
            muted: false,
            shuffle: false,
            repeat: RepeatMode::Off,
            track_id: 0,
            title: String::new(),
            artist: String::new(),
            path: String::new(),
            q_size: 0,
            q_idx: 0,
            version: String::new(),
        }
    }
}

impl Status {
    #[must_use]
    pub fn is_paused(&self) -> bool {
        self.state == PlaybackState::Paused
    }

    #[must_use]
    pub fn is_stopped(&self) -> bool {
        self.state == PlaybackState::Stopped
    }

    #[must_use]
    pub fn is_playing(&self) -> bool {
        self.state == PlaybackState::Playing
    }

    fn from_json(j: &Value) -> Result<Self> {
        let mut status = Self::default();
        if let Some(text) = j.get("state").and_then(Value::as_str) {
            status.state = PlaybackState::from_wire(text)
                .ok_or_else(|| WireError::invalid_arg("unknown PlaybackState"))?;
        } else if let Some(value) = j.get("state_value").and_then(Value::as_i64) {
            status.state = PlaybackState::from_value(value);
        }
        if let Some(value) = j.get("pos").and_then(Value::as_f64) {
            status.pos = value;
        }
        if let Some(value) = j.get("dur").and_then(Value::as_f64) {
            status.dur = value;
        }
        if let Some(value) = j.get("vol").and_then(Value::as_f64) {
            status.vol = value as f32;
        }
        if let Some(value) = j.get("muted").and_then(Value::as_bool) {
            status.muted = value;
        }
        if let Some(value) = j.get("shuffle").and_then(Value::as_bool) {
            status.shuffle = value;
        }
        if let Some(text) = j.get("repeat").and_then(Value::as_str) {
            status.repeat = RepeatMode::from_wire(text)
                .ok_or_else(|| WireError::invalid_arg("unknown RepeatMode"))?;
        } else if let Some(value) = j.get("repeat_value").and_then(Value::as_i64) {
            status.repeat = RepeatMode::from_value(value);
        }
        if let Some(value) = j.get("track_id").and_then(Value::as_i64) {
            status.track_id = value;
        }
        if let Some(value) = j.get("q_size").and_then(Value::as_u64) {
            status.q_size = value;
        }
        if let Some(value) = j.get("q_idx").and_then(Value::as_u64) {
            status.q_idx = value;
        }
        status.title = string_field(j, "title").unwrap_or_default();
        status.artist = string_field(j, "artist").unwrap_or_default();
        status.path = string_field(j, "path").unwrap_or_default();
        status.version = string_field(j, "version").unwrap_or_default();
        Ok(status)
    }
}

/// One `QueueAddBatch` outcome (`ipc::QueueAddItem`).
#[derive(Clone, Debug)]
pub struct QueueAddItem {
    pub query: String,
    /// Absent when the query resolved to nothing.
    pub track: Option<Track>,
    pub status: String,
    pub detail: Option<String>,
}

/// One `QueueRemoveBatch` outcome (`ipc::QueueRemoveItem`).
#[derive(Clone, Debug)]
pub struct QueueRemoveItem {
    pub selector: String,
    /// Absent when the selector did not match.
    pub track: Option<Track>,
    pub status: String,
    pub detail: Option<String>,
}

/// A typed daemon result. `Other` preserves replies this crate does not model.
#[derive(Clone, Debug)]
pub enum Reply {
    Status(Status),
    QueueTracks(Vec<Track>),
    QueueAddReport {
        items: Vec<QueueAddItem>,
    },
    QueueRemoveReport {
        items: Vec<QueueRemoveItem>,
    },
    Queues(Vec<QueueEntry>),
    QueueCreated {
        id: i64,
        name: String,
    },
    PlaylistLoaded {
        queue_id: i64,
        status: Status,
    },
    VolumeInfo {
        vol: f32,
        muted: bool,
    },
    LibraryStats(LibraryStats),
    Tracks(Vec<Track>),
    SearchResults {
        query: String,
        tracks: Vec<Track>,
    },
    ScanReport {
        added: u64,
    },
    AnalyzeReport {
        tracks: i64,
        analyzed: i64,
        failed: i64,
        skipped: i64,
    },
    Playlists(Vec<Playlist>),
    PlaylistData {
        tracks: Vec<Track>,
        format: String,
    },
    PlaylistCreated {
        id: i64,
        name: String,
    },
    PlaylistImportReport {
        pid: i64,
        name: String,
        tracks: Vec<Track>,
        matched: u64,
        skipped: u64,
        duplicates: u64,
    },
    LibraryMaintenanceReport {
        tracks_checked: u64,
        missing_tracks: Vec<Track>,
        duplicate_groups: u64,
        duplicate_tracks: u64,
    },
    ConfigValue(ConfigValue),
    ConfigValues(Vec<ConfigValue>),
    SingleTrack {
        track: Track,
        cover_mime: Option<String>,
        cover_data: Option<String>,
    },
    TrackInfo {
        track: Track,
        play_count: i64,
        last_played: i64,
    },
    HistoryEntry(HistoryEntry),
    History(Vec<HistoryEntry>),
    Bookmarks(Vec<Bookmark>),
    Lyrics(Lyrics),
    TagBatchEditResult {
        succeeded: u64,
        failed: u64,
        results: Vec<TagBatchEditEntryResult>,
    },
    Devices(Vec<DeviceInfo>),
    Empty,
    Other,
}

/// A parsed reply plus the original result JSON.
///
/// `json` is the untouched `result` value, so JSON mode can re-emit fields
/// this crate does not model.
#[derive(Clone, Debug)]
pub struct Payload {
    pub json: Value,
    pub reply: Reply,
}

impl Payload {
    /// Parses a `result` object into a typed reply.
    ///
    /// # Errors
    /// Returns `InvalidArg` for a missing/malformed `type` or an unknown
    /// state/repeat spelling, and `Corrupt` for malformed member values.
    pub fn from_json(value: &Value) -> Result<Self> {
        Ok(Self {
            json: value.clone(),
            reply: Reply::from_json(value)?,
        })
    }

    #[must_use]
    pub fn status(&self) -> Option<&Status> {
        match &self.reply {
            Reply::Status(status) => Some(status),
            _ => None,
        }
    }
}

impl Reply {
    #[allow(clippy::too_many_lines)]
    fn from_json(j: &Value) -> Result<Self> {
        let type_name = j
            .get("type")
            .and_then(Value::as_str)
            .ok_or_else(|| WireError::invalid_arg("missing Result type"))?;
        match type_name {
            "Status" => Ok(Self::Status(Status::from_json(j)?)),
            "QueueTracks" => Ok(Self::QueueTracks(track_array(j, "tracks")?)),
            "QueueAddReport" => Ok(Self::QueueAddReport {
                items: queue_add_items(j)?,
            }),
            "QueueRemoveReport" => Ok(Self::QueueRemoveReport {
                items: queue_remove_items(j)?,
            }),
            "Queues" => Ok(Self::Queues(array(j, "queues")?)),
            "QueueCreated" => Ok(Self::QueueCreated {
                id: i64_field(j, "id"),
                name: string_field(j, "name").unwrap_or_default(),
            }),
            "PlaylistLoaded" => Ok(Self::PlaylistLoaded {
                queue_id: i64_field(j, "queue_id"),
                status: match j.get("status") {
                    Some(value) => Status::from_json(value)?,
                    None => Status::default(),
                },
            }),
            "VolumeInfo" => Ok(Self::VolumeInfo {
                vol: f64_field(j, "vol").unwrap_or(1.0) as f32,
                muted: bool_field(j, "muted").unwrap_or(false),
            }),
            "LibraryStats" => Ok(Self::LibraryStats(library_stats(j, false)?)),
            "LibraryStatsDetailed" => Ok(Self::LibraryStats(library_stats(j, true)?)),
            "Tracks" => Ok(Self::Tracks(track_array(j, "tracks")?)),
            "SearchResults" => Ok(Self::SearchResults {
                query: string_field(j, "query").unwrap_or_default(),
                tracks: track_array(j, "tracks")?,
            }),
            "ScanReport" => Ok(Self::ScanReport {
                added: u64_field(j, "added"),
            }),
            "AnalyzeReport" => Ok(Self::AnalyzeReport {
                tracks: i64_field(j, "tracks"),
                analyzed: i64_field(j, "analyzed"),
                failed: i64_field(j, "failed"),
                skipped: i64_field(j, "skipped"),
            }),
            "Playlists" => Ok(Self::Playlists(array(j, "playlists")?)),
            "PlaylistData" => Ok(Self::PlaylistData {
                tracks: track_array(j, "tracks")?,
                format: string_field(j, "format").unwrap_or_default(),
            }),
            "PlaylistCreated" => Ok(Self::PlaylistCreated {
                id: i64_field(j, "id"),
                name: string_field(j, "name").unwrap_or_default(),
            }),
            "PlaylistImportReport" => Ok(Self::PlaylistImportReport {
                pid: i64_field(j, "pid"),
                name: string_field(j, "name").unwrap_or_default(),
                tracks: track_array(j, "tracks")?,
                matched: u64_field(j, "matched"),
                skipped: u64_field(j, "skipped"),
                duplicates: u64_field(j, "duplicates"),
            }),
            "LibraryMaintenanceReport" => Ok(Self::LibraryMaintenanceReport {
                tracks_checked: u64_field(j, "tracks_checked"),
                missing_tracks: track_array(j, "missing_tracks")?,
                duplicate_groups: u64_field(j, "duplicate_groups"),
                duplicate_tracks: u64_field(j, "duplicate_tracks"),
            }),
            "ConfigValue" => Ok(Self::ConfigValue(ConfigValue {
                key: string_field(j, "key").unwrap_or_default(),
                value: string_field(j, "value").unwrap_or_default(),
            })),
            "ConfigValues" => Ok(Self::ConfigValues(array(j, "values")?)),
            "SingleTrack" => Ok(Self::SingleTrack {
                track: normalize_track(track_field(j, "track")?),
                cover_mime: string_field(j, "cover_mime"),
                cover_data: string_field(j, "cover_data"),
            }),
            "TrackInfo" => Ok(Self::TrackInfo {
                track: normalize_track(track_field(j, "track")?),
                play_count: i64_field(j, "play_count"),
                last_played: i64_field(j, "last_played"),
            }),
            "HistoryEntry" => Ok(Self::HistoryEntry(
                serde_json::from_value(j.clone())
                    .map_err(|err| WireError::corrupt(format!("malformed HistoryEntry: {err}")))?,
            )),
            "History" => Ok(Self::History(array(j, "entries")?)),
            "Bookmarks" => Ok(Self::Bookmarks(array(j, "entries")?)),
            "Lyrics" => Ok(Self::Lyrics(serde_json::from_value(j.clone()).map_err(
                |err| WireError::corrupt(format!("malformed Lyrics: {err}")),
            )?)),
            "TagBatchEditResult" => Ok(Self::TagBatchEditResult {
                succeeded: u64_field(j, "succeeded"),
                failed: u64_field(j, "failed"),
                results: array(j, "results")?,
            }),
            "Devices" => Ok(Self::Devices(array(j, "devices")?)),
            "Empty" => Ok(Self::Empty),
            _ => Ok(Self::Other),
        }
    }
}

fn library_stats(j: &Value, detailed: bool) -> Result<LibraryStats> {
    let mut stats = LibraryStats {
        tracks: u64_field(j, "tracks"),
        queues: u64_field(j, "queues"),
        playlists: u64_field(j, "playlists"),
        total_duration_ms: i64_field(j, "total_duration_ms"),
        total_play_time_ms: i64_field(j, "total_play_time_ms"),
        ..LibraryStats::default()
    };
    stats.top_tracks = optional_track_array(j, "top_tracks")?;
    stats.top_albums = optional_array::<TopEntity>(j, "top_albums")?;
    stats.top_genres = optional_array::<TopEntity>(j, "top_genres")?;
    stats.top_artists = optional_array::<TopEntity>(j, "top_artists")?;
    if detailed && stats.top_tracks.is_none() {
        stats.top_tracks = Some(Vec::new());
    }
    Ok(stats)
}

/// Mirrors `trackFromJson`: unknown `library_id` defaults to 1 and a `path`
/// source fills its empty key/display name from the track path.
fn normalize_track(mut track: Track) -> Track {
    if track.library_id == 0 {
        track.library_id = 1;
    }
    if track.source.kind == "path" {
        if track.source.key.is_empty() {
            track.source.key.clone_from(&track.path);
        }
        if track.source.display_name.is_empty() {
            track.source.display_name.clone_from(&track.path);
        }
    }
    track
}

fn track_array(j: &Value, key: &str) -> Result<Vec<Track>> {
    Ok(array(j, key)?.into_iter().map(normalize_track).collect())
}

fn optional_track_array(j: &Value, key: &str) -> Result<Option<Vec<Track>>> {
    match j.get(key) {
        Some(Value::Array(_)) => track_array(j, key).map(Some),
        _ => Ok(None),
    }
}

fn array<T: DeserializeOwned>(j: &Value, key: &str) -> Result<Vec<T>> {
    match j.get(key) {
        Some(Value::Array(items)) => items
            .iter()
            .map(|item| {
                serde_json::from_value(item.clone())
                    .map_err(|err| WireError::corrupt(format!("malformed {key}: {err}")))
            })
            .collect(),
        _ => Ok(Vec::new()),
    }
}

fn optional_array<T: DeserializeOwned>(j: &Value, key: &str) -> Result<Option<Vec<T>>> {
    match j.get(key) {
        Some(Value::Array(_)) => array(j, key).map(Some),
        _ => Ok(None),
    }
}

fn queue_add_items(j: &Value) -> Result<Vec<QueueAddItem>> {
    let mut items = Vec::new();
    for item in report_item_objects(j) {
        items.push(QueueAddItem {
            query: string_field(&item, "query").unwrap_or_default(),
            track: report_item_track(&item)?,
            status: string_field(&item, "status").unwrap_or_default(),
            detail: string_field(&item, "detail"),
        });
    }
    Ok(items)
}

fn queue_remove_items(j: &Value) -> Result<Vec<QueueRemoveItem>> {
    let mut items = Vec::new();
    for item in report_item_objects(j) {
        items.push(QueueRemoveItem {
            selector: string_field(&item, "selector").unwrap_or_default(),
            track: report_item_track(&item)?,
            status: string_field(&item, "status").unwrap_or_default(),
            detail: string_field(&item, "detail"),
        });
    }
    Ok(items)
}

fn report_item_objects(j: &Value) -> Vec<Value> {
    match j.get("items") {
        Some(Value::Array(items)) => items.clone(),
        _ => Vec::new(),
    }
}

/// C++ omits `track` when the entry did not resolve; a present non-object is
/// treated as absent rather than fatal, matching the crate's tolerant parsing.
fn report_item_track(item: &Value) -> Result<Option<Track>> {
    match item.get("track") {
        Some(Value::Object(_)) => Ok(Some(normalize_track(track_field(item, "track")?))),
        _ => Ok(None),
    }
}

fn track_field(j: &Value, key: &str) -> Result<Track> {
    match j.get(key) {
        Some(value @ Value::Object(_)) => serde_json::from_value(value.clone())
            .map_err(|err| WireError::corrupt(format!("malformed {key}: {err}"))),
        _ => Ok(Track::default()),
    }
}

fn string_field(j: &Value, key: &str) -> Option<String> {
    j.get(key).and_then(Value::as_str).map(ToString::to_string)
}

fn bool_field(j: &Value, key: &str) -> Option<bool> {
    j.get(key).and_then(Value::as_bool)
}

fn i64_field(j: &Value, key: &str) -> i64 {
    j.get(key).and_then(Value::as_i64).unwrap_or_default()
}

fn u64_field(j: &Value, key: &str) -> u64 {
    j.get(key).and_then(Value::as_u64).unwrap_or_default()
}

fn f64_field(j: &Value, key: &str) -> Option<f64> {
    j.get(key).and_then(Value::as_f64)
}

fn error_from_json(value: &Value) -> WireError {
    let code = value
        .get("code")
        .and_then(Value::as_str)
        .map(StatusCode::from_wire)
        .or_else(|| {
            value
                .get("code_value")
                .and_then(Value::as_u64)
                .map(|v| StatusCode::from_value(u8::try_from(v).unwrap_or(255)))
        })
        .unwrap_or(StatusCode::Internal);
    WireError::new(code, string_field(value, "message").unwrap_or_default())
}

/// Parses a framed reply body, mapping `ok:false` to a [`WireError`].
///
/// # Errors
/// Fails on malformed JSON (`Corrupt`), a missing `result` (`InvalidArg`),
/// a daemon error object, or a malformed typed result.
pub fn parse_reply(bytes: &[u8]) -> Result<Payload> {
    let json: Value = serde_json::from_slice(bytes)
        .map_err(|err| WireError::corrupt(format!("malformed reply: {err}")))?;
    let ok = json
        .get("ok")
        .and_then(Value::as_bool)
        .unwrap_or_else(|| json.get("error").is_none());
    if !ok {
        let error = json
            .get("error")
            .or_else(|| json.get("result"))
            .ok_or_else(|| WireError::invalid_arg("missing error"))?;
        return Err(error_from_json(error));
    }
    let result = json
        .get("result")
        .ok_or_else(|| WireError::invalid_arg("missing result"))?;
    Payload::from_json(result)
}
