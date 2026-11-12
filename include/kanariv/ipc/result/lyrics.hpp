#pragma once

/**
 * @file lyrics.hpp
 * @brief Lyrics results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 * @details The payload owns a snapshot of lyrics data. It can therefore be
 * retained after the client connection or database operation that produced
 * it has ended.
 */

#include <kanariv/types/record.hpp>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Lyrics Results
// ---------------------------------------------------------------------------

/**
 * @struct Lyrics
 * @brief Lyrics for a single track (returned by LyricsGet).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Lyrics", "id": 1, "track_id": 123, "lrc_text": "[00:12.34]Hello world", "is_synced": true, "source": "import", "fetched_at": 1234567890}
 *
 * @param id Row id (PK).
 * @param track_id Owning track id.
 * @param lrc_text Lyrics text (LRC format when synced).
 * @param is_synced True when lrc_text contains time-coded lines.
 * @param source Provenance label.
 * @param fetched_at Unix seconds when lyrics were stored.
 */
struct Lyrics final {
    /**
     * @brief Row id (PK).
     */
    int64_t id{};
    /**
     * @brief Owning track id.
     */
    int64_t track_id{};
    /**
     * @brief Lyrics text (LRC format when synced).
     */
    std::string lrc_text{};
    /**
     * @brief True when lrc_text contains time-coded lines.
     */
    bool is_synced{false};
    /**
     * @brief Provenance label.
     */
    std::string source{};
    /**
     * @brief Unix seconds when lyrics were stored.
     */
    int64_t fetched_at{};
};

} // namespace kanariv::ipc
