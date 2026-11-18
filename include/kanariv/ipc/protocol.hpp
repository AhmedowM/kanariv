#pragma once

/**
 * @file protocol.hpp
 * @brief IPC protocol implementation for kanariv CLI communication.
 * @ingroup kanariv_ipc
 *
 * Provides JSON serialization/deserialization for commands and results,
 * IPC request/reply framing, and the wire protocol implementation.
 *
 * ## IPC Protocol Structure
 *
 * All communication between CLI clients and the kanariv daemon uses a
 * length-prefixed JSON format over a Unix domain socket (Linux/macOS) or
 * named pipe (Windows).
 *
 * ### Wire Format
 *
 * Each message consists of:
 * - 4-byte big-endian length prefix (uint32_t) - the size of the JSON payload in bytes
 * - JSON payload (UTF-8 encoded)
 *
 * ### Request Format (Client -> Daemon)
 *
 * ```json
 * {
 *   "id": 1,
 *   "cmd": {
 *     "type": "Play"
 *   }
 * }
 * ```
 *
 * ### Reply Format (Daemon -> Client)
 *
 * Success:
 * ```json
 * {
 *   "id": 1,
 *   "ok": true,
 *   "result": {
 *     "type": "Status",
 *     "state": "Playing",
 *     ...
 *   }
 * }
 * ```
 *
 * Error:
 * ```json
 * {
 *   "id": 1,
 *   "ok": false,
 *   "error": {
 *     "type": "Error",
 *     "code": "NotFound",
 *     "code_value": 2,
 *     "message": "Track not found"
 *   }
 * }
 * ```
 *
 * @see kanariv::ipc::Command for command types
 * @see kanariv::ipc::Result for result types
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::ipc {

// JSON vocabulary: the opaque Json (nlohmann stays in src/).
// TrackRow conversion lives in kanariv::utils (utils/record_json.hpp) -- single
// definition, no ipc::detail duplicate.
/** @brief Opaque JSON value used by the IPC serializers. */
using Json = kanariv::utils::Json;

/**
 * @struct IpcRequest
 * @brief IPC request message sent from client to daemon.
 * @ingroup kanariv_ipc
 *
 * @param id Request identifier for matching replies.
 * @param cmd Command to execute.
 */
struct IpcRequest final {
    uint32_t id{0}; ///< Request identifier echoed by the daemon in its reply.
    Command cmd{};  ///< Command variant to dispatch.
};

/**
 * @struct IpcReply
 * @brief IPC reply message sent from daemon to client.
 * @ingroup kanariv_ipc
 *
 * @param id Request identifier (matches the request).
 * @param result Result value on success, or error on failure.
 * @details A reply always carries exactly one of the two outcomes. A
 * default-constructed `IpcReply` is an *unset* reply and therefore fails
 * closed as `Internal`: it must never be mistaken for a successful `Empty`
 * result. Every producer assigns `result` on every path before serializing.
 */
struct IpcReply final {
    uint32_t id{0}; ///< Request identifier copied from the originating request.
    kanariv::utils::Expected<Result> result{std::unexpected{kanariv::utils::makeError(
        kanariv::utils::StatusCode::Internal, "reply result not populated")}};
};
// ---------------------------------------------------------------------------
// Command JSON
// ---------------------------------------------------------------------------
/**
 * @brief Serialize a Command variant to JSON.
 * @ingroup kanariv_ipc
 * @param cmd Command to serialize.
 * @return JSON object with "type" field and command-specific fields.
 */
KANARIV_API Json toJson(const Command& cmd);

/**
 * @brief Deserialize a Command from JSON.
 * @ingroup kanariv_ipc
 * @param j JSON object with "type" field and command-specific fields.
 * @return Command variant, or error if type is unknown or parsing fails.
 */
KANARIV_API kanariv::utils::Expected<Command> commandFromJson(const Json& j);

/**
 * @brief Deserialize a Result from JSON (forward-declared so
 * fromJson<Result> dispatches instead of erroring).
 * @ingroup kanariv_ipc
 * @param j JSON object with "type" field and result-specific fields.
 * @return Result variant, or error if type is unknown or parsing fails.
 */
KANARIV_API kanariv::utils::Expected<Result> resultFromJson(const Json& j);

// Generic fromJson template wrapper: fromJson<Command>(json)
/**
 * @brief Generic template for deserializing Command or Result from JSON.
 * @ingroup kanariv_ipc
 * @tparam T Type to deserialize (Command or Result).
 * @param j JSON object to parse.
 * @return Deserialized value, or error if type is unsupported or parsing fails.
 */
template <typename T>
kanariv::utils::Expected<T> fromJson(const Json& j) {
    if constexpr (std::is_same_v<T, Command>) {
        return commandFromJson(j);
    } else if constexpr (std::is_same_v<T, Result>) {
        return resultFromJson(j);
    } else {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Unsupported,
                                                         "unsupported fromJson type")};
    }
}

