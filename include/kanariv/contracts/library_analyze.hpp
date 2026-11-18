#pragma once

/**
 * @file library_analyze.hpp
 * @brief Library analysis port: the explicit ReplayGain measurement pass.
 * @ingroup kanariv_contracts
 * @details Measuring a library decodes every track, which only the composition
 * root that links the media stack can do. The daemon therefore depends on this
 * narrow port and never on `kanariv::library`, exactly as it does for scanning
 * (`kanariv/contracts/library_scan.hpp`).
 *
 * @par Error behavior
 * Implementations return the store error when the track list cannot be read;
 * a track that fails to decode is counted in `failed` and does not abort the
 * pass. An implementation that is absent from the wiring (`Storage::analyzer`
 * empty) is a configuration error the service reports itself.
 */

#include <cstdint>
#include <expected>
#include <functional>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <string_view>

namespace kanariv::contracts {

/**
 * @brief Options for one analysis pass.
 * @ingroup kanariv_contracts
 */
struct LibraryAnalyzeOptions {
    /// Re-measure tracks that already carry gains; default measures only the
    /// missing ones.
    bool reanalyze{false};
};

/**
 * @brief Outcome counters of one analysis pass.
 * @ingroup kanariv_contracts
 * @details `0` doubles as "not analyzed" in the gain columns, so a skipped
 * track is one whose track and album gains are both nonzero.
 */
struct LibraryAnalyzeReport {
    int64_t tracks{};   ///< Tracks considered (whole library, or filtered).
    int64_t analyzed{}; ///< Tracks measured and written in this pass.
    int64_t failed{};   ///< Tracks that could not be measured or stored.
    int64_t skipped{};  ///< Tracks left alone because they already had gains.
};

/**
 * @brief Measures loudness and persists ReplayGain 2.0 gains.
 * @ingroup kanariv_contracts
 */
class ILibraryAnalyzer {
  public:
    /**
     * @brief Destroys the analyzer view.
     * @ingroup kanariv_contracts
     */
    virtual ~ILibraryAnalyzer() = default;

    /**
     * @brief Measures the tracks of one library (or all) and writes the gains.
     * @param libraryId Library identifier, or 0 for every track.
     * @param options Re-analysis switch.
     * @param progress Optional callback `progress(done, total, path)`.
     * @return Counters, or the store error when the track list cannot be read.
     * @details Takes the store lock per group write; the caller must not hold
     * it across the call.
     */
    virtual kanariv::utils::Expected<LibraryAnalyzeReport>
    analyzeLibrary(int64_t libraryId, LibraryAnalyzeOptions options = {},
                   std::function<void(int64_t, int64_t, std::string_view)> progress = {}) = 0;
};

} // namespace kanariv::contracts
