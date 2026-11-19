#pragma once

#include <cstdint>

/**
 * @file wire.hpp
 * @brief Dep-free wire vocabulary shared by the engine and the IPC layer.
 * @defgroup kanariv_types kanariv wire value types
 * @details First inhabitant of the `kanariv::types` layer: plain scalar types
 * that both `kanariv::engine` and `kanariv::ipc` depend on, so neither component
 * has to include the other. `RepeatMode` and `PlaybackState` cross the
 * daemon/client boundary (fields of `kanariv::ipc::Status` and of several
 * commands) and have a fixed `std::uint8_t` underlying type. Their JSON wire
 * text lives in `types/format.hpp`, together with its inverse.
 *
 * These values are owned directly by `kanariv::types`. This
 * layer is deliberately dependency-free (no `fwd.hpp`, no std headers) so it
 * can sit at the bottom of the component graph.
 */

namespace kanariv::types {

/**
 * @brief Timestamp unit for every persisted and wire-carried time value.
 * @ingroup kanariv_types
 * @details Epoch milliseconds (UTC). Every `*_at`, `*_added`, `*_scanned` and
 * `*_played` record field uses this unit, the database stores it as an INTEGER,
 * and the IPC codecs pass it through unchanged.
 */
using UnixMillis = std::int64_t;

/**
 * @brief Repeat mode for queue advancement.
 * @ingroup kanariv_types
 * @details Controls wrap behaviour in Engine::queueNextLocked and
 * Engine::queuePrevLocked when the cursor reaches the end or start.
 * - `Off`: linear; shuffle wrappers reshuffle and non-shuffle wraps to 0.
 * - `All`: same wrap as Off; single-track repeat is only via `One`.
 * - `One`: repeats the current track without advancing the cursor.
 * @see kanariv::ipc::Status
 */
enum class RepeatMode : std::uint8_t {
    Off = 0, ///< Advance normally and wrap according to the active queue mode.
    All = 1, ///< Repeat the queue after its last item.
    One = 2  ///< Repeat the current track without advancing.
};

// The wire order is authoritative: persisted columns and IPC payloads encode
// these values. Changing an enumerator value is a storage/wire break.
static_assert(static_cast<int>(RepeatMode::Off) == 0, "RepeatMode::Off wire value changed");
static_assert(static_cast<int>(RepeatMode::All) == 1, "RepeatMode::All wire value changed");
static_assert(static_cast<int>(RepeatMode::One) == 2, "RepeatMode::One wire value changed");

/**
 * @brief Playback state machine (wire value).
 * @ingroup kanariv_types
 * @details The process-wide playback vocabulary: the engine's transport state
 * and the player's fault state share this enum.
 * State transitions (Engine):
 * - `Stopped -> Playing` via Engine::play().
 * - `Playing -> Paused` via Engine::pause().
 * - `Paused -> Playing` via Engine::resume() / play() resume path.
 * - `Playing -> Playing` (restart) via play() when already playing (seek 0).
 * - Any -> `Stopped` via Engine::stop().
 * `Error` is the player's arm: a decoder or audio-device failure ends
 * playback and only a reopen clears it. The engine never emits it, so it does
 * not appear in `ipc::Status` payloads.
 * @see kanariv::ipc::Status
 */
enum class PlaybackState : std::uint8_t {
    Stopped = 0, ///< No track is currently playing.
    Ready = 1,   ///< The engine is initialized and ready to play.
    Playing = 2, ///< Audio playback is active.
    Paused = 3,  ///< Playback is paused with the current position retained.
    Error = 4    ///< Playback stopped on a decoder/device failure; reopen to play.
};

} // namespace kanariv::types
