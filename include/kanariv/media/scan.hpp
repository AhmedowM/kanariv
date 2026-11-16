#pragma once

/**
 * @file scan.hpp
 * @brief Filesystem scan helpers for audio tracks (discovery, fingerprint, tags).
 * @ingroup kanariv_media
 * @details Library scanning is split at the persistence seam:
 * - `kanariv::media` owns the pure, DB-free half: filesystem walk,
 *   extension filter, BLAKE3 fingerprinting and metadata extraction,
 *   producing `kanariv::types::TrackRow` records.
 * - `kanariv::library` keeps the DB-coupled half: `kanariv::library::Scanner`
 *   (batched upsert of the discovered tracks under the database mutex).
 *
 * Every scan entry point yields only tracks that carry a real fingerprint: a
 * file that cannot be opened or hashed is skipped instead of being returned
 * with an all-zero digest. Callers that need to know *what* was skipped use
 * the issue-carrying overloads (`ScanIssueSink`); callers that need to know
 * whether the result is complete use `scanTracksToVector`, whose `Error`
 * reports a missing/unreadable root and a truncated walk.
 */

#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::media {

/**
 * @brief Scan fingerprinting mode.
 * @ingroup kanariv_media
 */
enum class ScanMode {
    Sampled, ///< Sampled BLAKE3 (head+tail+size+version) -- fast, default.
    Full     ///< Full-file BLAKE3 -- slower, more collision-resistant.
};

/**
 * @brief Why a scan produced fewer tracks than files.
 * @ingroup kanariv_media
 * @see ScanIssue
 */
enum class ScanIssueKind {
    WalkError,   ///< The directory walk failed; the returned library is incomplete.
    FileSkipped, ///< One file was skipped (unreadable or not fingerprintable).
};

/**
 * @brief One per-file (or per-walk) problem encountered during a scan.
 * @ingroup kanariv_media
 * @details Carries enough context to be logged verbatim: the offending path
 * (empty for a walk error that has no single file) and the readable cause.
 * @see ScanIssueKind
 * @see ScanIssueSink
 */
struct ScanIssue {
    ScanIssueKind kind{ScanIssueKind::FileSkipped}; ///< Severity/precedence of the issue.
    std::string path{};                             ///< UTF-8 path, empty when not file-specific.
    std::string message{};                          ///< Human-readable cause.
};

/**
 * @brief Callback receiving each issue as the scan discovers it.
 * @ingroup kanariv_media
 * @details Pass an empty sink to ignore issues. A `WalkError` is reported once,
 * after the per-file issues that preceded it, and ends the scan.
 */
using ScanIssueSink = std::function<void(const ScanIssue&)>;

/**
 * @brief Lazily scans a directory tree yielding Tracks.
 * @ingroup kanariv_media
 * @param root Root directory to traverse recursively.
 * @param mode Fingerprinting mode (Sampled vs Full).
 * @return Generator yielding one `TrackRow` per *readable, fingerprinted* audio
 * file. Files that cannot be hashed are skipped; a missing root or a failing
 * walk ends the generator early. Neither condition is reported by this
 * overload -- use the issue-taking overload for that.
 * @details Uses `recursive_directory_iterator` with `skip_permission_denied`
 * and checks the iterator's `error_code` after every increment, so a walk
 * stopped by an I/O error looks like the walk ended, not like an empty
 * directory. The generator stays lazy (one `TrackRow` at a time, constant
 * memory), which is what makes it suitable for very large libraries.
 * @par Thread safety
 * Not thread-safe with concurrent filesystem mutation; otherwise re-entrant.
 * @see ScanMode
 * @see scanTracksToVector
 */
KANARIV_API kanariv::utils::Generator<kanariv::types::TrackRow>
scanTracks(const std::filesystem::path& root, ScanMode mode = ScanMode::Sampled);

