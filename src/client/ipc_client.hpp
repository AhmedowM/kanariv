#pragma once

/**
 * @file ipc_client.hpp
 * @brief IPC client for connecting to kanariv service via Unix socket or Windows named pipe.
 * @ingroup kanariv_client
 */

#include <chrono>
#include <filesystem>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <memory>
#include <stop_token>
#include <string_view>

namespace kanariv::client {

/**
 * @brief IPC client for communicating with the kanariv service.
 *
 * Provides synchronous request/response over Unix domain sockets (POSIX) or
 * named pipes (Windows). Handles framing (4-byte big-endian length prefix,
 * owned by `kanariv::ipc`), JSON serialization, and connection lifecycle.
 *
 * Every wait is bounded: the transport polls, so a connect/send/receive ends
 * at the caller's deadline or when the caller's stop token fires instead of
 * parking the thread on a wedged daemon. Cancelling a call that is already
 * in flight on another thread is done with `abort()` (or by letting the
 * deadline elapse).
 *
 * Thread safety: Not thread-safe, except that `abort()` and `close()` may be
 * called from another thread to cancel a blocked transfer. Use one IpcClient
 * per thread or synchronize externally.
 */
class IpcClient {
  public:
    /**
     * @brief Absolute time at which a bounded operation gives up.
     * @details `Deadline::max()` means "no deadline".
     */
    using Deadline = std::chrono::steady_clock::time_point;

    /**
     * @brief Connect to the service using canonical socket path derived from dbPath.
     * @param dbPath Database path used to derive default socket/pid/lock paths.
     * @param socketPathOverride Optional explicit socket path (e.g., from --socket or config).
     * @param deadline Absolute time after which the attempt fails instead of waiting.
     * @param st Stop token polled while waiting for the endpoint.
     * @return IpcClient on success, Error on connection failure, deadline
     * expiry (`Io`) or cancellation (`Busy`).
     *
     * If socketPathOverride is empty, uses config::socketPathFor(dbPath) to derive
     * the canonical socket path (honors XDG/LOCALAPPDATA + hash of dbPath).
     * POSIX connects non-blocking and polls; Windows waits for a free pipe
     * instance in short slices, so neither platform can block past `deadline`.
     */
    static kanariv::utils::Expected<IpcClient> connect(const std::filesystem::path& dbPath,
                                                       std::string_view socketPathOverride = {},
                                                       Deadline deadline = Deadline::max(),
                                                       std::stop_token st = {});

    /**
     * @brief Destructor: closes the connection.
     */
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    /**
     * @brief Move constructor.
     * Transfers ownership of the socket/pipe handle.
     */
    IpcClient(IpcClient&& other) noexcept;

    /**
     * @brief Move assignment operator.
     * Closes current connection, takes ownership of other's handle.
     */
    IpcClient& operator=(IpcClient&& other) noexcept;

    /**
     * @brief Send a command and receive the response (synchronous RPC).
     * @param cmd Command to send.
     * @param deadline Absolute time after which a blocked transfer gives up.
     * @param st Stop token polled inside the transfer loops.
     * @return Result variant on success, Error on failure (connection,
     * timeout, cancellation, serialization, protocol mismatch, or service
     * error).
     *
     * Serializes command to JSON, frames with 4-byte BE length prefix, sends,
     * receives one frame (header decoded by `kanariv::ipc`), deserializes,
     * verifies the reply id, and returns Result.
     * Not thread-safe; serialize calls externally if needed.
     */
    kanariv::utils::Expected<kanariv::ipc::Result> send(const kanariv::ipc::Command& cmd,
                                                        Deadline deadline = Deadline::max(),
                                                        std::stop_token st = {});

    /**
     * @brief Close the connection (idempotent).
     * Closes socket or pipe handle, resets internal state.
     */
    void close() noexcept;

    /**
     * @brief Cancels a transfer blocked in another thread (idempotent).
     * @details Shuts the socket down (POSIX) or cancels pending I/O (Windows)
     * so a blocked `send()` returns promptly; the handle stays owned by this
     * object and is still closed by `close()` or the destructor.
     */
    void abort() noexcept;

  private:
    IpcClient();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::client
