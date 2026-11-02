#pragma once

/**
 * @file database.hpp
 * @brief Main SQLite database handle with thread-safe CRUD.
 * @ingroup kanariv_db
 * @details Implements `Database` with a shared_mutex for the SQLite handle,
 * a mutex for the prepared-statement cache, and a background writer thread
 * for batching writes. All public methods are thread-safe; see each method
 * for locking requirements. Move operations are not thread-safe after open.
 */

#include <array>
#include <kanariv/contracts/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kanariv::db {

/**
 * @brief Options for opening a Database.
 * @ingroup kanariv_db
 */
struct DatabaseOptions {
    std::size_t writeBatchSize =
        256; ///< Queue capacity / batch size for the background writer (default 256).
    /// Non-owning logger (may be null = silent). Must outlive the Database;
    /// never emitted from destructors. Set from opts at open().
    kanariv::utils::Logger* logger{nullptr};
};

/**
 * @brief Main SQLite database handle with thread-safe CRUD.
 * @ingroup kanariv_db
 * @details Thread safety: dbMutex_ (shared_mutex) guards the handle and all
 * table access; cacheMutex_ (mutex) guards stmtCache_. Lock order is
 * dbMutex_ -> cacheMutex_. Readers take shared_lock on dbMutex_, writers
 * take unique_lock. getCachedForUse() requires the caller to already hold
 * cacheMutex_. Move operations are not thread-safe after open.
 * @see DatabaseOptions
 * @see SqliteStatement
 */
class KANARIV_API Database final : public kanariv::contracts::IDatabase {
  public:
    // Lifecycle and connection management
    /**
     * @brief Default-constructs an empty (not open) Database.
     * @ingroup kanariv_db
     */
    Database();
    /**
     * @brief Constructs with options (sets the writer batch size).
     * @ingroup kanariv_db
     * @param opts Options; writeBatchSize forwarded to the background writer.
     */
    // Out-of-line (core.cpp): Impl is defined there.
    explicit Database(const DatabaseOptions& opts);
    /**
     * @brief Closes the writer, clears the statement cache and resets the handle.
     * @ingroup kanariv_db
     */
    ~Database() override;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    // Move is not thread-safe after open: caller must ensure no concurrent DB access.
    // The moved-from object is left empty; its accessors must not be used.
    Database(Database&& o) noexcept;
    Database& operator=(Database&& o) noexcept;
    /**
     * @brief Opens (or creates) a database.
     * @ingroup kanariv_db
     * @param path Filesystem path (empty => :memory:).
     * @param opts Options (writeBatchSize).
     * @return Owning Database pointer or Error Io/Corrupt.
     * @details Executes schema, handles the queue UNIQUE migration gracefully
     * (ignores duplicate-position errors), ensures the default library row and
     * migrates engine_state.active_queue_id if missing. Starts the background
     * writer thread.
     * @par Lifetime
     * The returned `unique_ptr` owns the database and must remain alive while
     * callers use the handle or any value-producing operation that depends on
     * it. The logger supplied in `opts` is borrowed.
     * @par Thread safety
     * Thread-safe for distinct paths; the returned Database is not shared until open returns.
     */
    static std::expected<std::unique_ptr<Database>, kanariv::utils::Error>
    open(std::string_view path, const DatabaseOptions& opts = {});
    /**
     * @brief Reports whether the SQLite handle is open.
     * @ingroup kanariv_db
     * @return True when the database is ready for operations.
     * @par Thread safety
     * Thread-safe; reads the immutable open-state flag.
     */
    bool isOpen() const noexcept override;
    /**
     * @brief Returns the non-owning logger from DatabaseOptions (may be null).
     * @ingroup kanariv_db
     * @details For free functions (scan) that act on the database but take
     * no logger of their own. Never null-dereference; check before use.
     */
    kanariv::utils::Logger* logger() const noexcept override;
    /**
     * @brief Flushes the background writer queue.
     * @ingroup kanariv_db
     * @return Success or Error with StatusCode::Busy on 200 ms timeout.
     * @par Thread safety
     * Thread-safe; delegates to the writer's flush().
     */
    std::expected<void, kanariv::utils::Error> flush();

