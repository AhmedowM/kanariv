#pragma once

/**
 * @file tag.hpp
 * @brief Tag editing commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Tag Commands
// ---------------------------------------------------------------------------

/**
 * @struct TagEdit
 * @brief Edit a metadata tag for a track.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "TagEdit", "id": 123, "field": "artist", "value": "New Artist"}
 *
 * @param id Track ID.
 * @param field Tag field name (e.g., "title", "artist", "album", "genre", "year", "track_number",
 * "disc_number", "album_artist").
 * @param value New value for the tag.
 * @return Empty (success with no data).
 */
struct TagEdit final {
    /**
     * @brief Track ID.
     */
    int64_t id{0};
    /**
     * @brief Tag field name (e.g., "title", "artist", "album", "genre", "year",
     * "track_number", "disc_number", "album_artist").
     */
    std::string field{};
    /**
     * @brief New value for the tag.
     */
    std::string value{};
};

/**
 * @struct TagGet
 * @brief Get metadata for a track, optionally including cover art.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "TagGet", "id": 123}
 *
 * @param id Track ID.
 * @param cover When true, include base64-encoded cover art and MIME type.
 * @return SingleTrack object with full track metadata; cover_mime and
 *   cover_data populated when cover=true and cover art is available.
 */
struct TagGet final {
    /**
     * @brief Track ID. Mutually exclusive with `path`.
     */
    std::optional<int64_t> id{};
    /**
     * @brief File path to look up the track by. Mutually exclusive with `id`.
     */
    std::optional<std::string> path{};
    /**
     * @brief When true, include cover art (base64-encoded) and MIME type.
     * Reads the file at track.cover_art_path.
     */
    bool cover{false};
};

/**
 * @struct TagBatchEditEntry
 * @brief A single tag assignment within a batch edit.
 * @ingroup kanariv_ipc
 *
 * Either `id` or `path` must be set to identify the track.
 *
 * @json_example {"id": 123, "field": "artist", "value": "New Artist"}
 *
 * @param id Track id (optional if path is given).
 * @param path File path to identify the track (optional if id is given).
 * @param field Tag field name (title, artist, album, album_artist, genre,
 *   year, track_number, disc_number).
 * @param value New value for the tag.
 */
struct TagBatchEditEntry final {
    std::optional<int64_t> id{};
    std::optional<std::string> path{};
    std::string field{};
    std::string value{};
};

/**
 * @struct TagBatchEdit
 * @brief Batch-edit multiple tag fields across one or more tracks.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "TagBatchEdit", "entries": [{"id": 1, "field": "artist", "value": "A"}]}
 *
 * @param entries One or more tag assignments to apply.
 * @param dry_run When true, validate and report what would change without
 *   writing any file or DB row.
 * @return TagBatchEditResult with per-entry success/failure.
 */
struct TagBatchEdit final {
    std::vector<TagBatchEditEntry> entries{};
    bool dry_run{false};
};

} // namespace kanariv::ipc
