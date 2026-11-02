#include <algorithm>
#include <cstdint>
#include <engine/shuffle.hpp>
#include <random>
#include <vector>

namespace kanariv::engine::detail {

void shufflePerm(std::vector<int64_t>& perm, std::mt19937& rng) {
    std::ranges::shuffle(perm, rng);
}

void shufflePerm(std::vector<int64_t>& perm) {
    std::random_device rd;
    std::mt19937 gen(rd());
    shufflePerm(perm, gen);
}

} // namespace kanariv::engine::detail
