#pragma once

/**
 * @file library.hpp
 * @brief Library commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <optional>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Library Commands
// ---------------------------------------------------------------------------

/**
 * @struct LibraryScan
 * @brief Scan a directory for audio files and add them to the library.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryScan", "path": "/music", "full_hash": false}
 *
 * @param path Root directory to scan. If omitted, uses the music directory adjacent to the
 * database.
 * @param full_hash Full-file BLAKE3 instead of the default sampled fingerprint.
 * @return ScanReport object with the added track count.
 */
struct LibraryScan final {
    /**
     * @brief Root directory to scan. If omitted, uses the music directory
     * adjacent to the database.
     */
    std::optional<std::string> path{};
    /**
     * @brief Full-file BLAKE3 instead of the default sampled fingerprint.
     */
    bool full_hash{false};
};

/**
 * @struct LibrarySearch
 * @brief Search the library using full-text search.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibrarySearch", "query": "beatles", "limit": 50}
 *
 * @param query Search query string.
 * @param limit Maximum number of results to return.
 * @return Tracks object with vector of matching tracks.
 */
struct LibrarySearch final {
    /**
     * @brief Search query string.
     */
    std::string query{};
    /**
     * @brief Maximum number of results to return.
     */
    int limit{50};
};

/**
 * @struct LibraryStats
 * @brief Get library statistics, optionally with the top tracks.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryStats", "top_tracks": 10}
 *
 * @param top_tracks Omitted for the summary projection; a value (0 = totals only)
 * requests the detailed projection with that many top tracks.
 * @return `kanariv::types::LibraryStats` object with counts, totals, and
 * `top_tracks` engaged exactly when `top_tracks` was supplied.
 */
struct LibraryStats final {
    /**
     * @brief Number of top tracks by total plays; omitted or non-positive = totals only.
     */
    std::optional<int> top_tracks{};
    /**
     * @brief Number of top albums by total plays; omitted or non-positive = not requested.
     */
    std::optional<int> top_albums{};
    /**
     * @brief Number of top genres by total plays; omitted or non-positive = not requested.
     */
    std::optional<int> top_genres{};
    /**
     * @brief Number of top artists by total plays; omitted or non-positive = not requested.
     * @details Multi-artist tracks are split so each individual artist is counted separately.
     */
    std::optional<int> top_artists{};
};

/**
 * @struct LibraryAdd
 * @brief Add a file or directory to the library with metadata extraction.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryAdd", "path": "/music/new.mp3", "recursive": false}
 *
 * @param path Path to audio file or directory.
 * @param recursive If true and path is a directory, scan recursively.
 * @return Empty (success with no data).
 */
struct LibraryAdd final {
    /**
     * @brief Path to audio file or directory.
     */
    std::string path{};
    /**
     * @brief If true and path is a directory, scan recursively.
     */
    bool recursive{false};
};

/**
 * @struct LibraryRemove
 * @brief Remove a track from the library by ID or path.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryRemove", "query": "123"}
 *
 * @param query Track ID or path to remove.
 * @return Empty (success with no data).
 */
struct LibraryRemove final {
    /**
     * @brief Track ID or path to remove.
     */
    std::string query{};
};

/**
 * @struct LibraryList
 * @brief List library tracks with optional filtering and pagination.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryList", "query": "rock", "limit": 20, "offset": 0, "artist":
 * "Beatles", "album": "Abbey Road", "genre": "Rock"}
 *
 * @param query Optional search query.
 * @param limit Maximum results to return.
 * @param offset Offset for pagination.
 * @param artist Filter by artist.
 * @param album Filter by album.
 * @param genre Filter by genre.
 * @return Tracks object with vector of matching tracks.
 */
struct LibraryList final {
    /**
     * @brief Optional search query.
     */
    std::optional<std::string> query{};
    /**
     * @brief Maximum results to return.
     */
    int limit{50};
    /**
     * @brief Offset for pagination.
     */
    int offset{0};
    /**
     * @brief Sort field: id, title, artist, album, year, rating, play_count,
     * duration, date_added. Empty = default (id).
     */
    std::string sort{};
    /**
     * @brief Sort direction: "asc" or "desc". Default: asc.
     */
    std::string order{};
    /**
     * @brief Filter by artist.
     */
    std::optional<std::string> artist{};
    /**
     * @brief Filter by album.
     */
    std::optional<std::string> album{};
    /**
     * @brief Filter by genre.
     */
    std::optional<std::string> genre{};
    /**
     * @brief Filter by user rating (0..5).
     */
    std::optional<int32_t> rating{};
};

/**
 * @struct LibraryMaintenance
 * @brief Library reconciliation report or prune missing rows.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryMaintenance", "action": "report", "dry_run": true}
 *
 * @param action "report" (default) lists missing/duplicate tracks, "prune" deletes missing rows.
 * @param dry_run When true (default), report planned actions without changing the database.
 * @param library_id Library id to inspect (default: 1).
 * @return LibraryMaintenanceReport object or Empty on prune success.
 */
struct LibraryMaintenance final {
    /**
     * @brief Maintenance action: "report" or "prune".
     */
    std::string action{"report"};
    /**
     * @brief Report planned actions without changing the database (default true).
     */
    bool dry_run{true};
    /**
     * @brief Library id to inspect (default: 1).
     */
    int64_t library_id{1};
};

} // namespace kanariv::ipc
