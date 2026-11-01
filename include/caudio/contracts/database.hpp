#pragma once

/**
 * @file database.hpp
 * @brief Neutral persistence contract used by the playback engine.
 * @defgroup caudio_contracts caudio persistence contracts
 */

#include <array>
#include <caudio/fwd.hpp>
#include <caudio/types/db.hpp>
#include <caudio/types/engine.hpp>
#include <caudio/types/record.hpp>
#include <caudio/types/wire.hpp>
#include <caudio/utils/error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace caudio::contracts {

/**
 * @brief Backend-neutral persistence interface used by engine and service.
 * @ingroup caudio_contracts
 * @details Implementations provide the queue, track, playlist, bookmark,
 * library and playback-state operations required by higher-level components.
 * Methods return `std::expected` when an operation can fail. The interface
 * does not own the concrete backend, and callers must keep the implementation
 * alive for the duration of every call and any operation that depends on it.
 * Implementations own synchronization; value results are copies and do not
 * expose backend-owned storage.
 *
 * @par Thread safety
 * Concrete implementations must document their concurrency guarantees.
 * The engine may call this interface from its monitor/decode and service
 * paths, so implementations used with `Engine` must serialize their own
 * database access.
 *
 * @par Error behavior
 * Implementations should use `StatusCode::NotFound` for absent entities,
 * `InvalidArg` for invalid caller input, `State` for an invalid lifecycle
 * state, and `Io`, `Corrupt`, or `Internal` for backend failures.
 */
class IDatabase {
  public:
    /**
     * @brief Destroys the database contract.
     * @ingroup caudio_contracts
     */
    virtual ~IDatabase() = default;

    // Connection and diagnostics
    /**
     * @brief Reports whether the backing database is open and usable.
     * @return `true` when database operations may be issued; otherwise `false`.
     * @par Thread safety
     * The implementation must make this query safe to call concurrently with
     * other public operations.
     */
    virtual bool isOpen() const noexcept = 0;
    /**
     * @brief Returns the logger associated with the implementation.
     * @return A non-owning logger pointer, or `nullptr` when logging is disabled.
     * @par Ownership
     * The database retains no ownership of the returned logger. The logger must
     * outlive the database and all calls that use it.
     */
    virtual caudio::utils::Logger* logger() const noexcept = 0;

