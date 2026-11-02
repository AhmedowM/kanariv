#include <catch2/catch_test_macros.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
namespace kanariv::utils::test {

bool result_toString_all() {
    if (std::string(toString(StatusCode::Ok)).compare("Ok") != 0)
        return false;
    if (std::string(toString(StatusCode::InvalidArg)).compare("InvalidArg") != 0)
        return false;
    if (std::string(toString(StatusCode::NotFound)).compare("NotFound") != 0)
        return false;
    if (std::string(toString(StatusCode::Unsupported)).compare("Unsupported") != 0)
        return false;
    if (std::string(toString(StatusCode::Io)).compare("Io") != 0)
        return false;
    if (std::string(toString(StatusCode::Device)).compare("Device") != 0)
        return false;
    if (std::string(toString(StatusCode::State)).compare("State") != 0)
        return false;
    if (std::string(toString(StatusCode::NoMem)).compare("NoMem") != 0)
        return false;
    if (std::string(toString(StatusCode::Internal)).compare("Internal") != 0)
        return false;
    if (std::string(toString(StatusCode::AlreadyExists)).compare("AlreadyExists") != 0)
        return false;
    if (std::string(toString(StatusCode::Busy)).compare("Busy") != 0)
        return false;
    if (std::string(toString(StatusCode::Corrupt)).compare("Corrupt") != 0)
        return false;
    if (std::string(toString(StatusCode::NoSpace)).compare("NoSpace") != 0)
        return false;
    if (std::string(toString(static_cast<StatusCode>(99))).compare("Unknown") != 0)
        return false;
    return true;
}

bool result_enum_sequential() {
    if (static_cast<int>(StatusCode::Ok) != 0)
        return false;
    if (static_cast<int>(StatusCode::InvalidArg) != 1)
        return false;
    if (static_cast<int>(StatusCode::NotFound) != 2)
        return false;
    if (static_cast<int>(StatusCode::Unsupported) != 3)
        return false;
    if (static_cast<int>(StatusCode::Io) != 4)
        return false;
    if (static_cast<int>(StatusCode::Device) != 5)
        return false;
    if (static_cast<int>(StatusCode::State) != 6)
        return false;
    if (static_cast<int>(StatusCode::NoMem) != 7)
        return false;
    if (static_cast<int>(StatusCode::Internal) != 8)
        return false;
    if (static_cast<int>(StatusCode::AlreadyExists) != 9)
        return false;
    if (static_cast<int>(StatusCode::Busy) != 10)
        return false;
    if (static_cast<int>(StatusCode::Corrupt) != 11)
        return false;
    if (static_cast<int>(StatusCode::NoSpace) != 12)
        return false;
    return true;
}

bool result_noexcept_check() {
    static_assert(noexcept(toString(StatusCode::Ok)));
    return noexcept(toString(StatusCode::Ok));
}

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("StatusCode toString maps all codes", "[utils][StatusCode]") {
    REQUIRE(result_toString_all());
}

TEST_CASE("StatusCode enum values sequential 0..12", "[utils][StatusCode]") {
    REQUIRE(result_enum_sequential());
}

TEST_CASE("StatusCode toString noexcept", "[utils][StatusCode]") {
    REQUIRE(result_noexcept_check());
}
