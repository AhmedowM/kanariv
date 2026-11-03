#include <iostream>
#include <kanariv/client/client.hpp>

int main() {
    kanariv::client::Client client("library.db");
    std::cout << "database=" << client.dbPath().string() << '\n';
}
