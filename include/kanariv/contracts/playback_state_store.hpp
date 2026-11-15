#pragma once

/**
 * @file playback_state_store.hpp
 * @brief Playback-engine state persistence port.
 * @ingroup kanariv_contracts
 * @details The engine's durable state (queue cursor, current track, repeat,
 * shuffle permutation, volume, resume position) is a fixed record with no
 * relational needs. The default adapter (`db::JsonPlaybackStateStore`) writes a
 * versioned JSON sidecar, so remembering volume and position does not require a
 * database; `db::makeStorage` seeds it once from the legacy `engine_state` row
 * of an existing database.
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
     * @details The snapshot is self-contained: adapters replace the stored
     * record, so cursor, shuffle and repeat changes always go through this one
     * call rather than through field-specific setters.
     */
    virtual std::expected<void, kanariv::utils::Error>
    save(const kanariv::types::EngineState& state) = 0;
};

} // namespace kanariv::contracts