    /**
     * @brief Enqueues multiple tracks atomically (single transaction).
     * @ingroup kanariv_db
     * @param qid Queue id (0 defaults to 1).
     * @param tids Track ids to enqueue.
     * @return Success or Error (Internal/Busy/NotFound).
     * @details Holds dbMutex_ exclusively and uses BEGIN IMMEDIATE / COMMIT
     * so concurrent listQueueItems never sees a partial batch.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     * @see queueReplaceAll
     */
    std::expected<void, kanariv::utils::Error>
    queueEnqueueBatch(int64_t qid, std::span<const int64_t> tids) override;
    std::expected<void, kanariv::utils::Error>
    queueEnqueueBatch(int64_t qid, const std::vector<int64_t>& tids) override;

    /**
     * @brief Replaces an entire queue atomically.
     * @ingroup kanariv_db
     * @param qid Queue id (0 defaults to 1).
     * @param ids Track ids to set (clears then enqueues).
     * @return Success or Error.
     * @details Single transaction: queueClearLocked + queueEnqueueLocked per id.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     */
    std::expected<void, kanariv::utils::Error>
    queueReplaceAll(int64_t qid, std::span<const int64_t> ids) override;
    std::expected<void, kanariv::utils::Error>
    queueReplaceAll(int64_t qid, const std::vector<int64_t>& ids) override;

  private:
    friend struct DatabaseAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    // Track CRUD and search
    /**
     * @brief Track CRUD -- inserts, updates, deletes and queries tracks.
     * @ingroup kanariv_db
     */
    /**
     * @brief Inserts a track.
     * @ingroup kanariv_db
     * @param t Track to insert (fingerprint must be unique).
     * @return New row id or Error AlreadyExists/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     * @see insertTrackLocked
     */
    std::expected<int64_t, kanariv::utils::Error>
    insertTrack(const kanariv::types::Track& t) override;
    /**
     * @brief Updates a track by id.
     * @ingroup kanariv_db
     * @param t Track with fields to persist (id must be non-zero).
     * @return Success or Error InvalidArg/NotFound/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     */
    std::expected<void, kanariv::utils::Error> updateTrack(const kanariv::types::Track& t) override;
    /**
     * @brief Deletes a track by id.
     * @ingroup kanariv_db
     * @param id Track id (0 is InvalidArg).
     * @return Success or Error NotFound/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     */
    std::expected<void, kanariv::utils::Error> deleteTrack(int64_t id) override;
    /**
     * @brief Gets a track by id.
     * @ingroup kanariv_db
     * @param id Track id.
     * @return Track or Error InvalidArg/NotFound/Internal.
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_.
     */
    std::expected<kanariv::types::Track, kanariv::utils::Error> getTrack(int64_t id) override;

