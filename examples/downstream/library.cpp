#include <iostream>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>

int main() {
    auto database = kanariv::db::Database::open(":memory:");
    if (!database)
        return 1;
    kanariv::types::EngineConfig engineConfig;
    engineConfig.pollMs = 25;
    std::cout << "database plus engine configuration ready: " << engineConfig.pollMs << " ms\n";
}