// ---------------------------------------------------------------------------
// Result JSON
// ---------------------------------------------------------------------------
/**
 * @brief Serialize a Result variant to JSON.
 * @ingroup kanariv_ipc
 * @param r Result to serialize.
 * @return JSON object with "type" field and result-specific fields.
 */
KANARIV_API Json toJson(const Result& r);

// ---------------------------------------------------------------------------
// IpcRequest / IpcReply serialization
// ---------------------------------------------------------------------------
/**
 * @brief Serialize an IpcRequest to JSON string.
 * @ingroup kanariv_ipc
 * @param req Request to serialize.
 * @return JSON string (compact format).
 */
KANARIV_API std::string serializeRequest(const IpcRequest& req);

/**
 * @brief Deserialize an IpcRequest from JSON string.
 * @ingroup kanariv_ipc
 * @param sv JSON string to parse.
 * @return IpcRequest, or error if parsing fails or required fields are missing.
 */
KANARIV_API kanariv::utils::Expected<IpcRequest> deserializeRequest(std::string_view sv);

/**
 * @brief Serialize an IpcReply to JSON string.
 * @ingroup kanariv_ipc
 * @param rep Reply to serialize.
 * @return JSON string (compact format).
 */
KANARIV_API std::string serializeReply(const IpcReply& rep);

/**
 * @brief Deserialize an IpcReply from JSON string.
 * @ingroup kanariv_ipc
 * @param sv JSON string to parse.
 * @return IpcReply, or error if parsing fails or required fields are missing.
 */
KANARIV_API kanariv::utils::Expected<IpcReply> deserializeReply(std::string_view sv);

// ---------------------------------------------------------------------------
// Framing: [4-byte BE len][json]
// ---------------------------------------------------------------------------
/**
 * @brief Maximum accepted JSON payload size for one frame (16 MiB).
 * @ingroup kanariv_ipc
 * @details Enforced in both directions: `frame` refuses to emit an oversized
 * payload and `deframe` refuses to allocate one claimed by a peer.
 */
inline constexpr std::size_t maxFrameBytes = 16 * 1024 * 1024;

/**
 * @brief Size of the framing header: one 4-byte big-endian payload length.
 * @ingroup kanariv_ipc
 * @details This constant, `encodeFrameLength` and `decodeFrameLength` are the
 * single owner of the header layout and of the `maxFrameBytes` cap. Transports
 * read/write the payload themselves but must not re-implement the header.
 */
inline constexpr std::size_t frameHeaderBytes = 4;

/**
 * @brief Encode a payload length as framing header bytes.
 * @ingroup kanariv_ipc
 * @param length Payload length in bytes; the caller guarantees it is within
 * `maxFrameBytes` (`frame` checks before narrowing).
 * @return The 4-byte big-endian header.
 */
KANARIV_API std::array<std::byte, frameHeaderBytes>
encodeFrameLength(std::uint32_t length) noexcept;

/**
 * @brief Decode and validate a framing header.
 * @ingroup kanariv_ipc
 * @param header The first `frameHeaderBytes` bytes of a frame.
 * @return The declared payload length, or `InvalidArg` when it exceeds
 * `maxFrameBytes` (rejected before the caller allocates).
 */
KANARIV_API kanariv::utils::Expected<std::uint32_t>
decodeFrameLength(std::span<const std::byte, frameHeaderBytes> header);

/**
 * @brief Frame a JSON string with 4-byte big-endian length prefix.
 * @ingroup kanariv_ipc
 * @param json JSON payload to frame.
 * @return Vector of bytes `[len_be][json...]`, or `InvalidArg` when the
 * payload exceeds `maxFrameBytes` (no silent `uint32_t` truncation).
 */
KANARIV_API kanariv::utils::Expected<std::vector<std::byte>> frame(std::string_view json);

/**
 * @brief Extract JSON payload from framed buffer.
 * @ingroup kanariv_ipc
 * @param buf Buffer containing exactly one `[4-byte BE len][json...]` frame.
 * @return JSON string payload, or an error when the buffer is short, the
 * declared length exceeds `maxFrameBytes`, the frame is incomplete, or bytes
 * trail the declared frame (protocol desync).
 */
KANARIV_API kanariv::utils::Expected<std::string> deframe(std::span<const std::byte> buf);

/**
 * @brief Serialize a Result to pretty-printed JSON string.
 * @ingroup kanariv_ipc
 * @param r Result to serialize.
 * @return Pretty-printed JSON string (2-space indentation).
 */
KANARIV_API std::string toJsonString(const Result& r);

/**
 * @brief Serialize a Command to pretty-printed JSON string.
 * @ingroup kanariv_ipc
 * @param c Command to serialize.
 * @return Pretty-printed JSON string (2-space indentation).
 */
KANARIV_API std::string toJsonString(const Command& c);

} // namespace kanariv::ipc
