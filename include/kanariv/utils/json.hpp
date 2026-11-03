#pragma once

/**
 * @file json.hpp
 * @brief Opaque JSON value type -- the only JSON vocabulary in public API.
 * @ingroup kanariv_utils
 * @details `kanariv::utils::Json` is a small ordered JSON document/value with
 * value semantics. The backend (currently nlohmann::ordered_json) lives
 * behind the pimpl wall in `src/utils/json.cpp` and never appears in
 * installed headers, so downstream consumers need no JSON package.
 *
 * Error model: bounds/type violations throw `JsonError` (a
 * `std::runtime_error`); all library parse entry points already translate
 * `std::exception` to `StatusCode::Corrupt`. Non-throwing `at()`/`tryParse()`
 * return `std::expected` for new code. Insertion order of object keys is
 * preserved (ordered backend).
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace kanariv::utils {

/**
 * @brief Thrown by `Json` on bounds/type violations (missing key, index
 * out of range, key access on non-object).
 * @ingroup kanariv_utils
 */
class KANARIV_API JsonError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/** @brief Non-owning proxy returned for mutable JSON member access. */
class KANARIV_API JsonRef;

/**
 * @brief Opaque ordered JSON value with value semantics.
 * @ingroup kanariv_utils
 */
class KANARIV_API Json {
  public:
    /// @brief Constructs null. @ingroup kanariv_utils
    Json();
    /// @brief Destructor (out-of-line: pimpl). @ingroup kanariv_utils
    ~Json();
    /// @brief Deep copy. @ingroup kanariv_utils
    Json(const Json& other);
    /// @brief Deep copy-assign. @ingroup kanariv_utils
    Json& operator=(const Json& other);
    /// @brief Move (defined in .cpp: pimpl). @ingroup kanariv_utils
    Json(Json&& other) noexcept;
    /// @brief Move-assign (defined in .cpp: pimpl). @ingroup kanariv_utils
    Json& operator=(Json&& other) noexcept;
    /// @brief Replace content with text. @ingroup kanariv_utils
    Json& operator=(std::string_view v);
    /// @brief Replace content with text literal. @ingroup kanariv_utils
    Json& operator=(const char* v);
    /// @brief Replace content with boolean. @ingroup kanariv_utils
    Json& operator=(bool v);
    /// @brief Replace content with null. @ingroup kanariv_utils
    Json& operator=(std::nullptr_t);
    /// @brief Replace content with a number. @ingroup kanariv_utils
    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    Json& operator=(T v) {
        if constexpr (std::is_floating_point_v<T>) {
            assignDouble(static_cast<double>(v));
        } else if constexpr (std::is_signed_v<T>) {
            assignInt(static_cast<int64_t>(v));
        } else {
            assignUInt(static_cast<uint64_t>(v));
        }
        return *this;
    }
    /**
     * @brief Removes an object member; false when absent or not an object.
     * @ingroup kanariv_utils
     */
    bool erase(std::string_view key) noexcept;

    /** @brief Creates an empty object. @ingroup kanariv_utils */
    static Json object();
    /** @brief Creates an empty array. @ingroup kanariv_utils */
    static Json array();
    /**
     * @brief Parses a JSON document.
     * @ingroup kanariv_utils
     * @param text UTF-8 JSON text.
     * @return Parsed value.
     * @throws JsonError on syntax failure.
     */
    static Json parse(std::string_view text);
    /**
     * @brief Parses a JSON document without throwing.
     * @ingroup kanariv_utils
     * @param text UTF-8 JSON text.
     * @return Parsed value, or `Corrupt` Error on syntax failure.
     */
    static std::expected<Json, Error> tryParse(std::string_view text) noexcept;

    /**
     * @brief Serializes to text.
     * @ingroup kanariv_utils
     * @param indent -1 for compact, >=0 for pretty-printed with that indent.
     */
    std::string dump(int indent = -1) const;

    /** @brief Reports whether this value is an object. @return True for an object. */
    bool isObject() const noexcept;
    /** @brief Reports whether this value is an array. @return True for an array. */
    bool isArray() const noexcept;
    /** @brief Reports whether this value is a string. @return True for a string. */
    bool isString() const noexcept;
    /** @brief Reports whether this value is numeric. @return True for any number. */
    bool isNumber() const noexcept;
    /** @brief Reports whether this value is an integer. @return True for an integer. */
    bool isInteger() const noexcept;
    /** @brief Reports whether this value is unsigned. @return True for an unsigned integer. */
    bool isUnsigned() const noexcept;
    /** @brief Reports whether this value is boolean. @return True for a boolean. */
    bool isBoolean() const noexcept;
    /** @brief Reports whether this value is null. @return True for JSON null. */
    bool isNull() const noexcept;

    /**
     * @brief Membership test (false for non-objects, never throws).
     * @ingroup kanariv_utils
     */
    bool contains(std::string_view key) const noexcept;
    /// @brief Element count (0 for null, 1 for scalars). @ingroup kanariv_utils
    std::size_t size() const noexcept;

