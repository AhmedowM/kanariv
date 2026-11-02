#pragma once

/**
 * @file math.hpp
 * @brief Miscellaneous math utilities.
 * @ingroup kanariv_utils
 * @note This header is fully inline; no .cpp is required.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace kanariv::utils {

/**
 * @brief Clamps a volume value to the valid range [0.0, 1.0].
 * @ingroup kanariv_utils
 * @param v Volume value to clamp.
 * @return Clamped volume; 0.0 for NaN/infinity.
 */
[[nodiscard]] inline float clampVolume(float v) noexcept {
    if (!std::isfinite(v))
        return 0.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

/**
 * @brief Converts 32 bytes to a 64-char lowercase hex string.
 * @ingroup kanariv_utils
 * @param bytes Input bytes.
 * @return Lowercase hex string.
 */
[[nodiscard]] inline std::string toHex(const std::array<std::uint8_t, 32>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (std::uint8_t b : bytes) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 0xf]);
    }
    return s;
}

/**
 * @brief Parses a 64-char hex string into 32 bytes.
 * @ingroup kanariv_utils
 * @param hexStr Hex view (must be 64 chars, case-insensitive).
 * @param out Output bytes.
 * @return true on success, false on length/character error.
 */
inline bool fromHex(std::string_view hexStr, std::array<std::uint8_t, 32>& out) {
    if (hexStr.size() != 64)
        return false;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; i++) {
        int hi = value(hexStr[i * 2]);
        int lo = value(hexStr[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

/**
 * @brief Formats seconds as mm:ss or hh:mm:ss.
 * @ingroup kanariv_utils
 * @param secs Position or duration in seconds (negative clamps to 0).
 * @return Formatted time string.
 */
[[nodiscard]] inline std::string formatClock(double secs) {
    if (secs < 0)
        secs = 0;
    long total = static_cast<long>(secs);
    if (long h = total / 3600; h > 0)
        return std::format("{:02}:{:02}:{:02}", h, (total % 3600) / 60, total % 60);
    return std::format("{:02}:{:02}", (total % 3600) / 60, total % 60);
}

} // namespace kanariv::utils
