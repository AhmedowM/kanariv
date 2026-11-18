#pragma once

/**
 * @file protocol_internal.hpp
 * @brief Internal IPC JSON helpers -- not part of the public API.
 * @ingroup kanariv_ipc
 * @details Declares the JSON serialization helpers for enums, playlists,
 * and errors used by `src/ipc/protocol_common.cpp`. Lives in `src/` (not
 * installed) so downstream consumers never see them.
 */

#include <cstddef>
#include <expected>
#include <format>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kanariv::ipc::detail {

using Json = kanariv::utils::Json;

/**
 * @brief Error for a command payload missing a field the serializer always writes.
 * @ingroup kanariv_ipc
 * @param key Wire field name.
 * @return `InvalidArg` carrying the field name.
 */
inline kanariv::utils::Error missingFieldError(std::string_view key) {
    return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                     std::format("missing required '{}'", key));
}

/**
 * @brief Error for a present field whose JSON type contradicts the schema.
 * @ingroup kanariv_ipc
 * @param key Wire field name.
 * @param expected Human-readable expected type ("a string", "an integer", ...).
 * @return `InvalidArg` naming the field and the expected type.
 */
inline kanariv::utils::Error typeMismatchError(std::string_view key, std::string_view expected) {
    return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                     std::format("field '{}' must be {}", key, expected));
}

/**
 * @brief Error for a numeric field that does not fit its C++ destination type.
 * @ingroup kanariv_ipc
 * @param key Wire field name.
 * @return `InvalidArg` naming the field.
 */
inline kanariv::utils::Error outOfRangeError(std::string_view key) {
    return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                     std::format("field '{}' is out of range", key));
}

/**
 * @brief Reads a mandatory string field.
 * @ingroup kanariv_ipc
 * @details Strictness policy for command payloads: a field the serializer
 * always writes is mandatory; a field the serializer emits as `null` (or
 * omits) when unset stays optional; a present field of the wrong type is
 * always `InvalidArg`, never a silent default.
 */
inline kanariv::utils::Expected<void> requiredString(const Json& j, std::string_view key,
                                                     std::string& out) {
    if (!j.contains(key))
        return std::unexpected{missingFieldError(key)};
    const Json v = j[key];
    if (!v.isString())
        return std::unexpected{typeMismatchError(key, "a string")};
    out = v.get<std::string>();
    return {};
}

/** @brief Reads an optional string field; absent or `null` keeps `out` unset. */
inline kanariv::utils::Expected<void> optionalString(const Json& j, std::string_view key,
                                                     std::optional<std::string>& out) {
    if (!j.contains(key))
        return {};
    const Json v = j[key];
    if (v.isNull())
        return {};
    if (!v.isString())
        return std::unexpected{typeMismatchError(key, "a string or null")};
    out = v.get<std::string>();
    return {};
}

/** @brief Reads a mandatory boolean field. */
inline kanariv::utils::Expected<void> requiredBool(const Json& j, std::string_view key, bool& out) {
    if (!j.contains(key))
        return std::unexpected{missingFieldError(key)};
    const Json v = j[key];
    if (!v.isBoolean())
        return std::unexpected{typeMismatchError(key, "a boolean")};
    out = v.get<bool>();
    return {};
}

/** @brief Reads an optional boolean field; absent or `null` keeps `out` unset. */
inline kanariv::utils::Expected<void> optionalBool(const Json& j, std::string_view key,
                                                   std::optional<bool>& out) {
    if (!j.contains(key))
        return {};
    const Json v = j[key];
    if (v.isNull())
        return {};
    if (!v.isBoolean())
        return std::unexpected{typeMismatchError(key, "a boolean or null")};
    out = v.get<bool>();
    return {};
}

/**
 * @brief Converts an already type-checked JSON number into `T`.
 * @ingroup kanariv_ipc
 * @details The JSON backend reports unsigned integers as integers too and
 * its signed conversion wraps past `int64_t::max` instead of throwing, so the
 * unsigned magnitude is checked here before the conversion.
 */
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
kanariv::utils::Expected<void> readNumber(const Json& v, std::string_view key, T& out) {
    try {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            if (v.isUnsigned() &&
                v.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return std::unexpected{outOfRangeError(key)};
        }
        out = v.get<T>();
    } catch (const kanariv::utils::JsonError&) {
        return std::unexpected{outOfRangeError(key)};
    }
    return {};
}

