#include <kanariv/service/service.hpp>
#include <iostream>

int main() {
    kanariv::service::ServiceConfig config;
    std::cout << "service configured with log level " << config.logLevel << '\n';
}
