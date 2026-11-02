#pragma once

/**
 * @file device.hpp
 * @brief Audio output device results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <kanariv/types/record.hpp>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Device Results
// ---------------------------------------------------------------------------

/**
 * @struct Devices
 * @brief List of audio output devices (returned by DeviceList).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Devices", "devices": [{"id": "hw:0,0", "name": "Built-in Audio",
 * "isDefault": true}, ...]}
 *
 * @param devices Vector of device information.
 */
struct Devices final {
    /**
     * @brief Vector of device information.
     */
    std::vector<types::DeviceInfo> devices{};
};

} // namespace kanariv::ipc
