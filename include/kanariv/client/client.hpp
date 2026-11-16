#pragma once

/**
 * @file client.hpp
 * @brief Client for the kanariv daemon (used by the CLI and third-party frontends).
 * @ingroup kanariv_client
 */

#include <kanariv/config.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
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
 * Every call is bounded: `probe()` gives up after a short connect deadline and
 * `send()` enforces its timeout on the transport (not only on the daemon's own
 * wait). A timeout cancels the in-flight command -- the connection is shut down
 * and the worker is joined -- so no thread outlives the call. Reply ids are
 * validated; a mismatched reply is reported as `Corrupt`.
 *
 * @par Thread safety
 * A `Client` instance is safe for concurrent `probe()` and `send()` calls
 * because configuration is immutable after construction. Each call uses its
 * own transport connection.
 */
class KANARIV_API Client {
    Config config_;

  public:
    /**
     * @brief Constructs from an explicit transport config.
     * @ingroup kanariv_client
     * @details A relative `dbPath` is resolved once here (see `dbPath()`).
     */
    explicit Client(Config cfg);
    /**
     * @brief Constructs for a database path (socket path derived).
     * @ingroup kanariv_client
     * @details A relative path is resolved once here, so it derives the same
     * socket/pid/lock names as the absolute spelling.
     */
    explicit Client(std::filesystem::path dbPath);
    /** @brief Constructs for a database path with explicit socket path. @ingroup kanariv_client */
    explicit Client(std::filesystem::path dbPath, std::string_view socketPath);
    /** @brief Constructs from a loaded app config. @ingroup kanariv_client */
    explicit Client(const kanariv::config::Config& cfg);

    /** @brief Returns the transport configuration used by this client. @ingroup kanariv_client */
    const Config& config() const noexcept;
    /**
     * @brief Returns the database path used for endpoint derivation.
     * @ingroup kanariv_client
     * @details Same as the constructor argument except that a relative path is
     * resolved against the current directory; a missing database is not an
     * error (`probe()` must work before the first start).
     */
    std::filesystem::path dbPath() const noexcept;

    /**
     * @brief One-shot liveness check for the daemon at the configured paths.
     * @ingroup kanariv_client
     * @return True when a connection can be established within the probe
     * deadline (currently ~400 ms); no command is sent.
     * @details Never blocks longer than the deadline, so a wedged daemon cannot
     * stall startup.
     */
    bool probe() const;

    /**
     * @brief Sends one command to the daemon and waits for its result.
     * @ingroup kanariv_client
     * @param cmd Command to serialize and transmit synchronously.
     * @param timeout Maximum time to wait for connection and response; `0`
     * selects the command's transport policy
     * (`kanariv::ipc::transportPolicyFor`): Interactive 2 s, Batch 5 min,
     * LongRunning 30 min.
     * @return Command result, or a transport/timeout error.
     * @par Thread safety
     * The call blocks until the reply arrives or `timeout` expires. On timeout
     * the command is cancelled: the connection is shut down and the worker is
     * joined before the error is returned.
     */
    kanariv::utils::Expected<kanariv::ipc::Result> send(const kanariv::ipc::Command& cmd,
                                                        std::chrono::milliseconds timeout = {});

    /**
     * @brief Wait for playback status to change, then return current status.
     * @ingroup kanariv_client
     * @param timeout Maximum time to wait for a status change.
     * @return Current Status result, or timeout/error.
     * @details Sends a `StatusWait` command to the daemon. The daemon blocks
     * until the status changes or `timeout` elapses, then returns the current
     * status. The transport wait allows `timeout` plus a small margin, so the
     * daemon's own deadline is always the one that fires. Use this for
     * efficient status watching instead of polling.
     */
    kanariv::utils::Expected<kanariv::ipc::Result>
    waitForStatus(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
};

/**
 * @brief Long-lived client connection for repeated commands (watch loops).
 * @ingroup kanariv_client
 * @details `Client` opens and closes a connection per call. A stream that sends
 * a command every interval -- `status --watch`, `lyrics get --live` -- would pay
 * a connect, accept and thread per poll; a `Session` keeps one connection and
 * reuses it for every request, which the daemon serves until the peer closes
 * or idles past its request timeout.
 *
 * A failed transfer means the stream is unusable (the daemon closed it while
 * idle, or the transport broke). The connection is dropped and the next call
 * reconnects. The failed command is **not** retried: the daemon may already
 * have executed it, so a retry could apply a state change twice. Callers that
 * want retry semantics keep their own backoff loop and call again.
 *
 * @par Thread safety
 * Calls are serialized by an internal mutex (the transport carries one request
 * at a time), so concurrent calls are safe but do not overlap. Destroying the
 * object while a call is in flight on another thread is not supported.
 */
class KANARIV_API Session {
  public:
    /**
     * @brief Constructs from an explicit transport config.
     * @ingroup kanariv_client
     * @details A relative `dbPath` is resolved once here, as with `Client`.
     */
    explicit Session(Config cfg);
    /** @brief Constructs for a database path (socket path derived). @ingroup kanariv_client */
    explicit Session(std::filesystem::path dbPath);
    /** @brief Constructs for a database path with explicit socket path. @ingroup kanariv_client */
    explicit Session(std::filesystem::path dbPath, std::string_view socketPath);
    /** @brief Constructs from a loaded app config. @ingroup kanariv_client */
    explicit Session(const kanariv::config::Config& cfg);
    /** @brief Closes the connection if one is open. */
    ~Session();

    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /** @brief Returns the transport configuration used by this session. @ingroup kanariv_client */
    const Config& config() const noexcept;

    /**
     * @brief Sends one command over the reused connection, connecting if needed.
     * @ingroup kanariv_client
     * @param cmd Command to serialize and transmit synchronously.
     * @param timeout Maximum time for connecting and for the reply; `0` selects
     * the command's transport policy, as with `Client::send()`.
     * @return Command result, or a transport/timeout error. A transport error
     * also closes the connection; the command is not retried.
     */
    kanariv::utils::Expected<kanariv::ipc::Result> send(const kanariv::ipc::Command& cmd,
                                                        std::chrono::milliseconds timeout = {});

    /**
     * @brief Waits for a playback status change over the reused connection.
     * @ingroup kanariv_client
     * @param timeout Maximum time the daemon waits for a change.
     * @return Current Status result, or timeout/error (which drops the connection).
     */
    kanariv::utils::Expected<kanariv::ipc::Result>
    waitForStatus(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    /**
     * @brief Closes the connection; the next `send()` connects again.
     * @ingroup kanariv_client
     * @details Use before a long idle gap, or to release the daemon-side
     * connection slot immediately instead of waiting for its idle timeout.
     */
    void reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::client
