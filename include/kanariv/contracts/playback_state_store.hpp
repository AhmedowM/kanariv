#pragma once

/**
 * @file playback_state_store.hpp
 * @brief Playback-engine state persistence port.
 * @ingroup kanariv_contracts
 * @details The engine's durable state (queue cursor, current track, repeat,
 * shuffle permutation, volume, resume position) is a fixed record with no
 * relational needs. The default adapter writes a versioned JSON sidecar, so
 * remembering volume and position does not require a database; the
 * database-backed adapter exists only for the one-time import of the legacy
 * `engine_state` row.
 *
 * @par Ownership
 * State is produced and consumed by the daemon only (one writer), so adapters
 * need no cross-process locking. Implementations must be safe for the engine's
 * monitor thread and command handlers.
 */

#include <cstdint>
#include <expected>
#include <kanariv/contracts/store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <span>

namespace kanariv::contracts {

/**
 * @brief Durable playback-engine state.
 * @ingroup kanariv_contracts
 */
class IPlaybackStateStore : public IStore {
  public:
    /**
     * @brief Loads the persisted playback-engine state.
     * @return Saved state, or a backend error when it cannot be read. A
     * missing state is success with a default snapshot.
     */
    virtual std::expected<kanariv::types::EngineState, kanariv::utils::Error> load() = 0;

    /**
     * @brief Persists the complete playback-engine state.
     * @param state State snapshot to write.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error>
    save(const kanariv::types::EngineState& state) = 0;

    /**
     * @brief Persists shuffle enablement, permutation, and cursor position.
     * @param enabled Whether shuffle is active.
     * @param permutation Ordered queue positions used for shuffle playback.
     * @param cursor_position Current position in the permutation.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error>
    saveShuffle(bool enabled, std::span<const int64_t> permutation, int64_t cursor_position) = 0;

    /**
     * @brief Persists the current queue cursor position.
     * @param cursor_position Zero-based cursor position.
     * @return Success, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> saveCursor(int64_t cursor_position) = 0;
};

} // namespace kanariv::contracts
