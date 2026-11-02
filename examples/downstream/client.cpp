#include <kanariv/client/client.hpp>
#include <iostream>

int main() {
    kanariv::client::Client client("library.db");
    std::cout << "database=" << client.dbPath().string() << '\n';
}
