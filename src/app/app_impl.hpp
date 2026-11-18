#pragma once

/**
 * @file app_impl.hpp
 * @brief Private implementation of kanariv::app::App.
 * @ingroup kanariv_app
 * @details Holds all private methods and data of App. Lives in src/
 * (not installed) so downstream consumers never see them.
 */

#include <chrono>
#include <expected>
#include <filesystem>
#include <kanariv/app/app.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/config.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::app {

struct App::Impl {
    /**
     * @brief Returns a consistent snapshot of the bound config.
     * @details Handlers take exactly one snapshot per call, so a concurrent
     * setConfig() either applies to the whole call or not at all; no handler
     * can observe a mixed (torn) config.
     */
    kanariv::config::Config config() const;

    /** @brief Replaces the config snapshot atomically. */
    void setConfig(kanariv::config::Config cfg);

    /** @brief Returns the configured daemon re-spawn executable path. */
    std::string argv0() const;

    /** @brief Replaces the daemon re-spawn executable path atomically. */
    void setArgv0(std::string argv0);

    /**
     * @brief Builds a client from one config snapshot.
     * @return Client bound to the snapshot's db/socket paths.
     */
    kanariv::client::Client makeClient() const;

    /**
     * @brief One-shot, bounded daemon liveness probe.
     * @return True when a connection can be established right now.
     * @details Only used on error paths; the success path never pays for it.
     */
    bool daemonReachable() const;

    /**
     * @brief Sends a command and normalizes server-side errors.
     * @param cmd IPC command to send.
     * @param timeout Per-attempt timeout; `0` uses the command's
     * transport policy (see `kanariv::ipc::transportPolicyFor`).
     * @return Server result, or the transport/server error. A transport
     * failure followed by a failed liveness probe is reported as the
     * canonical unreachable-daemon error.
     */
    kanariv::utils::Expected<kanariv::ipc::Result> sendRaw(const kanariv::ipc::Command& cmd,
                                                           std::chrono::milliseconds timeout = {});

    /**
     * @brief Pairs a result with its success line, passing errors through.
     * @param res Daemon outcome to wrap.
     * @param line Human success line; nullopt means bare-render the result.
     * @return Outcome data for the frontend; errors surface unchanged.
     */
    AppResult confirm(kanariv::utils::Expected<kanariv::ipc::Result>&& res,
                      std::optional<std::string> line = std::nullopt);

    /**
     * @brief Spawns a detached daemon child for the given config.
     * @param cfg Config selecting db/socket paths for the child.
     * @return Success, or the OS error code when spawning failed.
     */
    std::expected<void, std::uint32_t> spawnDaemon(const kanariv::config::Config& cfg);

    /**
     * @brief Derives the pid-file path for the bound config.
     * @return Canonical pid path, or a db-adjacent fallback.
     */
    std::filesystem::path pidPathForConfig() const;

    /**
     * @brief Sends a play-like command, autostarting a down daemon.
     * @param cmd IPC command to send.
     */
    kanariv::utils::Expected<kanariv::ipc::Result> sendPlay(const kanariv::ipc::Command& cmd);

    /**
     * @brief Starts the daemon (foreground service or background spawn).
     * @param foreground Run the service in-process and block (child path).
     * @param stop Token that ends a foreground run gracefully.
     * @return Outcome data on success; std::unexpected for real start
     *         failures (host failure, spawn failure, readiness timeout).
     */
    AppResult startDaemon(bool foreground, std::stop_token stop);

    /**
     * @brief Hosts the service in this process until the stop token fires.
     * @param stop Token observed by the service loop.
     * @return "daemon stopped" on a clean run; a `failed` outcome for
     *         database, service or daemon-loop startup failures.
     */
    AppResult runForeground(std::stop_token stop);

    /**
     * @brief Human track label for the last transport result.
     * @return TrackRow label, or "unknown track" for non-status results.
     */
    std::string transportWho(const kanariv::utils::Expected<kanariv::ipc::Result>& res);

  private:
    mutable std::mutex stateMutex_;  ///< Guards config_ and argv0_.
    kanariv::config::Config config_; ///< Bound config (db/socket/log paths).
    std::string argv0_;              ///< Executable path for POSIX re-spawn.
};

} // namespace kanariv::app
