#pragma once

/**
 * @file engine_types.hpp
 * @brief Engine runtime state types (queue cursor/shuffle).
 * @ingroup kanariv_engine
 * @details `QueueState` is a member of `Engine::Impl` only: nothing in the
 * public API names it. It lives here so the installed
 * `kanariv/types/engine.hpp` stays limited to the types the public surface
 * actually exposes (`EngineConfig`, `EngineCallbacks`, `EngineEvent`,
 * `EngineEventType`, `EngineState`).
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

} // namespace kanariv::engine
