/**
 * @file preview.cpp
 * @brief Ephemeral local file preview (no daemon).
 * @details The CLI owns the player-backed preview (the app layer stays free
 * of the player dependency): opens a file in a throwaway player and blocks
 * until it finishes. Frontends reuse this for quick audition without
 * touching the queue.
 */

#include "preview.hpp"

#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace kanariv::app::cli::detail {

kanariv::app::AppResult runLocalPreview(const std::string& file) {
    std::filesystem::path p{file};
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return kanariv::app::Outcome::fail(std::format("preview: file not found {}", file));
    }
    auto playerRes = kanariv::player::Player::create();
    if (!playerRes) {
        return kanariv::app::Outcome::fail(std::format(
            "preview: player create failed ({}): {}",
            kanariv::utils::toString(playerRes.error().code), playerRes.error().message));
    }
    auto& player = *playerRes.value();
    auto openRes = player.open(file);
    if (!openRes) {
        return kanariv::app::Outcome::fail(std::format("preview: open failed ({}): {}",
                                                      kanariv::utils::toString(openRes.error().code),
                                                      openRes.error().message));
    }
    auto playRes = player.play();
    if (!playRes) {
        return kanariv::app::Outcome::fail(std::format("preview: play failed ({}): {}",
                                                      kanariv::utils::toString(playRes.error().code),
                                                      playRes.error().message));
    }
    std::string done = std::format("preview playing {}\npreview done", file);
    while (player.state() == kanariv::player::State::Playing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return kanariv::app::Outcome{.result=std::nullopt, .line=std::move(done), .toStderr=false, .failed=false};
}

} // namespace kanariv::app::cli::detail
