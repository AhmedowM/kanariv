//! Reply fixtures for every result kind, mirroring `src/ipc/protocol.cpp`.

#![allow(clippy::needless_pass_by_value)]

use kanariv_ipc::{parse_reply, Payload, PlaybackState, RepeatMode, Reply, StatusCode};
use serde_json::json;

fn parse(value: serde_json::Value) -> Reply {
    Payload::from_json(&value).expect("reply parses").reply
}

#[test]
fn status_parses_both_spellings() {
    let reply = parse(json!({
        "type": "Status",
        "state": "Playing",
        "state_value": 2,
        "pos": 1.5,
        "dur": 10.0,
        "vol": 0.5,
        "muted": false,
        "shuffle": true,
        "repeat": "All",
        "repeat_value": 1,
        "track_id": 4,
        "title": "T",
        "artist": "A",
        "path": "/p",
        "q_size": 3,
        "q_idx": 1,
        "version": "v2"
    }));
    let Reply::Status(status) = reply else {
        panic!("expected Status");
    };
    assert_eq!(status.state, PlaybackState::Playing);
    assert_eq!(status.repeat, RepeatMode::All);
    assert!(status.is_playing());
    assert_eq!(status.q_idx, 1);
    assert_eq!(status.version, "v2");

    let reply = parse(json!({"type": "Status", "state_value": 3, "repeat_value": 2}));
    let Reply::Status(status) = reply else {
        panic!("expected Status");
    };
    assert_eq!(status.state, PlaybackState::Paused);
    assert_eq!(status.repeat, RepeatMode::One);
}

#[test]
fn status_rejects_unknown_spellings() {
    let err = Payload::from_json(&json!({"type": "Status", "state": "Nope"})).unwrap_err();
    assert_eq!(err.code, StatusCode::InvalidArg);
    let err = Payload::from_json(&json!({"type": "Status", "repeat": "Nope"})).unwrap_err();
    assert_eq!(err.code, StatusCode::InvalidArg);
}

#[test]
fn queue_replies_parse() {
    let Reply::QueueTracks(tracks) = parse(json!({
        "type": "QueueTracks",
        "tracks": [{"id": 1, "title": "A", "path": "/a.mp3"}]
    })) else {
        panic!("expected QueueTracks");
    };
    assert_eq!(tracks.len(), 1);
    assert_eq!(tracks[0].title, "A");

    let Reply::Queues(queues) = parse(json!({
        "type": "Queues",
        "queues": [{"id": 1, "name": "q", "tracks": 3, "active": true, "temp": false}]
    })) else {
        panic!("expected Queues");
    };
    assert_eq!(queues[0].tracks, 3);
    assert!(queues[0].active);

    let Reply::QueueCreated { id, name } =
        parse(json!({"type": "QueueCreated", "id": 3, "name": "mix"}))
    else {
        panic!("expected QueueCreated");
    };
    assert_eq!((id, name.as_str()), (3, "mix"));
}

#[test]
fn playlist_loaded_embeds_status() {
    let Reply::PlaylistLoaded { queue_id, status } = parse(json!({
        "type": "PlaylistLoaded",
        "queue_id": 9,
        "status": {"type": "Status", "state": "Paused", "track_id": 5}
    })) else {
        panic!("expected PlaylistLoaded");
    };
    assert_eq!(queue_id, 9);
    assert_eq!(status.state, PlaybackState::Paused);
    assert_eq!(status.track_id, 5);
}

#[test]
fn volume_and_stats_replies_parse() {
    let Reply::VolumeInfo { vol, muted } =
        parse(json!({"type": "VolumeInfo", "vol": 0.25, "muted": true}))
    else {
        panic!("expected VolumeInfo");
    };
    assert!((vol - 0.25).abs() < f32::EPSILON);
    assert!(muted);

    let Reply::LibraryStats(stats) = parse(json!({
        "type": "LibraryStats",
        "tracks": 10,
        "queues": 4,
        "playlists": 2,
        "total_duration_ms": 1000,
        "total_play_time_ms": 500,
        "top_tracks": [{"id": 1}],
        "top_albums": [{"name": "A", "track_count": 2, "total_plays": 6}],
        "top_genres": [],
        "top_artists": [{"name": "X", "track_count": 1, "total_plays": 3}]
    })) else {
        panic!("expected LibraryStats");
    };
    assert_eq!(stats.tracks, 10);
    assert_eq!(stats.top_tracks.as_ref().map(Vec::len), Some(1));
    assert_eq!(stats.top_albums.as_ref().unwrap()[0].total_plays, 6);
    assert!(stats.top_genres.as_ref().unwrap().is_empty());
}

#[test]
fn detailed_stats_alias_engages_empty_top_tracks() {
    let Reply::LibraryStats(stats) = parse(json!({"type": "LibraryStatsDetailed", "tracks": 2}))
    else {
        panic!("expected LibraryStats");
    };
    assert_eq!(stats.top_tracks, Some(Vec::new()));
    assert!(stats.top_albums.is_none());
}

