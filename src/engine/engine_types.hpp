#pragma once

/**
 * @file engine_types.hpp
 * @brief Engine runtime state types (queue cursor/shuffle, persisted state).
 * @ingroup kanariv_engine
 * @details `QueueState` and `EngineState` are members of `Engine::Impl`
 * only: nothing in the public API names them. They live here so the
 * installed `kanariv/types/engine.hpp` stays limited to the types the
 * public surface actually exposes (`EngineConfig`, `EngineCallbacks`,
 * `EngineEvent`, `EngineEventType`).
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <kanariv/types/wire.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kanariv::engine {

/**
 * @brief In-memory queue cursor and shuffle state.
 * @ingroup kanariv_engine
 * @details `perm` is empty when shuffle is off; when on it holds a
 * permutation of `[0, queueCount)` shuffled via detail::shufflePerm.
 * `cursor` is the next index to consume (persisted as `cursor_pos`).
 * `queue_id` is the active queue (persisted as `active_queue_id`).
 * Protected by `Engine::queueMutex_` for writers; readers use atomics
 * or hold the mutex.
 */
struct QueueState {
    bool shuffle{false}; ///< Whether shuffle is enabled.
    kanariv::types::RepeatMode repeat{kanariv::types::RepeatMode::Off}; ///< Repeat mode.
    std::vector<int64_t> perm{}; ///< Shuffle permutation (positions, not track_ids).
    size_t cursor{0};            ///< Next position index in perm or linear queue.
    int64_t queue_id{1};         ///< Active queue id.
};

/**
 * @brief Persisted engine state (row `engine_state.id=1`).
 * @ingroup kanariv_engine
 * @details Mirrors columns `shuffle_enabled, repeat_mode, cursor_pos,
 * current_track_id, volume, shuffle_perm (BLOB), active_queue_id,
 * pre_mute_volume, position_ms`. `active_queue_id` may be missing on old DBs -- loadState falls
 * back to `1` and saveState tries `sqlNew` then `sqlOld`. `pre_mute_volume` and `position_ms` are
 * best-effort both ways (missing columns keep their defaults). `shuffle_perm` is `perm` serialized
 * as `int64_t` blob.
 * @see Engine::loadState
 * @see Engine::saveState
 */
struct EngineState {
    int shuffleEnabled{0}; ///< Persisted shuffle flag (0/1).
    kanariv::types::RepeatMode repeatMode{
        kanariv::types::RepeatMode::Off}; ///< Persisted repeat mode.
    int64_t cursorPos{0};                ///< Persisted cursor (QueueState::cursor).
    int64_t currentTrackId{0};           ///< Last current track id.
    float volume{1.0f};                  ///< Persisted volume [0,1].
    int64_t activeQueueId{1};            ///< Persisted active queue id.
    float preMuteVolume{0.5f};           ///< Restore level for unmute (0,1].
    int64_t positionMs{0};               ///< Persisted position in the current track.
};

} // namespace kanariv::engine
