module;
#include <kanariv/ipc/protocol.hpp>

export module kanariv.ipc:protocol;

export namespace kanariv::ipc {
using ::kanariv::ipc::commandFromJson;
using ::kanariv::ipc::deframe;
using ::kanariv::ipc::deserializeReply;
using ::kanariv::ipc::deserializeRequest;
using ::kanariv::ipc::frame;
using ::kanariv::ipc::fromJson;
using ::kanariv::ipc::IpcReply;
using ::kanariv::ipc::IpcRequest;
using ::kanariv::ipc::Json;
using ::kanariv::ipc::resultFromJson;
using ::kanariv::ipc::serializeReply;
using ::kanariv::ipc::serializeRequest;
using ::kanariv::ipc::toJson;
using ::kanariv::ipc::toJsonString;
} // namespace kanariv::ipc
