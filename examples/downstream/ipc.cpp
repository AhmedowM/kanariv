#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <iostream>

int main() {
    kanariv::ipc::Command command{kanariv::ipc::StatusReq{}};
    std::cout << "command variant index=" << command.index() << '\n';
}
