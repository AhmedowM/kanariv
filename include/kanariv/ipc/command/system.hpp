#pragma once

/**
 * @file system.hpp
 * @brief Info and utility commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Info and Utility Commands
// ---------------------------------------------------------------------------

/**
 * @struct Info
 * @brief Get detailed information about the currently playing track.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Info"}
 *
 * @return TrackInfo object with track metadata and play statistics.
 */
struct Info final {};

/**
 * @struct Shutdown
 * @brief Request the service daemon to shut down.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Shutdown"}
 *
 * @return Empty (success with no data).
 */
struct Shutdown final {};

/**
 * @struct Preview
 * @brief Preview an audio file (play without adding to queue).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Preview", "file": "/music/song.mp3"}
 *
 * @param file Path to audio file.
 * @return Empty (success with no data).
 */
struct Preview final {
    /**
     * @brief Path to audio file.
     */
    std::string file{};
};

} // namespace kanariv::ipc
