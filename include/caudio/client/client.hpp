#pragma once

/**
 * @file client.hpp
 * @brief Client for the caudio daemon (used by the CLI and third-party frontends).
 * @ingroup caudio_client
 */

#include <caudio/config.hpp>
#include <caudio/fwd.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/result.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace caudio::client {

/**
 * @brief Transport view of the daemon location.
 * @ingroup caudio_client
 * @details Deliberately narrower than `caudio::config::Config`: only the
 * database and socket paths are needed to connect, and the `library.db`
 * default is part of this type's contract. Persisted configuration remains
 * `caudio::config::Config`.
 */
struct CAUDIO_API Config final {
    std::filesystem::path dbPath{"library.db"};
    std::string socketPath{};

    Config() = default;
    explicit Config(std::filesystem::path db) : dbPath(std::move(db)) {}
    Config(std::filesystem::path db, std::string sock)
        : dbPath(std::move(db)), socketPath(std::move(sock)) {}
    explicit Config(const caudio::config::Config& cfg)
        : dbPath(cfg.dbPath), socketPath(cfg.socketPath) {}
};

/**
 * @brief Synchronous client for a running caudio daemon.
 * @ingroup caudio_client
 * @details Each `send()` call opens a transport connection, sends one
 * length-prefixed IPC request, waits for its reply, and closes the
 * connection. The client does not own or start the daemon.
 *
 * @par Thread safety
 * A `Client` instance is safe for concurrent `probe()` and `send()` calls
 * because configuration is immutable after construction. Each call uses its
 * own transport connection.
 */
class CAUDIO_API Client {
    Config config_;

  public:
    /** @brief Constructs from an explicit transport config. @ingroup caudio_client */
    explicit Client(Config cfg);
    /** @brief Constructs for a database path (socket path derived). @ingroup caudio_client */
    explicit Client(std::filesystem::path dbPath);
    /** @brief Constructs for a database path with explicit socket path. @ingroup caudio_client */
    explicit Client(std::filesystem::path dbPath, std::string_view socketPath);
    /** @brief Constructs from a loaded app config. @ingroup caudio_client */
    explicit Client(const caudio::config::Config& cfg);

    /** @brief Returns the transport configuration used by this client. @ingroup caudio_client */
    const Config& config() const noexcept;
    /** @brief Returns the configured database path. @ingroup caudio_client */
    std::filesystem::path dbPath() const noexcept;

    /**
     * @brief One-shot liveness check for the daemon at the configured paths.
     * @ingroup caudio_client
     * @return True when a connection can be established; no command is sent.
     */
    bool probe() const;

    /**
     * @brief Sends one command to the daemon and waits for its result.
     * @ingroup caudio_client
     * @param cmd Command to serialize and transmit synchronously.
     * @param timeout Maximum time to wait for connection and response.
     * @return Command result, or a transport/timeout error.
     * @par Thread safety
     * The call blocks until the reply arrives or `timeout` expires.
     */
    caudio::utils::Expected<caudio::ipc::Result>
    send(const caudio::ipc::Command& cmd,
         std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    /**
     * @brief Wait for playback status to change, then return current status.
     * @ingroup caudio_client
     * @param timeout Maximum time to wait for a status change.
     * @return Current Status result, or timeout/error.
     * @details Sends a `StatusWait` command to the daemon. The daemon blocks
     * until the status changes or `timeout` elapses, then returns the current
     * status. Use this for efficient status watching instead of polling.
     */
    caudio::utils::Expected<caudio::ipc::Result>
    waitForStatus(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
};

} // namespace caudio::client
