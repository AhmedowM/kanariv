#pragma once

/**
 * @file raw.hpp
 * @brief Raw key/value config access (internal to kanariv::config::detail).
 * @ingroup kanariv_config
 * @details Generic JSON-backed key/value helpers used by the service for
 * arbitrary config pairs. Not part of the stable public API: callers are
 * `src/app/config.cpp` and `src/service/dispatch_config.cpp` only.
 */

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/result.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::config::detail {

/**
 * @brief One raw key/value pair from the config file.
 */
struct RawConfigValue {
    std::string key{};
    std::string value{};
};

// KANARIV_API below is the one deliberate exception to "only public headers are
// exported": app.dll and service.dll call these through config.dll. Keep the
// annotated set minimal -- everything else in src/ stays DLL-internal.
/**
 * @brief Get a raw config value by key from JSON file.
 * @param p Config file path.
 * @param key Key to read.
 * @return Value string (JSON string, "null", or JSON dump) on success, Error on failure.
 */
KANARIV_API kanariv::utils::Expected<std::string> configGetRaw(const std::filesystem::path& p,
                                                             std::string_view key);

/**
 * @brief Set a raw config value in JSON file.
 * Parses value as JSON if valid; otherwise stores as string.
 * Creates file and parent directories if needed.
 * @param p Config file path.
 * @param key Key to write.
 * @param value Value to write (JSON-parsed if valid JSON).
 * @return void on success, Error on failure.
 */
KANARIV_API kanariv::utils::Expected<void> configSetRaw(const std::filesystem::path& p,
                                                      std::string_view key, std::string_view value);

/**
 * @brief List all config key-value pairs from JSON file.
 * Skips empty keys and "type" key.
 * @param p Config file path.
 * @return Vector of RawConfigValue on success, Error on failure.
 */
KANARIV_API kanariv::utils::Expected<std::vector<RawConfigValue>>
configListRaw(const std::filesystem::path& p);

/**
 * @brief Delete a config key from JSON file.
 * @param p Config file path.
 * @param key Key to delete.
 * @return void on success, Error on failure (not found, corrupt, IO).
 */
KANARIV_API kanariv::utils::Expected<void> configDeleteRaw(const std::filesystem::path& p,
                                                         std::string_view key);

/**
 * @brief Reset entire config (delete config file).
 * @param p Config file path.
 * @return void on success, Error on failure.
 */
KANARIV_API kanariv::utils::Expected<void> configResetAllRaw(const std::filesystem::path& p);

} // namespace kanariv::config::detail
