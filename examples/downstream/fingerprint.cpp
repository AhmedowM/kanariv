#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <iostream>

int main() {
    auto result = kanariv::media::scanTracksToVector(".");
    if (!result) return 1;
    std::cout << "discovered " << result->size() << " tracks\n";
}
