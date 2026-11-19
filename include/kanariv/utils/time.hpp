#pragma once

/**
 * @file time.hpp
 * @brief Wall-clock helper for persisted and wire-carried timestamps.
 * @ingroup kanariv_utils
 * @details `types::UnixMillis` values are for storage and display, never for
 * measuring elapsed time. Use a steady clock for durations.
 */

#include <chrono>
#include <kanariv/types/wire.hpp>

namespace kanariv::utils {

/**
 * @brief Current wall-clock time as epoch milliseconds.
 * @ingroup kanariv_utils
 * @return `types::UnixMillis`, the unit of every persisted timestamp.
 */
[[nodiscard]] inline kanariv::types::UnixMillis unixMillis() noexcept {
    using namespace std::chrono;
    return static_cast<kanariv::types::UnixMillis>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace kanariv::utils