    /**
     * @brief Finds a track by fingerprint.
     * @ingroup kanariv_db
     * @param fp Fingerprint to search.
     * @return Track or Error NotFound/Internal.
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_.
     */
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    findByFingerprint(const std::array<uint8_t, 32>& fp) override;
    /**
     * @brief Finds a track by path.
     * @ingroup kanariv_db
     * @param path Path to search.
     * @return Track or Error NotFound/Internal.
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_.
     */
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    findByPath(std::string_view path) override;
    /**
     * @brief Finds a track by durable source identity.
     * @ingroup kanariv_db
     * @param source Source kind and stable key. Path rows normally use the
     * filesystem path as the key after `normalizedSource()`.
     * @return Track or Error InvalidArg (empty key)/NotFound/Internal.
     * @details Implements deterministic source deduplication: callers compare
     * `(source.kind, source.key)` instead of guessing a filesystem path for a
     * content URI. A transient descriptor number is never part of this key.
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_.
     * @see kanariv::types::normalizedSource
     */
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    findBySource(const kanariv::types::SourceRef& source) override;
    /**
     * @brief Lists tracks with optional filters.
     * @ingroup kanariv_db
     * @param q Optional filter (nullptr = no filter, ordered by id).
     * @return Vector of tracks or Error Internal.
     * @details Builds WHERE clauses from kanariv::types::TrackQuery fields; search uses LIKE ESCAPE.
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_.
     * @see kanariv::types::TrackQuery
     */
    std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
    listTracks(const kanariv::types::TrackQuery* q = nullptr) override;
    /**
     * @brief Sets the dirty flag for a track.
     * @ingroup kanariv_db
     * @param id Track id.
     * @param dirty Dirty value (0/1).
     * @return Success or Error NotFound/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     */
    std::expected<void, kanariv::utils::Error> setDirty(int64_t id, int dirty);
    /**
     * @brief Sets the user rating for a track.
     * @ingroup kanariv_db
     * @param id Track id.
     * @param rating Rating value (0..5).
     * @return Success or Error InvalidArg/NotFound/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     */
    std::expected<void, kanariv::utils::Error> setRating(int64_t id, int32_t rating);

    // Playlists
    // Queue operations
    /**
     * @brief Creates a playlist.
     * @ingroup kanariv_db
     * @param name Playlist name (must be non-empty).
     * @param type Playlist type (0 = manual).
     * @param smart_query Smart filter (empty for manual).
     * @return New id or Error InvalidArg/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock on dbMutex_.
     */
    std::expected<int64_t, kanariv::utils::Error>
    createPlaylist(std::string_view name, int type = 0, std::string_view smart_query = {}) override;
    /**
     * @brief Gets a playlist by id.
     * @ingroup kanariv_db
     * @param id Playlist id.
     * @return Playlist or Error NotFound/InvalidArg.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<kanariv::types::Playlist, kanariv::utils::Error> getPlaylist(int64_t id) override;
    /**
     * @brief Updates a playlist.
     * @ingroup kanariv_db
     * @param p Playlist with updated fields (id must be non-zero).
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> updatePlaylist(const kanariv::types::Playlist& p);
    /**
     * @brief Deletes a playlist.
     * @ingroup kanariv_db
     * @param id Playlist id.
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> deletePlaylist(int64_t id) override;
    /**
     * @brief Renames a playlist.
     * @ingroup kanariv_db
     * @param id Playlist id.
     * @param newName New name (must be non-empty).
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> renamePlaylist(int64_t id,
                                                             std::string_view newName) override;
    /**
     * @brief Creates a manual playlist and populates it in one operation.
     * @param name Playlist display name; must be non-empty.
     * @param trackIds Track identifiers in their desired order.
     * @return New playlist identifier, or an error; no partial playlist is
     * retained when population fails.
     * @par Thread safety
     * Thread-safe; serializes the write under the database mutex.
     */
    std::expected<int64_t, kanariv::utils::Error>
    createPlaylistFromTracks(std::string_view name, std::span<const int64_t> trackIds) override;
    /**
     * @brief Lists all playlists ordered by id.
     * @ingroup kanariv_db
     * @return Vector of playlists or Error.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<std::vector<kanariv::types::Playlist>, kanariv::utils::Error>
    listPlaylists() override;
    /**
     * @brief Adds a track to a playlist.
     * @param pid Playlist identifier.
     * @param tid Track identifier.
     * @param pos Zero-based insertion position, or `-1` to append.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error> playlistAddTrack(int64_t pid, int64_t tid,
                                                               int64_t pos = -1) override;
    /**
     * @brief Removes a track from a playlist.
     * @param pid Playlist identifier.
     * @param tid Track identifier.
     * @return Success, or `NotFound`/a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error> playlistRemoveTrack(int64_t pid,
                                                                  int64_t tid) override;
    /**
     * @brief Moves a playlist item between positions.
     * @param pid Playlist identifier.
     * @param from Existing zero-based position.
     * @param to Destination zero-based position.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error> playlistReorder(int64_t pid, int64_t from,
                                                              int64_t to) override;
    /**
     * @brief Gets all tracks in a playlist ordered by position.
     * @ingroup kanariv_db
     * @param pid Playlist id.
     * @return Tracks or Error.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
    playlistGetTracks(int64_t pid) override;

    /**
     * @brief Queue operations -- forwarded to queue partition (see queue.cppm).
     * @ingroup kanariv_db
     * @details All queue methods normalize qid == 0 to 1 and enforce the
     * UNIQUE(queue_id, position) invariant via dbMutex_ serialization.
     */
    /**
     * @brief Enqueues a track.
     * @ingroup kanariv_db
     * @param qid Queue id (0 -> 1).
     * @param tid Track id.
     * @param pos Position (-1 = append).
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> queueEnqueue(int64_t qid, int64_t tid,
                                                           int64_t pos = -1) override;

    /**
     * @brief Dequeues the head item.
     * @ingroup kanariv_db
     * @param qid Queue id.
     * @return QueueItem or Error NotFound.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<kanariv::types::QueueItem, kanariv::utils::Error> queueDequeue(int64_t qid);

    /**
     * @brief Removes item at position.
     * @ingroup kanariv_db
     * @param qid Queue id (0 -> 1).
     * @param pos Position to remove.
     * @return Success or Error NotFound.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> queueRemove(int64_t qid, int64_t pos) override;

    /**
     * @brief Clears a queue.
     * @ingroup kanariv_db
     * @param qid Queue id (0 -> 1).
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> queueClear(int64_t qid) override;

    /**
     * @brief Lists all items in a queue ordered by position.
     * @ingroup kanariv_db
     * @param qid Queue id (0 -> 1).
     * @return Vector of items or Error.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<std::vector<kanariv::types::QueueItem>, kanariv::utils::Error>
    listQueueItems(int64_t qid) override;

    /**
     * @brief Loads the track at a queue position.
     * @param qid Queue identifier; zero selects the default queue.
     * @param position Zero-based queue position.
     * @return Track, or `NotFound` when the position is absent.
     * @par Thread safety
     * Thread-safe; takes a shared database lock.
     * @ingroup kanariv_db
     */
    std::expected<kanariv::types::Track, kanariv::utils::Error>
    queueTrackAt(int64_t qid, int64_t position) override;

