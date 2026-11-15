#pragma once

/**
 * @file preview.hpp
 * @brief CLI-owned ephemeral local file preview (no daemon).
 * @details The app layer stays free of the player dependency; the CLI
 * shell owns the player-backed preview and injects it into the App
 * orchestrator via App::attachPreviewHost.
 */

#include <kanariv/app/app.hpp>
#include <stop_token>
#include <string>

namespace kanariv::app::cli::detail {

/**
 * @brief Previews a local file in a throwaway player (no daemon, no queue).
 * @param file Path to the audio file.
 * @param stop Process stop token; a request stops the player and returns.
 * @return Outcome data; blocks until playback finishes or stop is requested.
 * @details Announcements bracket the playback: "preview playing <file>" is
 * printed before playback starts, and the returned line is "preview done"
 * (or "preview stopped" when cut short by the stop token).
 */
kanariv::app::AppResult runLocalPreview(const std::string& file, std::stop_token stop = {});

} // namespace kanariv::app::cli::detail
