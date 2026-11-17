#pragma once

/**
 * @file render.hpp
 * @brief Outcome rendering shared by every CLI domain (internal).
 * @details One place decides how an App outcome becomes bytes and an exit
 * status: errors (with exit-code mapping), JSON vs human lines, bare results,
 * batch flushes and the version block. Domain files stay dispatch-only.
 * Human/JSON rendering of a `Result` itself is `kanariv::client::OutputFormatter`
 * (shared with the app's batch reports), re-exported here so the CLI has one
 * formatting header.
 */

#include <filesystem>
#include <kanariv/app/app.hpp>
#include <kanariv/client/output_formatter.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <string>
#include <string_view>

namespace kanariv::app::cli::detail {

/**
 * @brief Prints `Error: <Code>[: message]` on stderr and maps the exit code.
 * @param e Error to render.
 * @return Process status from the exit-code table.
 */
int renderError(const kanariv::utils::Error& e);

/** @brief Prints a result as JSON on stdout (caller decides the status). */
void renderJson(const kanariv::ipc::Result& r);

/**
 * @brief Terminal/ANSI detection for renderers (NO_COLOR honored).
 * @return True when colour and escape sequences may be emitted.
 */
bool useColor();

/**
 * @brief Renders a single outcome: error, failed line, JSON, human line,
 * bare result, or silent success, in that order.
 * @param res Outcome to render.
 * @param asJson Render the carried result as JSON.
 * @param columns Optional column allowlist for the bare result.
 * @return Process status.
 */
int render(kanariv::app::AppResult res, bool asJson, std::string_view columns = {});

/**
 * @brief Flushes a batch report: stderr blob, JSON payload, stdout blob, status.
 * @param res Batch outcome to render.
 * @param asJson Render the carried result as JSON.
 * @return The batch's recorded exit code.
 */
int renderBatch(kanariv::app::BatchResult res, bool asJson);

/** @brief Prints version plus build provenance (human block or flat JSON). */
int renderVersion(bool asJson);

/** @brief Human one-line label for a playback status. */
std::string statusLine(const kanariv::ipc::Status& st);

/**
 * @brief Persists first-run defaults at scope exit for mutating commands.
 * @details A config that did not exist before this invocation is written
 * after a mutating command (or a daemon start) so the next run sees the same
 * defaults. Read-only commands stay read-only. A failed write is a warning,
 * not a command failure: the command already ran, and a read-only config
 * location must not turn `queue add` into an error.
 */
class PersistDefaultsGuard {
  public:
    PersistDefaultsGuard(const kanariv::config::Config& cfg, bool enabled)
        : cfg_(cfg), enabled_(enabled) {}
    PersistDefaultsGuard(const PersistDefaultsGuard&) = delete;
    PersistDefaultsGuard& operator=(const PersistDefaultsGuard&) = delete;
    ~PersistDefaultsGuard();

  private:
    const kanariv::config::Config& cfg_;
    bool enabled_;
};

} // namespace kanariv::app::cli::detail
