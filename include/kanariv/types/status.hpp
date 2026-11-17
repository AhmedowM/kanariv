#pragma once

/**
 * @file status.hpp
 * @brief Dependency-free status codes for `std::expected` error reporting.
 * @ingroup kanariv_types
 * @details `StatusCode` is shared vocabulary, so it lives in the bottom value
 * layer instead of `kanariv::utils`. This keeps `types/engine.hpp` (whose
 * callbacks carry an error code) from including `utils/status_format.hpp`, which
 * would reintroduce the historical `types` -> `utils` include cycle. The
 * enum is a plain scalar type with no `<format>` or utility dependencies.
 *
 * `kanariv::utils::StatusCode` remains a valid name through a using-declaration
 * for source compatibility; `utils/status_format.hpp` keeps the `toString` helper and
 * the `std::formatter` specialization.
 * @see kanariv::utils::Error
 */

#include <cstdint>

namespace kanariv::types {

/**
 * @brief Status codes used across kanariv for `std::expected` error reporting.
 * @ingroup kanariv_types
 * @details Mirrors the C API `ca_status` values. `Ok` (0) indicates success;
 * all other values indicate failure. Pass to `kanariv::utils::toString()` for
 * diagnostics or construct an `Error` with `makeError()`.
 * @see kanariv::utils::Error
 */
enum class StatusCode : std::uint8_t {
    Ok = 0,            ///< Success.
    InvalidArg = 1,    ///< Invalid argument supplied by caller.
    NotFound = 2,      ///< Requested entity not found.
    Unsupported = 3,   ///< Operation not supported on this platform/build.
    Io = 4,            ///< Generic I/O error.
    Device = 5,        ///< Audio device error.
    State = 6,         ///< Invalid state for operation (e.g. queue empty).
    NoMem = 7,         ///< Memory allocation failure.
    Internal = 8,      ///< Internal invariant violation.
    AlreadyExists = 9, ///< Entity already exists.
    Busy = 10,         ///< Resource busy or queue full.
    Corrupt = 11,      ///< Data corruption detected.
    NoSpace = 12,      ///< No space left (buffer/queue full).
    Timeout = 13       ///< Deadline expired before the operation completed.
};

} // namespace kanariv::types

namespace kanariv::utils {

/**
 * @brief Backward-compatible alias for `kanariv::types::StatusCode`.
 * @ingroup kanariv_utils
 * @details Existing code using `kanariv::utils::StatusCode` keeps compiling;
 * the enumeration itself is owned by the value layer.
 */
using kanariv::types::StatusCode;

} // namespace kanariv::utils
