#pragma once

/**
 * @file track.hpp
 * @brief Track commands for the caudio IPC protocol.
 * @ingroup caudio_ipc
 */

#include <cstdint>
#include <string>

namespace caudio::ipc {

// ---------------------------------------------------------------------------
// Track Commands
// ---------------------------------------------------------------------------

/**
 * @struct TrackRate
 * @brief Set the user rating (0..5) on a library track.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "TrackRate", "track_id": 7, "rating": 5}
 *
 * @param track_id Library track ID.
 * @param rating User rating (0..5).
 * @return Empty (success with no data).
 */
struct TrackRate final {
    /**
     * @brief Library track ID.
     */
    int64_t track_id{0};
    /**
     * @brief User rating (0..5).
     */
    int32_t rating{0};
};

} // namespace caudio::ipc