    /**
     * @brief Mutable element access; missing object keys insert null.
     * @ingroup kanariv_utils
     */
    JsonRef operator[](std::string_view key);
    /**
     * @brief Mutable array element access.
     * @ingroup kanariv_utils
     * @throws JsonError if not an array or index out of range.
     */
    JsonRef operator[](std::size_t index);
    /**
     * @brief Read access snapshot (deep copy).
     * @ingroup kanariv_utils
     * @throws JsonError on missing key.
     */
    Json operator[](std::string_view key) const;
    /**
     * @brief Non-throwing read access.
     * @ingroup kanariv_utils
     */
    std::expected<Json, Error> at(std::string_view key) const noexcept;
    /**
     * @brief Non-throwing array element access.
     * @ingroup kanariv_utils
     */
    std::expected<Json, Error> at(std::size_t index) const noexcept;
    /**
     * @brief Key of the object member at position `index` (for index loops).
     * @ingroup kanariv_utils
     */
    std::expected<std::string, Error> keyAt(std::size_t index) const noexcept;

    /**
     * @brief Typed read; strings, bools, integers, floats supported.
     * @ingroup kanariv_utils
     * @tparam T One of std::string, bool, int, int32_t, uint32_t, int64_t,
     * uint64_t, std::size_t, float, double.
     * @return Value (narrowing range-checked).
     * @throws JsonError on type mismatch or overflow -- all library parse
     * entry points already translate `std::exception` to `Corrupt`, matching
     * previous backend-throw behavior line-for-line.
     */
    template <typename T>
    T get() const {
        if constexpr (std::is_same_v<T, std::string>) {
            return getString();
        } else if constexpr (std::is_same_v<T, bool>) {
            return getBool();
        } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
            return static_cast<T>(getDouble());
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            int64_t v = getInt64();
            if (v < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                v > static_cast<int64_t>(std::numeric_limits<T>::max()))
                throw JsonError("json integer out of range");
            return static_cast<T>(v);
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            uint64_t v = getUInt64();
            if (v > static_cast<uint64_t>(std::numeric_limits<T>::max()))
                throw JsonError("json integer out of range");
            return static_cast<T>(v);
        } else {
            static_assert(!sizeof(T), "Json::get<T> supports string/bool/integer/float only");
        }
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class JsonRef;

    std::string getString() const;
    bool getBool() const;
    int64_t getInt64() const;
    uint64_t getUInt64() const;
    double getDouble() const;
    void assignInt(int64_t v);
    void assignUInt(uint64_t v);
    void assignDouble(double v);
    // Reference resolution for operator[]/JsonRef (defined in .cpp where the
    // backend is complete). The file-local JsonSlots reaches them through
    // friendship (JsonRef is befriended above).
    friend struct JsonSlots;
    static Impl& resolveImpl(Json* owner, std::string_view key, std::size_t index, bool byIndex);
    static const Impl& resolveConstImpl(const Json* owner, std::string_view key, std::size_t index,
                                        bool byIndex);
};

/**
 * @brief Mutable reference proxy into a `Json` value (returned by
 * non-const `operator[]`); assign scalars/values or `push_back` into arrays.
 * @ingroup kanariv_utils
 * @details Convert to `Json` (snapshot) to pass into read APIs. Do not
 * store: bound to the parent's lifetime, like a container reference.
 */
class KANARIV_API JsonRef {
  public:
    /// @brief Assign a JSON value (deep copy). @ingroup kanariv_utils
    JsonRef& operator=(const Json& v);
    /// @brief Assign text. @ingroup kanariv_utils
    JsonRef& operator=(std::string_view v);
    /// @brief Assign text literal. @ingroup kanariv_utils
    JsonRef& operator=(const char* v);
    /// @brief Assign boolean. @ingroup kanariv_utils
    JsonRef& operator=(bool v);
    /// @brief Assign null. @ingroup kanariv_utils
    JsonRef& operator=(std::nullptr_t);
    /**
     * @brief Assign a number (signed -> integer, unsigned -> unsigned,
     * floating -> double).
     * @ingroup kanariv_utils
     */
    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    JsonRef& operator=(T v) {
        return assignNumber(v);
    }
    /// @brief Appends to the referenced array. @ingroup kanariv_utils
    void push_back(const Json& v);
    /// @brief Snapshots the referenced value. @ingroup kanariv_utils
    Json snapshot() const;
    /// @brief Implicit snapshot (lets proxies bind to `const Json&` params). @ingroup kanariv_utils
    operator Json() const {
        return snapshot();
    }

  private:
    friend class Json;
    Json* owner_ = nullptr;
    std::string key_{};
    std::size_t index_ = 0;
    bool byIndex_ = false;
    JsonRef(Json* owner, std::string_view key);
    JsonRef(Json* owner, std::size_t index);

    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    JsonRef& assignNumber(T v) {
        if constexpr (std::is_floating_point_v<T>) {
            assignDouble(static_cast<double>(v));
        } else if constexpr (std::is_signed_v<T>) {
            assignInt(static_cast<int64_t>(v));
        } else {
            assignUInt(static_cast<uint64_t>(v));
        }
        return *this;
    }
    void assignInt(int64_t v);
    void assignUInt(uint64_t v);
    void assignDouble(double v);
};

} // namespace kanariv::utils
