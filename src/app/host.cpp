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
#include <kanariv/library/analyze.hpp>
#include <kanariv/library/scan.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/print.hpp>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

namespace kanariv::app {

AppResult App::Impl::runForeground(std::stop_token stop) {
    // One config snapshot for the whole run, like the other lifecycle paths.
    const auto cfg = config();
    kanariv::service::ServiceConfig scfg{cfg};
    scfg.logToStderr = true;
    auto db = kanariv::db::Database::open(scfg.base.dbPath.generic_string());
    if (!db) {
        return Outcome::fail(std::format("start failed: {} (db: {})", db.error().message,
                                         scfg.base.dbPath.generic_string()));
    }
    // The scanner shares ownership of the open database, so the ports and the
    // scanner stay valid for the service's lifetime (one composition root).
    std::shared_ptr<kanariv::db::Database> database = std::move(db.value());
    auto storage = kanariv::db::makeStorage(database);
    storage.scanner = std::make_shared<kanariv::library::Scanner>(database);
    storage.analyzer = std::make_shared<kanariv::library::Analyzer>(database);
    auto svc = kanariv::service::Service::create(scfg, std::move(storage));
    if (!svc) {
        return Outcome::fail(std::format("start failed: {} (db: {})", svc.error().message,
                                         scfg.base.dbPath.generic_string()));
    }
    // Announce before run() blocks: the line used to be returned only after
    // the daemon exited, so users never saw it while it started.
    kanariv::println(std::cout, "starting daemon foreground at {} db={}", scfg.base.socketPath,
                     scfg.base.dbPath.generic_string());
    std::cout.flush();
    // The stop token makes Ctrl+C a graceful shutdown: run() observes it,
    // tears down (pid/socket/lock cleanup) and returns.
    auto res = svc.value()->run(stop);
    if (!res) {
        return Outcome::fail(std::format("daemon error: {} (dbPath={})", res.error().message,
                                         scfg.base.dbPath.generic_string()));
    }
    return Outcome{std::nullopt, std::string("daemon stopped"), false, false};
}

} // namespace kanariv::app
