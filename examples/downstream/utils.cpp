#include <kanariv/utils/error.hpp>
#include <kanariv/utils/ring.hpp>
#include <iostream>

int main() {
    kanariv::utils::SpscRing<float> ring(8, 2);
    std::cout << "capacity=" << ring.capacity() << '\n';
}
