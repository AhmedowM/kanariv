#pragma once

/**
 * @file client.hpp
 * @brief Client for the kanariv daemon (used by the CLI and third-party frontends).
 * @ingroup kanariv_client
 */

#include <chrono>
#include <filesystem>
#include <kanariv/config.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <string>
#include <string_view>

namespace kanariv::client {

/**
 * @brief Transport view of the daemon location.
 * @ingroup kanariv_client
 * @details Deliberately narrower than `kanariv::config::Config`: only the
 * database and socket paths are needed to connect, and the `library.db`
 * default is part of this type's contract. Persisted configuration remains
 * `kanariv::config::Config`.
 */
struct KANARIV_API Config final {
    std::filesystem::path dbPath{"library.db"};
    std::string socketPath{};

    /** @brief Uses the default `library.db` database and derived socket. */
    Config() = default;
    /**
     * @brief Constructs a transport configuration for a database.
     * @param db Database path; the socket is derived from it.
     */
    explicit Config(std::filesystem::path db) : dbPath(std::move(db)) {}
    /**
     * @brief Constructs a transport configuration with an explicit endpoint.
     * @param db Database path.
     * @param sock Socket or named-pipe endpoint; empty derives it from `db`.
     */
    Config(std::filesystem::path db, std::string sock)
        : dbPath(std::move(db)), socketPath(std::move(sock)) {}
    /**
     * @brief Copies the database and socket settings from application config.
     * @param cfg Loaded application configuration.
     */
    explicit Config(const kanariv::config::Config& cfg)
        : dbPath(cfg.dbPath), socketPath(cfg.socketPath) {}
};

/**
 * @brief Synchronous client for a running kanariv daemon.
 * @ingroup kanariv_client
 * @details Each `send()` call opens a transport connection, sends one
 * length-prefixed IPC request, waits for its reply, and closes the
 * connection. The client does not own or start the daemon.
 *
 * @par Thread safety
 * A `Client` instance is safe for concurrent `probe()` and `send()` calls
 * because configuration is immutable after construction. Each call uses its
 * own transport connection.
 */
class KANARIV_API Client {
    Config config_;

  public:
    /** @brief Constructs from an explicit transport config. @ingroup kanariv_client */
    explicit Client(Config cfg);
    /** @brief Constructs for a database path (socket path derived). @ingroup kanariv_client */
    explicit Client(std::filesystem::path dbPath);
    /** @brief Constructs for a database path with explicit socket path. @ingroup kanariv_client */
    explicit Client(std::filesystem::path dbPath, std::string_view socketPath);
    /** @brief Constructs from a loaded app config. @ingroup kanariv_client */
    explicit Client(const kanariv::config::Config& cfg);

    /** @brief Returns the transport configuration used by this client. @ingroup kanariv_client */
    const Config& config() const noexcept;
    /** @brief Returns the configured database path. @ingroup kanariv_client */
    std::filesystem::path dbPath() const noexcept;

    /**
     * @brief One-shot liveness check for the daemon at the configured paths.
     * @ingroup kanariv_client
     * @return True when a connection can be established; no command is sent.
     */
    bool probe() const;

    /**
     * @brief Sends one command to the daemon and waits for its result.
     * @ingroup kanariv_client
     * @param cmd Command to serialize and transmit synchronously.
     * @param timeout Maximum time to wait for connection and response.
     * @return Command result, or a transport/timeout error.
     * @par Thread safety
     * The call blocks until the reply arrives or `timeout` expires.
     */
    kanariv::utils::Expected<kanariv::ipc::Result>
    send(const kanariv::ipc::Command& cmd,
         std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    /**
     * @brief Wait for playback status to change, then return current status.
     * @ingroup kanariv_client
     * @param timeout Maximum time to wait for a status change.
     * @return Current Status result, or timeout/error.
     * @details Sends a `StatusWait` command to the daemon. The daemon blocks
     * until the status changes or `timeout` elapses, then returns the current
     * status. Use this for efficient status watching instead of polling.
     */
    kanariv::utils::Expected<kanariv::ipc::Result>
    waitForStatus(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
};

} // namespace kanariv::client
