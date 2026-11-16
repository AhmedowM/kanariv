//! Behavioural tests for the Rust store, mirroring the C++ `test_db*` suites.

use kanariv_db::{DbError, SourceKind, SourceRef, Store, TrackRow};

fn fingerprint(seed: &str) -> [u8; 32] {
    let mut out = [0_u8; 32];
    for (slot, byte) in out.iter_mut().zip(seed.bytes()) {
        *slot = byte;
    }
    out
}

fn track(path: &str, title: &str) -> TrackRow {
    TrackRow {
        path: path.to_owned(),
        title: title.to_owned(),
        fingerprint: fingerprint(path),
        duration: 3.5,
        ..TrackRow::default()
    }
}

fn insert(store: &Store, path: &str, title: &str) -> i64 {
    store.insert_track(&track(path, title)).expect("insert")
}

#[test]
fn track_round_trip_and_lookups() {
    let store = Store::open_in_memory().unwrap();
    let mut t = track("C:/music/a.mp3", "Alpha");
    t.library_id = 0;
    t.source = SourceRef {
        kind: SourceKind::Path,
        ..SourceRef::default()
    };
    let id = store.insert_track(&t).unwrap();

    let got = store.get_track(id).unwrap();
    assert_eq!(got.library_id, 1, "zero library id normalizes to 1");
    assert_eq!(got.source.key, "C:/music/a.mp3");
    assert_eq!(got.source.display_name, "C:/music/a.mp3");
    assert_eq!(got.date_added, 0, "missing date is stored as NULL");
    assert_eq!(got.fingerprint, t.fingerprint);

    assert_eq!(store.find_by_path("C:/music/a.mp3").unwrap().id, id);
    assert_eq!(store.find_by_fingerprint(&t.fingerprint).unwrap().id, id);
    let source = SourceRef {
        kind: SourceKind::Path,
        key: "C:/music/a.mp3".to_owned(),
        ..SourceRef::default()
    };
    assert_eq!(store.find_by_source(&source).unwrap().id, id);
}

#[test]
fn duplicate_fingerprint_is_already_exists() {
    let store = Store::open_in_memory().unwrap();
    let t = track("C:/music/a.mp3", "Alpha");
    store.insert_track(&t).unwrap();

    let mut duplicate = t.clone();
    duplicate.path = "C:/music/elsewhere.mp3".to_owned();
    assert!(matches!(
        store.insert_track(&duplicate),
        Err(DbError::AlreadyExists(_))
    ));
}

#[test]
fn missing_and_invalid_track_operations() {
    let store = Store::open_in_memory().unwrap();
    assert!(matches!(store.get_track(99), Err(DbError::NotFound(_))));
    assert!(matches!(store.delete_track(99), Err(DbError::NotFound(_))));
    assert!(matches!(
        store.set_dirty(99, true),
        Err(DbError::NotFound(_))
    ));
    assert!(matches!(store.set_rating(99, 3), Err(DbError::NotFound(_))));

    let id = insert(&store, "C:/music/a.mp3", "Alpha");
    assert!(matches!(
        store.set_rating(id, 6),
        Err(DbError::InvalidArg(_))
    ));
    store.set_rating(id, 5).unwrap();
    assert_eq!(store.get_track(id).unwrap().rating, 5);
}

#[test]
fn search_uses_fts_then_prefix_then_like() {
    let store = Store::open_in_memory().unwrap();
    insert(&store, "C:/a.flac", "Abbey Road");
    insert(&store, "C:/b.flac", "Hello");
    insert(&store, "C:/c.flac", "Fuzzbuzz");
    insert(&store, "C:/d.flac", "100% Pure");

    let exact = store.search_tracks("Abbey Road", 10).unwrap();
    assert_eq!(exact.len(), 1);
    assert_eq!(exact[0].title, "Abbey Road");

    let prefix = store.search_tracks("Abb", 10).unwrap();
    assert_eq!(prefix.len(), 1, "prefix stage matches the token");

    let middle = store.search_tracks("zzb", 10).unwrap();
    assert_eq!(middle.len(), 1, "LIKE fallback matches a substring");
    assert_eq!(middle[0].title, "Fuzzbuzz");

    let literal_percent = store.search_tracks("100%", 10).unwrap();
    assert_eq!(literal_percent.len(), 1, "LIKE escapes % literally");

    let operators = store.search_tracks("Hello OR AND NOT", 10).unwrap();
    assert_eq!(operators.len(), 1, "operator words are stripped");

    assert!(store.search_tracks("", 10).unwrap().is_empty());
    assert!(store.search_tracks("   ", 10).unwrap().is_empty());
}

