#pragma once

/**
 * @file device.hpp
 * @brief Audio output device commands for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <optional>
#include <string>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Device Commands
// ---------------------------------------------------------------------------

/**
 * @struct DeviceList
 * @brief List available audio output devices.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "DeviceList"}
 *
 * @return Devices object with vector of device information.
 */
struct DeviceList final {};

/**
 * @struct DeviceSet
 * @brief Set the active audio output device.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "DeviceSet", "id": "alsa:68773a"}
 *
 * @param id Device reference: an opaque id from DeviceList, an exact device
 * name, or a legacy numeric enumeration index.
 * @return Empty (success with no data).
 * @details Persists the resolved stable id and its display name; loudspeaker
 * playback falls back to the system default when neither resolves.
 */
struct DeviceSet final {
    /**
     * @brief Device id (or name, or legacy index) to set as active.
     */
    std::string id{};
};

/**
 * @struct DeviceTest
 * @brief Check an audio output device is available.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "DeviceTest", "id": "alsa:68773a"}
 *
 * @param id Device reference to check. If omitted, checks the configured
 * device (or the default one when none is configured).
 * @return Empty (success with no data).
 * @details Enumeration-only: verifies the reference resolves against the
 * listed devices. Emits no sound.
 */
struct DeviceTest final {
    /**
     * @brief Device reference to check. If omitted, checks the configured device.
     */
    std::optional<std::string> id{};
    /**
     * @brief Play an audible test tone instead of enumeration-only check.
     */
    bool tone{false};
};

} // namespace kanariv::ipc
