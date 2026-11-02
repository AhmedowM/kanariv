#include <kanariv/contracts/database.hpp>
#include <type_traits>

static_assert(std::is_abstract_v<kanariv::contracts::IDatabase>);

int main() {}
