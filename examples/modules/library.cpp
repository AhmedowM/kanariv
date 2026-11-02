#include <iostream>
#include <memory>
import kanariv.db;
import kanariv.engine;

int main() {
    auto database = kanariv::db::Database::open(":memory:");
    auto engine = kanariv::engine::Engine::create();
    if (!database || !engine) return 1;
    if (auto result = engine.value()->attachDatabase(
            std::shared_ptr<kanariv::db::Database>(std::move(database.value())));
        !result) return 1;
    std::cout << "module consumer ready\n";
}
