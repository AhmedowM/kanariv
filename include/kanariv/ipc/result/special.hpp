#pragma once

/**
 * @file special.hpp
 * @brief Empty result type for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 *
 * @details A command failure is never a `Result`: `IpcReply.result` is the
 * single error channel (`kanariv::utils::Expected<Result>`), so there is no
 * `Error` alternative in the variant.
 */

#include <kanariv/fwd.hpp>
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

} // namespace kanariv::ipc