    /**
     * @brief Loads a queue container by identifier.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Queue, or `InvalidArg`/`NotFound`/a backend error.
     * @par Thread safety
     * Thread-safe; takes a shared database lock.
     * @ingroup kanariv_db
     */
    std::expected<kanariv::types::Queue, kanariv::utils::Error> getQueue(int64_t qid) override;
    /**
     * @brief Reports whether a queue exists.
     * @param qid Queue identifier; zero selects the default queue.
     * @return `true` when the queue row exists, otherwise `false`, or an error
     * when the existence query cannot be completed.
     * @par Thread safety
     * Thread-safe; takes a shared database lock.
     * @ingroup kanariv_db
     */
    std::expected<bool, kanariv::utils::Error> queueExists(int64_t qid) override;

    /**
     * @brief Lists queue containers.
     * @return Queue values, or a backend error.
     * @par Thread safety
     * Thread-safe; takes a shared database lock.
     * @ingroup kanariv_db
     */
    std::expected<std::vector<kanariv::types::Queue>, kanariv::utils::Error> listQueues() override;

    /**
     * @brief Creates a queue container.
     * @param name Display name for the queue.
     * @param library_id Owning library identifier.
     * @return New queue identifier, or `InvalidArg`/a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<int64_t, kanariv::utils::Error> createQueue(std::string_view name,
                                                             int64_t library_id = 1) override;

    /**
     * @brief Deletes a queue container.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Success, or `NotFound`/a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error> deleteQueue(int64_t qid) override;

    /**
     * @brief Sets the repeat mode stored for a queue.
     * @param qid Queue identifier; zero selects the default queue.
     * @param repeat_mode Stored repeat-mode value.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error> setQueueRepeat(int64_t qid, int repeat_mode);

    /**
     * @brief Sets the temporary flag for a queue.
     * @ingroup kanariv_db
     * @param qid Queue id.
     * @param temp New flag.
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> setQueueTemp(int64_t qid, bool temp) override;

    /**
     * @brief Returns the number of items in a queue.
     * @ingroup kanariv_db
     * @param qid Queue id (0 -> 1).
     * @return Item count.
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_; delegates to queueCountLocked().
     */
    std::size_t queueCount(int64_t qid) override;

