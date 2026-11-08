#pragma once

/**
 * @file path_utf8.hpp
 * @brief UTF-8 conversion helpers for `std::filesystem::path`.
 * @details Internal header: lives in `src/`, not installed.
 *
 * Windows narrow filesystem APIs (`path::string()`, `path::generic_string()`)
 * convert to the system ANSI code page and *throw* `std::system_error` for
 * characters it cannot represent (e.g. U+0130 "İ" on a CP1252 system). Stored
 * paths therefore have to be UTF-8:
 * - FFmpeg interprets narrow filenames as UTF-8 on Windows
 *   (`avpriv_open` uses `CP_UTF8`), so metadata probing and playback work.
 * - SQLite stores text as bytes, and JSON is UTF-8 by definition.
 *
 * Use `pathToUtf8()` whenever a path becomes a stored/transmitted string,
 * and `pathFromUtf8()` whenever such a string becomes a path again.
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace kanariv::utils::detail {

/**
 * @brief Converts a path to UTF-8 using the generic (forward-slash) form.
 * @param p Path to convert.
 * @return UTF-8 bytes; empty when `p` is empty. Never throws for valid paths.
 * @details Uses `generic_u8string()` (C++20), which is locale independent and
 * lossless for every Windows wide-character path.
 */
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

/**
 * @brief Builds a path from its UTF-8 representation.
 * @param s UTF-8 bytes previously produced by `pathToUtf8()`.
 * @return Path decoded as UTF-8 (not the ANSI code page) on Windows.
 */
inline std::filesystem::path pathFromUtf8(std::string_view s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

} // namespace kanariv::utils::detail
