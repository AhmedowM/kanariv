//! Golden JSON checks for every command, mirroring `src/ipc/protocol.cpp`.

#![allow(clippy::needless_pass_by_value, clippy::too_many_lines)]

use kanariv_ipc::{encode_request, Command, RepeatMode, TagBatchEditEntry};
use serde_json::{json, Value};

fn check(command: Command, expected: Value) {
    let actual = serde_json::to_value(&command).expect("command serializes");
    assert_eq!(actual, expected, "command {command:?}");
}

#[test]
fn playback_commands_match_cpp_schema() {
    check(Command::Play, json!({"type": "Play"}));
    check(Command::Pause, json!({"type": "Pause"}));
    check(Command::Resume, json!({"type": "Resume"}));
    check(Command::Restart, json!({"type": "Restart"}));
    check(Command::Stop, json!({"type": "Stop"}));
    check(Command::Next, json!({"type": "Next"}));
    check(Command::Prev, json!({"type": "Prev"}));
    check(
        Command::Seek { seconds: 12.5 },
        json!({"type": "Seek", "seconds": 12.5}),
    );
    check(
        Command::SeekBy { delta: -10.0 },
        json!({"type": "SeekBy", "delta": -10.0}),
    );
    check(Command::StatusReq, json!({"type": "StatusReq"}));
    check(
        Command::StatusWait { timeout_ms: 5000 },
        json!({"type": "StatusWait", "timeout_ms": 5000}),
    );
    check(
        Command::VolumeSet {
            level: Some(50.0),
            mute: Some(false),
            delta_pct: Some(10),
        },
        json!({"type": "VolumeSet", "level": 50.0, "mute": false, "deltaPct": 10}),
    );
    check(
        Command::VolumeSet {
            level: None,
            mute: None,
            delta_pct: None,
        },
        json!({"type": "VolumeSet", "level": null, "mute": null, "deltaPct": null}),
    );
    check(Command::Shutdown, json!({"type": "Shutdown"}));
    check(Command::Info, json!({"type": "Info"}));
}

#[test]
fn queue_commands_match_cpp_schema() {
    check(
        Command::QueueList {
            order: "playback".to_string(),
        },
        json!({"type": "QueueList", "order": "playback"}),
    );
    check(Command::QueueQueues, json!({"type": "QueueQueues"}));
    check(
        Command::QueueCreate {
            name: "mix".to_string(),
        },
        json!({"type": "QueueCreate", "name": "mix"}),
    );
    check(
        Command::QueueDelete { qid: 2 },
        json!({"type": "QueueDelete", "qid": 2}),
    );
    check(
        Command::PlayFiles {
            paths: vec!["/music/a.mp3".to_string()],
            save: false,
        },
        json!({"type": "PlayFiles", "paths": ["/music/a.mp3"], "save": false}),
    );
    check(
        Command::QueueSwitch { qid: 2 },
        json!({"type": "QueueSwitch", "qid": 2}),
    );
    check(
        Command::QueueAdd {
            query: "song.mp3".to_string(),
            search: false,
        },
        json!({"type": "QueueAdd", "query": "song.mp3", "search": false}),
    );
    check(
        Command::QueueAddBatch {
            queries: vec!["12".to_string(), "13".to_string()],
            search: false,
        },
        json!({"type": "QueueAddBatch", "queries": ["12", "13"], "search": false}),
    );
    check(
        Command::QueueRemove {
            id_or_index: "3".to_string(),
        },
        json!({"type": "QueueRemove", "idOrIndex": "3"}),
    );
    check(
        Command::QueueRemoveBatch {
            selectors: vec!["#pos:2".to_string(), "#id:7".to_string()],
        },
        json!({"type": "QueueRemoveBatch", "selectors": ["#pos:2", "#id:7"]}),
    );
    check(
        Command::QueueMove { from: 1, to: 4 },
        json!({"type": "QueueMove", "from": 1, "to": 4}),
    );
    check(Command::QueueClear, json!({"type": "QueueClear"}));
    check(
        Command::QueueShuffle { on: Some(true) },
        json!({"type": "QueueShuffle", "on": true}),
    );
    check(
        Command::QueueShuffle { on: None },
        json!({"type": "QueueShuffle", "on": null}),
    );
    check(
        Command::QueueRepeat {
            mode: Some(RepeatMode::All),
        },
        json!({"type": "QueueRepeat", "mode": "All"}),
    );
    check(
        Command::QueueRepeat { mode: None },
        json!({"type": "QueueRepeat", "mode": null}),
    );
}

