#pragma once

/**
 * @file output_formatter.hpp
 * @brief Output formatting for CLI results: table and JSON output.
 * @ingroup caudio_app
 */

#include <caudio/ipc/result.hpp>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace caudio::app::cli {

/**
 * @brief Formats CLI results for human-readable table output or JSON.
 * @ingroup caudio_client
 *
 * Provides print() for formatted output and printWithStatus() for exit code handling.
 * Supports all Result variant types: Status, QueueTracks, VolumeInfo, LibraryStats, etc.
 * JSON mode uses caudio::utils::Json pretty-printing (2-space indent).
 */
class OutputFormatter {
    bool json_{false};
    bool color_{false};
    int64_t highlightTrackId_{0};
    std::string highlightNeedle_;
    std::set<std::string> columns_;  ///< If non-empty, only these track columns are printed.

    /** @brief mm:ss rendering for durations. */
    static std::string formatTime(double secs);
    /** @brief Ellipsizes overlong table cells. */
    static std::string truncateField(const std::string& s, std::size_t maxLen = 40);
    /** @brief Shared track-detail block for SingleTrack/TrackInfo. */
    static void printTrackDetail(std::ostream& os, const caudio::types::Track& t);

  public:
    /**
     * @brief Construct formatter.
     * @param json If true, output JSON; otherwise formatted table/text.
     * @param color If true, highlight the marked row with ANSI color (TTY only).
     */
    explicit OutputFormatter(bool json = false, bool color = false) : json_(json), color_(color) {}

    /**
     * @brief Mark one track id in QueueTracks tables (current track).
     * @param id Track id to prefix with `>`; 0 disables.
     */
    void setHighlightTrackId(int64_t id) {
        highlightTrackId_ = id;
    }

    /**
     * @brief Highlight a query substring in SearchResults cells.
     * @param needle Case-insensitive substring; empty disables.
     */
    void setHighlightNeedle(std::string needle) {
        highlightNeedle_ = std::move(needle);
    }

    /**
     * @brief Print a Result to an output stream.
     * @param r Result variant to format.
     * @param os Output stream (stdout/stderr).
     *
     * In JSON mode: pretty-prints entire Result as JSON.
     * In table mode: dispatches to type-specific formatter for each Result alternative.
     */
    void print(const caudio::ipc::Result& r, std::ostream& os) const;

    /**
     * @brief Restrict track-table output to an allowlist of columns.
     * @ingroup caudio_client
     * @param columns Comma-separated column names (id, title, artist, album,
     * genre, year, rating, duration, path). Empty clears the filter (all
     * columns). Unknown names are ignored; never a free format string.
     */
    void setColumns(std::string_view columns);

    /**
     * @brief Print Result with automatic stderr/stdout routing and exit code.
     * @param r Result to print.
     * @param out Output stream for success (default stdout).
     * @param err Output stream for errors (default stderr).
     * @return 0 on success, 1 if Result holds Error variant.
     *
     * Routes Error variants to err stream, others to out stream.
     * Suitable for CLI main() to return appropriate exit code.
     */
    int printWithStatus(const caudio::ipc::Result& r, std::ostream& out, std::ostream& err) const;
};

} // namespace caudio::app::cli
