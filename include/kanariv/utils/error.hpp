#pragma once

/**
 * @file error.hpp
 * @brief Error type, Expected alias and helpers.
 * @ingroup kanariv_utils
 */

#include <kanariv/fwd.hpp>
#include <kanariv/utils/result.hpp>
#include <format>
#include <string>
#include <string_view>

namespace kanariv::utils {

/**
 * @brief Carries a StatusCode plus an optional diagnostic message.
 * @ingroup kanariv_utils
 * @details Intended as the error type for `std::expected<T, Error>`.
 * Equality is defaulted; message is owned (`std::string`) so the view
 * passed to the constructor is copied.
 *
 * An `Error` always describes a failure: the default constructor is deleted,
 * so `std::unexpected(Error{})` (an error that claims success) cannot be
 * written. `code` must not be `StatusCode::Ok`; that is asserted in debug
 * builds and never produced by the library.
 * @see StatusCode
 * @see makeError
 * @see Expected
 */
struct KANARIV_API Error {
    StatusCode code;     ///< Machine-readable failure code; never `StatusCode::Ok`.
    std::string message; ///< Human-readable context; may be empty.

    /**
     * @brief Deleted: an error cannot be success, so it must name a code.
     * @ingroup kanariv_utils
     */
    Error() = delete;

    /**
     * @brief Constructs an error with code and message view.
     * @ingroup kanariv_utils
     * @param c Failure status code (not `StatusCode::Ok`).
     * @param msg Diagnostic message view (copied).
     */
    explicit Error(StatusCode c, std::string_view msg);

    /**
     * @brief Equality comparison (compares code and message).
     * @ingroup kanariv_utils
     * @param other Error to compare.
     * @return true if code and message are equal.
     */
    bool operator==(const Error& other) const = default;
};

/**
 * @brief Helper to construct an Error.
 * @ingroup kanariv_utils
 * @param c Status code.
 * @param msg Optional diagnostic view (default empty).
 * @return Error instance with code and copied message.
 * @see Error
 */
KANARIV_API Error makeError(StatusCode c, std::string_view msg = {});

} // namespace kanariv::utils

/**
 * @brief std::formatter specialization for Error.
 * @details Formats as `"<Code>[: message]"`, e.g. `Busy: queue full`.
 */
template <>
struct std::formatter<kanariv::utils::Error> : std::formatter<std::string> {
    /** @brief Formats an error as `Code[: message]`. */
    auto format(const kanariv::utils::Error& e, auto& ctx) const {
        std::string s = std::string(kanariv::utils::toString(e.code));
        if (!e.message.empty()) {
            s += ": ";
            s += e.message;
        }
        return std::formatter<std::string>::format(s, ctx);
    }
};
