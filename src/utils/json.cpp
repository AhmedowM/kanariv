#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iterator>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/result.hpp>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "nlohmann/json_fwd.hpp"

namespace kanariv::utils {

struct Json::Impl {
    nlohmann::ordered_json v = nlohmann::ordered_json(nullptr);
};

namespace {

[[noreturn]] void throwJson(const std::string& what) {
    throw JsonError(what);
}

} // namespace

auto Json::resolveImpl(Json* owner, [[maybe_unused]] std::string_view key, std::size_t index,
                       bool byIndex) -> Json::Impl& {
    if (owner == nullptr || owner->impl_ == nullptr) {
        throwJson("json: null reference");
    }
    auto& impl = *owner->impl_;
    auto& v = impl.v;
    if (byIndex) {
        if (!v.is_array() || index >= v.size()) {
            throwJson("json: array index out of range");
        }
        return impl;
    }
    if (v.is_null()) {
        v = nlohmann::ordered_json::object();
    }
    if (!v.is_object()) {
        throwJson("json: key access on non-object");
    }
    return impl;
}

auto Json::resolveConstImpl(const Json* owner, std::string_view key, std::size_t index,
                            bool byIndex) -> const Json::Impl& {
    if (owner == nullptr || owner->impl_ == nullptr) {
        throwJson("json: null reference");
    }
    const auto& impl = *owner->impl_;
    const auto& v = impl.v;
    if (byIndex) {
        if (!v.is_array() || index >= v.size()) {
            throwJson("json: array index out of range");
        }
        return impl;
    }
    if (!v.is_object() || !v.contains(std::string(key))) {
        throwJson("json: missing key");
    }
    return impl;
}

// Slot selection after resolveImpl/resolveConstImpl validated shape.
// A befriended file-local struct so no backend type appears in any header
// declaration; Json/JsonRef members call through it.
struct JsonSlots {
    static auto mut(Json::Impl& impl, std::string_view key, std::size_t index, bool byIndex)
        -> nlohmann::ordered_json& {
        auto& v = impl.v;
        if (byIndex) {
            return v.at(index);
        }
        return v[std::string(key)]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    static auto get(const Json::Impl& impl, std::string_view key, std::size_t index, bool byIndex)
        -> const nlohmann::ordered_json& {
        const auto& v = impl.v;
        if (byIndex) {
            return v.at(index);
        }
        return v.at(std::string(key));
    }
};

Json::Json() : impl_(std::make_unique<Impl>()) {}
Json::~Json() = default;
Json::Json(const Json& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
auto Json::operator=(const Json& other) -> Json& {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}
Json::Json(Json&& other) noexcept = default;
auto Json::operator=(Json&& other) noexcept -> Json& = default;

auto Json::operator=(std::string_view v) -> Json& {
    impl_->v = std::string(v);
    return *this;
}

auto Json::operator=(const char* v) -> Json& {
    impl_->v = v != nullptr ? std::string(v) : std::string();
    return *this;
}

auto Json::operator=(bool v) -> Json& {
    impl_->v = v;
    return *this;
}

auto Json::operator=(std::nullptr_t) -> Json& {
    impl_->v = nullptr;
    return *this;
}

void Json::assignInt(int64_t v) {
    impl_->v = v;
}

void Json::assignUInt(uint64_t v) {
    impl_->v = v;
}

void Json::assignDouble(double v) {
    impl_->v = v;
}

auto Json::erase(std::string_view key) noexcept -> bool {
    if (!impl_ || !impl_->v.is_object()) {
        return false;
    }
    return impl_->v.erase(std::string(key)) > 0;
}

auto Json::object() -> Json {
    Json j;
    j.impl_->v = nlohmann::ordered_json::object();
    return j;
}

auto Json::array() -> Json {
    Json j;
    j.impl_->v = nlohmann::ordered_json::array();
    return j;
}

auto Json::parse(std::string_view text) -> Json {
    try {
        Json j;
        j.impl_->v = nlohmann::ordered_json::parse(text.begin(), text.end());
        return j;
    } catch (const nlohmann::json::exception& e) {
        throwJson(std::string("json parse: ") + e.what());
    }
}

auto Json::tryParse(std::string_view text) noexcept -> std::expected<Json, Error> {
    try {
        return parse(text);
    } catch (const JsonError& e) {
        return std::unexpected{makeError(StatusCode::Corrupt, e.what())};
    } catch (const std::exception& e) {
        return std::unexpected{makeError(StatusCode::Corrupt, e.what())};
    }
}

auto Json::dump(int indent) const -> std::string {
    if (!impl_) {
        return "null";
    }
    if (indent < 0) {
        return impl_->v.dump();
    }
    return impl_->v.dump(indent);
}

auto Json::isObject() const noexcept -> bool {
    return impl_ && impl_->v.is_object();
}
auto Json::isArray() const noexcept -> bool {
    return impl_ && impl_->v.is_array();
}
auto Json::isString() const noexcept -> bool {
    return impl_ && impl_->v.is_string();
}
auto Json::isNumber() const noexcept -> bool {
    return impl_ && impl_->v.is_number();
}
auto Json::isInteger() const noexcept -> bool {
    return impl_ && impl_->v.is_number_integer();
}
auto Json::isUnsigned() const noexcept -> bool {
    return impl_ && impl_->v.is_number_unsigned();
}
auto Json::isBoolean() const noexcept -> bool {
    return impl_ && impl_->v.is_boolean();
}
auto Json::isNull() const noexcept -> bool {
    return !impl_ || impl_->v.is_null();
}

auto Json::contains(std::string_view key) const noexcept -> bool {
    if (!impl_ || !impl_->v.is_object()) {
        return false;
    }
    return impl_->v.contains(std::string(key));
}

auto Json::size() const noexcept -> std::size_t {
    if (!impl_) {
        return 0;
    }
    return impl_->v.size();
}

auto Json::operator[](std::string_view key) -> JsonRef {
    return JsonRef(this, key);
}

auto Json::operator[](std::size_t index) -> JsonRef {
    return JsonRef(this, index);
}

auto Json::operator[](std::string_view key) const -> Json {
    Json out;
    out.impl_->v = JsonSlots::get(resolveConstImpl(this, key, 0, false), key, 0, false);
    return out;
}

auto Json::at(std::string_view key) const noexcept -> std::expected<Json, Error> {
    try {
        Json out;
        out.impl_->v = JsonSlots::get(resolveConstImpl(this, key, 0, false), key, 0, false);
        return out;
    } catch (const std::exception& e) {
        return std::unexpected{makeError(StatusCode::Corrupt, e.what())};
    }
}

auto Json::at(std::size_t index) const noexcept -> std::expected<Json, Error> {
    try {
        Json out;
        out.impl_->v = JsonSlots::get(resolveConstImpl(this, {}, index, true), {}, index, true);
        return out;
    } catch (const std::exception& e) {
        return std::unexpected{makeError(StatusCode::Corrupt, e.what())};
    }
}

auto Json::keyAt(std::size_t index) const noexcept -> std::expected<std::string, Error> {
    try {
        if (!impl_ || !impl_->v.is_object() || index >= impl_->v.size()) {
            throwJson("json: key index out of range");
        }
        auto it = impl_->v.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        return it.key();
    } catch (const std::exception& e) {
        return std::unexpected{makeError(StatusCode::Corrupt, e.what())};
    }
}

auto Json::getString() const -> std::string {
    if (!impl_ || !impl_->v.is_string()) {
        throwJson("json: not a string");
    }
    return impl_->v.get<std::string>();
}

auto Json::getBool() const -> bool {
    if (!impl_ || !impl_->v.is_boolean()) {
        throwJson("json: not a boolean");
    }
    return impl_->v.get<bool>();
}

auto Json::getInt64() const -> int64_t {
    if (!impl_) {
        throwJson("json: null value");
    }
    const auto& v = impl_->v;
    if (v.is_number_integer()) {
        return v.get<int64_t>();
    }
    if (v.is_number_unsigned()) {
        auto u = v.get<uint64_t>();
        if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throwJson("json: unsigned out of int64 range");
        }
        return static_cast<int64_t>(u);
    }
    if (v.is_number_float()) {
        return static_cast<int64_t>(v.get<double>());
    }
    throwJson("json: not a number");
}

auto Json::getUInt64() const -> uint64_t {
    if (!impl_) {
        throwJson("json: null value");
    }
    const auto& v = impl_->v;
    if (v.is_number_unsigned()) {
        return v.get<uint64_t>();
    }
    if (v.is_number_integer()) {
        auto s = v.get<int64_t>();
        if (s < 0) {
            throwJson("json: negative out of uint64 range");
        }
        return static_cast<uint64_t>(s);
    }
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (d < 0) {
            throwJson("json: negative out of uint64 range");
        }
        return static_cast<uint64_t>(d);
    }
    throwJson("json: not a number");
}

auto Json::getDouble() const -> double {
    if (!impl_) {
        throwJson("json: null value");
    }
    const auto& v = impl_->v;
    if (v.is_number_float()) {
        return v.get<double>();
    }
    if (v.is_number_integer()) {
        return static_cast<double>(v.get<int64_t>());
    }
    if (v.is_number_unsigned()) {
        return static_cast<double>(v.get<uint64_t>());
    }
    throwJson("json: not a number");
}

JsonRef::JsonRef(Json* owner, std::string_view key) : owner_(owner), key_(key), byIndex_(false) {}
JsonRef::JsonRef(Json* owner, std::size_t index) : owner_(owner), index_(index), byIndex_(true) {}

auto JsonRef::operator=(const Json& v) -> JsonRef& {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) =
        v.impl_ ? v.impl_->v : nlohmann::ordered_json(nullptr);
    return *this;
}

