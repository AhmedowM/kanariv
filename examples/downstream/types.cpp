#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <iostream>

int main() {
    kanariv::types::Track track;
    track.title = "An in-memory track";
    track.artist = "kanariv";
    kanariv::types::EngineConfig config;
    config.pollMs = 25;
    std::cout << track.title << " (" << config.pollMs << " ms)\n";
}