    // Tracks
    /**
     * @brief Loads a track by id.
     * @return Track on success, or NotFound when no row has this id.
     */
    virtual std::expected<caudio::types::Track, caudio::utils::Error> getTrack(int64_t id) = 0;
    /**
     * @brief Inserts a track and returns its new row identifier.
     * @param t Track value to persist.
     * @return The new identifier, or an error such as `AlreadyExists`,
     * `InvalidArg`, or `Internal`.
     */
    virtual std::expected<int64_t, caudio::utils::Error>
    insertTrack(const caudio::types::Track& t) = 0;
    /**
     * @brief Finds a track by its 32-byte content fingerprint.
     * @param fp Fingerprint to look up.
     * @return The matching track, or `NotFound` when no row matches.
     */
    virtual std::expected<caudio::types::Track, caudio::utils::Error>
    findByFingerprint(const std::array<uint8_t, 32>& fp) = 0;
    /**
     * @brief Finds a track by filesystem path.
     * @param path Path key used by the database.
     * @return The matching track, or `NotFound` when no row matches.
     */
    virtual std::expected<caudio::types::Track, caudio::utils::Error>
    findByPath(std::string_view path) = 0;
    /**
     * @brief Finds a track by durable source identity.
     * @param source Source kind and stable key; path rows use the filesystem
     * path as the key after normalization.
     * @return Track on success, or NotFound.
     */
    virtual std::expected<caudio::types::Track, caudio::utils::Error>
    findBySource(const caudio::types::SourceRef& source) = 0;
    /**
     * @brief Updates an existing track row.
     * @param t Track containing the row identifier and fields to persist.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error>
    updateTrack(const caudio::types::Track& t) = 0;
    /**
     * @brief Deletes a track by identifier.
     * @param id Track identifier.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> deleteTrack(int64_t id) = 0;

    // Queue lookup and mutation
    /**
     * @brief Loads a queue container by identifier.
     * @param qid Queue identifier; implementations normalize zero to the default queue.
     * @return Queue value, or `NotFound`/a backend error.
     */
    virtual std::expected<caudio::types::Queue, caudio::utils::Error> getQueue(int64_t qid) = 0;
    /**
     * @brief Reports whether a queue exists.
     * @param qid Queue id; implementations normalize zero to the default queue.
     */
    virtual std::expected<bool, caudio::utils::Error> queueExists(int64_t qid) = 0;
    /**
     * @brief Lists all queue containers.
     * @return Queue values, or a backend error.
     */
    virtual std::expected<std::vector<caudio::types::Queue>, caudio::utils::Error> listQueues() = 0;
    /**
     * @brief Returns the number of items in a queue.
     * @return Item count; a missing or empty queue returns zero.
     */
    virtual std::size_t queueCount(int64_t qid) = 0;
    /**
     * @brief Finds a track's zero-based position in a queue.
     * @return Position of the first matching item, or `std::nullopt` when absent.
     */
    virtual std::optional<std::size_t> queueIndexOf(int64_t qid, int64_t trackId) = 0;
    /**
     * @brief Loads the track at a queue position.
     * @return Track on success, or NotFound for an invalid position or missing row.
     */
    virtual std::expected<caudio::types::Track, caudio::utils::Error>
    queueTrackAt(int64_t qid, int64_t position) = 0;
    /**
     * @brief Lists queue items in ascending position order.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Queue items, or a backend error.
     */
    virtual std::expected<std::vector<caudio::types::QueueItem>, caudio::utils::Error>
    listQueueItems(int64_t qid) = 0;
    /**
     * @brief Creates a queue container.
     * @param name Display name for the queue.
     * @param library_id Owning library identifier.
     * @return The new queue identifier, or `InvalidArg`/a backend error.
     */
    virtual std::expected<int64_t, caudio::utils::Error> createQueue(std::string_view name,
                                                                     int64_t library_id = 1) = 0;
    /**
     * @brief Deletes a queue container.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> deleteQueue(int64_t qid) = 0;
    /**
     * @brief Sets whether a queue is temporary.
     * @param qid Queue identifier.
     * @param temp `true` to mark the queue temporary, otherwise `false`.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> setQueueTemp(int64_t qid, bool temp) = 0;
    /**
     * @brief Enqueues track identifiers atomically.
     * @param qid Queue identifier; zero selects the default queue.
     * @param tids Ordered track identifiers to append.
     * @return Success, or an error; no partial batch is committed.
     */
    virtual std::expected<void, caudio::utils::Error>
    queueEnqueueBatch(int64_t qid, std::span<const int64_t> tids) = 0;
    /**
     * @brief Enqueues a vector of track identifiers atomically.
     * @param qid Queue identifier; zero selects the default queue.
     * @param tids Ordered track identifiers to append.
     * @return Success, or an error; no partial batch is committed.
     */
    virtual std::expected<void, caudio::utils::Error>
    queueEnqueueBatch(int64_t qid, const std::vector<int64_t>& tids) = 0;
    /**
     * @brief Enqueues one track at a position.
     * @param qid Queue identifier; zero selects the default queue.
     * @param tid Track identifier.
     * @param pos Zero-based insertion position, or `-1` to append.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> queueEnqueue(int64_t qid, int64_t tid,
                                                                   int64_t pos = -1) = 0;
    /**
     * @brief Removes the item at a queue position.
     * @param qid Queue identifier; zero selects the default queue.
     * @param pos Zero-based position to remove.
     * @return Success, or `NotFound` when no item has that position.
     */
    virtual std::expected<void, caudio::utils::Error> queueRemove(int64_t qid, int64_t pos) = 0;
    /**
     * @brief Replaces all items in a queue from an ordered span of identifiers.
     * @param qid Queue identifier; zero selects the default queue.
     * @param ids New ordered track identifiers.
     * @return Success, or an error; implementations should commit atomically.
     */
    virtual std::expected<void, caudio::utils::Error>
    queueReplaceAll(int64_t qid, std::span<const int64_t> ids) = 0;
    /**
     * @brief Replaces all items in a queue from an ordered vector of identifiers.
     * @param qid Queue identifier; zero selects the default queue.
     * @param ids New ordered track identifiers.
     * @return Success, or an error; implementations should commit atomically.
     */
    virtual std::expected<void, caudio::utils::Error>
    queueReplaceAll(int64_t qid, const std::vector<int64_t>& ids) = 0;
    /**
     * @brief Removes all items from a queue.
     * @param qid Queue identifier; zero selects the default queue.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> queueClear(int64_t qid) = 0;

    // Engine persistence and history
    /**
     * @brief Loads the persisted playback-engine state.
     * @return Saved state, or a backend error when it cannot be read.
     */
    virtual std::expected<caudio::types::EngineState, caudio::utils::Error> loadEngineState() = 0;
    /**
     * @brief Persists the complete playback-engine state.
     * @param state State snapshot to write.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error>
    saveEngineState(const caudio::types::EngineState& state) = 0;
    /**
     * @brief Persists shuffle enablement, permutation, and cursor position.
     * @param enabled Whether shuffle is active.
     * @param permutation Ordered queue positions used for shuffle playback.
     * @param cursor_position Current position in the permutation.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error>
    saveEngineShuffle(bool enabled, std::span<const int64_t> permutation,
                      int64_t cursor_position) = 0;
    /**
     * @brief Persists the current queue cursor position.
     * @param cursor_position Zero-based cursor position.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> saveEngineCursor(int64_t cursor_position) = 0;
    /**
     * @brief Records a completed or partially completed track play.
     * @param track_id Track identifier.
     * @param started_at Start timestamp in the implementation's persisted time unit.
     * @param completed_at Completion timestamp in the same unit.
     * @param position_ms Position reached in milliseconds.
     * @param completion_pct Completion percentage.
     * @param queue_id Queue from which the track was played.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error>
    markPlayed(int64_t track_id, int64_t started_at, int64_t completed_at, int64_t position_ms,
               double completion_pct, int64_t queue_id) = 0;
    /**
     * @brief Lists playback history entries.
     * @param limit Maximum number of entries; the implementation defines the
     * behavior of non-positive values.
     * @return History entries, or a backend error.
     */
    virtual std::expected<std::vector<caudio::types::HistoryEntry>, caudio::utils::Error>
    engineHistoryList(int limit) = 0;
    /**
     * @brief Clears playback history.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> engineHistoryClear() = 0;

    // Libraries and statistics
    /**
     * @brief Lists configured library roots.
     * @return Library values, or a backend error.
     */
    virtual std::expected<std::vector<caudio::types::Library>, caudio::utils::Error>
    listLibraries() = 0;
    /**
     * @brief Updates an existing library definition.
     * @param l Library value containing the identifier and new fields.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error>
    libraryUpdate(const caudio::types::Library& l) = 0;
    /**
     * @brief Deletes a library root by identifier.
     * @param id Library identifier.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> libraryDelete(int64_t id) = 0;
    /**
     * @brief Returns aggregate database statistics.
     * @return Statistics snapshot, or a backend error.
     */
    virtual std::expected<caudio::types::PlaybackStats, caudio::utils::Error> getStats() = 0;
    /**
     * @brief Returns aggregate playback statistics for engine consumers.
     * @return Statistics snapshot, or a backend error.
     */
    virtual std::expected<caudio::types::PlaybackStats, caudio::utils::Error> playbackStats() = 0;
    /**
     * @brief Returns library totals and optional top-N listening reports.
     * @param topTracks Number of top tracks by total plays; omitted or non-positive = totals only.
     * @param topAlbums Number of top albums by total plays; omitted or non-positive = not requested.
     * @param topGenres Number of top genres by total plays; omitted or non-positive = not requested.
     * @param topArtists Number of top artists by total plays; omitted or non-positive = not requested.
     * @details Multi-artist tracks are split so each individual artist is counted
     * separately in `top_artists`.
     */
    virtual std::expected<caudio::types::LibraryStats, caudio::utils::Error>
    libraryStats(std::optional<int> topTracks = std::nullopt,
                 std::optional<int> topAlbums = std::nullopt,
                 std::optional<int> topGenres = std::nullopt,
                 std::optional<int> topArtists = std::nullopt) = 0;
    /**
     * @brief Lists tracks matching an optional filter.
     * @param q Optional, non-owning filter valid for the duration of the call;
     * fields are combined with AND and `search` matches common text metadata.
     */
    virtual std::expected<std::vector<caudio::types::Track>, caudio::utils::Error>
    listTracks(const caudio::types::TrackQuery* q = nullptr) = 0;