/**
 * @brief Lazily scans a directory tree, reporting per-file issues as they occur.
 * @ingroup kanariv_media
 * @param root Root directory to traverse recursively.
 * @param mode Fingerprinting mode (Sampled vs Full).
 * @param onIssue Invoked for every skipped file and for a walk error. May be empty.
 * @return Generator yielding one `TrackRow` per readable, fingerprinted audio file,
 * in the same order as the plain overload.
 * @details Same semantics as `scanTracks(root, mode)`, plus reporting: a
 * missing root produces a single `WalkError` issue and no tracks, a file that
 * cannot be fingerprinted produces a `FileSkipped` issue and no track, and a
 * filesystem error mid-walk produces a `WalkError` issue and ends the walk.
 * @par Thread safety
 * `onIssue` runs on the iterating thread. Not thread-safe with concurrent
 * filesystem mutation; otherwise re-entrant.
 * @see ScanIssue
 * @see scanTracksToVector
 */
KANARIV_API kanariv::utils::Generator<kanariv::types::TrackRow>
scanTracks(const std::filesystem::path& root, ScanMode mode, ScanIssueSink onIssue);

/**
 * @brief Scans a directory and collects all tracks into a vector.
 * @ingroup kanariv_media
 * @param root Root directory.
 * @param mode Fingerprinting mode.
 * @return Vector of fingerprinted tracks, or `Error` with `StatusCode::NotFound`
 * when the root does not exist and `StatusCode::Io` when it cannot be opened or
 * when the walk failed partway (in which case the partial result is discarded).
 * @details A truncated walk is reported as an error rather than as a shorter
 * library, so an empty vector always means "no audio files", never "the walk
 * broke". Files that cannot be fingerprinted are skipped (they do not fail the
 * scan); use the issue-taking overload to enumerate them.
 * @par Thread safety
 * Re-entrant; not thread-safe with concurrent filesystem mutation.
 * @see scanTracks
 */
KANARIV_API std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode = ScanMode::Sampled);

/**
 * @brief Scans a directory into a vector, reporting per-file issues as they occur.
 * @ingroup kanariv_media
 * @param root Root directory.
 * @param mode Fingerprinting mode.
 * @param onIssue Invoked for every skipped file and for a walk error. May be empty.
 * @return Same result and error contract as `scanTracksToVector(root, mode)`.
 * @details The issue sink fires before the function returns, so a caller can
 * log skipped files while the scan runs and still get the completeness
 * guarantee of the returned `expected`.
 * @par Thread safety
 * `onIssue` runs on the calling thread. Re-entrant; not thread-safe with
 * concurrent filesystem mutation.
 * @see ScanIssue
 */
KANARIV_API std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode, ScanIssueSink onIssue);

/**
 * @brief Private loudness heuristic: gain (dB) that brings a track's high-passed RMS to -14 dBFS.
 * @ingroup kanariv_media
 * @param path Audio file path.
 * @return Gain in dB (positive = boost, negative = attenuate), or `Error` when
 * the file cannot be opened, decoded or contains no audible samples.
 * @details Decodes the full stream with FFmpeg, applies a first-order 65 Hz
 * high-pass (the ReplayGain 1.0 "head" filter, no A-weighting) and returns
 * `-14 dBFS - 20*log10(rms)`. This is **not** ReplayGain: neither ReplayGain
 * 1.0 (89 dB reference SPL) nor ReplayGain 2.0 (EBU R128 / -18 LUFS), and gains
 * computed this way are not interchangeable with tagged values.
 * @note Values from this function are deliberately **not** persisted into
 * `tracks.replay_gain_track` / `tracks.replay_gain_album` and must not be
 * advertised as ReplayGain to other players (e.g. over MPRIS).
 * @par Thread safety
 * Re-entrant; the FFmpeg session is local to the call.
 */
KANARIV_API kanariv::utils::Expected<double> computeLoudnessGain(std::string_view path);

} // namespace kanariv::media
