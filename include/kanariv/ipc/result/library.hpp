#pragma once

/**
 * @file library.hpp
 * @brief Library statistics and track results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <kanariv/types/record.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Library Statistics Results
// ---------------------------------------------------------------------------

// The canonical type is `kanariv::types::LibraryStats`.

// ---------------------------------------------------------------------------
// TrackRow and Playlist Results
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
    std::vector<kanariv::types::TrackRow> tracks{};
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
    std::vector<kanariv::types::TrackRow> tracks{};
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

/**
 * @struct AnalyzeReport
 * @brief ReplayGain analysis outcome (returned by LibraryAnalyze).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "AnalyzeReport", "tracks": 120, "analyzed": 118, "failed": 2,
 *   "skipped": 0}
 *
 * @param tracks Tracks considered in the pass.
 * @param analyzed Tracks measured and written.
 * @param failed Tracks that could not be measured or stored.
 * @param skipped Tracks left alone because they already had gains.
 */
struct AnalyzeReport final {
    /**
     * @brief Tracks considered in the pass.
     */
    int64_t tracks{0};
    /**
     * @brief Tracks measured and written.
     */
    int64_t analyzed{0};
    /**
     * @brief Tracks that could not be measured or stored.
     */
    int64_t failed{0};
    /**
     * @brief Tracks left alone because they already had gains.
     */
    int64_t skipped{0};
};

// ---------------------------------------------------------------------------
// TrackRow Detail Results
// ---------------------------------------------------------------------------

/**
 * @struct SingleTrack
 * @brief Single track with full metadata (returned by TagGet, LibrarySearch single result).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "SingleTrack", "track": {"id": 1, "title": "Song", ...}}
 *
 * @param track TrackRow metadata.
 * @param cover_mime MIME type of cover art (e.g., "image/jpeg"), empty when no cover.
 * @param cover_data Base64-encoded cover art (RFC 4648), empty when no cover.
 */
struct SingleTrack final {
    /**
     * @brief TrackRow metadata.
     */
    kanariv::types::TrackRow track{};
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
 * @brief TrackRow with play statistics (returned by Info command).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "TrackInfo", "track": {...}, "play_count": 42, "last_played":
 * 1699999999000}
 *
 * @param track TrackRow metadata.
 * @param play_count Number of times this track has been played.
 * @param last_played Timestamp of last play (milliseconds since epoch, 0 if never).
 */
struct TrackInfo final {
    /**
     * @brief TrackRow metadata.
     */
    kanariv::types::TrackRow track{};
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
    std::vector<kanariv::types::TrackRow> missing_tracks{};
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
