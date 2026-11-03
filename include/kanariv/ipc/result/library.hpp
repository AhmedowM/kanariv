#pragma once

/**
 * @file library.hpp
 * @brief Library statistics and track results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <cstddef>
#include <cstdint>
#include <kanariv/types/record.hpp>
#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Library Statistics Results
// ---------------------------------------------------------------------------

// The canonical type is `kanariv::types::LibraryStats`.

// ---------------------------------------------------------------------------
// Track and Playlist Results
// ---------------------------------------------------------------------------

/**
 * @struct Tracks
 * @brief List of tracks (returned by LibraryList).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Tracks", "tracks": [{"id": 1, "title": "Song", ...}]}
 *
 * @param tracks Vector of tracks.
 */
struct Tracks final {
    /**
     * @brief Vector of tracks.
     */
    std::vector<kanariv::types::Track> tracks{};
};

/**
 * @struct SearchResults
 * @brief Library search hits with the query (returned by LibrarySearch).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "SearchResults", "query": "beatles", "tracks": [...]}
 *
 * @param query Search query string (for match highlighting).
 * @param tracks Matching tracks.
 */
struct SearchResults final {
    /**
     * @brief Search query string.
     */
    std::string query{};
    /**
     * @brief Matching tracks.
     */
    std::vector<kanariv::types::Track> tracks{};
};

/**
 * @struct ScanReport
 * @brief Library scan outcome (returned by LibraryScan).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ScanReport", "added": 12}
 *
 * @param added Number of newly inserted tracks.
 */
struct ScanReport final {
    /**
     * @brief Number of newly inserted tracks.
     */
    std::size_t added{0};
};

// ---------------------------------------------------------------------------
// Track Detail Results
// ---------------------------------------------------------------------------

/**
 * @struct SingleTrack
 * @brief Single track with full metadata (returned by TagGet, LibrarySearch single result).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "SingleTrack", "track": {"id": 1, "title": "Song", ...}}
 *
 * @param track Track metadata.
 * @param cover_mime MIME type of cover art (e.g., "image/jpeg"), empty when no cover.
 * @param cover_data Base64-encoded cover art (RFC 4648), empty when no cover.
 */
struct SingleTrack final {
    /**
     * @brief Track metadata.
     */
    kanariv::types::Track track{};
    /**
     * @brief MIME type of cover art (e.g., "image/jpeg"), empty when no cover.
     */
    std::string cover_mime{};
    /**
     * @brief Base64-encoded cover art (RFC 4648), empty when no cover.
     */
    std::string cover_data{};
};

/**
 * @struct TrackInfo
 * @brief Track with play statistics (returned by Info command).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "TrackInfo", "track": {...}, "play_count": 42, "last_played":
 * 1699999999000}
 *
 * @param track Track metadata.
 * @param play_count Number of times this track has been played.
 * @param last_played Timestamp of last play (milliseconds since epoch, 0 if never).
 */
struct TrackInfo final {
    /**
     * @brief Track metadata.
     */
    kanariv::types::Track track{};
    /**
     * @brief Number of times this track has been played.
     */
    int64_t play_count{0};
    /**
     * @brief Timestamp of last play (milliseconds since epoch, 0 if never).
     */
    int64_t last_played{0};
};

/**
 * @struct LibraryMaintenanceReport
 * @brief Library reconciliation report (returned by LibraryMaintenance).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LibraryMaintenanceReport", "tracks_checked": 100,
 *   "missing_tracks": [...], "duplicate_groups": 2, "duplicate_tracks": 3}
 *
 * @param tracks_checked Number of live rows examined.
 * @param missing_tracks Tracks whose paths no longer exist on disk.
 * @param duplicate_groups Number of path-sharing groups.
 * @param duplicate_tracks Number of redundant rows in duplicate groups.
 */
struct LibraryMaintenanceReport final {
    /**
     * @brief Number of live rows examined.
     */
    std::size_t tracks_checked{};
    /**
     * @brief Tracks whose paths no longer exist on disk.
     */
    std::vector<kanariv::types::Track> missing_tracks{};
    /**
     * @brief Number of path-sharing groups.
     */
    std::size_t duplicate_groups{};
    /**
     * @brief Number of redundant rows in duplicate groups.
     */
    std::size_t duplicate_tracks{};
};

} // namespace kanariv::ipc
