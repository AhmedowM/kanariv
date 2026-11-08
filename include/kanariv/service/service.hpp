#pragma once

/**
 * @file service.hpp
 * @brief Service: owns Engine, Database, Logger, IPC server, and dispatches
 * commands (daemon runtime used by the CLI service host).
 * @ingroup kanariv_service
 * @details Command handlers, the internal dispatch visitor and component ownership live in
 * Service::Impl (defined in the non-installed src/service/service_impl.hpp),
 * so this header exposes only the public daemon surface.
 */

#include <kanariv/config.hpp>
#include <kanariv/fwd.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

namespace kanariv::service {

/**
 * @brief Configuration for Service creation.
 * @ingroup kanariv_service
 */
struct ServiceConfig : public kanariv::config::Config {
    /** @brief Echo log lines to stderr (foreground mode). Default: false. */
    bool logToStderr{false};

    /**
     * @brief Run without desktop daemon scaffolding. Default: false.
     *
     * When true, Service::create does not acquire a lock file, write a PID
     * file, create a socket/named pipe, listen for IPC, or allocate shared
     * status memory. Commands are dispatched directly through Service::run's
     * in-process loop and Host. This is the mode an Android foreground service
     * uses; desktop CLI and daemon behavior is unchanged when false.
     */
    bool inProcess{false};

    /**
     * @brief Explicit host-supplied directories and transport path.
     *
     * Empty fields retain existing desktop behavior. When `database` is set it
     * takes precedence over the inherited `dbPath`; `socket` takes precedence
     * over the inherited `socketPath`. In-process hosts should set at least
     * `database` so no desktop path discovery is required.
     */
    kanariv::config::RuntimePaths runtime{};
};

/**
 * @brief Main service class that owns core components and dispatches IPC commands.
 * @ingroup kanariv_service
 *
 * @par Thread safety
 * Service is not thread-safe for concurrent method calls. The IPC server runs
 * on its own thread and invokes its internal dispatcher serially. External callers should not
 * invoke Service methods concurrently with server->run().
 *
 * @par Lifetime
 * Created via create(), runs via run(stop_token), cleaned up via shutdown() in
 * destructor. Uses RAII for all resources (DB, Engine, IPC server, PID file, lock file, shared
 * memory).
 */
class KANARIV_API Service final {
  public:
    /**
     * @brief Result type returned by Service creation.
     * @ingroup kanariv_service
     * @details Contains an owning service instance on success. On failure,
     * the error identifies configuration, single-instance, database, IPC,
     * or shared-memory initialization failure.
     */
    using ExpectedService = std::expected<std::unique_ptr<Service>, kanariv::utils::Error>;

    /**
     * @brief Factory method to create and initialize a Service instance.
     * @ingroup kanariv_service
     * @param cfg Service configuration (dbPath, optional socketPath, configPath, logLevel).
     * @param db Optional already-open database contract; when omitted, Service
     * works without one (Engine attaches whatever is supplied).
     * @return Expected unique_ptr to Service, or Error if initialization fails.
     *
     * Resolves host-supplied runtime paths first (`runtime.database`,
     * `runtime.configuration`, `runtime.socket` take precedence over the
     * inherited fields) and creates any supplied directories.
     *
     * When `cfg.inProcess` is false, performs single-instance enforcement via
     * flock lock file and PID file checks, creates a PID file, initializes the
     * shared memory status block, and starts the IPC server listening on the
     * derived or explicit socket path.
     *
     * When `cfg.inProcess` is true, none of the desktop scaffolding is
     * created: no lock, PID, socket, IPC server, or shared memory. Only the
     * database, engine, and optional log file are initialized; callers drive
     * the loop through `run()` and the `Host` wrapper.
     *
     * @see Host
     */
    static ExpectedService create(const ServiceConfig& cfg,
                                  std::shared_ptr<kanariv::contracts::IDatabase> db = nullptr);

    /**
     * @brief Destructor: ensures cleanup via shutdown().
     * @ingroup kanariv_service
     */
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;

    /**
     * @brief Run the service event loop until stop_token is triggered.
     * @ingroup kanariv_service
     * @param st Stop token to request graceful shutdown.
     * @return void on success, Error if already running.
     *
     * Starts the IPC server with an internal dispatcher for incoming commands.
     * Blocks until stop_token signals or shutdown() is called.
     * @par Thread safety
     * Must not be called concurrently with another run().
     * The token is observed between dispatch iterations; call `shutdown()` to
     * request immediate teardown from another thread.
     */
    kanariv::utils::Expected<void> run(std::stop_token st);

    /**
     * @brief Reports whether the service loop is active.
     * @ingroup kanariv_service
     * @return True from the moment `run()` starts until `shutdown()` begins.
     *
     * Host implementations use this to wait for readiness after starting
     * `run()` on a worker thread.
     */
    [[nodiscard]] bool running() const noexcept;

    /**
     * @brief Replaces the attached database and reloads engine state from it.
     * @ingroup kanariv_service
     * @param db Shared database contract (must be non-null and open).
     * @return Success or Error InvalidArg if db is null or not open.
     */
    kanariv::utils::Expected<void> attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db);

    /**
     * @brief Access the audio engine instance.
     * @return Reference to the Engine.
     * @details Exposed for host integrations (e.g. Android foreground service)
     * that need direct engine access without going through IPC.
     */
    kanariv::engine::Engine& engine() noexcept;

    /**
     * @brief Access the database instance.
     * @return Reference to the Database.
     * @details Exposed for host integrations (e.g. Android foreground service)
     * that need direct database access without going through IPC.
     */
    kanariv::contracts::IDatabase& db() noexcept;

    /**
     * @brief Gracefully shut down the service.
     * @ingroup kanariv_service
     *
     * Signals shutdown to IPC server and Engine, removes PID file, socket file (Unix),
     * and releases the flock lock file. Shared memory handle is cleaned up via RAII.
     * Safe to call multiple times; idempotent.
     */
    void shutdown();

  private:
    /**
     * @brief Access the IPC server instance.
     * @return Reference to the IpcServer.
     */
    IpcServer& server() noexcept;

    /**
     * @brief Get the shared memory name used for status block.
     * @return Shared memory name (hash of dbPath).
     */
    const std::string& shmName() const noexcept;

    /**
     * @brief Get the shared memory status writer (if available).
     * @return Pointer to ShmStatusWriter or nullptr if SHM creation failed.
     */
    kanariv::ipc::ShmStatusWriter* shmWriter() noexcept;

    /**
     * @brief Private constructor used by create().
     */
    Service(const ServiceConfig& cfg, std::shared_ptr<kanariv::contracts::IDatabase> db,
            std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
            std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
            std::string socketPath, std::intptr_t lockFd,
            std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::service
