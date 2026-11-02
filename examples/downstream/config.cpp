#include <kanariv/config.hpp>
#include <kanariv/utils/error.hpp>
#include <iostream>

int main() {
    const auto result = kanariv::config::socketPathFor("library.db");
    if (!result) return 1;
    std::cout << *result << '\n';
}
