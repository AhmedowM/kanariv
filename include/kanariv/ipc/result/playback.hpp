#pragma once

/**
 * @file playback.hpp
 * @brief Playback status and volume results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <kanariv/types/wire.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Playback Status Result
// ---------------------------------------------------------------------------

/**
 * @struct Status
 * @brief Current playback status returned by StatusReq and most mutating commands.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Status", "state": "Playing", "pos": 120.5, "dur": 240.0, "vol": 0.8,
 * "muted": false, "shuffle": true, "repeat": "Off", "track_id": 123, "title": "Song", "artist":
 * "Artist", "path": "/music/song.mp3", "q_size": 10, "q_idx": 0}
 *
 * @param state Current playback state.
 * @param pos Current playback position in seconds.
 * @param dur Total duration of current track in seconds.
 * @param vol Current volume level (0.0 to 1.0).
 * @param muted Whether playback is muted.
 * @param shuffle Whether shuffle mode is enabled.
 * @param repeat Current repeat mode.
 * @param track_id ID of the currently playing track (0 if none).
 * @param title Title of the current track.
 * @param artist Artist of the current track.
 * @param path File path of the current track.
 * @param q_size Number of tracks in the active queue.
 * @param q_idx Index of the current track in the queue (0-based).
 */
struct Status final {
    /**
     * @brief Current playback state.
     */
    kanariv::types::PlaybackState state{kanariv::types::PlaybackState::Stopped};
    /**
     * @brief Current playback position in seconds.
     */
    double pos{0.0};
    /**
     * @brief Total duration of current track in seconds.
     */
    double dur{0.0};
    /**
     * @brief Current volume level (0.0 to 1.0).
     */
    float vol{1.0f};
    /**
     * @brief Whether playback is muted.
     */
    bool muted{false};
    /**
     * @brief Whether shuffle mode is enabled.
     */
    bool shuffle{false};
    /**
     * @brief Current repeat mode.
     */
    kanariv::types::RepeatMode repeat{kanariv::types::RepeatMode::Off};
    /**
     * @brief ID of the currently playing track (0 if none).
     */
    int64_t track_id{0};
    /**
     * @brief Title of the current track.
     */
    std::string title{};
    /**
     * @brief Artist of the current track.
     */
    std::string artist{};
    /**
     * @brief File path of the current track.
     */
    std::string path{};
    /**
     * @brief Number of tracks in the active queue.
     */
    std::size_t q_size{0};
    /**
     * @brief Index of the current track in the queue (0-based).
     */
    std::size_t q_idx{0};
    /**
     * @brief Daemon/library version (full git tag).
     * @details Populated by the daemon (service_detail::buildStatus); empty
     * when the producer did not set one.
     */
    std::string version;
};

// ---------------------------------------------------------------------------
// Volume Results
// ---------------------------------------------------------------------------

/**
 * @struct VolumeInfo
 * @brief Current volume level and mute state (returned by VolumeSet).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "VolumeInfo", "vol": 0.8, "muted": false}
 *
 * @param vol Current volume level (0.0 to 1.0).
 * @param muted Whether playback is muted.
 */
struct VolumeInfo final {
    /**
     * @brief Current volume level (0.0 to 1.0).
     */
    float vol{1.0f};
    /**
     * @brief Whether playback is muted.
     */
    bool muted{false};
};

} // namespace kanariv::ipc
