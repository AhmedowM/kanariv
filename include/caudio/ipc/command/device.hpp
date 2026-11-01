#pragma once

/**
 * @file device.hpp
 * @brief Audio output device commands for the caudio IPC protocol.
 * @ingroup caudio_ipc
 */

#include <optional>
#include <string>

namespace caudio::ipc {

// ---------------------------------------------------------------------------
// Device Commands
// ---------------------------------------------------------------------------

/**
 * @struct DeviceList
 * @brief List available audio output devices.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "DeviceList"}
 *
 * @return Devices object with vector of device information.
 */
struct DeviceList final {};

/**
 * @struct DeviceSet
 * @brief Set the active audio output device.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "DeviceSet", "id": "hw:0,0"}
 *
 * @param id Device ID to set as active.
 * @return Empty (success with no data).
 */
struct DeviceSet final {
    /**
     * @brief Device ID to set as active.
     */
    std::string id{};
};

/**
 * @struct DeviceTest
 * @brief Check an audio output device is available.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "DeviceTest", "id": "hw:0,0"}
 *
 * @param id Device ID to check. If omitted, checks the default device.
 * @return Empty (success with no data).
 * @details Enumeration-only: verifies the id resolves against the listed
 * devices. Emits no sound.
 */
struct DeviceTest final {
    /**
     * @brief Device ID to check. If omitted, checks the default device.
     */
    std::optional<std::string> id{};
    /**
     * @brief Play an audible test tone instead of enumeration-only check.
     */
    bool tone{false};
};

} // namespace caudio::ipc
