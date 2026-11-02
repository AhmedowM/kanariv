#include <kanariv/config.hpp>
#include <kanariv/utils/error.hpp>
#include <iostream>

int main() {
    kanariv::config::Config config;
    config.device = "auto";
    config.logLevel = 3;
    if (auto result = kanariv::config::saveConfig(config); !result) return 1;
    std::cout << "configuration saved\n";
}
