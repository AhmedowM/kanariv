#pragma once

/**
 * @file config_detail.hpp
 * @brief Internal path helpers for config (not public API).
 * @ingroup kanariv_config
 */

#include <kanariv/utils.hpp>
#include <filesystem>
#include <string>

namespace kanariv::config::detail {

/**
 * @brief Get default database path.
 * Windows: %LOCALAPPDATA%\kanariv\library.db
 * POSIX: $XDG_DATA_HOME/kanariv/library.db or ~/.local/share/kanariv/library.db
 * @details Precedence: explicit `dbPath` in the config file, then this default.
 * No environment variable overrides `dbPath` directly.
 * @return Default database path.
 */
std::filesystem::path defaultDbPath();

/**
 * @brief Get default config file path.
 * Windows: %LOCALAPPDATA%\kanariv\config.json (same as db dir)
 * POSIX: $XDG_CONFIG_HOME/kanariv/config.json or ~/.config/kanariv/config.json
 * @return Default config file path.
 */
std::filesystem::path defaultConfigPath();

/**
 * @brief Get the legacy Windows config path, if any.
 * Windows: %USERPROFILE%\.config\kanariv\config.json
 * POSIX: empty (defaultConfigPath already used this location).
 * @details Read-only fallback for configs written before Windows defaulted to
 * %LOCALAPPDATA%. An explicit --config or configPath always wins.
 * @return Legacy path, or an empty path on POSIX.
 */
std::filesystem::path legacyConfigPath();

/**
 * @brief Read entire file into string.
 * @param p File path.
 * @return File contents on success, Error on failure.
 */
kanariv::utils::Expected<std::string> readFileString(const std::filesystem::path& p);

/**
 * @brief Stable endpoint identifier used in socket/pid/lock names.
 * @param dbPath Database path.
 * @return 16-character lowercase hex string: FNV-1a 64-bit over the canonical
 * UTF-8 form of @p dbPath (`generic_u8string()` with `/` separators, or the
 * raw path spelling if that conversion fails).
 * @details Self-contained and toolchain independent; `std::hash` is never used.
 * The same digest names the socket/pipe, PID file and lock file.
 * @note KANARIV_API: service.dll derives socket/pipe names through config.dll.
 */
KANARIV_API std::string endpointIdForDb(const std::filesystem::path& dbPath);

/**
 * @brief Get base directory for socket/pid/lock files.
 * Windows: %LOCALAPPDATA%\kanariv
 * POSIX: $XDG_RUNTIME_DIR/kanariv > $XDG_DATA_HOME/kanariv > ~/.local/share/kanariv > temp/kanariv
 * @return Base directory path, or Error when the directory cannot be created
 * or fails the ownership/mode check.
 * @details POSIX: the directory is created owner-only when absent and accepted
 * only when the current user owns it with no group/other write bit; anything
 * else is refused rather than chmod'ed (endpoint-squatting guard for the
 * shared temp fallback).
 */
kanariv::utils::Expected<std::filesystem::path> baseDirForSocket();

} // namespace kanariv::config::detail
