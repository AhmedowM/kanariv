#pragma once

/**
 * @file host.hpp
 * @brief Host: runs a Service in-process without desktop daemon scaffolding.
 * @ingroup kanariv_service
 *
 * A native host (for example an Android foreground service) needs to own the
 * service lifecycle directly: start it on a worker thread, observe readiness,
 * request cooperative cancellation, and wait for a clean shutdown. Host wraps
 * that pattern so every host does not reimplement thread and stop-token
 * handling.
 *
 * Host always creates its Service with `ServiceConfig::inProcess = true`, so
 * no lock file, PID file, socket, IPC server, or shared memory is created.
 * Desktop code should keep using `Service::create` and the daemon entry
 * points; desktop behavior is unchanged.
 *
 * @code
 * kanariv::service::ServiceConfig cfg;
 * cfg.runtime.database = "/data/app/kanariv/kanariv.db";
 * cfg.runtime.logs = "/data/app/kanariv/logs";
 * auto host = kanariv::service::Host::create(cfg);
 * if (!host) { ... }
 * // ... use host->service() ...
 * host->stop();
 * @endcode
 */

#include <kanariv/fwd.hpp>
#include <kanariv/service/service.hpp>
#include <memory>
#include <optional>

namespace kanariv::service {

/**
 * @class Host
 * @brief Owns an in-process Service and its worker thread.
 * @ingroup kanariv_service
 *
 * @par Lifetime
 * Create with `create()`, which starts the service loop before
 * returning. `stop()` requests cancellation and waits. The destructor does the
 * same, so an early return cannot leak the worker thread.
 *
 * @par Thread safety
 * `requestStop()`, `running()`, `ready()`, and `error()` are
 * safe to call from any thread. `wait()` and `stop()` must not be called from
 * the worker thread (it cannot join itself) and must not run concurrently with
 * each other.
 *
 * The wrapped Service is not thread-safe for concurrent command calls; a host
 * must serialize any command dispatch it performs through `service()`.
 */
class KANARIV_API Host final {
  public:
    /**
     * @brief Result type returned by Host creation.
     * @ingroup kanariv_service
     */
    using ExpectedHost = std::expected<std::unique_ptr<Host>, kanariv::utils::Error>;

    /**
     * @brief Creates and starts an in-process service.
     * @ingroup kanariv_service
     * @param cfg Service configuration. `inProcess` is forced to true.
     * @param db Optional already-open database contract; when omitted the
     *        service opens `cfg.runtime.database` (or `cfg.dbPath`, or an
     *        in-memory database when both are empty).
     * @return Started Host, or Error from directory creation, database open,
     *         engine initialization, or the service loop.
     *
     * Returns only after the service loop reports running. If the loop fails
     * during startup, the Error is returned and no Host is created.
     */
    static ExpectedHost create(ServiceConfig cfg,
                               std::shared_ptr<kanariv::contracts::IDatabase> db = nullptr);

    /**
     * @brief Requests cancellation and waits for the worker thread.
     * @ingroup kanariv_service
     *
     * Idempotent. Never throws.
     */
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) = delete;
    Host& operator=(Host&&) = delete;

    /**
     * @brief Requests cooperative shutdown without waiting.
     * @ingroup kanariv_service
     *
     * Safe to call repeatedly and from any thread. The service loop observes
     * the request within its polling interval and performs its own cleanup.
     */
    void requestStop() noexcept;

    /**
     * @brief Waits for the worker thread to finish.
     * @ingroup kanariv_service
     * @return True when the service loop returned success; false when it
     *         returned an error (see `error()`).
     *
     * Call `requestStop()` first if the service should stop now.
     */
    [[nodiscard]] bool wait();

    /**
     * @brief Requests cancellation and waits for the worker thread.
     * @ingroup kanariv_service
     *
     * Equivalent to `requestStop()` followed by `wait()`.
     */
    void stop();

    /**
     * @brief Reports whether the service loop is running.
     * @ingroup kanariv_service
     */
    [[nodiscard]] bool running() const noexcept;

    /**
     * @brief Reports whether the worker thread started successfully.
     * @ingroup kanariv_service
     *
     * Always true for a Host returned by `create()`; retained so a host can
     * assert readiness after an asynchronous restart design change.
     */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief Returns the terminal error, if the service loop failed.
     * @ingroup kanariv_service
     * @return A copy of the error after the loop ended, or nullopt while
     *         running and after a successful stop.
     */
    [[nodiscard]] std::optional<kanariv::utils::Error> error() const;

    /** @brief Returns the wrapped service. @ingroup kanariv_service */
    Service& service() noexcept;
    /** @brief Returns the wrapped service. @ingroup kanariv_service */
    const Service& service() const noexcept;

  private:
    /**
     * @brief Wraps an already-created implementation.
     * @param impl Host state (must be non-null).
     */
    explicit Host(std::unique_ptr<struct HostImpl> impl) noexcept;

    std::unique_ptr<struct HostImpl> impl_;
};

} // namespace kanariv::service
