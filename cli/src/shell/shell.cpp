/**
 * @file shell.cpp
 * @brief Shell construction and the linear run() chain.
 * @details Option declaration and per-domain dispatch live in the
 * `commands_*.cpp` files (see commands.hpp); this file owns argument parsing
 * policy (CLI11 remap), config resolution and the order domains are tried.
 */

#include "shell.hpp"

#include <CLI/CLI.hpp>
#include <chrono>
#include <expected>
#include <filesystem>
#include <iostream>
#include <kanariv/app/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/parse.hpp>
#include <kanariv/utils/print.hpp>
#include <kanariv/version.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "parse.hpp"
#include "render.hpp"

namespace kanariv::app::cli {

namespace detail {
// Single source: delegate to kanariv::app::cli::parse::parseTime (parse.hpp) and adapt error type.
// parse.hpp returns expected<double,string>; app layer wraps string into utils::Error.
std::expected<double, kanariv::utils::Error> parseTime(std::string_view s) {
    auto r = kanariv::app::cli::parse::parseTime(s);
    if (!r)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, r.error())};
    return *r;
}
// Single source: delegate to kanariv::app::cli::parse::parseSeek and adapt error type.
std::expected<double, kanariv::utils::Error> parseSeek(std::string_view s) {
    auto r = kanariv::app::cli::parse::parseSeek(s);
    if (!r)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, r.error())};
    return *r;
}
// Single source: delegate to kanariv::app::cli::parse::parseVolume and adapt error type.
std::expected<kanariv::ipc::VolumeSet, kanariv::utils::Error> parseVolume(std::string_view s) {
    auto r = kanariv::app::cli::parse::parseVolume(s);
    if (!r)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, r.error())};
    kanariv::app::cli::parse::ParsedVolume pv = *r;
    kanariv::ipc::VolumeSet vs{};
    vs.level = pv.level;
    vs.mute = pv.mute;
    vs.deltaPct = pv.deltaPct;
    return vs;
}
std::chrono::duration<double> parseDuration(std::string_view s) {
    auto t = parseTime(s);
    if (!t)
        return std::chrono::duration<double>{0};
    return std::chrono::duration<double>{*t};
}

} // namespace detail
using detail::parseSeek;
using detail::parseTime;
using detail::parseVolume;

Shell::Shell(kanariv::config::Config cfg, std::stop_token stop)
    : config_(cfg), app_(std::move(cfg)),
      cli_(std::make_unique<CLI::App>("kanariv - terminal player")), stop_(stop) {
    cli_->set_version_flag("--version", std::string(kanariv::version::version()));
}

Shell::~Shell() = default;

int Shell::run(int argc, char** argv) {
    if (argc > 0 && argv && argv[0])
        app_.setArgv0(argv[0]);
    detail::Options opts;
    detail::declareAll(*cli_, opts);
    try {
        cli_->parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // CLI11's own codes (100..115) collide with this CLI's 105/106/109, so
        // the process status is normalised (L8): help/version stay 0, every
        // rejected invocation is a usage error. exit() still prints the message
        // or the requested help exactly as before.
        const bool informational = e.get_exit_code() == 0;
        (void)cli_->exit(e);
        return informational ? detail::exitOk : detail::exitUsage;
    }
    if (opts.lifecycle.version->parsed())
        return detail::renderVersion(opts.lifecycle.versionJson);
    if (!opts.global.configPath.empty())
        config_.configPath = std::filesystem::path(opts.global.configPath);
    bool configMissing = false;
    {
        auto loaded = kanariv::config::loadConfig(config_.configPath);
        if (loaded) {
            config_ = *loaded;
            if (!opts.global.configPath.empty())
                config_.configPath = std::filesystem::path(opts.global.configPath);
            // First-run detection only: defaults are persisted by mutating
            // commands (see PersistDefaultsGuard below), never by read-only
            // ones -- `kanariv status` must not create files.
            std::error_code ec;
            configMissing = !std::filesystem::exists(config_.configPath, ec);
        } else {
            // loadConfig failed (e.g. corrupt); keep current config_ which has robust defaults
            // ensure directories exist for db and config
            std::error_code ec;
            if (!config_.dbPath.empty()) {
                auto parent = config_.dbPath.parent_path();
                if (!parent.empty())
                    std::filesystem::create_directories(parent, ec);
            }
        }
    }
    if (!opts.global.device.empty())
        config_.device = opts.global.device;
    if (!opts.global.logLevel.empty()) {
        auto lvl = kanariv::utils::parseLogLevel(opts.global.logLevel);
        if (!lvl) {
            kanariv::println(std::cerr,
                             "--log-level: invalid '{}' (expected trace|debug|info|warn|error)",
                             opts.global.logLevel);
            return detail::exitUsage;
        }
        config_.logLevel = std::to_underlying(*lvl);
    }
    if (config_.socketPath.empty() && !config_.dbPath.empty()) {
        auto sp = kanariv::config::socketPathFor(config_.dbPath);
        if (sp)
            config_.socketPath = *sp;
    }
    // Ensure db parent dir exists before any operation (fixes "unable to open database file")
    {
        std::error_code ec;
        auto parent = config_.dbPath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
    }
    // The App orchestrator was bound to the pre-parse config in the Shell
    // ctor; sync it now that reload plus socket derivation are final. Every
    // daemon interaction below must observe these paths.
    app_.setConfig(config_);
    // Read-only commands must not write the config file on first run (it makes
    // `status`/`config get` mutating, races concurrent invocations and fails
    // on read-only homes). Persist defaults only when the invoked command
    // mutates state; the guard warns instead of silently dropping a failure.
    detail::PersistDefaultsGuard persistGuard{config_, configMissing && detail::isMutating(opts)};
    // Internal daemon mode: if --daemon present, run Service foreground immediately (child process)
    if (opts.global.daemon)
        return detail::render(app_.startDaemon(true, stop_), false);
    if (auto rc = detail::runDirectPlay(app_, *cli_, opts))
        return *rc;
    if (auto rc = detail::runLifecycle(app_, stop_, opts))
        return *rc;
    if (auto rc = detail::runPlayback(app_, config_, stop_, opts))
        return *rc;
    if (auto rc = detail::runQueue(app_, opts))
        return *rc;
    if (auto rc = detail::runPlaylist(app_, opts))
        return *rc;
    if (auto rc = detail::runLibrary(app_, opts))
        return *rc;
    if (auto rc = detail::runTags(app_, opts))
        return *rc;
    if (auto rc = detail::runHistory(app_, opts))
        return *rc;
    if (auto rc = detail::runLyrics(app_, config_, stop_, opts))
        return *rc;
    if (auto rc = detail::runPreview(app_, config_, stop_, opts))
        return *rc;
    if (auto rc = detail::runConfig(app_, opts))
        return *rc;
    std::cout << cli_->help() << "\n";
    return 0;
}

} // namespace kanariv::app::cli