#[test]
fn library_replies_parse() {
    let Reply::Tracks(tracks) = parse(json!({"type": "Tracks", "tracks": [{"id": 2}]})) else {
        panic!("expected Tracks");
    };
    assert_eq!(tracks[0].id, 2);

    let Reply::SearchResults { query, tracks } = parse(json!({
        "type": "SearchResults",
        "query": "hello",
        "tracks": [{"id": 2}]
    })) else {
        panic!("expected SearchResults");
    };
    assert_eq!(query, "hello");
    assert_eq!(tracks.len(), 1);

    let Reply::ScanReport { added } = parse(json!({"type": "ScanReport", "added": 7})) else {
        panic!("expected ScanReport");
    };
    assert_eq!(added, 7);

    let Reply::SingleTrack {
        track,
        cover_mime,
        cover_data,
    } = parse(json!({
        "type": "SingleTrack",
        "track": {"id": 3},
        "cover_mime": "image/jpeg",
        "cover_data": "AA=="
    }))
    else {
        panic!("expected SingleTrack");
    };
    assert_eq!(track.id, 3);
    assert_eq!(cover_mime.as_deref(), Some("image/jpeg"));
    assert_eq!(cover_data.as_deref(), Some("AA=="));

    let Reply::TrackInfo {
        track,
        play_count,
        last_played,
    } = parse(json!({
        "type": "TrackInfo",
        "track": {"id": 4},
        "play_count": 9,
        "last_played": 123
    }))
    else {
        panic!("expected TrackInfo");
    };
    assert_eq!((track.id, play_count, last_played), (4, 9, 123));
}

#[test]
fn track_normalization_matches_cpp() {
    let Reply::Tracks(tracks) = parse(json!({
        "type": "Tracks",
        "tracks": [{"id": 1, "path": "/a.mp3", "library_id": 0}]
    })) else {
        panic!("expected Tracks");
    };
    assert_eq!(tracks[0].library_id, 1);
    assert_eq!(tracks[0].source.kind, "path");
    assert_eq!(tracks[0].source.key, "/a.mp3");
    assert_eq!(tracks[0].source.display_name, "/a.mp3");
    assert!(tracks[0].source.available);
}

#[test]
fn playlist_replies_parse() {
    let Reply::Playlists(playlists) = parse(json!({
        "type": "Playlists",
        "playlists": [{
            "id": 1,
            "name": "mix",
            "type": 0,
            "smart_query": "",
            "created": 10,
            "modified": 20,
            "library_id": 1
        }]
    })) else {
        panic!("expected Playlists");
    };
    assert_eq!(playlists[0].name, "mix");
    assert_eq!(playlists[0].kind, 0);

    let Reply::PlaylistData { tracks, format } = parse(json!({
        "type": "PlaylistData",
        "tracks": [{"id": 1}],
        "format": "m3u"
    })) else {
        panic!("expected PlaylistData");
    };
    assert_eq!(tracks.len(), 1);
    assert_eq!(format, "m3u");

    let Reply::PlaylistCreated { id, name } =
        parse(json!({"type": "PlaylistCreated", "id": 2, "name": "p"}))
    else {
        panic!("expected PlaylistCreated");
    };
    assert_eq!((id, name.as_str()), (2, "p"));

    let Reply::PlaylistImportReport {
        pid,
        name,
        tracks,
        matched,
        skipped,
        duplicates,
    } = parse(json!({
        "type": "PlaylistImportReport",
        "pid": 2,
        "name": "p",
        "tracks": [{"id": 1}],
        "matched": 1,
        "skipped": 2,
        "duplicates": 3
    }))
    else {
        panic!("expected PlaylistImportReport");
    };
    assert_eq!((pid, name.as_str()), (2, "p"));
    assert_eq!(tracks.len(), 1);
    assert_eq!((matched, skipped, duplicates), (1, 2, 3));
}

#[test]
fn maintenance_and_config_replies_parse() {
    let Reply::LibraryMaintenanceReport {
        tracks_checked,
        missing_tracks,
        duplicate_groups,
        duplicate_tracks,
    } = parse(json!({
        "type": "LibraryMaintenanceReport",
        "tracks_checked": 5,
        "missing_tracks": [{"id": 1}],
        "duplicate_groups": 1,
        "duplicate_tracks": 2
    }))
    else {
        panic!("expected LibraryMaintenanceReport");
    };
    assert_eq!(tracks_checked, 5);
    assert_eq!(missing_tracks.len(), 1);
    assert_eq!((duplicate_groups, duplicate_tracks), (1, 2));

    let Reply::ConfigValue(value) = parse(json!({"type": "ConfigValue", "key": "k", "value": "v"}))
    else {
        panic!("expected ConfigValue");
    };
    assert_eq!((value.key.as_str(), value.value.as_str()), ("k", "v"));

    let Reply::ConfigValues(values) = parse(json!({
        "type": "ConfigValues",
        "values": [{"key": "a", "value": "1"}, {"key": "b", "value": "2"}]
    })) else {
        panic!("expected ConfigValues");
    };
    assert_eq!(values.len(), 2);
}