    // Bookmarks
    // Bookmarks
    /**
     * @brief Lists bookmarks belonging to a track.
     * @param tid Track identifier.
     * @return Bookmarks ordered by the implementation, or a backend error.
     */
    virtual std::expected<std::vector<caudio::types::Bookmark>, caudio::utils::Error>
    listBookmarks(int64_t tid) = 0;
    /**
     * @brief Loads a bookmark by identifier.
     * @param id Bookmark identifier.
     * @return Bookmark, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<caudio::types::Bookmark, caudio::utils::Error>
    bookmarkGet(int64_t id) = 0;
    /**
     * @brief Adds a bookmark at a position in milliseconds.
     * @param tid Track identifier.
     * @param pos Position in milliseconds.
     * @param note Optional user note.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> bookmarkAdd(int64_t tid, int64_t pos,
                                                                  std::string_view note = {}) = 0;
    /**
     * @brief Removes a bookmark by identifier.
     * @param id Bookmark identifier.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> bookmarkRemove(int64_t id) = 0;
    // Playlists
    /**
     * @brief Lists playlists.
     * @return Playlist values, or a backend error.
     */
    virtual std::expected<std::vector<caudio::types::Playlist>, caudio::utils::Error>
    listPlaylists() = 0;
    /**
     * @brief Lists tracks in a playlist ordered by position.
     * @param pid Playlist identifier.
     * @return Track values, or `NotFound`/a backend error.
     */
    virtual std::expected<std::vector<caudio::types::Track>, caudio::utils::Error>
    playlistGetTracks(int64_t pid) = 0;
    /**
     * @brief Loads a playlist by identifier.
     * @param id Playlist identifier.
     * @return Playlist, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<caudio::types::Playlist, caudio::utils::Error>
    getPlaylist(int64_t id) = 0;
    /**
     * @brief Creates a playlist and returns its identifier.
     * @param name Display name; implementations may reject empty names.
     * @param type Playlist type, where zero denotes a manual playlist.
     * @param smart_query Smart-playlist query; empty for a manual playlist.
     * @return New identifier, or `InvalidArg`/a backend error.
     */
    virtual std::expected<int64_t, caudio::utils::Error>
    createPlaylist(std::string_view name, int type = 0, std::string_view smart_query = {}) = 0;
    /**
     * @brief Adds a track to a playlist.
     * @param pid Playlist identifier.
     * @param tid Track identifier.
     * @param pos Zero-based insertion position, or `-1` to append.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> playlistAddTrack(int64_t pid, int64_t tid,
                                                                       int64_t pos = -1) = 0;
    /**
     * @brief Removes a track from a playlist.
     * @param pid Playlist identifier.
     * @param tid Track identifier.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> playlistRemoveTrack(int64_t pid,
                                                                          int64_t tid) = 0;
    /**
     * @brief Moves a playlist item between positions.
     * @param pid Playlist identifier.
     * @param from Existing zero-based position.
     * @param to Destination zero-based position.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> playlistReorder(int64_t pid, int64_t from,
                                                                      int64_t to) = 0;
    /**
     * @brief Changes a playlist name.
     * @param id Playlist identifier.
     * @param newName Replacement display name.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> renamePlaylist(int64_t id,
                                                                     std::string_view newName) = 0;
    /**
     * @brief Creates a playlist from an ordered list of track identifiers.
     * @param name Display name.
     * @param trackIds Track identifiers in playlist order.
     * @return New playlist identifier, or an error; implementations should
     * create the playlist and its items atomically.
     */
    virtual std::expected<int64_t, caudio::utils::Error>
    createPlaylistFromTracks(std::string_view name, std::span<const int64_t> trackIds) = 0;
    /**
     * @brief Deletes a playlist by identifier.
     * @param id Playlist identifier.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, caudio::utils::Error> deletePlaylist(int64_t id) = 0;
};

} // namespace caudio::contracts
