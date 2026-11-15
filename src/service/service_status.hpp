#pragma once

/**
 * @file service_status.hpp
 * @brief Daemon status snapshot builder for kanariv.service (internal).
 * @ingroup kanariv_service
 */

#include <expected>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils.hpp>

namespace kanariv::engine {
class Engine;
} // namespace kanariv::engine

namespace kanariv::service::detail {

/**
 * @brief Build a Status object from Engine and optional persistence ports.
 * Populates playback state, position, duration, volume, shuffle, repeat,
 * current track info (title, artist, path), and active queue size/index.
 * @param eng Engine reference.
 * @param storage Persistence ports; when incomplete, only engine state is
 * reported (title/artist/path and queue counters stay at their defaults).
 * @return Status on success, Error on failure.
 */
std::expected<kanariv::ipc::Status, kanariv::utils::Error>
buildStatus(kanariv::engine::Engine& eng, const kanariv::contracts::Storage& storage);

} // namespace kanariv::service::detail
