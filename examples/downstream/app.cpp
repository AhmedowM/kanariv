#include <kanariv/app/paths.hpp>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::string> paths;
    std::vector<std::string> unmatched;
    kanariv::app::expandAddToken(".", false, paths, unmatched);
    std::cout << "expanded paths=" << paths.size() << '\n';
}
