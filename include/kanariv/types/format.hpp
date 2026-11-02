#pragma once

/**
 * @file format.hpp
 * @brief Wire text and `std::format` support for the `kanariv::types` enums.
 * @ingroup kanariv_types
 * @details `PlaybackState` and `RepeatMode` cross the daemon/client
 * boundary, so they need a stable textual form. `toString()` returns that
 * form -- the exact bytes the IPC wire format uses -- and the
 * `std::formatter` specializations render it, so
 * `std::format("{}", state)` produces the wire text directly. Values
 * outside the enum range render as `"Unknown"`.
 *
 * Wire spellings (byte-identical to the IPC JSON payload):
 * - `PlaybackState`: "Stopped", "Ready", "Playing", "Paused", "Unknown"
 * - `RepeatMode`: "Off", "All", "One", "Unknown"
 *
 * The inverse mapping lives in `kanariv/types/parse.hpp`.
 */

#include <kanariv/types/wire.hpp>
#include <format>
#include <string_view>

namespace kanariv::types {

/**
 * @brief Converts a PlaybackState to its wire text.
 * @ingroup kanariv_types
 * @param s State to stringify.
 * @return Non-owning string view; `"Unknown"` for out-of-range values.
 */
constexpr std::string_view toString(PlaybackState s) noexcept {
    using enum PlaybackState;
    switch (s) {
    case Stopped:
        return "Stopped";
    case Ready:
        return "Ready";
    case Playing:
        return "Playing";
    case Paused:
        return "Paused";
    default:
        return "Unknown";
    }
}

/**
 * @brief Converts a RepeatMode to its wire text.
 * @ingroup kanariv_types
 * @param m Repeat mode to stringify.
 * @return Non-owning string view; `"Unknown"` for out-of-range values.
 * @details "All" is the wire spelling of `RepeatMode::All` (the enum member
 * was renamed from `Queue` in v0.33.0; the wire text moved with it).
 */
constexpr std::string_view toString(RepeatMode m) noexcept {
    using enum RepeatMode;
    switch (m) {
    case Off:
        return "Off";
    case All:
        return "All";
    case One:
        return "One";
    default:
        return "Unknown";
    }
}

} // namespace kanariv::types

/**
 * @brief std::formatter specialization for PlaybackState.
 * @details Enables `std::format("{}", PlaybackState::Playing)` via toString().
 */
template <>
struct std::formatter<kanariv::types::PlaybackState> : std::formatter<std::string_view> {
    /** @brief Formats the state using its stable IPC spelling. */
    auto format(kanariv::types::PlaybackState s, auto& ctx) const {
        return std::formatter<std::string_view>::format(kanariv::types::toString(s), ctx);
    }
};

/**
 * @brief std::formatter specialization for RepeatMode.
 * @details Enables `std::format("{}", RepeatMode::All)` via toString().
 */
template <>
struct std::formatter<kanariv::types::RepeatMode> : std::formatter<std::string_view> {
    /** @brief Formats the repeat mode using its stable IPC spelling. */
    auto format(kanariv::types::RepeatMode m, auto& ctx) const {
        return std::formatter<std::string_view>::format(kanariv::types::toString(m), ctx);
    }
};