#[test]
fn search_uses_fts_stemming_not_only_like() {
    let store = Store::open_in_memory().unwrap();
    insert(&store, "C:/stem.flac", "Happy");
    // Porter stems "happiness" and "Happy" to the same root; the LIKE
    // fallback would not match, so this only succeeds when the FTS stage runs.
    let hits = store.search_tracks("happiness", 10).unwrap();
    assert_eq!(hits.len(), 1);
    assert_eq!(hits[0].title, "Happy");
}

#[test]
fn search_matches_quoted_phrase() {
    let store = Store::open_in_memory().unwrap();
    insert(&store, "C:/abbey.flac", "Abbey Road");
    let hits = store.search_tracks("\"Abbey Road\"", 10).unwrap();
    assert_eq!(hits.len(), 1);
}

#[test]
fn fts_triggers_follow_update_and_delete() {
    let store = Store::open_in_memory().unwrap();
    let id = insert(&store, "C:/music/x.mp3", "Oldname");
    assert_eq!(store.search_tracks("Oldname", 10).unwrap().len(), 1);

    let mut updated = store.get_track(id).unwrap();
    updated.title = "Newname".to_owned();
    store.update_track(&updated).unwrap();
    assert!(store.search_tracks("Oldname", 10).unwrap().is_empty());
    assert_eq!(store.search_tracks("Newname", 10).unwrap().len(), 1);

    store.delete_track(id).unwrap();
    assert!(store.search_tracks("Newname", 10).unwrap().is_empty());
}

#[test]
fn queue_uses_position_gaps_and_midpoint_insertion() {
    let store = Store::open_in_memory().unwrap();
    let a = insert(&store, "C:/a.mp3", "A");
    let b = insert(&store, "C:/b.mp3", "B");
    let c = insert(&store, "C:/c.mp3", "C");
    let d = insert(&store, "C:/d.mp3", "D");

    let default = store.get_queue(0).unwrap();
    assert_eq!((default.id, default.name.as_str()), (1, "default"));

    let qid = store.create_queue("mix", 1).unwrap();
    store.enqueue_batch(qid, &[a, b, c]).unwrap();
    let positions: Vec<i64> = store
        .list_queue_items(qid)
        .unwrap()
        .iter()
        .map(|item| item.position)
        .collect();
    assert_eq!(positions, vec![0, 1_000_000_000, 2_000_000_000]);

    store.enqueue(qid, d, 1).unwrap();
    let positions: Vec<i64> = store
        .list_queue_items(qid)
        .unwrap()
        .iter()
        .map(|item| item.position)
        .collect();
    assert_eq!(
        positions,
        vec![0, 500_000_000, 1_000_000_000, 2_000_000_000]
    );
    assert_eq!(store.queue_index_of(qid, d).unwrap(), Some(1));
    assert_eq!(store.queue_count(qid).unwrap(), 4);

    store.remove_at_position(qid, 500_000_000).unwrap();
    assert!(matches!(
        store.remove_at_position(qid, 500_000_000),
        Err(DbError::NotFound(_))
    ));
    store.clear_queue(qid).unwrap();
    assert_eq!(store.queue_count(qid).unwrap(), 0);
}

#[test]
fn queue_batch_rolls_back_on_failure() {
    let store = Store::open_in_memory().unwrap();
    let a = insert(&store, "C:/a.mp3", "A");
    let qid = store.create_queue("mix", 1).unwrap();

    assert!(store.enqueue_batch(qid, &[a, 99_999]).is_err());
    assert_eq!(store.queue_count(qid).unwrap(), 0, "batch is atomic");
}

#[test]
fn queue_metadata_updates() {
    let store = Store::open_in_memory().unwrap();
    let qid = store.create_queue("mix", 1).unwrap();
    store.set_queue_repeat(qid, 2).unwrap();
    store.set_queue_temp(qid, true).unwrap();
    let queue = store.get_queue(qid).unwrap();
    assert_eq!(queue.repeat_mode, 2);
    assert!(queue.temp);

    assert!(matches!(
        store.set_queue_repeat(999, 1),
        Err(DbError::NotFound(_))
    ));
}

