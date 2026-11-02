#pragma once

/**
 * @file config.hpp
 * @brief Configuration commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <optional>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Config Commands
// ---------------------------------------------------------------------------

/**
 * @struct ConfigGet
 * @brief Get a configuration value by key.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigGet", "key": "volume"}
 *
 * @param key Configuration key.
 * @return ConfigValue object with key and value.
 */
struct ConfigGet final {
    /**
     * @brief Configuration key.
     */
    std::string key{};
};

/**
 * @struct ConfigSet
 * @brief Set a configuration value.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigSet", "key": "volume", "value": "0.8"}
 *
 * @param key Configuration key.
 * @param value Configuration value (JSON-encoded if complex type).
 * @return Empty (success with no data).
 */
struct ConfigSet final {
    /**
     * @brief Configuration key.
     */
    std::string key{};
    /**
     * @brief Configuration value (JSON-encoded if complex type).
     */
    std::string value{};
};

/**
 * @struct ConfigList
 * @brief List all configuration values.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigList"}
 *
 * @return ConfigValues object with vector of key-value pairs.
 */
struct ConfigList final {};

/**
 * @struct ConfigExport
 * @brief Export configuration to a file.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigExport", "path": "/tmp/config.json"}
 *
 * @param path Output file path.
 * @return Empty (success with no data).
 */
struct ConfigExport final {
    /**
     * @brief Output file path.
     */
    std::string path{};
};

/**
 * @struct ConfigImport
 * @brief Import configuration from a file.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigImport", "path": "/tmp/config.json"}
 *
 * @param path Input file path.
 * @return Empty (success with no data).
 */
struct ConfigImport final {
    /**
     * @brief Input file path.
     */
    std::string path{};
};

/**
 * @struct ConfigReset
 * @brief Reset a specific config key or all configuration to defaults.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigReset", "key": "volume"}
 *
 * @param key Key to reset. If omitted, resets all configuration.
 * @return Empty (success with no data).
 */
struct ConfigReset final {
    /**
     * @brief Key to reset. If omitted, resets all configuration.
     */
    std::optional<std::string> key{};
};

} // namespace kanariv::ipc