/**
 * @brief Reads a mandatory numeric field, range-checked into `T`.
 * @ingroup kanariv_ipc
 * @tparam T Arithmetic destination type other than bool.
 * @details Integral destinations accept only JSON integers; a fractional
 * number for an id or position is a client bug, not something to truncate.
 */
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
kanariv::utils::Expected<void> requiredNumber(const Json& j, std::string_view key, T& out) {
    if (!j.contains(key))
        return std::unexpected{missingFieldError(key)};
    const Json v = j[key];
    if constexpr (std::is_floating_point_v<T>) {
        if (!v.isNumber())
            return std::unexpected{typeMismatchError(key, "a number")};
    } else {
        if (!v.isInteger() && !v.isUnsigned())
            return std::unexpected{typeMismatchError(key, "an integer")};
    }
    return readNumber(v, key, out);
}

/** @brief Reads an optional numeric field; absent or `null` keeps `out` unset. */
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
kanariv::utils::Expected<void> optionalNumber(const Json& j, std::string_view key,
                                              std::optional<T>& out) {
    if (!j.contains(key))
        return {};
    const Json v = j[key];
    if (v.isNull())
        return {};
    if constexpr (std::is_floating_point_v<T>) {
        if (!v.isNumber())
            return std::unexpected{typeMismatchError(key, "a number or null")};
    } else {
        if (!v.isInteger() && !v.isUnsigned())
            return std::unexpected{typeMismatchError(key, "an integer or null")};
    }
    T value{};
    if (auto r = readNumber(v, key, value); !r)
        return r;
    out = value;
    return {};
}

/**
 * @brief Reads a mandatory array of strings, rejecting non-string entries.
 * @ingroup kanariv_ipc
 * @details The previous parser dropped non-string items, so a malformed
 * `paths` list played a subset of what the client sent.
 */
inline kanariv::utils::Expected<void> requiredStringArray(const Json& j, std::string_view key,
                                                          std::vector<std::string>& out) {
    if (!j.contains(key))
        return std::unexpected{missingFieldError(key)};
    const Json v = j[key];
    if (!v.isArray())
        return std::unexpected{typeMismatchError(key, "an array of strings")};
    for (std::size_t i = 0; i < v.size(); ++i) {
        auto item = v.at(i);
        if (!item)
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::InvalidArg,
                std::format("field '{}' item {} could not be read", key, i))};
        if (!item->isString())
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::InvalidArg,
                std::format("field '{}' item {} must be a string", key, i))};
        out.push_back(item->get<std::string>());
    }
    return {};
}

/**
 * @brief Convert StatusCode enum to string representation.
 * @ingroup kanariv_ipc
 * @param r Status code to convert.
 * @return String representation (e.g., "Ok", "InvalidArg", "NotFound").
 */
std::string resultCodeToString(kanariv::utils::StatusCode r);

/**
 * @brief Convert string to StatusCode enum.
 * @ingroup kanariv_ipc
 * @param sv String to parse.
 * @return StatusCode enum value, or error if unknown.
 */
kanariv::utils::Expected<kanariv::utils::StatusCode> resultCodeFromString(std::string_view sv);

/**
 * @brief Convert Playlist struct to JSON object.
 * @ingroup kanariv_ipc
 * @param p Playlist to convert.
 * @return JSON object with all playlist fields.
 */
Json playlistToJson(const kanariv::types::Playlist& p);

/**
 * @brief Convert JSON object to Playlist struct.
 * @ingroup kanariv_ipc
 * @param j JSON object with playlist fields.
 * @return Playlist struct, or error if parsing fails.
 */
kanariv::utils::Expected<kanariv::types::Playlist> playlistFromJson(const Json& j);

/**
 * @brief Convert Error struct to JSON object.
 * @ingroup kanariv_ipc
 * @param e Error to convert.
 * @return JSON object with type, code, code_value, and message fields.
 */
Json errorToJson(const kanariv::utils::Error& e);

/**
 * @brief Convert JSON object to Error struct.
 * @ingroup kanariv_ipc
 * @param j JSON object with code/code_value and message fields.
 * @return Error struct, or error if parsing fails.
 */
kanariv::utils::Expected<kanariv::utils::Error> errorFromJson(const Json& j);

} // namespace kanariv::ipc::detail
