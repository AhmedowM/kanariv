#pragma once

/**
 * @file app_impl.hpp
 * @brief Private implementation of kanariv::app::App.
 * @ingroup kanariv_app
 * @details Holds all private methods and data of App. Lives in src/
 * (not installed) so downstream consumers never see them.
 */

#include <kanariv/app/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::app {

struct App::Impl {
    /**
     * @brief Sends a command and normalizes server-side errors.
     * @param cmd IPC command to send.
     * @param timeout Per-attempt timeout (default 2 s).
     * @return Server result, or the transport/server error.
     */
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    sendRaw(const kanariv::ipc::Command& cmd,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

    /**
     * @brief Pairs a result with its success line, passing errors through.
     * @param res Daemon outcome to wrap.
     * @param line Human success line; nullopt means bare-render the result.
     * @return Outcome data for the frontend; errors surface unchanged.
     */
    AppResult confirm(std::expected<kanariv::ipc::Result, kanariv::utils::Error>&& res,
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
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    sendPlay(const kanariv::ipc::Command& cmd);

    /**
     * @brief Starts the daemon (foreground service or background spawn).
     * @param foreground Run the service in-process and block (child path).
     * @return Outcome data; success chatter is a line frontends skip in
     *         JSON mode, failures are failed lines.
     */
    AppResult startDaemon(bool foreground);

    /**
     * @brief Human track label for the last transport result.
     * @return Track label, or "unknown track" for non-status results.
     */
    std::string transportWho(const std::expected<kanariv::ipc::Result, kanariv::utils::Error>& res);

    kanariv::config::Config config_; ///< Bound config (db/socket/log paths).
    std::string argv0_;             ///< Executable path for POSIX re-spawn.
    std::function<AppResult(bool foreground)> daemonHost_{}; ///< Injected foreground daemon host.
    std::function<AppResult(const std::string& file)>
        previewHost_{}; ///< Injected local preview host.
};

} // namespace kanariv::app