#[test]
fn playlist_add_reorder_and_delete() {
    let store = Store::open_in_memory().unwrap();
    let a = insert(&store, "C:/a.mp3", "A");
    let b = insert(&store, "C:/b.mp3", "B");
    let pid = store.create_playlist("mix", 0, "").unwrap();
    store.playlist_add_track(pid, a, -1).unwrap();
    store.playlist_add_track(pid, b, -1).unwrap();

    let titles: Vec<String> = store
        .playlist_tracks(pid)
        .unwrap()
        .into_iter()
        .map(|track| track.title)
        .collect();
    assert_eq!(titles, vec!["A", "B"]);

    store.playlist_reorder(pid, 0, 1).unwrap();
    let titles: Vec<String> = store
        .playlist_tracks(pid)
        .unwrap()
        .into_iter()
        .map(|track| track.title)
        .collect();
    assert_eq!(titles, vec!["B", "A"]);

    store.rename_playlist(pid, "renamed").unwrap();
    assert_eq!(store.get_playlist(pid).unwrap().name, "renamed");

    assert!(matches!(
        store.create_playlist("", 0, ""),
        Err(DbError::InvalidArg(_))
    ));

    store.delete_playlist(pid).unwrap();
    assert!(matches!(store.get_playlist(pid), Err(DbError::NotFound(_))));
}

#[test]
fn history_updates_play_count_and_stats() {
    let store = Store::open_in_memory().unwrap();
    let id = insert(&store, "C:/a.mp3", "A");

    store.mark_played(id, 100, 200, 3_500, 100.0, 0).unwrap();
    store.mark_played(id, 300, 400, 3_500, 100.0, 0).unwrap();

    let track = store.get_track(id).unwrap();
    assert_eq!(track.play_count, 2);
    assert_eq!(track.last_played, 400);

    let history = store.history_list(10).unwrap();
    assert_eq!(history.len(), 2);
    assert_eq!(history[0].started_at, 300, "newest first");
    assert_eq!(history[0].title, "A");

    let stats = store.stats().unwrap();
    assert_eq!(stats.num_tracks, 1);
    assert_eq!(stats.num_history, 2);
    assert_eq!(stats.total_duration_ms, 3_500);

    store.history_clear().unwrap();
    assert_eq!(store.stats().unwrap().num_history, 0);
}

#[test]
fn bookmarks_and_lyrics_round_trip() {
    let store = Store::open_in_memory().unwrap();
    let id = insert(&store, "C:/a.mp3", "A");

    store.bookmark_add(id, 2_000, "chorus").unwrap();
    store.bookmark_add(id, 1_000, "intro").unwrap();
    let marks = store.list_bookmarks(id).unwrap();
    assert_eq!(marks.len(), 2);
    assert_eq!(marks[0].position_ms, 1_000);
    store.bookmark_remove(marks[0].id).unwrap();
    assert_eq!(store.list_bookmarks(id).unwrap().len(), 1);

    store.lyrics_set(id, "[00:01.00]hi", true, "local").unwrap();
    store
        .lyrics_set(id, "[00:02.00]hi again", true, "import")
        .unwrap();
    let lyrics = store.lyrics_get(id).unwrap();
    assert_eq!(lyrics.source, "import");
    assert_eq!(lyrics.lrc_text, "[00:02.00]hi again");
    store.lyrics_remove(id).unwrap();
    assert!(matches!(store.lyrics_get(id), Err(DbError::NotFound(_))));
}

#[test]
fn deleting_a_track_cascades() {
    let store = Store::open_in_memory().unwrap();
    let id = insert(&store, "C:/a.mp3", "A");
    let qid = store.create_queue("mix", 1).unwrap();
    store.enqueue(qid, id, -1).unwrap();
    store.bookmark_add(id, 1_000, "").unwrap();
    store.lyrics_set(id, "text", false, "local").unwrap();
    let pid = store.create_playlist("mix", 0, "").unwrap();
    store.playlist_add_track(pid, id, -1).unwrap();
    store.mark_played(id, 1, 2, 3, 100.0, qid).unwrap();

    store.delete_track(id).unwrap();
    assert_eq!(store.queue_count(qid).unwrap(), 0);
    assert!(store.list_bookmarks(id).unwrap().is_empty());
    assert!(store.playlist_tracks(pid).unwrap().is_empty());
    assert_eq!(store.stats().unwrap().num_history, 0);
}

#[test]
fn default_library_exists() {
    let store = Store::open_in_memory().unwrap();
    let libraries = store.list_libraries().unwrap();
    assert_eq!(libraries.len(), 1);
    assert_eq!(libraries[0].id, 1);
    assert_eq!(libraries[0].path, "");
}
