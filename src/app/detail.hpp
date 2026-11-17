#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/result.hpp>
#include <string>

/**
 * @file detail.hpp
 * @brief App-internal shared reporting builders (NOT public API).
 * @details Text builders for batch reports; frontends own final rendering
 * and must use the public `kanariv/app/` headers.
 */
namespace kanariv::app::detail {

/**
 * @brief Total wall-clock budget for detecting a freshly spawned daemon.
 * @details Both the loop deadline and the per-attempt timeout derive from
 * this one value, so the documented budget and the code cannot drift.
 */
inline constexpr std::chrono::milliseconds daemonReadyBudget{5000};

/** @brief Upper bound for one readiness status round-trip. */
inline constexpr std::chrono::milliseconds daemonReadyAttempt{500};

/** @brief Pause between readiness attempts. */
inline constexpr std::chrono::milliseconds daemonReadyPoll{100};

/**
 * @brief Timeout for a readiness attempt started with `remaining` budget.
 * @param remaining Budget left before the readiness deadline (may be <= 0).
 * @return min(daemonReadyAttempt, remaining), clamped at zero.
 */
std::chrono::milliseconds readinessAttemptTimeout(std::chrono::milliseconds remaining);

/**
 * @brief Appends one line to a blob (newline-joined, no trailing newline).
 */
void emitLine(std::string& blob, const std::string& line);

/**
 * @brief Appends formatter-style block text, preserving interior blanks.
 */
void appendBlock(std::string& blob, const std::string& text);

/**
 * @brief Renders a result with `kanariv::client::OutputFormatter` into a blob.
 */
void renderInto(std::string& blob, const kanariv::ipc::Result& res);

/**
 * @brief Builds per-item lines for a QueueAddReport result.
 * @details Adds go to @p out, duplicates and failures to @p err.
 * @return The newly added count.
 */
int printAddReportText(const kanariv::ipc::Result& res, std::string& out, std::string& err);

/**
 * @brief Builds per-item lines for a QueueRemoveReport result.
 * @details Removals go to @p out, missing selectors and failures to @p err.
 * @return The removed count.
 */
int printRemoveReportText(const kanariv::ipc::Result& res, std::string& out, std::string& err);

/**
 * @brief Builds the "N track(s) added" total line.
 */
void countLineText(int added, std::string& out);

/**
 * @brief Renders an error into a blob.
 * @details Shared by batch loops and the shell error path through the one
 * `OutputFormatter::printError`, so both render byte-identical text.
 */
KANARIV_API void renderErrorInto(std::string& err, const kanariv::utils::Error& e);

/**
 * @brief Canonical error for an unreachable daemon, including the start hint.
 * @details Produced structurally (a failed send followed by a failed
 * liveness probe), never by matching a message. Frontends render the
 * message verbatim, so the hint text lives here instead of in a renderer
 * that would have to re-derive the cause.
 */
kanariv::utils::Error daemonDownError();

} // namespace kanariv::app::detail
