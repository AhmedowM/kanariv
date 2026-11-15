/**
 * @file config.cpp
 * @brief Configuration command handlers.
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders. The daemon
 * owns config when it is running (it persists the file it resolves and
 * applies live values); with no daemon the file is the only authority, so a
 * read still works before `kanariv start`.
 */

#include <app/app_impl.hpp>
#include <kanariv/app/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <expected>
#include <format>
#include <optional>
#include <string>

#include "config/raw.hpp"

namespace kanariv::app {

AppResult App::configGet(const std::string& key) {
    // Ask the daemon while it is up, so a value it clamped or canonicalized is
    // what the user sees; fall back to the file when it is down.
    if (impl_->daemonReachable()) {
        kanariv::ipc::Command cmd{kanariv::ipc::ConfigGet{key}};
        return impl_->confirm(impl_->sendRaw(cmd));
    }
    const auto cfg = impl_->config();
    auto v = kanariv::config::detail::configGetRaw(cfg.configPath, key);
    if (!v)
        return std::unexpected{v.error()};
    return impl_->confirm(kanariv::ipc::Result{kanariv::ipc::ConfigValue{key, *v}});
}

AppResult App::configSet(const std::string& key, const std::string& value) {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigSet{key, value}};
    // The reply carries the stored value, so a normalized (or rejected) write
    // is visible instead of every success looking like a no-op.
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::configList() {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigList{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::configExport(const std::string& path) {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigExport{path}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Exported config to {}", path));
}

AppResult App::configImport(const std::string& path) {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigImport{path}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Imported config from {}", path));
}

AppResult App::configReset(std::optional<std::string> key) {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigReset{key}};
    std::string line =
        key.has_value() ? std::format("Reset config key '{}'", *key) : "Reset all config";
    return impl_->confirm(impl_->sendRaw(cmd), std::move(line));
}

} // namespace kanariv::app
