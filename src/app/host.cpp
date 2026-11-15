/**
 * @file host.cpp
 * @brief In-process daemon hosting for the foreground start path.
 * @ingroup kanariv_app
 * @details Composing a daemon (database + service) belongs to the
 * orchestrator, so every frontend gets the same foreground behaviour; the CLI
 * shell keeps only its syntax and rendering. Moved verbatim out of the shell.
 */

#include <app/app_impl.hpp>
#include <format>
#include <iostream>
#include <kanariv/db/database.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/print.hpp>
#include <stop_token>
#include <string>
#include <utility>

namespace kanariv::app {

AppResult App::Impl::runForeground(std::stop_token stop) {
    // One config snapshot for the whole run, like the other lifecycle paths.
    const auto cfg = config();
    kanariv::service::ServiceConfig scfg{cfg};
    scfg.logToStderr = true;
    auto db = kanariv::db::Database::open(scfg.dbPath.generic_string());
    if (!db) {
        return Outcome::fail(std::format("start failed: {} (db: {})", db.error().message,
                                         scfg.dbPath.generic_string()));
    }
    auto svc =
        kanariv::service::Service::create(scfg, kanariv::db::makeStorage(std::move(db.value())));
    if (!svc) {
        return Outcome::fail(std::format("start failed: {} (db: {})", svc.error().message,
                                         scfg.dbPath.generic_string()));
    }
    // Announce before run() blocks: the line used to be returned only after
    // the daemon exited, so users never saw it while it started.
    kanariv::println(std::cout, "starting daemon foreground at {} db={}", scfg.socketPath,
                     scfg.dbPath.generic_string());
    std::cout.flush();
    // The stop token makes Ctrl+C a graceful shutdown: run() observes it,
    // tears down (pid/socket/lock cleanup) and returns.
    auto res = svc.value()->run(stop);
    if (!res) {
        return Outcome::fail(std::format("daemon error: {} (dbPath={})", res.error().message,
                                         scfg.dbPath.generic_string()));
    }
    return Outcome{std::nullopt, std::string("daemon stopped"), false, false};
}

} // namespace kanariv::app
