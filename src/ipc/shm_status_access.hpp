#pragma once

/**
 * @file shm_status_access.hpp
 * @brief White-box access to `ShmStatusReader`/`ShmStatusWriter` internals.
 * @ingroup kanariv_ipc
 * @details Sole friend of both shared-memory classes. Exposes the mapped
 * `AtomicShmStatus` so tests can simulate a crashed writer (odd `seq`) and
 * inspect the reset header. Production code must keep using the public
 * `snapshot`/`publish` API.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <kanariv/ipc/shm_status.hpp>

namespace kanariv::ipc {

/**
 * @brief Static forwarders to the private mapping of the status classes.
 * @ingroup kanariv_ipc
 */
struct ShmStatusAccess {
    /**
     * @brief Returns the mapped segment of a reader (nullptr when not mapped).
     * @ingroup kanariv_ipc
     * @param reader Reader instance.
     */
    static AtomicShmStatus* map(ShmStatusReader& reader) noexcept {
        return reader.map_;
    }

    /**
     * @brief Returns the mapped segment of a writer (nullptr when not mapped).
     * @ingroup kanariv_ipc
     * @param writer Writer instance.
     */
    static AtomicShmStatus* map(ShmStatusWriter& writer) noexcept {
        return writer.map_;
    }
};

} // namespace kanariv::ipc
