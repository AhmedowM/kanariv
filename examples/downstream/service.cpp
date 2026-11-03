#include <iostream>
#include <kanariv/service/service.hpp>

int main() {
    kanariv::service::ServiceConfig config;
    std::cout << "service configured with log level " << config.logLevel << '\n';
}
