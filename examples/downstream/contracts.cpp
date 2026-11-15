#include <kanariv/contracts/storage.hpp>
#include <type_traits>

static_assert(std::is_abstract_v<kanariv::contracts::ITrackStore>);
static_assert(std::is_abstract_v<kanariv::contracts::ILibraryStore>);

int main() {}
