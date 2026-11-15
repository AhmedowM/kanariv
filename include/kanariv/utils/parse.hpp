#pragma once

/**
 * @file parse.hpp
 * @brief Strict decimal parsing shared by every CLI, app and service boundary.
 * @ingroup kanariv_utils
 * @note This header is fully inline; no .cpp is required.
 */

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kanariv::utils {

/**
 * @brief Parses a non-negative decimal id or position.
 * @ingroup kanariv_utils
 * @param text Candidate value, ASCII digits only.
 * @return The value, or `std::nullopt` for empty input, a sign, whitespace,
 * trailing garbage or overflow.
 * @details One rule for every numeric boundary in the program: library ids,
 * queue positions and playlist ids are all non-negative decimal integers. A
 * value that does not fit `std::int64_t` is rejected rather than truncated, so
 * callers can use the result as an id without a second range check.
 */
[[nodiscard]] inline std::optional<std::int64_t> parseId(std::string_view text) noexcept {
    if (text.empty())
        return std::nullopt;
    // from_chars accepts a leading '-', but ids and positions are non-negative,
    // so the character check is what pins the accepted grammar.
    for (char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
    }
    std::int64_t value = 0;
    const auto* last = text.data() + text.size();
    const auto [end, ec] = std::from_chars(text.data(), last, value);
    if (ec != std::errc{} || end != last)
        return std::nullopt;
    return value;
}

} // namespace kanariv::utils