#[test]
fn playlist_commands_match_cpp_schema() {
    check(Command::PlaylistList, json!({"type": "PlaylistList"}));
    check(
        Command::PlaylistTracks { pid: 1 },
        json!({"type": "PlaylistTracks", "pid": 1}),
    );
    check(
        Command::PlaylistCreate {
            name: "mix".to_string(),
            smart_query: String::new(),
        },
        json!({"type": "PlaylistCreate", "name": "mix", "smart_query": ""}),
    );
    check(
        Command::PlaylistAdd {
            pid: 1,
            track_id: 7,
        },
        json!({"type": "PlaylistAdd", "pid": 1, "track_id": 7}),
    );
    check(
        Command::PlaylistLoad {
            pid: 1,
            play: true,
            replace: false,
        },
        json!({"type": "PlaylistLoad", "pid": 1, "play": true, "replace": false}),
    );
    check(
        Command::PlaylistSave {
            name: "mix".to_string(),
            queue_id: Some(3),
        },
        json!({"type": "PlaylistSave", "name": "mix", "queue_id": 3}),
    );
    check(
        Command::PlaylistSave {
            name: "mix".to_string(),
            queue_id: None,
        },
        json!({"type": "PlaylistSave", "name": "mix", "queue_id": null}),
    );
    check(
        Command::PlaylistDelete { pid: 1 },
        json!({"type": "PlaylistDelete", "pid": 1}),
    );
    check(
        Command::PlaylistRename {
            pid: 1,
            new_name: "New Name".to_string(),
        },
        json!({"type": "PlaylistRename", "pid": 1, "newName": "New Name"}),
    );
    check(
        Command::PlaylistExport {
            pid: 1,
            path: "/tmp/p.m3u".to_string(),
            format: "m3u".to_string(),
        },
        json!({"type": "PlaylistExport", "pid": 1, "path": "/tmp/p.m3u", "format": "m3u"}),
    );
    check(
        Command::PlaylistImport {
            path: "/tmp/p.m3u".to_string(),
            name: Some("Imported".to_string()),
        },
        json!({"type": "PlaylistImport", "path": "/tmp/p.m3u", "name": "Imported"}),
    );
    check(
        Command::PlaylistImport {
            path: "/tmp/p.m3u".to_string(),
            name: None,
        },
        json!({"type": "PlaylistImport", "path": "/tmp/p.m3u", "name": null}),
    );
}

#[test]
fn library_commands_match_cpp_schema() {
    check(
        Command::LibraryScan {
            path: Some("/music".to_string()),
            full_hash: true,
        },
        json!({"type": "LibraryScan", "path": "/music", "full_hash": true}),
    );
    check(
        Command::LibraryScan {
            path: None,
            full_hash: false,
        },
        json!({"type": "LibraryScan", "path": null, "full_hash": false}),
    );
    check(
        Command::LibraryAnalyze {
            reanalyze: true,
            library_id: Some(2),
        },
        json!({"type": "LibraryAnalyze", "reanalyze": true, "library_id": 2}),
    );
    check(
        Command::LibraryAnalyze {
            reanalyze: false,
            library_id: None,
        },
        json!({"type": "LibraryAnalyze", "reanalyze": false, "library_id": null}),
    );
    check(
        Command::LibrarySearch {
            query: "hello".to_string(),
            limit: 50,
        },
        json!({"type": "LibrarySearch", "query": "hello", "limit": 50}),
    );
    check(
        Command::LibraryStats {
            top_tracks: Some(10),
            top_albums: Some(5),
            top_genres: Some(5),
            top_artists: Some(5),
        },
        json!({
            "type": "LibraryStats",
            "top_tracks": 10,
            "top_albums": 5,
            "top_genres": 5,
            "top_artists": 5
        }),
    );
    check(
        Command::LibraryStats {
            top_tracks: None,
            top_albums: None,
            top_genres: None,
            top_artists: None,
        },
        json!({"type": "LibraryStats"}),
    );
    check(
        Command::LibraryAdd {
            path: "/music".to_string(),
            recursive: true,
        },
        json!({"type": "LibraryAdd", "path": "/music", "recursive": true}),
    );
    check(
        Command::LibraryRemove {
            query: "gone".to_string(),
        },
        json!({"type": "LibraryRemove", "query": "gone"}),
    );
    check(
        Command::LibraryList {
            query: Some("q".to_string()),
            limit: 50,
            offset: 0,
            sort: "title".to_string(),
            order: "asc".to_string(),
            artist: Some("A".to_string()),
            album: None,
            genre: None,
            rating: Some(5),
        },
        json!({
            "type": "LibraryList",
            "query": "q",
            "limit": 50,
            "offset": 0,
            "sort": "title",
            "order": "asc",
            "artist": "A",
            "album": null,
            "genre": null,
            "rating": 5
        }),
    );
    check(
        Command::LibraryMaintenance {
            action: "report".to_string(),
            dry_run: true,
            library_id: 1,
        },
        json!({
            "type": "LibraryMaintenance",
            "action": "report",
            "dry_run": true,
            "library_id": 1
        }),
    );
}

