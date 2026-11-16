#pragma once

/**
 * @file policy.hpp
 * @brief Per-command transport deadlines for clients of the IPC protocol.
 * @ingroup kanariv_ipc
 * @details One vocabulary replaces the timeout literals that used to sit at
 * call sites (client default, app overrides). The class is attached to each
 * command's schema row, so adding a command cannot forget it: the row must
 * name a policy.
 */

#include <chrono>
#include <cstdint>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command/variant.hpp>

namespace kanariv::ipc {

/**
 * @enum TransportPolicy
 * @brief How long a client may wait for one command.
 * @ingroup kanariv_ipc
 */
enum class TransportPolicy : std::uint8_t {
    Interactive, ///< User-facing commands answered promptly.
    Batch,       ///< One request carrying bulk payloads (files, tracks, tags).
    LongRunning  ///< Tree walks and bulk maintenance that outlive a batch.
};

/// Deadline for TransportPolicy::Interactive: 2 s.
inline constexpr std::chrono::milliseconds interactiveDeadline{2000};

/// Deadline for TransportPolicy::Batch: 5 min.
inline constexpr std::chrono::milliseconds batchDeadline{5 * 60 * 1000};

/// Deadline for TransportPolicy::LongRunning: 30 min.
inline constexpr std::chrono::milliseconds longRunningDeadline{30 * 60 * 1000};

/**
 * @brief Deadline for a policy class.
 * @ingroup kanariv_ipc
 */
[[nodiscard]] constexpr std::chrono::milliseconds
transportDeadline(TransportPolicy policy) noexcept {
    switch (policy) {
    case TransportPolicy::Interactive:
        return interactiveDeadline;
    case TransportPolicy::Batch:
        return batchDeadline;
    case TransportPolicy::LongRunning:
        return longRunningDeadline;
    }
    return interactiveDeadline;
}

/**
 * @brief Policy attached to the command's schema row.
 * @ingroup kanariv_ipc
 * @details Commands not named in the table are Interactive. The table lives
 * with the wire schema, so the classification cannot drift from the command
 * set.
 */
[[nodiscard]] KANARIV_API TransportPolicy transportPolicyFor(const Command& cmd) noexcept;

} // namespace kanariv::ipc
