module;
#include <kanariv/ipc/shm_status.hpp>

export module kanariv.ipc:shm_status;

export namespace kanariv::ipc {
using ::kanariv::ipc::atomicLoadDouble;
using ::kanariv::ipc::atomicLoadFloat;
using ::kanariv::ipc::AtomicShmStatus;
using ::kanariv::ipc::atomicStoreDouble;
using ::kanariv::ipc::atomicStoreFloat;
using ::kanariv::ipc::ShmStatus;
using ::kanariv::ipc::ShmStatusReader;
using ::kanariv::ipc::ShmStatusUpdate;
using ::kanariv::ipc::ShmStatusWriter;
} // namespace kanariv::ipc
