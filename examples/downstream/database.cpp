#include <kanariv/db/database.hpp>
#include <kanariv/utils/error.hpp>
#include <iostream>

int main() {
    auto result = kanariv::db::Database::open(":memory:");
    if (!result) return 1;
    std::cout << "database opened\n";
}
