#pragma once

/**
 * @file analyze.hpp
 * @brief Explicit ReplayGain 2.0 analysis pass over a library's tracks.
 * @ingroup kanariv_library
 * @details Decoding a whole library is a deliberate, expensive operation, so it
 * is never part of a scan: `analysis` (this pass) measures loudness and writes
 * `tracks.replay_gain_track` / `tracks.replay_gain_album`, and playback only
 * reads those columns. Nothing writes them implicitly.
 *
 * A track's gain is `kanariv::media::analyzeLoudness` (EBU R128 integrated
 * loudness against the -18 LUFS ReplayGain 2.0 reference, limited by true
 * peak). Album gain is measured, not averaged: the pass concatenates the
 * per-track 400 ms block energies of every track in the group and integrates
 * once, limited by the album's highest true peak. Tracks are grouped by
 * `(album_artist, album)`, falling back to `artist` when `album_artist` is
 * empty; a track with no album tag forms a group of its own, so its album gain
 * equals its track gain.
 */

#include <cstdint>
#include <functional>
#include <kanariv/contracts/library_analyze.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <string_view>

namespace kanariv::db {
class Database;
}

namespace kanariv::library {

/**
 * @brief Options for `analyzeLibrary`.
 * @ingroup kanariv_library
 */
struct AnalyzeOptions {
    /// Re-measure tracks that already carry a gain. Default measures only the
    /// missing ones.
    bool reanalyze{false};
};

/**
 * @brief Outcome counters of one `analyzeLibrary` call.
 * @ingroup kanariv_library
 */
struct AnalyzeReport {
    int64_t tracks{};   ///< Tracks considered (whole library, or filtered).
    int64_t analyzed{}; ///< Tracks measured and written in this pass.
    int64_t failed{};   ///< Tracks that could not be measured or stored (logged).
    int64_t skipped{};  ///< Tracks left alone because they already had gains.
};

/**
 * @brief Measures loudness and persists ReplayGain 2.0 gains.
 * @ingroup kanariv_library
 * @param db Open database to read tracks from and write gains to.
 * @param libraryId Restrict to one library, or 0 for every track.
 * @param options Pass `reanalyze` to re-measure tracks that already have gains.
 * @param progress Optional `progress(done, total, path)` after each track; the
 * total is the number of tracks the pass will touch (analyzed + failed).
 * @return Counters, or the store error when the track list cannot be read.
 * @details A track is skipped when both gain columns are nonzero and
 * `reanalyze` is false; a legitimate exactly-0.0 dB gain is re-measured on the
 * next pass, which is why 0 doubles as "unanalyzed". A file that cannot be
 * decoded is counted in `failed`, logged, and leaves its columns untouched; it
 * does not abort the pass. Album gain is written only when every track of the
 * group was measured successfully, so a later pass completes the group instead
 * of persisting a gain computed from part of an album.
 * @par Thread safety
 * Takes the database lock per group write. The caller must not hold it.
 */
KANARIV_API std::expected<AnalyzeReport, kanariv::utils::Error>
analyzeLibrary(kanariv::db::Database& db, int64_t libraryId = 0, AnalyzeOptions options = {},
               std::function<void(int64_t, int64_t, std::string_view)> progress = {});

/**
 * @brief Analyzer over one open database, exposed through the daemon's port.
 * @ingroup kanariv_library
 * @details Implements `contracts::ILibraryAnalyzer` so the service can trigger
 * a pass without linking this component. The adapter keeps the database alive
 * for as long as it lives.
 * @par Thread safety
 * Thread-safe: the pass takes the database lock per group write. The caller
 * must not hold the database lock across the call.
 */
class KANARIV_API Analyzer final : public contracts::ILibraryAnalyzer {
  public:
    /**
     * @brief Creates an analyzer over an open database.
     * @param db Database to read tracks from and write gains to; must not be null.
     */
    explicit Analyzer(std::shared_ptr<kanariv::db::Database> db);

    /**
     * @brief Measures one library's tracks (or all) and writes the gains.
     * @param libraryId Library identifier, or 0 for every track.
     * @param options Re-analysis switch.
     * @param progress Optional callback `progress(done, total, path)`.
     * @return Counters, or the store error when the track list cannot be read.
     */
    std::expected<contracts::LibraryAnalyzeReport, kanariv::utils::Error>
    analyzeLibrary(int64_t libraryId, contracts::LibraryAnalyzeOptions options = {},
                   std::function<void(int64_t, int64_t, std::string_view)> progress = {}) override;

  private:
    std::shared_ptr<kanariv::db::Database> db_;
};

} // namespace kanariv::library
