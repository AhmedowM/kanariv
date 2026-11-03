#include <iostream>
#include <kanariv/config.hpp>
#include <kanariv/utils/error.hpp>

int main() {
    const auto result = kanariv::config::socketPathFor("library.db");
    if (!result)
        return 1;
    std::cout << *result << '\n';
}