    /**
     * @brief Finds the 0-based index of a track in a queue.
     * @ingroup kanariv_db
     * @param qid Queue id (0 -> 1).
     * @param trackId Track id to locate (first occurrence by position).
     * @return Index into the ORDER BY position listing, or std::nullopt if
     * the track is not in the queue.
     * @details O(log n) via the queue position index (see
     * queueIndexOfLocked); O(1) extra memory vs listQueueItems().
     * @par Thread safety
     * Thread-safe: shared_lock on dbMutex_.
     */
    std::optional<std::size_t> queueIndexOf(int64_t qid, int64_t trackId) override;

    // History
    /**
     * @brief Adds a history entry.
     * @ingroup kanariv_db
     * @param e Entry to insert.
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> historyAdd(const kanariv::types::HistoryEntry& e);
    /**
     * @brief Lists history entries with optional filters.
     * @ingroup kanariv_db
     * @param q Optional filter.
     * @return Vector of entries or Error.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    listHistory(const kanariv::types::HistoryQuery* q = nullptr);

    /**
     * @brief Clears all history.
     * @ingroup kanariv_db
     * @return Success or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> historyClear();

    // Bookmarks
    /**
     * @brief Adds a bookmark.
     * @ingroup kanariv_db
     * @param tid Track id.
     * @param pos Position in ms.
     * @param note Optional note.
     * @return Success or Error InvalidArg/Internal.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> bookmarkAdd(int64_t tid, int64_t pos,
                                                          std::string_view note = {}) override;
    /**
     * @brief Lists bookmarks for a track.
     * @ingroup kanariv_db
     * @param tid Track id.
     * @return Bookmarks or Error.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<std::vector<kanariv::types::Bookmark>, kanariv::utils::Error>
    listBookmarks(int64_t tid) override;
    /**
     * @brief Gets a bookmark by id.
     * @ingroup kanariv_db
     * @param id Bookmark id.
     * @return Bookmark, InvalidArg, NotFound or Error.
     * @par Thread safety
     * Thread-safe: shared_lock.
     */
    std::expected<kanariv::types::Bookmark, kanariv::utils::Error> bookmarkGet(int64_t id) override;
    /**
     * @brief Removes a bookmark by id.
     * @ingroup kanariv_db
     * @param id Bookmark id.
     * @return Success, InvalidArg, NotFound or Internal.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<void, kanariv::utils::Error> bookmarkRemove(int64_t id) override;

    // Libraries and aggregate statistics
    /**
     * @brief Adds a library.
     * @ingroup kanariv_db
     * @param path Root path (must be non-empty, UNIQUE).
     * @param name Display name.
     * @return New id or Error.
     * @par Thread safety
     * Thread-safe: unique_lock.
     */
    std::expected<int64_t, kanariv::utils::Error> libraryAdd(std::string_view path,
                                                            std::string_view name = {});
    /**
     * @brief Lists all libraries.
     * @ingroup kanariv_db
     * @return Vector of libraries or Error.
     * @par Thread safety
     * Thread-safe: shared_lock (delegates to libraryListLocked).
     */
    std::expected<std::vector<kanariv::types::Library>, kanariv::utils::Error>
    listLibraries() override;
    /**
     * @brief Updates a library definition.
     * @param l Library value containing the existing identifier and new fields.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error>
    libraryUpdate(const kanariv::types::Library& l) override;
    /**
     * @brief Deletes a library definition.
     * @param id Library identifier.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     * @par Thread safety
     * Thread-safe; takes the database write lock.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error> libraryDelete(int64_t id) override;

    /**
     * @brief Returns aggregate DB stats.
     * @ingroup kanariv_db
     * @return kanariv::types::PlaybackStats or Error.
     * @par Thread safety
     * Thread-safe: shared_lock + cacheMutex_ for cached statements.
     */
    std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> getStats() override;
    /**
     * @brief Returns engine-oriented playback statistics.
     * @return Statistics snapshot, or a backend error.
     * @par Thread safety
     * Thread-safe; takes a shared database lock.
     * @ingroup kanariv_db
     */
    std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> playbackStats() override;

