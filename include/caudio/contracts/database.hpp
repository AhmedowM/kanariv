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
    /** @brief Returns whether the backing database is open. */
    virtual bool isOpen() const noexcept = 0;
    /** @brief Returns the non-owning logger used by the implementation. */
    virtual caudio::utils::Logger* logger() const noexcept = 0;

    // Tracks
    /**
     * @brief Loads a track by id.
     * @return Track on success, or NotFound when no row has this id.
     */
    virtual std::expected<caudio::types::Track, caudio::utils::Error> getTrack(int64_t id) = 0;
    /** @brief Inserts a track and returns its new row id. */
    virtual std::expected<int64_t, caudio::utils::Error>
    insertTrack(const caudio::types::Track& t) = 0;
    /** @brief Finds a track by its content fingerprint. */
    virtual std::expected<caudio::types::Track, caudio::utils::Error>
    findByFingerprint(const std::array<uint8_t, 32>& fp) = 0;
    /** @brief Finds a track by filesystem path. */
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
    /** @brief Updates an existing track row. */
    virtual std::expected<void, caudio::utils::Error>
    updateTrack(const caudio::types::Track& t) = 0;
    /** @brief Deletes a track by id. */
    virtual std::expected<void, caudio::utils::Error> deleteTrack(int64_t id) = 0;

    // Queue lookup and mutation
    /** @brief Loads a queue container by id. */
    virtual std::expected<caudio::types::Queue, caudio::utils::Error> getQueue(int64_t qid) = 0;
    /**
     * @brief Reports whether a queue exists.
     * @param qid Queue id; implementations normalize zero to the default queue.
     */
    virtual std::expected<bool, caudio::utils::Error> queueExists(int64_t qid) = 0;
    /** @brief Lists queue containers. */
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
    /** @brief Lists items in a queue ordered by position. */
    virtual std::expected<std::vector<caudio::types::QueueItem>, caudio::utils::Error>
    listQueueItems(int64_t qid) = 0;
    /** @brief Creates a queue container and returns its id. */
    virtual std::expected<int64_t, caudio::utils::Error> createQueue(std::string_view name,
                                                                     int64_t library_id = 1) = 0;
    /** @brief Deletes a queue container. */
    virtual std::expected<void, caudio::utils::Error> deleteQueue(int64_t qid) = 0;
    /** @brief Sets whether a queue is temporary. */
    virtual std::expected<void, caudio::utils::Error> setQueueTemp(int64_t qid, bool temp) = 0;
    /** @brief Enqueues a span of track ids atomically. */
    virtual std::expected<void, caudio::utils::Error>
    queueEnqueueBatch(int64_t qid, std::span<const int64_t> tids) = 0;
    /** @brief Enqueues a vector of track ids atomically. */
    virtual std::expected<void, caudio::utils::Error>
    queueEnqueueBatch(int64_t qid, const std::vector<int64_t>& tids) = 0;
    /** @brief Enqueues one track at a position, or appends when position is -1. */
    virtual std::expected<void, caudio::utils::Error> queueEnqueue(int64_t qid, int64_t tid,
                                                                   int64_t pos = -1) = 0;
    /** @brief Removes the item at a queue position. */
    virtual std::expected<void, caudio::utils::Error> queueRemove(int64_t qid, int64_t pos) = 0;
    /** @brief Replaces a queue from a span of track ids. */
    virtual std::expected<void, caudio::utils::Error>
    queueReplaceAll(int64_t qid, std::span<const int64_t> ids) = 0;
    /** @brief Replaces a queue from a vector of track ids. */
    virtual std::expected<void, caudio::utils::Error>
    queueReplaceAll(int64_t qid, const std::vector<int64_t>& ids) = 0;
    /** @brief Removes all items from a queue. */
    virtual std::expected<void, caudio::utils::Error> queueClear(int64_t qid) = 0;

    // Engine persistence and history
    /** @brief Loads the persisted playback-engine state. */
    virtual std::expected<caudio::types::EngineState, caudio::utils::Error> loadEngineState() = 0;
    /** @brief Persists the complete playback-engine state. */
    virtual std::expected<void, caudio::utils::Error>
    saveEngineState(const caudio::types::EngineState& state) = 0;
    /** @brief Persists shuffle enablement, permutation and cursor position. */
    virtual std::expected<void, caudio::utils::Error>
    saveEngineShuffle(bool enabled, std::span<const int64_t> permutation,
                      int64_t cursor_position) = 0;
    /** @brief Persists the current queue cursor position. */
    virtual std::expected<void, caudio::utils::Error> saveEngineCursor(int64_t cursor_position) = 0;
    /** @brief Records a completed or partially completed track play. */
    virtual std::expected<void, caudio::utils::Error>
    markPlayed(int64_t track_id, int64_t started_at, int64_t completed_at, int64_t position_ms,
               double completion_pct, int64_t queue_id) = 0;
    /** @brief Lists playback history entries, limited by the caller. */
    virtual std::expected<std::vector<caudio::types::HistoryEntry>, caudio::utils::Error>
    engineHistoryList(int limit) = 0;
    /** @brief Clears playback history. */
    virtual std::expected<void, caudio::utils::Error> engineHistoryClear() = 0;

    // Libraries and statistics
    /** @brief Lists configured library roots. */
    virtual std::expected<std::vector<caudio::types::Library>, caudio::utils::Error>
    listLibraries() = 0;
    /** @brief Updates an existing library definition; unknown ids are rejected. */
    virtual std::expected<void, caudio::utils::Error>
    libraryUpdate(const caudio::types::Library& l) = 0;
    /** @brief Deletes a library root by id. */
    virtual std::expected<void, caudio::utils::Error> libraryDelete(int64_t id) = 0;
    /** @brief Returns aggregate database statistics. */
    virtual std::expected<caudio::types::PlaybackStats, caudio::utils::Error> getStats() = 0;
    /** @brief Returns aggregate playback statistics for engine consumers. */
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
    /** @brief Lists bookmarks for a track. */
    virtual std::expected<std::vector<caudio::types::Bookmark>, caudio::utils::Error>
    listBookmarks(int64_t tid) = 0;
    /** @brief Loads a bookmark by id. */
    virtual std::expected<caudio::types::Bookmark, caudio::utils::Error>
    bookmarkGet(int64_t id) = 0;
    /** @brief Adds a bookmark at a position in milliseconds. */
    virtual std::expected<void, caudio::utils::Error> bookmarkAdd(int64_t tid, int64_t pos,
                                                                  std::string_view note = {}) = 0;
    /** @brief Removes a bookmark by id. */
    virtual std::expected<void, caudio::utils::Error> bookmarkRemove(int64_t id) = 0;
    // Playlists
    /** @brief Lists playlists. */
    virtual std::expected<std::vector<caudio::types::Playlist>, caudio::utils::Error>
    listPlaylists() = 0;
    /** @brief Lists tracks in a playlist ordered by position. */
    virtual std::expected<std::vector<caudio::types::Track>, caudio::utils::Error>
    playlistGetTracks(int64_t pid) = 0;
    /** @brief Loads a playlist by id. */
    virtual std::expected<caudio::types::Playlist, caudio::utils::Error>
    getPlaylist(int64_t id) = 0;
    /** @brief Creates a playlist and returns its id. */
    virtual std::expected<int64_t, caudio::utils::Error>
    createPlaylist(std::string_view name, int type = 0, std::string_view smart_query = {}) = 0;
    /** @brief Adds a track to a playlist. */
    virtual std::expected<void, caudio::utils::Error> playlistAddTrack(int64_t pid, int64_t tid,
                                                                       int64_t pos = -1) = 0;
    /** @brief Removes a track from a playlist. */
    virtual std::expected<void, caudio::utils::Error> playlistRemoveTrack(int64_t pid,
                                                                          int64_t tid) = 0;
    /** @brief Moves a playlist item between positions. */
    virtual std::expected<void, caudio::utils::Error> playlistReorder(int64_t pid, int64_t from,
                                                                      int64_t to) = 0;
    /** @brief Changes a playlist name. */
    virtual std::expected<void, caudio::utils::Error> renamePlaylist(int64_t id,
                                                                     std::string_view newName) = 0;
    /** @brief Creates a playlist from an ordered list of track ids. */
    virtual std::expected<int64_t, caudio::utils::Error>
    createPlaylistFromTracks(std::string_view name, std::span<const int64_t> trackIds) = 0;
    /** @brief Deletes a playlist by id. */
    virtual std::expected<void, caudio::utils::Error> deletePlaylist(int64_t id) = 0;
};

} // namespace caudio::contracts
