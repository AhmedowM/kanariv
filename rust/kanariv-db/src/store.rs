//! SQLite-backed store over the shared kanariv schema.
//!
//! This is the behavioural half of the experiment: it mirrors the C++ database
//! for tracks, FTS-backed search, queues, playlists, history, bookmarks,
//! lyrics, libraries and statistics. Engine-state persistence is intentionally
//! out of scope here; see `rust/README.md` for the phase boundary.

use std::path::Path;

use rusqlite::types::ValueRef;
use rusqlite::{params, Connection, OptionalExtension, Row};

use crate::error::{DbError, Result};
use crate::model::{
    Bookmark, HistoryEntry, Library, Lyrics, PlaybackStats, Playlist, Queue, QueueItem, SourceRef,
    TrackRow,
};
use crate::schema::{INSERT_TRACK, SCHEMA, UPDATE_TRACK};

/// Gap between queue positions, matching `positionGap` in `src/db/queue.cpp`.
const POSITION_GAP: i64 = 1_000_000_000;

/// Canonical track column order (`internal::selectTracksCols`).
const TRACK_FIELDS: &str = "id, fingerprint, path, size, mtime, duration, \
     sample_rate, channels, bitrate, title, artist, album, album_artist, genre, year, \
     track_num, disc_num, cover_art_path, rating, play_count, last_played, date_added, \
     last_scanned, dirty, library_id, source_kind, source_key, source_available, \
     source_display_name, replay_gain_track, replay_gain_album";

/// SQLite store.
pub struct Store {
    conn: Connection,
}

