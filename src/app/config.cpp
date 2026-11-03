/**
 * @file config.cpp
 * @brief Configuration command handlers.
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders. Reads
 * answer from the local file without a daemon; writes go through it.
 */

#include <app/app_impl.hpp>
#include <expected>
#include <format>
#include <kanariv/app/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <optional>
#include <string>

#include "config/raw.hpp"

namespace kanariv::app {

auto App::configGet(const std::string& key) -> AppResult {
    // Answered from the local file: no daemon needed.
    auto v = kanariv::config::detail::configGetRaw(impl_->config_.configPath, key);
    if (!v) {
        return std::unexpected{v.error()};
    }
    return impl_->confirm(kanariv::ipc::Result{kanariv::ipc::ConfigValue{.key = key, .value = *v}});
}

auto App::configSet(const std::string& key, const std::string& value) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigSet{.key = key, .value = value}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
    return Outcome{};
}

auto App::configList() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigList{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

auto App::configExport(const std::string& path) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigExport{path}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Exported config to {}", path));
}

auto App::configImport(const std::string& path) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigImport{path}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Imported config from {}", path));
}

auto App::configReset(std::optional<std::string> key) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::ConfigReset{key}};
    std::string line =
        key.has_value() ? std::format("Reset config key '{}'", *key) : "Reset all config";
    return impl_->confirm(impl_->sendRaw(cmd), std::move(line));
}

} // namespace kanariv::app