auto JsonRef::operator=(std::string_view v) -> JsonRef& {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) =
        std::string(v);
    return *this;
}

auto JsonRef::operator=(const char* v) -> JsonRef& {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) =
        v != nullptr ? std::string(v) : std::string();
    return *this;
}

auto JsonRef::operator=(bool v) -> JsonRef& {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) = v;
    return *this;
}

auto JsonRef::operator=(std::nullptr_t) -> JsonRef& {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) =
        nullptr;
    return *this;
}

void JsonRef::assignInt(int64_t v) {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) = v;
}

void JsonRef::assignUInt(uint64_t v) {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) = v;
}

void JsonRef::assignDouble(double v) {
    JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_) = v;
}

void JsonRef::push_back(const Json& v) {
    if (owner_ == nullptr || owner_->impl_ == nullptr) {
        throwJson("json: null reference");
    }
    auto& dst =
        JsonSlots::mut(Json::resolveImpl(owner_, key_, index_, byIndex_), key_, index_, byIndex_);
    if (!dst.is_array()) {
        throwJson("json: push_back on non-array");
    }
    dst.push_back(v.impl_ ? v.impl_->v : nlohmann::ordered_json(nullptr));
}

auto JsonRef::snapshot() const -> Json {
    Json out;
    out.impl_->v = JsonSlots::get(Json::resolveConstImpl(owner_, key_, index_, byIndex_), key_,
                                  index_, byIndex_);
    return out;
}

} // namespace kanariv::utils
