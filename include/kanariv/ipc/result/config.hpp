#pragma once

/**
 * @file config.hpp
 * @brief Configuration results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Config Results
// ---------------------------------------------------------------------------

/**
 * @struct ConfigValue
 * @brief Single configuration key-value pair (returned by ConfigGet).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigValue", "key": "volume", "value": "0.8"}
 *
 * @param key Configuration key.
 * @param value Configuration value.
 */
struct ConfigValue final {
    /**
     * @brief Configuration key.
     */
    std::string key{};
    /**
     * @brief Configuration value.
     */
    std::string value{};
};

/**
 * @struct ConfigValues
 * @brief Multiple configuration key-value pairs (returned by ConfigList).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "ConfigValues", "values": [{"key": "volume", "value": "0.8"}, ...]}
 *
 * @param values Vector of configuration values.
 */
struct ConfigValues final {
    /**
     * @brief Vector of configuration values.
     */
    std::vector<ConfigValue> values{};
};

} // namespace kanariv::ipc
