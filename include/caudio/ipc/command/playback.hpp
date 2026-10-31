#pragma once

/**
 * @file playback.hpp
 * @brief Playback control commands for the caudio IPC protocol.
 * @ingroup caudio_ipc
 */

#include <optional>

namespace caudio::ipc {

// ---------------------------------------------------------------------------
// Playback Control Commands
// ---------------------------------------------------------------------------

/**
 * @struct Play
 * @brief Start playback of the active queue.
 *
 * If no track is currently loaded, starts from the beginning of the queue.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Play"}
 */
struct Play final {};

/**
 * @struct Pause
 * @brief Pause the currently playing track.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Pause"}
 */
struct Pause final {};

/**
 * @struct Resume
 * @brief Resume playback from a paused state.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Resume"}
 */
struct Resume final {};

/**
 * @struct Restart
 * @brief Restart the current track from the beginning.
 *
 * Seeks to position 0 and ensures playback is started.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Restart"}
 */
struct Restart final {};

/**
 * @struct Stop
 * @brief Stop playback and clear the current track.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Stop"}
 */
struct Stop final {};

/**
 * @struct Next
 * @brief Advance to the next track in the queue.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Next"}
 */
struct Next final {};

/**
 * @struct Prev
 * @brief Go to the previous track in the queue.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Prev"}
 */
struct Prev final {};

/**
 * @struct Seek
 * @brief Seek to a specific position in the current track.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "Seek", "seconds": 120.5}
 *
 * @param seconds Target position in seconds.
 */

struct Seek final {
    double seconds{0.0};
};

/**
 * @struct StatusReq
 * @brief Request the current playback status.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "StatusReq"}
 *
 * @return Status object with current playback state.
 */
struct StatusReq final {};

/**
 * @struct StatusWait
 * @brief Wait for playback status to change, then return current status.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "StatusWait", "timeout_ms": 5000}
 *
 * @return Status object with current playback state once it changes (or
 *         when timeout_ms elapses).
 *
 * @param timeout_ms Maximum time to wait for a status change (default 5000).
 *         When the timeout elapses without a change, the current status is
 *         returned anyway. Use a short timeout (e.g. 1000 ms) for polling.
 */
struct StatusWait final {
    int timeout_ms{5000}; ///< Max milliseconds to wait for status change.
};

// ---------------------------------------------------------------------------
// Volume Commands
// ---------------------------------------------------------------------------

/**
 * @struct VolumeSet
 * @brief Set volume level, mute state, or apply a relative delta.
 *
 * Any combination of level, mute, and deltaPct can be provided.
 * If multiple are given, they are applied in order: level, then deltaPct, then mute.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "VolumeSet", "level": 50.0, "mute": false, "deltaPct": 10}
 *
 * @param level Absolute volume level as a percentage (0-100). If omitted, current level is
 * preserved unless modified by deltaPct.
 * @param mute Mute state. If true, volume is set to 0. If false and volume was 0, restores to 50%
 * (or level if provided).
 * @param deltaPct Relative volume change in percentage points (-100 to +100). Applied on top of
 * current or specified level.
 */
struct VolumeSet final {
    /**
     * @brief Absolute volume level as a percentage (0-100).
     * If omitted, current level is preserved unless modified by deltaPct.
     */
    std::optional<float> level{};

    /**
     * @brief Mute state. If true, volume is set to 0.
     * If false and volume was 0, restores to 50% (or level if provided).
     */
    std::optional<bool> mute{};

    /**
     * @brief Relative volume change in percentage points (-100 to +100).
     * Applied on top of current or specified level.
     */
    std::optional<int> deltaPct{};
};

} // namespace caudio::ipc
