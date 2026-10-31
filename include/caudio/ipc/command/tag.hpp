#pragma once

/**
 * @file tag.hpp
 * @brief Tag editing commands for the caudio IPC protocol.
 * @ingroup caudio_ipc
 */

#include <cstdint>
#include <string>

namespace caudio::ipc {

// ---------------------------------------------------------------------------
// Tag Commands
// ---------------------------------------------------------------------------

/**
 * @struct TagEdit
 * @brief Edit a metadata tag for a track.
 * @ingroup caudio_ipc
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
 * @ingroup caudio_ipc
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
     * @brief Track ID.
     */
    int64_t id{0};
    /**
     * @brief When true, include cover art (base64-encoded) and MIME type.
     * Reads the file at track.cover_art_path.
     */
    bool cover{false};
};

} // namespace caudio::ipc
