#pragma once

#include <expected>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <string_view>
#include <utility>

namespace kanariv::test_helpers {

inline kanariv::utils::Expected<std::unique_ptr<kanariv::engine::Engine>>
openEngineWithDatabase(std::string_view path, const kanariv::types::EngineConfig& config) {
    auto database = kanariv::db::Database::open(path);
    if (!database)
        return std::unexpected(database.error());

    auto engine = kanariv::engine::Engine::create(config);
    if (!engine)
        return std::unexpected(engine.error());

    auto instance = std::move(engine.value());
    std::shared_ptr<kanariv::db::Database> sharedDatabase(std::move(database.value()));
    if (auto attached =
            instance->attachStorage(kanariv::db::makeStorage(std::move(sharedDatabase)));
        !attached)
        return std::unexpected(attached.error());
    return instance;
}
inline kanariv::utils::Expected<std::unique_ptr<kanariv::service::Service>>
openServiceWithDatabase(std::string_view path, const kanariv::service::ServiceConfig& config) {
    auto database = kanariv::db::Database::open(path);
    if (!database)
        return std::unexpected(database.error());

    return kanariv::service::Service::create(config,
                                             kanariv::db::makeStorage(std::move(database.value())));
}

} // namespace kanariv::test_helpers
