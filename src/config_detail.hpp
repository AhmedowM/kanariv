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

} // namespace kanariv::config::detail

namespace kanariv::config::detail_paths {

/**
 * @brief Compute 8-char hex hash from dbPath for socket/pid/lock naming.
 * Uses std::hash on dbPath.generic_string(), folded to 32 bits, formatted as 8-char hex.
 * @param dbPath Database path.
 * @return 8-character lowercase hex string.
 * @note KANARIV_API: service.dll derives socket/SHM names through config.dll.
 */
KANARIV_API std::string hex8ForDb(const std::filesystem::path& dbPath);

/**
 * @brief Get base directory for socket/pid/lock files.
 * Windows: %LOCALAPPDATA%\kanariv
 * POSIX: $XDG_RUNTIME_DIR/kanariv > $XDG_DATA_HOME/kanariv > ~/.local/share/kanariv > temp/kanariv
 * @return Base directory path.
 */
std::filesystem::path baseDirForSocket();
} // namespace kanariv::config::detail_paths
