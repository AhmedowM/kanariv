#pragma once

/**
 * @file lyrics.hpp
 * @brief Lyrics commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <kanariv/types/record.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Lyrics Commands
// ---------------------------------------------------------------------------

/**
 * @struct LyricsGet
 * @brief Get lyrics for a track (default: current track).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LyricsGet", "track_id": 123}
 *
 * @param track_id Track id. If omitted, uses the current/persisted track.
 * @return Lyrics result.
 */
struct LyricsGet final {
    /**
     * @brief Track id. If omitted, uses the current/persisted track.
     */
    std::optional<int64_t> track_id{};
};

/**
 * @struct LyricsSet
 * @brief Set (upsert) lyrics for a track (default: current track).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LyricsSet", "track_id": 123, "lrc_text": "[00:12.34]Hello world", "is_synced": true, "source": "import"}
 *
 * @param lrc_text Lyrics text (LRC format when synced).
 * @param is_synced True when lrc_text contains time-coded lines.
 * @param source Provenance label (e.g. "local", "import").
 * @param track_id Track id. If omitted, uses the current/persisted track.
 * @return Empty (success with no data).
 */
struct LyricsSet final {
    /**
     * @brief Lyrics text (LRC format when synced).
     */
    std::string lrc_text{};
    /**
     * @brief True when lrc_text contains time-coded lines.
     */
    bool is_synced{false};
    /**
     * @brief Provenance label (e.g. "local", "import").
     */
    std::string source{};
    /**
     * @brief Track id. If omitted, uses the current/persisted track.
     */
    std::optional<int64_t> track_id{};
};

/**
 * @struct LyricsRemove
 * @brief Remove lyrics for a track (default: current track).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "LyricsRemove", "track_id": 123}
 *
 * @param track_id Track id. If omitted, uses the current/persisted track.
 * @return Empty (success with no data).
 */
struct LyricsRemove final {
    /**
     * @brief Track id. If omitted, uses the current/persisted track.
     */
    std::optional<int64_t> track_id{};
};

} // namespace kanariv::ipc
