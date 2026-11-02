#pragma once

/**
 * @file preview.hpp
 * @brief CLI-owned ephemeral local file preview (no daemon).
 * @details The app layer stays free of the player dependency; the CLI
 * shell owns the player-backed preview and injects it into the App
 * orchestrator via App::attachPreviewHost.
 */

#include <kanariv/app/app.hpp>
#include <string>

namespace kanariv::app::cli::detail {

/**
 * @brief Previews a local file in a throwaway player (no daemon, no queue).
 * @param file Path to the audio file.
 * @return Outcome data; blocks until playback finishes.
 */
kanariv::app::AppResult runLocalPreview(const std::string& file);

} // namespace kanariv::app::cli::detail
