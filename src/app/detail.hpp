#pragma once

#include <cstdint>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/result.hpp>
#include <set>
#include <string>

/**
 * @file detail.hpp
 * @brief App-internal shared reporting builders (NOT public API).
 * @details Text builders for batch reports; frontends own final rendering
 * and must use the public `kanariv/app/` headers.
 */
namespace kanariv::app::detail {

/**
 * @brief Appends one line to a blob (newline-joined, no trailing newline).
 */
void emitLine(std::string& blob, const std::string& line);

/**
 * @brief Appends formatter-style block text, preserving interior blanks.
 */
void appendBlock(std::string& blob, const std::string& text);

/**
 * @brief Renders a result with the human formatter into a blob.
 */
void renderInto(std::string& blob, const kanariv::ipc::Result& res);

/**
 * @brief Builds Added lines for a QueueTracks result, warning on ids
 * already seen (pre-existing queue members).
 * @return The newly added count.
 */
int printAddedText(const kanariv::ipc::Result& res, std::set<int64_t>& seen, std::string& out,
                   std::string& err);

/**
 * @brief Builds the "N track(s) added" total line.
 */
void countLineText(int added, std::string& out);

/**
 * @brief Renders an error plus the start hint into a blob.
 * @details Single implementation shared by batch loops and the shell
 * error path, so both render byte-identical text.
 */
KANARIV_API void renderErrorInto(std::string& err, const kanariv::utils::Error& e);

} // namespace kanariv::app::detail
