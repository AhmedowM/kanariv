#pragma once

/**
 * @file service_status.hpp
 * @brief Daemon status snapshot builder for kanariv.service (internal).
 * @ingroup kanariv_service
 */

#include <expected>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils.hpp>

namespace kanariv::engine {
class Engine;
} // namespace kanariv::engine

namespace kanariv::contracts {
class IDatabase;
} // namespace kanariv::contracts

namespace kanariv::service::detail {

/**
 * @brief Build a Status object from Engine and Database.
 * Populates playback state, position, duration, volume, shuffle, repeat,
 * current track info (title, artist, path), and active queue size/index.
 * @param eng Engine reference.
 * @param db Database reference.
 * @return Status on success, Error on failure.
 */
std::expected<kanariv::ipc::Status, kanariv::utils::Error>
buildStatus(kanariv::engine::Engine& eng, kanariv::contracts::IDatabase& db);

} // namespace kanariv::service::detail
