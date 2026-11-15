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
// Track conversion lives in kanariv::utils (utils/record_json.hpp) -- single
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
 */
struct IpcReply final {
    uint32_t id{0}; ///< Request identifier copied from the originating request.
    std::expected<Result, kanariv::utils::Error>
        result{}; ///< Result on success or transport/dispatch error.
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
KANARIV_API std::expected<Command, kanariv::utils::Error> commandFromJson(const Json& j);

/**
 * @brief Deserialize a Result from JSON (forward-declared so
 * fromJson<Result> dispatches instead of erroring).
 * @ingroup kanariv_ipc
 * @param j JSON object with "type" field and result-specific fields.
 * @return Result variant, or error if type is unknown or parsing fails.
 */
KANARIV_API std::expected<Result, kanariv::utils::Error> resultFromJson(const Json& j);

// Generic fromJson template wrapper: fromJson<Command>(json)
/**
 * @brief Generic template for deserializing Command or Result from JSON.
 * @ingroup kanariv_ipc
 * @tparam T Type to deserialize (Command or Result).
 * @param j JSON object to parse.
 * @return Deserialized value, or error if type is unsupported or parsing fails.
 */
template <typename T>
std::expected<T, kanariv::utils::Error> fromJson(const Json& j) {
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
KANARIV_API std::expected<IpcRequest, kanariv::utils::Error> deserializeRequest(std::string_view sv);

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
KANARIV_API std::expected<IpcReply, kanariv::utils::Error> deserializeReply(std::string_view sv);

// ---------------------------------------------------------------------------
// Framing: [4-byte BE len][json]
// ---------------------------------------------------------------------------
/**
 * @brief Maximum accepted JSON payload size for one frame (16 MiB).
 * @ingroup kanariv_ipc
 * @details Enforced in both directions: `frame` refuses to emit an oversized
 * payload and `deframe` refuses to allocate one claimed by a peer.
 */
inline constexpr std::size_t kMaxFrameBytes = 16 * 1024 * 1024;

/**
 * @brief Size of the framing header: one 4-byte big-endian payload length.
 * @ingroup kanariv_ipc
 * @details This constant, `encodeFrameLength` and `decodeFrameLength` are the
 * single owner of the header layout and of the `kMaxFrameBytes` cap. Transports
 * read/write the payload themselves but must not re-implement the header.
 */
inline constexpr std::size_t kFrameHeaderBytes = 4;

/**
 * @brief Encode a payload length as framing header bytes.
 * @ingroup kanariv_ipc
 * @param length Payload length in bytes; the caller guarantees it is within
 * `kMaxFrameBytes` (`frame` checks before narrowing).
 * @return The 4-byte big-endian header.
 */
KANARIV_API std::array<std::byte, kFrameHeaderBytes>
encodeFrameLength(std::uint32_t length) noexcept;

/**
 * @brief Decode and validate a framing header.
 * @ingroup kanariv_ipc
 * @param header The first `kFrameHeaderBytes` bytes of a frame.
 * @return The declared payload length, or `InvalidArg` when it exceeds
 * `kMaxFrameBytes` (rejected before the caller allocates).
 */
KANARIV_API std::expected<std::uint32_t, kanariv::utils::Error>
decodeFrameLength(std::span<const std::byte, kFrameHeaderBytes> header);

/**
 * @brief Frame a JSON string with 4-byte big-endian length prefix.
 * @ingroup kanariv_ipc
 * @param json JSON payload to frame.
 * @return Vector of bytes `[len_be][json...]`, or `InvalidArg` when the
 * payload exceeds `kMaxFrameBytes` (no silent `uint32_t` truncation).
 */
KANARIV_API std::expected<std::vector<std::byte>, kanariv::utils::Error>
frame(std::string_view json);

/**
 * @brief Extract JSON payload from framed buffer.
 * @ingroup kanariv_ipc
 * @param buf Buffer containing exactly one `[4-byte BE len][json...]` frame.
 * @return JSON string payload, or an error when the buffer is short, the
 * declared length exceeds `kMaxFrameBytes`, the frame is incomplete, or bytes
 * trail the declared frame (protocol desync).
 */
KANARIV_API std::expected<std::string, kanariv::utils::Error>
deframe(std::span<const std::byte> buf);

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
