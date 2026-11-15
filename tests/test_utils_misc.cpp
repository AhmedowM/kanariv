#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <kanariv/utils/generator.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/parse.hpp>
#include <kanariv/utils/print.hpp>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace kanariv::utils::test {

namespace {

Generator<int> yield123() {
    co_yield 1;
    co_yield 2;
    co_yield 3;
}

bool generator_move_makes_source_empty() {
    Generator<int> source = yield123();
    Generator<int> moved(std::move(source));
    std::vector<int> values;
    for (int v : moved)
        values.push_back(v);
    if (values != std::vector<int>{1, 2, 3})
        return false;
    // A moved-from handle must iterate as an empty range, not call done() on null.
    int count = 0;
    for ([[maybe_unused]] int v : source)
        ++count;
    return count == 0;
}

bool math_format_clock_guards() {
    if (formatClock(0.0) != "00:00")
        return false;
    if (formatClock(65.0) != "01:05")
        return false;
    if (formatClock(3661.0) != "01:01:01")
        return false;
    if (formatClock(-5.0) != "00:00")
        return false;
    if (formatClock(std::numeric_limits<double>::quiet_NaN()) != "00:00")
        return false;
    if (formatClock(std::numeric_limits<double>::infinity()) != "00:00")
        return false;
    if (formatClock(-std::numeric_limits<double>::infinity()) != "00:00")
        return false;
    // Finite but far beyond 32-bit long range: clamped to 1e9 seconds.
    if (formatClock(1e18) != "277777:46:40")
        return false;
    if (formatClock(1e300) != "277777:46:40")
        return false;
    return true;
}

std::size_t readBack(std::FILE* f, std::array<char, 8>& buf) {
    std::fflush(f);
    std::rewind(f);
    return std::fread(buf.data(), 1, buf.size(), f);
}

bool print_file_preserves_embedded_nul() {
    std::FILE* f = std::tmpfile();
    if (!f)
        return false;
    const std::string payload("a\0b", 3);
    kanariv::print(f, "{}", payload);
    std::array<char, 8> buf{};
    const std::size_t n = readBack(f, buf);
    std::fclose(f);
    return n == payload.size() && buf[0] == 'a' && buf[1] == '\0' && buf[2] == 'b';
}

bool println_file_preserves_embedded_nul() {
    std::FILE* f = std::tmpfile();
    if (!f)
        return false;
    const std::string payload("a\0b", 3);
    kanariv::println(f, "{}", payload);
    std::array<char, 8> buf{};
    const std::size_t n = readBack(f, buf);
    std::fclose(f);
    return n == payload.size() + 1 && buf[0] == 'a' && buf[1] == '\0' && buf[2] == 'b' &&
           buf[3] == '\n';
}

} // namespace

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("Generator moved-from handle is an empty range", "[utils][generator]") {
    REQUIRE(generator_move_makes_source_empty());
}

TEST_CASE("formatClock guards non-finite and out-of-range input", "[utils][math]") {
    REQUIRE(math_format_clock_guards());
}

TEST_CASE("print(FILE*) writes embedded NUL bytes", "[utils][print]") {
    REQUIRE(print_file_preserves_embedded_nul());
}

TEST_CASE("println(FILE*) writes embedded NUL bytes", "[utils][print]") {
    REQUIRE(println_file_preserves_embedded_nul());
}

TEST_CASE("parseId accepts only non-negative decimal ids", "[utils][parse]") {
    using kanariv::utils::parseId;
    REQUIRE(parseId("0") == 0);
    REQUIRE(parseId("42") == 42);
    REQUIRE(parseId("9223372036854775807") == std::numeric_limits<std::int64_t>::max());
    // One rule for every boundary: signs, whitespace, trailing garbage and
    // overflow are all rejected rather than partially parsed.
    CHECK_FALSE(parseId("").has_value());
    CHECK_FALSE(parseId("+5").has_value());
    CHECK_FALSE(parseId("-1").has_value());
    CHECK_FALSE(parseId(" 5").has_value());
    CHECK_FALSE(parseId("5 ").has_value());
    CHECK_FALSE(parseId("5x").has_value());
    CHECK_FALSE(parseId("0x10").has_value());
    CHECK_FALSE(parseId("9223372036854775808").has_value());
}
