#include <expected>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

import kanariv.player;
import kanariv.utils;

using namespace kanariv::player;
using namespace kanariv::utils;

namespace kanariv::examples {

class MiniPlayer {
  public:
    Expected<void> run(const std::string& path) {
        auto playerResult = Player::create();
        if (!playerResult) {
            return std::unexpected(playerResult.error());
        }
        auto player = std::move(playerResult.value());

        auto openResult = player->open(path);
        if (!openResult) {
            return std::unexpected(openResult.error());
        }

        std::cout << "Opened: " << path << "\n";
        std::cout << "Playing...\n";

        auto playResult = player->play();
        if (!playResult) {
            return std::unexpected(playResult.error());
        }

        // Wait for playback to finish
        while (player->state() == State::Playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            double pos = player->position().count();
            if (pos > 0) {
                std::cout << "\rPlayed: " << pos << "s" << std::flush;
            }
        }

        std::cout << "\nPlayback finished\n";
        return {};
    }
}; // class MiniPlayer

} // namespace kanariv::examples

int main(int argc, char** argv) {
    using namespace kanariv::examples;

    std::string path;
    if (argc > 1) {
        path = argv[1];
    } else {
        path = std::filesystem::current_path().string() + "/tests/fixtures/sample.wav";
        std::string musicPath = "C:/Users/Secondary/Music/ALL ABOUT MY CLIQUE - TOPDOGFIGHT.mp3";
        if (std::filesystem::exists(musicPath)) {
            std::cout << "Found MP3 in Music folder, using it instead\n";
            path = musicPath;
        }
    }

    if (!std::filesystem::exists(path)) {
        std::cerr << "File not found: " << path << "\n";
        return 1;
    }

    MiniPlayer mini;
    auto StatusCode = mini.run(path);
    if (!StatusCode) {
        std::cerr << "Error: " << StatusCode.error().message
                  << " (code: " << static_cast<int>(StatusCode.error().code) << ")\n";
        return 1;
    }
    return 0;
}
