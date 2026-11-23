//! Typed `Command` model mirroring `kanariv::ipc::toJson(const Command&)`.
//!
//! Serialization is key-for-key compatible with `src/ipc/protocol.cpp`:
//! optional fields that C++ emits as `null` stay `Option` without
//! `skip_serializing_if`, while the two commands that C++ omits when absent
//! (`TagGet`, `TagBatchEditEntry`) skip `None` explicitly.

use serde::Serialize;

use crate::model::RepeatMode;

/// One `TagBatchEdit` assignment.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct TagBatchEditEntry {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub id: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,
    pub field: String,
    pub value: String,
}

/// A command sent to the daemon in the `cmd` field of an `IpcRequest`.
#[derive(Clone, Debug, Serialize)]
#[serde(tag = "type")]
pub enum Command {
    Play,
    Pause,
    Resume,
    Restart,
    Stop,
    Next,
    Prev,
    Seek {
        seconds: f64,
    },
    SeekBy {
        delta: f64,
    },
    StatusReq,
    StatusWait {
        timeout_ms: i32,
    },
    VolumeSet {
        level: Option<f32>,
        mute: Option<bool>,
        #[serde(rename = "deltaPct")]
        delta_pct: Option<i32>,
    },
    QueueList {
        order: String,
    },
    QueueQueues,
    QueueCreate {
        name: String,
    },
    QueueDelete {
        qid: i64,
    },
    PlayFiles {
        paths: Vec<String>,
        save: bool,
    },
    QueueSwitch {
        qid: i64,
    },
    QueueAdd {
        query: String,
        search: bool,
    },
    QueueAddBatch {
        queries: Vec<String>,
        search: bool,
    },
    QueueRemove {
        #[serde(rename = "idOrIndex")]
        id_or_index: String,
    },
    QueueRemoveBatch {
        selectors: Vec<String>,
    },
    QueueMove {
        from: usize,
        to: usize,
    },
    QueueClear,
    QueueShuffle {
        on: Option<bool>,
    },
    QueueRepeat {
        mode: Option<RepeatMode>,
    },
    PlaylistList,
    PlaylistTracks {
        pid: i64,
    },
    PlaylistCreate {
        name: String,
        smart_query: String,
    },
    PlaylistAdd {
        pid: i64,
        track_id: i64,
    },
    PlaylistLoad {
        pid: i64,
        play: bool,
        replace: bool,
    },
    PlaylistSave {
        name: String,
        queue_id: Option<i64>,
    },
    PlaylistDelete {
        pid: i64,
    },
    PlaylistRename {
        pid: i64,
        #[serde(rename = "newName")]
        new_name: String,
    },
    PlaylistExport {
        pid: i64,
        path: String,
        format: String,
    },
    PlaylistImport {
        path: String,
        name: Option<String>,
    },
    LibraryScan {
        path: Option<String>,
        full_hash: bool,
    },
    LibraryAnalyze {
        reanalyze: bool,
        library_id: Option<i64>,
    },
    LibrarySearch {
        query: String,
        limit: i32,
    },
    LibraryStats {
        #[serde(skip_serializing_if = "Option::is_none")]
        top_tracks: Option<i32>,
        #[serde(skip_serializing_if = "Option::is_none")]
        top_albums: Option<i32>,
        #[serde(skip_serializing_if = "Option::is_none")]
        top_genres: Option<i32>,
        #[serde(skip_serializing_if = "Option::is_none")]
        top_artists: Option<i32>,
    },
    LibraryAdd {
        path: String,
        recursive: bool,
    },
    LibraryRemove {
        query: String,
    },
    LibraryList {
        query: Option<String>,
        limit: i32,
        offset: i32,
        sort: String,
        order: String,
        artist: Option<String>,
        album: Option<String>,
        genre: Option<String>,
        rating: Option<i32>,
    },
    LibraryMaintenance {
        action: String,
        dry_run: bool,
        library_id: i64,
    },
    TagEdit {
        id: i64,
        field: String,
        value: String,
    },
    TagGet {
        #[serde(skip_serializing_if = "Option::is_none")]
        id: Option<i64>,
        #[serde(skip_serializing_if = "Option::is_none")]
        path: Option<String>,
        cover: bool,
    },
    TagBatchEdit {
        dry_run: bool,
        entries: Vec<TagBatchEditEntry>,
    },
    ConfigGet {
        key: String,
    },
    ConfigSet {
        key: String,
        value: String,
    },
    ConfigList,
    ConfigExport {
        path: String,
    },
    ConfigImport {
        path: String,
    },
    ConfigReset {
        key: Option<String>,
    },
    HistoryList {
        limit: Option<i32>,
    },
    HistoryClear,
    BookmarkList {
        track_id: Option<i64>,
    },
    BookmarkAdd {
        position_ms: i64,
        note: String,
        track_id: Option<i64>,
    },
    BookmarkRemove {
        id: i64,
    },
    BookmarkGoto {
        id: i64,
    },
    LyricsGet {
        track_id: Option<i64>,
    },
    LyricsSet {
        lrc_text: String,
        is_synced: bool,
        source: String,
        track_id: Option<i64>,
    },
    LyricsRemove {
        track_id: Option<i64>,
    },
    Shutdown,
    Preview {
        file: String,
    },
    DeviceList,
    DeviceSet {
        id: String,
    },
    DeviceTest {
        id: Option<String>,
        tone: bool,
    },
    Info,
    TrackRate {
        track_id: i64,
        rating: i32,
    },
}
