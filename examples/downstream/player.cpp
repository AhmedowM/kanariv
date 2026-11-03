#include <iostream>
#include <kanariv/player/player.hpp>
#include <kanariv/utils/error.hpp>

int main() {
    auto result = kanariv::player::Player::create();
    if (!result)
        return 1;
    std::cout << "player created\n";
}
