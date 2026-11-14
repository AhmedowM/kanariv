#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace kanariv::utils::test {

namespace {

Json makeObject() {
    Json j;
    j["key"] = 7;
    return j;
}

bool json_copy_after_move_construct() {
    Json source = makeObject();
    Json moved(std::move(source));
    // The moved-from source is a valid null value, not a broken object.
    if (!source.isNull() || source.size() != 0 || source.contains("key"))
        return false;
    Json copy(source);
    if (!copy.isNull() || copy.dump() != "null")
        return false;
    Json assigned;
    assigned["other"] = true;
    source = assigned; // copy-assign into a null (moved-from) target
    const Json& view = source;
    return view["other"].get<bool>() && moved.at("key")->get<int>() == 7;
}

bool json_scalar_assign_after_move_assign() {
    Json source = makeObject();
    Json target;
    target = std::move(source);
    if (!source.isNull())
        return false;
    source = 42;
    if (!source.isNumber() || source.get<int>() != 42)
        return false;
    source = std::string("text");
    if (!source.isString() || source.get<std::string>() != "text")
        return false;
    source = true;
    if (!source.isBoolean() || !source.get<bool>())
        return false;
    source = 3.5;
    if (!source.isNumber() || source.get<double>() != 3.5)
        return false;
    source = nullptr;
    if (!source.isNull())
        return false;
    // Mutable key access through operator[] materialises storage again.
    source = std::move(target);     // source holds the object once more
    Json nulled(std::move(source)); // source is null again
    source["fresh"] = 9;            // write through the moved-from value
    auto at = source.at("fresh");
    return at.has_value() && at->get<int>() == 9 && nulled.at("key")->get<int>() == 7;
}

bool json_reads_on_moved_from() {
    Json source = makeObject();
    Json moved(std::move(source));
    if (source.contains("key") || !source.isNull() || source.isObject() || source.size() != 0)
        return false;
    if (source.dump() != "null" || source.dump(2) != "null")
        return false;
    if (source.at("key").has_value() || source.at(std::size_t{0}).has_value())
        return false;
    if (source.keyAt(0).has_value())
        return false;
    bool threw = false;
    const Json& view = source;
    try {
        (void)view["key"];
    } catch (const JsonError&) {
        threw = true; // like a missing key on an empty object, not a crash
    }
    return threw;
}

} // namespace

// The move stays noexcept: copy/assign are where storage is materialised.
static_assert(std::is_nothrow_move_constructible_v<Json>);
static_assert(std::is_nothrow_move_assignable_v<Json>);
// These helpers allocate on the failure path and must be allowed to throw.
static_assert(!noexcept(Json::tryParse(std::string_view{})));
static_assert(!noexcept(std::declval<const Json&>().at(std::string_view{})));
static_assert(!noexcept(std::declval<const Json&>().at(std::size_t{})));
static_assert(!noexcept(std::declval<const Json&>().keyAt(std::size_t{})));

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("Json copy works after move-construction", "[utils][json]") {
    REQUIRE(json_copy_after_move_construct());
}

TEST_CASE("Json scalar assignment works after move-assign", "[utils][json]") {
    REQUIRE(json_scalar_assign_after_move_assign());
}

TEST_CASE("Json moved-from reads behave as null", "[utils][json]") {
    REQUIRE(json_reads_on_moved_from());
}

TEST_CASE("Json tryParse reports Corrupt without terminating", "[utils][json]") {
    auto bad = kanariv::utils::Json::tryParse("{not json");
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().code == kanariv::utils::StatusCode::Corrupt);
    auto good = kanariv::utils::Json::tryParse(R"({"a":1})");
    REQUIRE(good.has_value());
    REQUIRE(good->at("a")->get<int>() == 1);
}