    /**
     * @brief Returns library stats (counts and totals, optionally top-N lists).
     * @ingroup kanariv_db
     * @param topTracks Omitted or non-positive = totals only; a value requests
     *   that many top-track entries.
     * @param topAlbums Omitted or non-positive = not requested; a value
     *   requests that many top albums by total plays.
     * @param topGenres Omitted or non-positive = not requested; a value
     *   requests that many top genres by total plays.
     * @param topArtists Omitted or non-positive = not requested; a value
     *   requests that many top artists by total plays. Multi-artist tracks
     *   are split so each individual artist is counted separately.
     * @return Stats or Error.
     * @par Thread safety
     * Thread-safe: shared_lock + cacheMutex_.
     */
    std::expected<kanariv::types::LibraryStats, kanariv::utils::Error>
    libraryStats(std::optional<int> topTracks = std::nullopt,
                 std::optional<int> topAlbums = std::nullopt,
                 std::optional<int> topGenres = std::nullopt,
                 std::optional<int> topArtists = std::nullopt) override;

    // Persisted engine state and playback history
    /**
     * @brief Loads persisted engine state.
     * @return State snapshot, or a backend error.
     * @par Thread safety
     * Thread-safe; takes the database lock.
     * @ingroup kanariv_db
     */
    std::expected<kanariv::types::EngineState, kanariv::utils::Error> loadEngineState() override;
    /**
     * @brief Persists engine state.
     * @param state State snapshot to write.
     * @return Success, or a backend error.
     * @par Thread safety
     * Thread-safe; serializes the write.
     * @ingroup kanariv_db
     */
    std::expected<void, kanariv::utils::Error>
    saveEngineState(const kanariv::types::EngineState& state) override;
    /**
     * @brief Persists shuffle state and its cursor.
     * @param enabled Whether shuffle is enabled.
     * @param permutation Ordered shuffle positions.
     * @param cursor_position Current position in the permutation.
     * @return Success, or a backend error.
     */
    std::expected<void, kanariv::utils::Error>
    saveEngineShuffle(bool enabled, std::span<const int64_t> permutation,
                      int64_t cursor_position) override;
    /**
     * @brief Persists the active queue cursor position.
     * @param cursor_position Zero-based cursor position.
     * @return Success, or a backend error.
     */
    std::expected<void, kanariv::utils::Error> saveEngineCursor(int64_t cursor_position) override;
    /**
     * @brief Records a completed or partially completed track play.
     * @param track_id Track identifier.
     * @param started_at Start timestamp.
     * @param completed_at Completion timestamp.
     * @param position_ms Position reached in milliseconds.
     * @param completion_pct Completion percentage.
     * @param queue_id Queue identifier.
     * @return Success, or a backend error.
     */
    std::expected<void, kanariv::utils::Error> markPlayed(int64_t track_id, int64_t started_at,
                                                         int64_t completed_at, int64_t position_ms,
                                                         double completion_pct,
                                                         int64_t queue_id) override;
    /**
     * @brief Lists engine history entries.
     * @param limit Maximum number of entries.
     * @return History entries, or a backend error.
     */
    std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    engineHistoryList(int limit) override;
    /**
     * @brief Clears engine playback history.
     * @return Success, or a backend error.
     */
    std::expected<void, kanariv::utils::Error> engineHistoryClear() override;

}; // class Database

} // namespace kanariv::db