impl Store {
    /// Opens (or creates) a database file and applies the shared schema.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        Self::init(Connection::open(path)?)
    }

    /// Creates an in-memory database for tests and experiments.
    pub fn open_in_memory() -> Result<Self> {
        Self::init(Connection::open_in_memory()?)
    }

    fn init(conn: Connection) -> Result<Self> {
        conn.execute_batch(SCHEMA)?;
        Ok(Self { conn })
    }

    /// Raw connection access for parity harnesses.
    #[must_use]
    pub fn connection(&self) -> &Connection {
        &self.conn
    }

    // ---------------------------------------------------------------- tracks

    /// Inserts a track and returns its row id.
    pub fn insert_track(&self, t: &TrackRow) -> Result<i64> {
        let source = t.source.normalized(&t.path);
        let date_added = (t.date_added != 0).then_some(t.date_added);
        let stored = self.conn.execute(
            INSERT_TRACK,
            params![
                &t.fingerprint[..],
                t.path,
                t.size,
                t.mtime,
                t.duration,
                i64::from(t.sample_rate),
                i64::from(t.channels),
                i64::from(t.bitrate),
                t.title,
                t.artist,
                t.album,
                t.album_artist,
                t.genre,
                i64::from(t.year),
                i64::from(t.track_num),
                i64::from(t.disc_num),
                t.cover_art_path,
                i64::from(t.rating),
                t.play_count,
                t.last_played,
                date_added,
                t.last_scanned,
                i64::from(t.dirty),
                t.normalized_library_id(),
                source.kind as i64,
                source.key,
                i64::from(source.available),
                source.display_name,
            ],
        );
        match stored {
            Ok(_) => Ok(self.conn.last_insert_rowid()),
            Err(err) if is_unique_violation(&err) => Err(DbError::AlreadyExists(err.to_string())),
            Err(err) => Err(err.into()),
        }
    }

    /// Updates all mutable columns of an existing track.
    pub fn update_track(&self, t: &TrackRow) -> Result<()> {
        if t.id == 0 {
            return Err(DbError::invalid_arg("id 0"));
        }
        let source = t.source.normalized(&t.path);
        let changed = self.conn.execute(
            UPDATE_TRACK,
            params![
                &t.fingerprint[..],
                t.path,
                t.size,
                t.mtime,
                t.duration,
                i64::from(t.sample_rate),
                i64::from(t.channels),
                i64::from(t.bitrate),
                t.title,
                t.artist,
                t.album,
                t.album_artist,
                t.genre,
                i64::from(t.year),
                i64::from(t.track_num),
                i64::from(t.disc_num),
                t.cover_art_path,
                i64::from(t.rating),
                t.play_count,
                t.last_played,
                t.date_added,
                t.last_scanned,
                i64::from(t.dirty),
                t.normalized_library_id(),
                source.kind as i64,
                source.key,
                i64::from(source.available),
                source.display_name,
                t.id,
            ],
        )?;
        if changed == 0 {
            return Err(DbError::not_found(format!("track {}", t.id)));
        }
        Ok(())
    }

    /// Loads a track by id.
    pub fn get_track(&self, id: i64) -> Result<TrackRow> {
        self.one_track(
            &format!("SELECT {TRACK_FIELDS} FROM tracks WHERE id=?"),
            [id],
        )?
        .ok_or_else(|| DbError::not_found(format!("track {id}")))
    }

    /// Finds a track by filesystem path (first row, as the C++ lookup does).
    pub fn find_by_path(&self, path: &str) -> Result<TrackRow> {
        self.one_track(
            &format!("SELECT {TRACK_FIELDS} FROM tracks WHERE path=?"),
            [path],
        )?
        .ok_or_else(|| DbError::not_found(format!("path {path}")))
    }

    /// Finds a track by its 32-byte content fingerprint.
    pub fn find_by_fingerprint(&self, fingerprint: &[u8; 32]) -> Result<TrackRow> {
        self.one_track(
            &format!("SELECT {TRACK_FIELDS} FROM tracks WHERE fingerprint=?"),
            [&fingerprint[..]],
        )?
        .ok_or_else(|| DbError::not_found("fingerprint"))
    }

    /// Finds a track by durable source identity.
    pub fn find_by_source(&self, source: &SourceRef) -> Result<TrackRow> {
        if source.key.is_empty() {
            return Err(DbError::invalid_arg("empty source key"));
        }
        self.one_track(
            &format!("SELECT {TRACK_FIELDS} FROM tracks WHERE source_kind=? AND source_key=?"),
            params![source.kind as i64, source.key],
        )?
        .ok_or_else(|| DbError::not_found("source"))
    }

    /// Deletes a track row (cascades to queue, playlist, history and lyrics).
    pub fn delete_track(&self, id: i64) -> Result<()> {
        if id == 0 {
            return Err(DbError::invalid_arg("id 0"));
        }
        let changed = self.conn.execute("DELETE FROM tracks WHERE id=?", [id])?;
        if changed == 0 {
            return Err(DbError::not_found(format!("track {id}")));
        }
        Ok(())
    }

    /// Sets the user rating (0..=5).
    pub fn set_rating(&self, id: i64, rating: i32) -> Result<()> {
        if id == 0 {
            return Err(DbError::invalid_arg("id 0"));
        }
        if !(0..=5).contains(&rating) {
            return Err(DbError::invalid_arg("rating must be 0..5"));
        }
        let changed = self
            .conn
            .execute("UPDATE tracks SET rating=? WHERE id=?", params![rating, id])?;
        if changed == 0 {
            return Err(DbError::not_found(format!("track {id}")));
        }
        Ok(())
    }

    /// Sets the dirty flag used by re-scan policies.
    pub fn set_dirty(&self, id: i64, dirty: bool) -> Result<()> {
        if id == 0 {
            return Err(DbError::invalid_arg("id 0"));
        }
        let changed = self.conn.execute(
            "UPDATE tracks SET dirty=? WHERE id=?",
            params![i64::from(dirty), id],
        )?;
        if changed == 0 {
            return Err(DbError::not_found(format!("track {id}")));
        }
        Ok(())
    }

    /// Lists live tracks, optionally scoped to one library, ordered by id.
    pub fn list_tracks(&self, library_id: Option<i64>) -> Result<Vec<TrackRow>> {
        let (sql, filter) = match library_id {
            Some(lib) => (
                format!("SELECT {TRACK_FIELDS} FROM tracks WHERE library_id=? ORDER BY id"),
                Some(lib),
            ),
            None => (
                format!("SELECT {TRACK_FIELDS} FROM tracks ORDER BY id"),
                None,
            ),
        };
        let mut stmt = self.conn.prepare(&sql)?;
        let rows = match filter {
            Some(lib) => stmt.query_map([lib], track_from_row)?,
            None => stmt.query_map([], track_from_row)?,
        };
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    fn one_track<P: rusqlite::Params>(&self, sql: &str, params: P) -> Result<Option<TrackRow>> {
        Ok(self
            .conn
            .query_row(sql, params, track_from_row)
            .optional()?)
    }

    // ---------------------------------------------------------------- search

    /// Three-stage search: exact FTS, prefix FTS, then LIKE fallback.
    pub fn search_tracks(&self, query: &str, limit: i64) -> Result<Vec<TrackRow>> {
        if query.is_empty() {
            return Ok(Vec::new());
        }
        let sanitized = sanitize_fts_term(query);
        if sanitized.is_empty() {
            return Ok(Vec::new());
        }
        let limit = if limit > 0 { limit } else { 50 };
        // A malformed MATCH term falls through to the next stage, as the C++
        // three-stage search does.
        let mut out = self.fts_query(&sanitized, limit).unwrap_or_default();
        if out.is_empty() {
            out = self
                .fts_query(&format!("{sanitized}*"), limit)
                .unwrap_or_default();
        }
        if out.is_empty() {
            out = self.like_query(&escape_like(&sanitized), limit)?;
        }
        Ok(out)
    }

    fn fts_query(&self, query: &str, limit: i64) -> Result<Vec<TrackRow>> {
        // `tracks_fts` mirrors column names, so the shared field list must be
        // qualified to avoid "ambiguous column name" errors that would
        // otherwise force every FTS hit through the LIKE fallback.
        let fields = TRACK_FIELDS
            .split(',')
            .map(str::trim)
            .map(|name| format!("t.{name}"))
            .collect::<Vec<_>>()
            .join(", ");
        let sql = format!(
            "SELECT {fields} FROM tracks t JOIN tracks_fts f ON t.id = f.rowid \
             WHERE tracks_fts MATCH ? ORDER BY rank LIMIT ?"
        );
        let mut stmt = self.conn.prepare(&sql)?;
        let rows = stmt.query_map(params![query, limit], track_from_row)?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    fn like_query(&self, escaped: &str, limit: i64) -> Result<Vec<TrackRow>> {
        let sql = format!(
            "SELECT {TRACK_FIELDS} FROM tracks WHERE title LIKE ? ESCAPE '\\' COLLATE NOCASE \
             OR artist LIKE ? ESCAPE '\\' COLLATE NOCASE \
             OR album LIKE ? ESCAPE '\\' COLLATE NOCASE \
             OR album_artist LIKE ? ESCAPE '\\' COLLATE NOCASE \
             OR genre LIKE ? ESCAPE '\\' COLLATE NOCASE \
             OR path LIKE ? ESCAPE '\\' COLLATE NOCASE LIMIT ?"
        );
        let pattern = format!("%{escaped}%");
        let mut stmt = self.conn.prepare(&sql)?;
        let rows = stmt.query_map(
            params![pattern, pattern, pattern, pattern, pattern, pattern, limit],
            track_from_row,
        )?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    // ---------------------------------------------------------------- queues

    /// Loads a queue container; id 0 selects the default queue.
    pub fn get_queue(&self, qid: i64) -> Result<Queue> {
        let qid = normalize_queue_id(qid);
        self.conn
            .query_row(
                "SELECT id, name, repeat_mode, temp, library_id FROM queues WHERE id=?",
                [qid],
                |row| {
                    Ok(Queue {
                        id: int_at(row, 0)?,
                        name: text_at(row, 1)?,
                        repeat_mode: i32::try_from(int_at(row, 2)?).unwrap_or(0),
                        temp: int_at(row, 3)? != 0,
                        library_id: int_at(row, 4)?,
                    })
                },
            )
            .optional()?
            .ok_or_else(|| DbError::not_found(format!("queue {qid}")))
    }

    /// Lists queue containers ordered by id.
    pub fn list_queues(&self) -> Result<Vec<Queue>> {
        let mut stmt = self
            .conn
            .prepare("SELECT id, name, repeat_mode, temp, library_id FROM queues ORDER BY id")?;
        let rows = stmt.query_map([], |row| {
            Ok(Queue {
                id: int_at(row, 0)?,
                name: text_at(row, 1)?,
                repeat_mode: i32::try_from(int_at(row, 2)?).unwrap_or(0),
                temp: int_at(row, 3)? != 0,
                library_id: int_at(row, 4)?,
            })
        })?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    /// Creates a queue container and returns its id.
    pub fn create_queue(&self, name: &str, library_id: i64) -> Result<i64> {
        if name.is_empty() {
            return Err(DbError::invalid_arg("queue name"));
        }
        self.conn.execute(
            "INSERT INTO queues (name, repeat_mode, library_id) VALUES (?,0,?)",
            params![name, library_id],
        )?;
        Ok(self.conn.last_insert_rowid())
    }

    /// Deletes a queue and its items.
    pub fn delete_queue(&self, qid: i64) -> Result<()> {
        let qid = normalize_queue_id(qid);
        let tx = self.conn.unchecked_transaction()?;
        queue_clear_in(&tx, qid)?;
        let changed = tx.execute("DELETE FROM queues WHERE id=?", [qid])?;
        if changed == 0 {
            return Err(DbError::not_found(format!("queue {qid}")));
        }
        tx.commit()?;
        Ok(())
    }

    /// Sets the stored repeat mode for a queue.
    pub fn set_queue_repeat(&self, qid: i64, repeat_mode: i32) -> Result<()> {
        let qid = normalize_queue_id(qid);
        let changed = self.conn.execute(
            "UPDATE queues SET repeat_mode=? WHERE id=?",
            params![repeat_mode, qid],
        )?;
        if changed == 0 {
            return Err(DbError::not_found(format!("queue {qid}")));
        }
        Ok(())
    }

    /// Sets the temporary flag for a queue.
    pub fn set_queue_temp(&self, qid: i64, temp: bool) -> Result<()> {
        let qid = normalize_queue_id(qid);
        let changed = self.conn.execute(
            "UPDATE queues SET temp=? WHERE id=?",
            params![i64::from(temp), qid],
        )?;
        if changed == 0 {
            return Err(DbError::not_found(format!("queue {qid}")));
        }
        Ok(())
    }

    /// Lists queue items in ascending position order.
    pub fn list_queue_items(&self, qid: i64) -> Result<Vec<QueueItem>> {
        let qid = normalize_queue_id(qid);
        let mut stmt = self.conn.prepare(
            "SELECT id, queue_id, track_id, position, added FROM queue \
             WHERE queue_id=? ORDER BY position",
        )?;
        let rows = stmt.query_map([qid], queue_item_from_row)?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    /// Returns the number of items in a queue.
    pub fn queue_count(&self, qid: i64) -> Result<usize> {
        let qid = normalize_queue_id(qid);
        let count: i64 = self.conn.query_row(
            "SELECT COUNT(*) FROM queue WHERE queue_id=?",
            [qid],
            |row| row.get(0),
        )?;
        Ok(usize::try_from(count).unwrap_or(0))
    }

    /// Finds a track's zero-based index in a queue.
    pub fn queue_index_of(&self, qid: i64, track_id: i64) -> Result<Option<usize>> {
        let qid = normalize_queue_id(qid);
        if track_id == 0 {
            return Ok(None);
        }
        let position: Option<i64> = self
            .conn
            .query_row(
                "SELECT MIN(position) FROM queue WHERE queue_id=? AND track_id=?",
                params![qid, track_id],
                |row| row.get(0),
            )
            .optional()?
            .flatten();
        let Some(position) = position else {
            return Ok(None);
        };
        let count: i64 = self.conn.query_row(
            "SELECT COUNT(*) FROM queue WHERE queue_id=? AND position<?",
            params![qid, position],
            |row| row.get(0),
        )?;
        Ok(Some(usize::try_from(count).unwrap_or(0)))
    }

    /// Appends a track, or inserts it at a zero-based index when `index >= 0`.
    pub fn enqueue(&self, qid: i64, track_id: i64, index: i64) -> Result<()> {
        enqueue_in(&self.conn, normalize_queue_id(qid), track_id, index)
    }

    /// Enqueues track ids atomically.
    pub fn enqueue_batch(&self, qid: i64, track_ids: &[i64]) -> Result<()> {
        let qid = normalize_queue_id(qid);
        let tx = self.conn.unchecked_transaction()?;
        for &tid in track_ids {
            enqueue_in(&tx, qid, tid, -1)?;
        }
        tx.commit()?;
        Ok(())
    }

    /// Removes the item at an exact position value.
    pub fn remove_at_position(&self, qid: i64, position: i64) -> Result<()> {
        let qid = normalize_queue_id(qid);
        let changed = self.conn.execute(
            "DELETE FROM queue WHERE queue_id=? AND position=?",
            params![qid, position],
        )?;
        if changed == 0 {
            return Err(DbError::not_found(format!("queue position {position}")));
        }
        Ok(())
    }

    /// Removes all items from a queue.
    pub fn clear_queue(&self, qid: i64) -> Result<()> {
        queue_clear_in(&self.conn, normalize_queue_id(qid))
    }

    /// Replaces all queue items atomically.
    pub fn replace_all(&self, qid: i64, track_ids: &[i64]) -> Result<()> {
        let qid = normalize_queue_id(qid);
        let tx = self.conn.unchecked_transaction()?;
        queue_clear_in(&tx, qid)?;
        for &tid in track_ids {
            enqueue_in(&tx, qid, tid, -1)?;
        }
        tx.commit()?;
        Ok(())
    }

    // ------------------------------------------------------------- playlists

    /// Creates a playlist and returns its id.
    pub fn create_playlist(
        &self,
        name: &str,
        playlist_type: i32,
        smart_query: &str,
    ) -> Result<i64> {
        if name.is_empty() {
            return Err(DbError::invalid_arg("playlist name"));
        }
        self.conn.execute(
            "INSERT INTO playlists (name, type, smart_query) VALUES (?,?,?)",
            params![name, playlist_type, smart_query],
        )?;
        Ok(self.conn.last_insert_rowid())
    }

    /// Loads a playlist by id.
    pub fn get_playlist(&self, pid: i64) -> Result<Playlist> {
        if pid == 0 {
            return Err(DbError::invalid_arg("playlist id 0"));
        }
        self.conn
            .query_row(
                "SELECT id, name, type, smart_query, created, modified, library_id \
                 FROM playlists WHERE id=?",
                [pid],
                playlist_from_row,
            )
            .optional()?
            .ok_or_else(|| DbError::not_found(format!("playlist {pid}")))
    }

    /// Lists playlists ordered by id.
    pub fn list_playlists(&self) -> Result<Vec<Playlist>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, name, type, smart_query, created, modified, library_id \
             FROM playlists ORDER BY id",
        )?;
        let rows = stmt.query_map([], playlist_from_row)?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    /// Appends a track, or inserts it at a zero-based index when `index >= 0`.
    pub fn playlist_add_track(&self, pid: i64, track_id: i64, index: i64) -> Result<()> {
        if pid == 0 || track_id == 0 {
            return Err(DbError::invalid_arg("playlist or track id 0"));
        }
        let tx = self.conn.unchecked_transaction()?;
        let position = if index < 0 {
            let next: i64 = tx.query_row(
                "SELECT COALESCE(MAX(position), -1)+1 FROM playlist_items WHERE playlist_id=?",
                [pid],
                |row| row.get(0),
            )?;
            next.max(0)
        } else {
            tx.execute(
                "UPDATE playlist_items SET position=position+1 \
                 WHERE playlist_id=? AND position>=?",
                params![pid, index],
            )?;
            index
        };
        tx.execute(
            "INSERT INTO playlist_items (playlist_id, track_id, position) VALUES (?,?,?)",
            params![pid, track_id, position],
        )?;
        tx.commit()?;
        Ok(())
    }

    /// Lists the tracks of a manual playlist in position order.
    pub fn playlist_tracks(&self, pid: i64) -> Result<Vec<TrackRow>> {
        if pid == 0 {
            return Err(DbError::invalid_arg("playlist id 0"));
        }
        let sql = format!(
            "SELECT {TRACK_FIELDS} FROM tracks JOIN playlist_items pi ON pi.track_id=tracks.id \
             WHERE pi.playlist_id=? ORDER BY pi.position"
        );
        let mut stmt = self.conn.prepare(&sql)?;
        let rows = stmt.query_map([pid], track_from_row)?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    /// Moves a playlist item between zero-based positions.
    pub fn playlist_reorder(&self, pid: i64, from: i64, to: i64) -> Result<()> {
        if pid == 0 {
            return Err(DbError::invalid_arg("playlist id 0"));
        }
        if from == to {
            return Ok(());
        }
        let tx = self.conn.unchecked_transaction()?;
        let track_id: Option<i64> = tx
            .query_row(
                "SELECT track_id FROM playlist_items WHERE playlist_id=? AND position=?",
                params![pid, from],
                |row| row.get(0),
            )
            .optional()?;
        let track_id = track_id.ok_or_else(|| DbError::not_found(format!("position {from}")))?;
        if from < to {
            tx.execute(
                "UPDATE playlist_items SET position=position-1 \
                 WHERE playlist_id=? AND position>? AND position<=?",
                params![pid, from, to],
            )?;
        } else {
            tx.execute(
                "UPDATE playlist_items SET position=position+1 \
                 WHERE playlist_id=? AND position>=? AND position<?",
                params![pid, to, from],
            )?;
        }
        tx.execute(
            "UPDATE playlist_items SET position=? WHERE playlist_id=? AND track_id=?",
            params![to, pid, track_id],
        )?;
        tx.commit()?;
        Ok(())
    }

    /// Renames a playlist.
    pub fn rename_playlist(&self, pid: i64, new_name: &str) -> Result<()> {
        if pid == 0 || new_name.is_empty() {
            return Err(DbError::invalid_arg("playlist id or name"));
        }
        let changed = self.conn.execute(
            "UPDATE playlists SET name=?, modified=CURRENT_TIMESTAMP WHERE id=?",
            params![new_name, pid],
        )?;
        if changed == 0 {
            return Err(DbError::not_found(format!("playlist {pid}")));
        }
        Ok(())
    }

    /// Deletes a playlist and its items.
    pub fn delete_playlist(&self, pid: i64) -> Result<()> {
        if pid == 0 {
            return Err(DbError::invalid_arg("playlist id 0"));
        }
        let changed = self
            .conn
            .execute("DELETE FROM playlists WHERE id=?", [pid])?;
        if changed == 0 {
            return Err(DbError::not_found(format!("playlist {pid}")));
        }
        Ok(())
    }

    // ------------------------------------------------------------- libraries

    /// Lists configured library roots ordered by id.
    pub fn list_libraries(&self) -> Result<Vec<Library>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, path, name, date_added, last_scanned, auto_scan, recursive, extensions \
             FROM libraries ORDER BY id",
        )?;
        let rows = stmt.query_map([], |row| {
            Ok(Library {
                id: int_at(row, 0)?,
                path: text_at(row, 1)?,
                name: text_at(row, 2)?,
                date_added: int_at(row, 3)?,
                last_scanned: int_at(row, 4)?,
                auto_scan: int_at(row, 5)? != 0,
                recursive: int_at(row, 6)? != 0,
                extensions: text_at(row, 7)?,
            })
        })?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    // --------------------------------------------------------------- history

    /// Records a play: increments the counter and inserts a history row.
    #[allow(clippy::too_many_arguments)]
    pub fn mark_played(
        &self,
        track_id: i64,
        started_at: i64,
        completed_at: i64,
        position_ms: i64,
        completion_pct: f64,
        queue_id: i64,
    ) -> Result<()> {
        let tx = self.conn.unchecked_transaction()?;
        let play_count: Option<i64> = tx
            .query_row(
                "SELECT play_count FROM tracks WHERE id=?",
                [track_id],
                |row| row.get(0),
            )
            .optional()?;
        let play_count = play_count.ok_or_else(|| DbError::not_found("track not found"))?;
        tx.execute(
            "UPDATE tracks SET play_count=?, last_played=? WHERE id=?",
            params![play_count + 1, completed_at, track_id],
        )?;
        tx.execute(
            "INSERT INTO history (track_id, started_at, completed_at, position_ms, completion_pct, \
             queue_id) VALUES (?,?,?,?,?,?)",
            params![
                track_id,
                started_at,
                completed_at,
                position_ms,
                completion_pct,
                normalize_queue_id(queue_id)
            ],
        )?;
        tx.commit()?;
        Ok(())
    }

    /// Lists history newest-first, joined with track snapshots.
    pub fn history_list(&self, limit: i64) -> Result<Vec<HistoryEntry>> {
        let mut sql = String::from(
            "SELECT h.id, h.track_id, h.started_at, h.completed_at, h.position_ms, \
             h.completion_pct, h.queue_id, t.title, t.artist, t.path, t.duration \
             FROM history h JOIN tracks t ON t.id=h.track_id ORDER BY h.started_at DESC",
        );
        if limit > 0 {
            sql.push_str(" LIMIT ?");
        }
        let mut stmt = self.conn.prepare(&sql)?;
        let mut rows = if limit > 0 {
            stmt.query([limit])?
        } else {
            stmt.query([])?
        };
        let mut out = Vec::new();
        while let Some(row) = rows.next()? {
            out.push(HistoryEntry {
                id: int_at(row, 0)?,
                track_id: int_at(row, 1)?,
                started_at: int_at(row, 2)?,
                completed_at: int_at(row, 3)?,
                position_ms: int_at(row, 4)?,
                completion_pct: real_at(row, 5)?,
                queue_id: int_at(row, 6)?,
                title: text_at(row, 7)?,
                artist: text_at(row, 8)?,
                path: text_at(row, 9)?,
                duration: real_at(row, 10)?,
            });
        }
        Ok(out)
    }

    /// Clears playback history.
    pub fn history_clear(&self) -> Result<()> {
        self.conn.execute("DELETE FROM history", [])?;
        Ok(())
    }

    // ------------------------------------------------------------- bookmarks

    /// Adds a bookmark at a position in milliseconds.
    pub fn bookmark_add(&self, track_id: i64, position_ms: i64, note: &str) -> Result<()> {
        if track_id == 0 || position_ms < 0 {
            return Err(DbError::invalid_arg("bookmark track or position"));
        }
        self.conn.execute(
            "INSERT INTO bookmarks (track_id, position_ms, note) VALUES (?,?,?)",
            params![track_id, position_ms, note],
        )?;
        Ok(())
    }

    /// Lists bookmarks for a track ordered by position.
    pub fn list_bookmarks(&self, track_id: i64) -> Result<Vec<Bookmark>> {
        if track_id == 0 {
            return Err(DbError::invalid_arg("track id 0"));
        }
        let mut stmt = self.conn.prepare(
            "SELECT id, track_id, position_ms, note, created FROM bookmarks \
             WHERE track_id=? ORDER BY position_ms",
        )?;
        let rows = stmt.query_map([track_id], |row| {
            Ok(Bookmark {
                id: int_at(row, 0)?,
                track_id: int_at(row, 1)?,
                position_ms: int_at(row, 2)?,
                note: text_at(row, 3)?,
                created: int_at(row, 4)?,
            })
        })?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    /// Removes a bookmark by id.
    pub fn bookmark_remove(&self, id: i64) -> Result<()> {
        let changed = self
            .conn
            .execute("DELETE FROM bookmarks WHERE id=?", [id])?;
        if changed == 0 {
            return Err(DbError::not_found(format!("bookmark {id}")));
        }
        Ok(())
    }

    // ---------------------------------------------------------------- lyrics

    /// Upserts lyrics for a track.
    pub fn lyrics_set(
        &self,
        track_id: i64,
        lrc_text: &str,
        is_synced: bool,
        source: &str,
    ) -> Result<()> {
        if track_id == 0 {
            return Err(DbError::invalid_arg("track id 0"));
        }
        let tx = self.conn.unchecked_transaction()?;
        tx.execute("DELETE FROM lyrics WHERE track_id=?", [track_id])?;
        tx.execute(
            "INSERT INTO lyrics (track_id, lrc_text, is_synced, source) VALUES (?,?,?,?)",
            params![track_id, lrc_text, i64::from(is_synced), source],
        )?;
        tx.commit()?;
        Ok(())
    }

    /// Loads lyrics for a track.
    pub fn lyrics_get(&self, track_id: i64) -> Result<Lyrics> {
        if track_id == 0 {
            return Err(DbError::invalid_arg("track id 0"));
        }
        self.conn
            .query_row(
                "SELECT id, track_id, lrc_text, is_synced, source, fetched_at \
                 FROM lyrics WHERE track_id=?",
                [track_id],
                |row| {
                    Ok(Lyrics {
                        id: int_at(row, 0)?,
                        track_id: int_at(row, 1)?,
                        lrc_text: text_at(row, 2)?,
                        is_synced: int_at(row, 3)? != 0,
                        source: text_at(row, 4)?,
                        fetched_at: int_at(row, 5)?,
                    })
                },
            )
            .optional()?
            .ok_or_else(|| DbError::not_found("lyrics"))
    }

    /// Removes lyrics for a track.
    pub fn lyrics_remove(&self, track_id: i64) -> Result<()> {
        let changed = self
            .conn
            .execute("DELETE FROM lyrics WHERE track_id=?", [track_id])?;
        if changed == 0 {
            return Err(DbError::not_found("lyrics"));
        }
        Ok(())
    }

    // ----------------------------------------------------------------- stats

    /// Returns aggregate database statistics.
    pub fn stats(&self) -> Result<PlaybackStats> {
        let count =
            |sql: &str| -> Result<i64> { Ok(self.conn.query_row(sql, [], |row| row.get(0))?) };
        Ok(PlaybackStats {
            num_tracks: count("SELECT COUNT(*) FROM tracks")?,
            num_playlists: count("SELECT COUNT(*) FROM playlists")?,
            num_queue_items: count("SELECT COUNT(*) FROM queue")?,
            num_history: count("SELECT COUNT(*) FROM history")?,
            num_bookmarks: count("SELECT COUNT(*) FROM bookmarks")?,
            num_libraries: count("SELECT COUNT(*) FROM libraries")?,
            total_duration_ms: {
                let seconds: f64 = self.conn.query_row(
                    "SELECT COALESCE(SUM(duration),0) FROM tracks",
                    [],
                    |row| row.get(0),
                )?;
                (seconds * 1000.0) as i64
            },
        })
    }
}

// ------------------------------------------------------------- helpers

fn normalize_queue_id(qid: i64) -> i64 {
    if qid == 0 {
        1
    } else {
        qid
    }
}

fn is_unique_violation(err: &rusqlite::Error) -> bool {
    matches!(
        err,
        rusqlite::Error::SqliteFailure(code, _)
            if code.code == rusqlite::ErrorCode::ConstraintViolation
    ) && err.to_string().contains("UNIQUE")
}

fn int_at(row: &Row<'_>, index: usize) -> rusqlite::Result<i64> {
    match row.get_ref(index)? {
        ValueRef::Integer(value) => Ok(value),
        ValueRef::Real(value) => Ok(value as i64),
        ValueRef::Text(bytes) => Ok(std::str::from_utf8(bytes)
            .ok()
            .and_then(|text| text.trim().parse().ok())
            .unwrap_or(0)),
        ValueRef::Null | ValueRef::Blob(_) => Ok(0),
    }
}

fn real_at(row: &Row<'_>, index: usize) -> rusqlite::Result<f64> {
    match row.get_ref(index)? {
        ValueRef::Integer(value) => Ok(value as f64),
        ValueRef::Real(value) => Ok(value),
        ValueRef::Text(bytes) => Ok(std::str::from_utf8(bytes)
            .ok()
            .and_then(|text| text.trim().parse().ok())
            .unwrap_or(0.0)),
        ValueRef::Null | ValueRef::Blob(_) => Ok(0.0),
    }
}

fn text_at(row: &Row<'_>, index: usize) -> rusqlite::Result<String> {
    match row.get_ref(index)? {
        ValueRef::Text(bytes) => Ok(String::from_utf8_lossy(bytes).into_owned()),
        _ => Ok(String::new()),
    }
}

fn fingerprint_at(row: &Row<'_>, index: usize) -> rusqlite::Result<[u8; 32]> {
    let mut out = [0_u8; 32];
    if let ValueRef::Blob(bytes) = row.get_ref(index)? {
        if bytes.len() == out.len() {
            out.copy_from_slice(bytes);
        }
    }
    Ok(out)
}

fn track_from_row(row: &Row<'_>) -> rusqlite::Result<TrackRow> {
    Ok(TrackRow {
        id: int_at(row, 0)?,
        fingerprint: fingerprint_at(row, 1)?,
        path: text_at(row, 2)?,
        size: int_at(row, 3)?,
        mtime: int_at(row, 4)?,
        duration: real_at(row, 5)?,
        sample_rate: u32::try_from(int_at(row, 6)?).unwrap_or(0),
        channels: u32::try_from(int_at(row, 7)?).unwrap_or(0),
        bitrate: i32::try_from(int_at(row, 8)?).unwrap_or(0),
        title: text_at(row, 9)?,
        artist: text_at(row, 10)?,
        album: text_at(row, 11)?,
        album_artist: text_at(row, 12)?,
        genre: text_at(row, 13)?,
        year: i32::try_from(int_at(row, 14)?).unwrap_or(0),
        track_num: i32::try_from(int_at(row, 15)?).unwrap_or(0),
        disc_num: i32::try_from(int_at(row, 16)?).unwrap_or(0),
        cover_art_path: text_at(row, 17)?,
        rating: i32::try_from(int_at(row, 18)?).unwrap_or(0),
        play_count: int_at(row, 19)?,
        last_played: int_at(row, 20)?,
        date_added: int_at(row, 21)?,
        last_scanned: int_at(row, 22)?,
        dirty: int_at(row, 23)? != 0,
        library_id: int_at(row, 24)?,
        source: SourceRef {
            kind: crate::model::SourceKind::from_db(int_at(row, 25)?),
            key: text_at(row, 26)?,
            available: int_at(row, 27)? != 0,
            display_name: text_at(row, 28)?,
        },
        replay_gain_track: real_at(row, 29)?,
        replay_gain_album: real_at(row, 30)?,
    })
}

fn queue_item_from_row(row: &Row<'_>) -> rusqlite::Result<QueueItem> {
    Ok(QueueItem {
        id: int_at(row, 0)?,
        queue_id: int_at(row, 1)?,
        track_id: int_at(row, 2)?,
        position: int_at(row, 3)?,
        added: int_at(row, 4)?,
    })
}

fn playlist_from_row(row: &Row<'_>) -> rusqlite::Result<Playlist> {
    Ok(Playlist {
        id: int_at(row, 0)?,
        name: text_at(row, 1)?,
        playlist_type: i32::try_from(int_at(row, 2)?).unwrap_or(0),
        smart_query: text_at(row, 3)?,
        created: int_at(row, 4)?,
        modified: int_at(row, 5)?,
        library_id: int_at(row, 6)?,
    })
}

fn position_at_index(conn: &Connection, qid: i64, index: i64) -> Result<Option<i64>> {
    Ok(conn
        .query_row(
            "SELECT position FROM queue WHERE queue_id=? ORDER BY position LIMIT 1 OFFSET ?",
            params![qid, index],
            |row| row.get(0),
        )
        .optional()?)
}

fn renormalize_queue(conn: &Connection, qid: i64) -> Result<()> {
    let mut stmt = conn.prepare("SELECT id FROM queue WHERE queue_id=? ORDER BY position")?;
    let ids = stmt
        .query_map([qid], |row| row.get::<_, i64>(0))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    drop(stmt);
    for (index, id) in ids.iter().enumerate() {
        let new_position = (index as i64) * POSITION_GAP;
        conn.execute(
            "UPDATE queue SET position=? WHERE id=?",
            params![new_position, id],
        )?;
    }
    Ok(())
}

fn enqueue_in(conn: &Connection, qid: i64, track_id: i64, index: i64) -> Result<()> {
    if track_id == 0 {
        return Err(DbError::invalid_arg("track id 0"));
    }
    let position = if index < 0 {
        let max: i64 = conn.query_row(
            "SELECT COALESCE(MAX(position), -1) FROM queue WHERE queue_id=?",
            [qid],
            |row| row.get(0),
        )?;
        if max < 0 {
            0
        } else {
            max + POSITION_GAP
        }
    } else {
        let mut prev = position_at_index(conn, qid, index - 1)?;
        let mut next = position_at_index(conn, qid, index)?;
        if matches!((prev, next), (Some(a), Some(b)) if b - a <= 1) {
            renormalize_queue(conn, qid)?;
            prev = position_at_index(conn, qid, index - 1)?;
            next = position_at_index(conn, qid, index)?;
        }
        match (prev, next) {
            (Some(prev), Some(next)) => i64::midpoint(prev, next),
            (Some(prev), None) => prev + POSITION_GAP,
            (None, Some(next)) => next - POSITION_GAP,
            (None, None) => 0,
        }
    };
    conn.execute(
        "INSERT INTO queue (queue_id, track_id, position) VALUES (?,?,?)",
        params![qid, track_id, position],
    )?;
    Ok(())
}

fn queue_clear_in(conn: &Connection, qid: i64) -> Result<()> {
    conn.execute("DELETE FROM queue WHERE queue_id=?", [qid])?;
    Ok(())
}

/// Port of `internal::escapeLike`.
fn escape_like(term: &str) -> String {
    let mut out = String::with_capacity(term.len() + 4);
    for ch in term.chars() {
        if matches!(ch, '%' | '_' | '\\') {
            out.push('\\');
        }
        out.push(ch);
    }
    out
}

/// Port of `internal::sanitizeFtsTerm`.
fn sanitize_fts_term(term: &str) -> String {
    if term.is_empty() {
        return String::new();
    }
    let mut cleaned = String::with_capacity(term.len());
    for ch in term.chars() {
        if matches!(ch, '\'' | '*' | ':' | '-' | '(' | ')' | '^' | '~') {
            continue;
        }
        if ch == '"' {
            cleaned.push_str("\"\"");
        } else {
            cleaned.push(ch);
        }
    }
    let mut upper = cleaned.to_ascii_uppercase();
    for op in [" AND ", " OR ", " NOT ", " NEAR "] {
        let mut pos = 0;
        while let Some(rel) = upper[pos..].find(op) {
            let abs = pos + rel;
            upper.replace_range(abs..abs + op.len(), &" ".repeat(op.len()));
            cleaned.replace_range(abs..abs + op.len(), &" ".repeat(op.len()));
            pos = abs + op.len();
        }
    }
    for op in ["AND ", "OR ", "NOT ", "NEAR "] {
        if upper.starts_with(op) {
            upper.replace_range(0..op.len(), &" ".repeat(op.len()));
            cleaned.replace_range(0..op.len(), &" ".repeat(op.len()));
        }
    }
    for op in [" AND", " OR", " NOT", " NEAR"] {
        if upper.ends_with(op) {
            let start = upper.len() - op.len();
            upper.replace_range(start.., &" ".repeat(op.len()));
            cleaned.replace_range(start.., &" ".repeat(op.len()));
        }
    }
    let mut collapsed = String::with_capacity(cleaned.len());
    let mut last_space = false;
    for ch in cleaned.chars() {
        if ch == ' ' {
            if !last_space {
                collapsed.push(' ');
                last_space = true;
            }
        } else {
            collapsed.push(ch);
            last_space = false;
        }
    }
    collapsed.trim().to_string()
}
