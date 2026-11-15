/**
 * @file preview.cpp
 * @brief Ephemeral local file preview (no daemon, no queue).
 * @ingroup kanariv_app
 * @details Opens a file in a throwaway player and blocks until it finishes or
 * the stop token is requested. Frontends reuse this for quick auditioning
 * without touching the queue; the shell keeps only its flag plumbing.
 */

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <kanariv/app/app.hpp>
#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/print.hpp>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>

namespace kanariv::app {

AppResult App::previewFile(const std::string& file, std::stop_token stop) {
    std::filesystem::path p{file};
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return Outcome::fail(std::format("preview: file not found {}", file));
    auto playerRes = kanariv::player::Player::create();
    if (!playerRes) {
        return Outcome::fail(std::format("preview: player create failed ({}): {}",
                                         kanariv::utils::toString(playerRes.error().code),
                                         playerRes.error().message));
    }
    auto& player = *playerRes.value();
    auto openRes = player.open(file);
    if (!openRes) {
        return Outcome::fail(std::format("preview: open failed ({}): {}",
                                         kanariv::utils::toString(openRes.error().code),
                                         openRes.error().message));
    }
    // Announce before playback: the line used to be returned only after the
    // file had finished, so a preview looked like a silent hang.
    kanariv::println(std::cout, "preview playing {}", file);
    std::cout.flush();
    auto playRes = player.play();
    if (!playRes) {
        return Outcome::fail(std::format("preview: play failed ({}): {}",
                                         kanariv::utils::toString(playRes.error().code),
                                         playRes.error().message));
    }
    bool interrupted = false;
    while (player.state() == kanariv::player::State::Playing) {
        if (stop.stop_requested()) {
            interrupted = true;
            (void)player.stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return Outcome{std::nullopt,
                   interrupted ? std::string("preview stopped") : std::string("preview done"),
                   false, false};
}

} // namespace kanariv::app
