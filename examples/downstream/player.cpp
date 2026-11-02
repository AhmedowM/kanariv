#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>
#include <iostream>

int main() {
    auto result = kanariv::player::Player::create();
    if (!result) return 1;
    std::cout << "player created\n";
}
