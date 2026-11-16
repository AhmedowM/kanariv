#pragma once

/**
 * @file result.hpp
 * @brief Status codes and string conversion for kanariv utilities.
 * @ingroup kanariv_utils
 */

#include <cstdint>
#include <format>
#include <kanariv/types/status.hpp>
#include <string_view>

namespace kanariv::utils {

/**
 * @brief Converts a StatusCode to a human-readable string view.
 * @ingroup kanariv_utils
 * @param r Status code to stringify.
 * @return Non-owning string view; `"Unknown"` for out-of-range values.
 * @see StatusCode
 */
constexpr std::string_view toString(StatusCode r) noexcept {
    using enum StatusCode;
    switch (r) {
    case Ok:
        return "Ok";
    case InvalidArg:
        return "InvalidArg";
    case NotFound:
        return "NotFound";
    case Unsupported:
        return "Unsupported";
    case Io:
        return "Io";
    case Device:
        return "Device";
    case State:
        return "State";
    case NoMem:
        return "NoMem";
    case Internal:
        return "Internal";
    case AlreadyExists:
        return "AlreadyExists";
    case Busy:
        return "Busy";
    case Corrupt:
        return "Corrupt";
    case NoSpace:
        return "NoSpace";
    case Timeout:
        return "Timeout";
    default:
        return "Unknown";
    }
}

} // namespace kanariv::utils

/**
 * @brief std::formatter specialization for StatusCode.
 * @ingroup kanariv_utils
 * @details Enables `std::format("{}", StatusCode::Busy)` via toString().
 */
template <>
struct std::formatter<kanariv::utils::StatusCode> : std::formatter<std::string_view> {
    /** @brief Formats a status code using its diagnostic spelling. */
    auto format(kanariv::utils::StatusCode r, auto& ctx) const {
        return std::formatter<std::string_view>::format(kanariv::utils::toString(r), ctx);
    }
};
