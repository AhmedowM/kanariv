#include <iostream>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>

int main() {
    kanariv::types::EngineConfig config;
    config.pollMs = 25;
    std::cout << "engine poll interval=" << config.pollMs << " ms\n";
}
