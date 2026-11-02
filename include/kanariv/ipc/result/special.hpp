#pragma once

/**
 * @file special.hpp
 * @brief Empty and error result types for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <kanariv/utils/error.hpp>
#include <variant>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Special Result Types
// ---------------------------------------------------------------------------

/**
 * @brief Empty result (success with no data) - uses std::monostate.
 * @ingroup kanariv_ipc
 */
using Empty = std::monostate;

/**
 * @brief Error type for CLI error responses.
 * @ingroup kanariv_ipc
 *
 * @see kanariv::utils::Error
 */
using CliError = kanariv::utils::Error;

} // namespace kanariv::ipc
