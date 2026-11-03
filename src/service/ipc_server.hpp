#pragma once

/**
 * @file ipc_server.hpp
 * @brief Accept-loop IPC server (daemon side).
 * @ingroup kanariv_service
 * @details Listens on a Unix domain socket (POSIX) or named pipe (Windows)
 * and dispatches each connection to the `Service` dispatcher on its own
 * thread. Windows: local API declarations are used when `windows.h` was
 * not included yet, so including this header never forces `windows.h`
 * (and its `min`/`max` macros) on consumers; see `shm_status.hpp` and
 * `utils/thread.hpp` for the same pattern.
 *
 * Internal header: lives in `src/`, not installed. `Service` owns the
 * server; the public surface never names it.
 */

#include <expected>
#include <filesystem>
#include <functional>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace kanariv::service {

/**
 * @brief Daemon-side accept loop; one thread per connection.
 * @ingroup kanariv_service
 */
class IpcServer {
  public:
    /** @brief Default-constructs an idle server (call listen() next). */
    IpcServer();
    /** @brief Shuts down and joins threads. */
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;

    /**
     * @brief Binds the socket/pipe derived from the database path.
     * @ingroup kanariv_service
     * @param dbPath Database path (socket path is derived from it).
     * @param socketPathOverride Explicit path, bypassing derivation.
     * @return Success or `Io` Error (address in use, permission).
     */
    kanariv::utils::Expected<void> listen(const std::filesystem::path& dbPath,
                                          std::string_view socketPathOverride = {});

    /**
     * @brief Serves connections until the stop token fires.
     * @ingroup kanariv_service
     * @param st Stop token ending the accept loop.
     * @param dispatch Called per request on a connection thread.
     */
    void run(std::stop_token st,
             std::function<std::expected<kanariv::ipc::Result, kanariv::utils::Error>(
                 const kanariv::ipc::Command&)>
                 dispatch);

    /**
     * @brief Stops accepting and joins connection threads (idempotent).
     * @ingroup kanariv_service
     */
    void shutdown();

    /** @brief Marks the calling connection thread done (self-retire). */
    void retireClient();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::service