#[test]
fn history_bookmarks_and_lyrics_replies_parse() {
    let Reply::HistoryEntry(entry) = parse(json!({
        "type": "HistoryEntry",
        "id": 1,
        "track_id": 2,
        "started_at": 10,
        "completed_at": 20,
        "position_ms": 30,
        "completion_pct": 50.0,
        "queue_id": 1,
        "title": "T",
        "artist": "A",
        "path": "/p",
        "duration": 12.5
    })) else {
        panic!("expected HistoryEntry");
    };
    assert_eq!(entry.track_id, 2);
    assert!((entry.completion_pct - 50.0).abs() < f64::EPSILON);

    let Reply::History(entries) = parse(json!({
        "type": "History",
        "entries": [{"id": 1, "track_id": 2, "title": "T"}]
    })) else {
        panic!("expected History");
    };
    assert_eq!(entries[0].title, "T");

    let Reply::Bookmarks(entries) = parse(json!({
        "type": "Bookmarks",
        "entries": [{"id": 1, "track_id": 2, "position_ms": 3000, "note": "n", "created": 5}]
    })) else {
        panic!("expected Bookmarks");
    };
    assert_eq!(entries[0].position_ms, 3000);

    let Reply::Lyrics(lyrics) = parse(json!({
        "type": "Lyrics",
        "id": 1,
        "track_id": 2,
        "lrc_text": "[00:01.00]hi",
        "is_synced": true,
        "source": "local",
        "fetched_at": 9
    })) else {
        panic!("expected Lyrics");
    };
    assert!(lyrics.is_synced);
    assert_eq!(lyrics.lrc_text, "[00:01.00]hi");
}

#[test]
fn tag_batch_devices_and_empty_replies_parse() {
    let Reply::TagBatchEditResult {
        succeeded,
        failed,
        results,
    } = parse(json!({
        "type": "TagBatchEditResult",
        "succeeded": 1,
        "failed": 1,
        "results": [
            {"id": 1, "field": "artist", "ok": true, "error": ""},
            {"path": "/p", "field": "album", "ok": false, "error": "denied"}
        ]
    }))
    else {
        panic!("expected TagBatchEditResult");
    };
    assert_eq!((succeeded, failed), (1, 1));
    assert_eq!(results[1].path.as_deref(), Some("/p"));
    assert_eq!(results[1].error, "denied");

    let Reply::Devices(devices) = parse(json!({
        "type": "Devices",
        "devices": [{"id": "d", "name": "Speakers", "isDefault": true}]
    })) else {
        panic!("expected Devices");
    };
    assert!(devices[0].is_default);

    assert!(matches!(parse(json!({"type": "Empty"})), Reply::Empty));
    assert!(matches!(
        parse(json!({"type": "FutureResult", "anything": 1})),
        Reply::Other
    ));
}

#[test]
fn payload_preserves_raw_json() {
    let raw = json!({"type": "FutureResult", "extra": {"nested": true}});
    let payload = Payload::from_json(&raw).expect("parses");
    assert_eq!(payload.json, raw);
}

#[test]
fn tolerant_containers_match_cpp() {
    let Reply::QueueTracks(tracks) =
        parse(json!({"type": "QueueTracks", "tracks": "not-an-array"}))
    else {
        panic!("expected QueueTracks");
    };
    assert!(tracks.is_empty());
    let Reply::Tracks(tracks) = parse(json!({"type": "Tracks"})) else {
        panic!("expected Tracks");
    };
    assert!(tracks.is_empty());
}

#[test]
fn envelope_errors_map_to_wire_codes() {
    let err = parse_reply(
        br#"{"id":1,"ok":false,"error":{"code":"NotFound","code_value":2,"message":"nope"}}"#,
    )
    .unwrap_err();
    assert_eq!(err.code, StatusCode::NotFound);
    assert_eq!(err.message, "nope");

    let err = parse_reply(br#"{"id":1,"ok":false,"error":{"code_value":6,"message":"state"}}"#)
        .unwrap_err();
    assert_eq!(err.code, StatusCode::State);

    let err = parse_reply(br#"{"id":1,"ok":false,"error":{"message":"unknown"}}"#).unwrap_err();
    assert_eq!(err.code, StatusCode::Internal);

    let err = parse_reply(br#"{"id":1,"ok":true}"#).unwrap_err();
    assert_eq!(err.code, StatusCode::InvalidArg);

    let err = parse_reply(b"not json").unwrap_err();
    assert_eq!(err.code, StatusCode::Corrupt);
}

#[test]
fn ok_reply_parses_through_envelope() {
    let payload = parse_reply(
        br#"{"id":3,"ok":true,"result":{"type":"VolumeInfo","vol":0.5,"muted":false}}"#,
    )
    .expect("reply");
    assert!(matches!(
        payload.reply,
        Reply::VolumeInfo { muted: false, .. }
    ));
    assert_eq!(payload.json["type"], "VolumeInfo");
}

#[test]
fn missing_type_is_an_error() {
    let err = Payload::from_json(&json!({"tracks": []})).unwrap_err();
    assert_eq!(err.code, StatusCode::InvalidArg);
}