#[test]
fn tag_commands_match_cpp_schema() {
    check(
        Command::TagEdit {
            id: 3,
            field: "artist".to_string(),
            value: "A".to_string(),
        },
        json!({"type": "TagEdit", "id": 3, "field": "artist", "value": "A"}),
    );
    check(
        Command::TagGet {
            id: Some(3),
            path: None,
            cover: true,
        },
        json!({"type": "TagGet", "id": 3, "cover": true}),
    );
    check(
        Command::TagGet {
            id: None,
            path: Some("/music/a.mp3".to_string()),
            cover: false,
        },
        json!({"type": "TagGet", "path": "/music/a.mp3", "cover": false}),
    );
    check(
        Command::TagBatchEdit {
            dry_run: false,
            entries: vec![
                TagBatchEditEntry {
                    id: Some(1),
                    path: None,
                    field: "artist".to_string(),
                    value: "A".to_string(),
                },
                TagBatchEditEntry {
                    id: None,
                    path: Some("/music/a.mp3".to_string()),
                    field: "album".to_string(),
                    value: "B".to_string(),
                },
            ],
        },
        json!({
            "type": "TagBatchEdit",
            "dry_run": false,
            "entries": [
                {"id": 1, "field": "artist", "value": "A"},
                {"path": "/music/a.mp3", "field": "album", "value": "B"}
            ]
        }),
    );
    check(
        Command::TrackRate {
            track_id: 3,
            rating: 5,
        },
        json!({"type": "TrackRate", "track_id": 3, "rating": 5}),
    );
}

#[test]
fn config_history_bookmark_and_lyrics_commands_match_cpp_schema() {
    check(
        Command::ConfigGet {
            key: "k".to_string(),
        },
        json!({"type": "ConfigGet", "key": "k"}),
    );
    check(
        Command::ConfigSet {
            key: "k".to_string(),
            value: "v".to_string(),
        },
        json!({"type": "ConfigSet", "key": "k", "value": "v"}),
    );
    check(Command::ConfigList, json!({"type": "ConfigList"}));
    check(
        Command::ConfigExport {
            path: "/tmp/c.json".to_string(),
        },
        json!({"type": "ConfigExport", "path": "/tmp/c.json"}),
    );
    check(
        Command::ConfigImport {
            path: "/tmp/c.json".to_string(),
        },
        json!({"type": "ConfigImport", "path": "/tmp/c.json"}),
    );
    check(
        Command::ConfigReset {
            key: Some("k".to_string()),
        },
        json!({"type": "ConfigReset", "key": "k"}),
    );
    check(
        Command::ConfigReset { key: None },
        json!({"type": "ConfigReset", "key": null}),
    );
    check(
        Command::HistoryList { limit: Some(10) },
        json!({"type": "HistoryList", "limit": 10}),
    );
    check(
        Command::HistoryList { limit: None },
        json!({"type": "HistoryList", "limit": null}),
    );
    check(Command::HistoryClear, json!({"type": "HistoryClear"}));
    check(
        Command::BookmarkList { track_id: Some(3) },
        json!({"type": "BookmarkList", "track_id": 3}),
    );
    check(
        Command::BookmarkList { track_id: None },
        json!({"type": "BookmarkList", "track_id": null}),
    );
    check(
        Command::BookmarkAdd {
            position_ms: 1234,
            note: "intro".to_string(),
            track_id: Some(3),
        },
        json!({
            "type": "BookmarkAdd",
            "position_ms": 1234,
            "note": "intro",
            "track_id": 3
        }),
    );
    check(
        Command::BookmarkRemove { id: 7 },
        json!({"type": "BookmarkRemove", "id": 7}),
    );
    check(
        Command::BookmarkGoto { id: 7 },
        json!({"type": "BookmarkGoto", "id": 7}),
    );
    check(
        Command::LyricsGet { track_id: Some(3) },
        json!({"type": "LyricsGet", "track_id": 3}),
    );
    check(
        Command::LyricsSet {
            lrc_text: "[00:12.34]Hi".to_string(),
            is_synced: true,
            source: "import".to_string(),
            track_id: Some(3),
        },
        json!({
            "type": "LyricsSet",
            "lrc_text": "[00:12.34]Hi",
            "is_synced": true,
            "source": "import",
            "track_id": 3
        }),
    );
    check(
        Command::LyricsRemove { track_id: None },
        json!({"type": "LyricsRemove", "track_id": null}),
    );
}

#[test]
fn device_and_preview_commands_match_cpp_schema() {
    check(
        Command::Preview {
            file: "/a.wav".to_string(),
        },
        json!({"type": "Preview", "file": "/a.wav"}),
    );
    check(Command::DeviceList, json!({"type": "DeviceList"}));
    check(
        Command::DeviceSet {
            id: "dev-1".to_string(),
        },
        json!({"type": "DeviceSet", "id": "dev-1"}),
    );
    check(
        Command::DeviceTest {
            id: Some("dev-1".to_string()),
            tone: true,
        },
        json!({"type": "DeviceTest", "id": "dev-1", "tone": true}),
    );
    check(
        Command::DeviceTest {
            id: None,
            tone: false,
        },
        json!({"type": "DeviceTest", "id": null, "tone": false}),
    );
}

#[test]
fn request_envelope_wraps_command_with_id() {
    let request = encode_request(7, &Command::StatusReq).expect("encode");
    assert_eq!(request, json!({"id": 7, "cmd": {"type": "StatusReq"}}));
}
