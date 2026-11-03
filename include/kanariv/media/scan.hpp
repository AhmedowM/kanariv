#pragma once

/**
 * @file scan.hpp
 * @brief Filesystem scan helpers for audio tracks (discovery, fingerprint, tags).
 * @ingroup kanariv_media
 * @details Library scanning is split at the persistence seam:
 * - `kanariv::media` owns the pure, DB-free half: filesystem walk,
 *   extension filter, BLAKE3 fingerprinting and metadata extraction,
 *   producing `kanariv::types::Track` records.
 * - `kanariv::db` keeps the DB-coupled half: `kanariv::db::scanLibrary`
 *   (batched upsert of the discovered tracks under the database mutex).
 */

#include <expected>
#include <filesystem>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
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
 * @brief Lazily scans a directory tree yielding Tracks.
 * @ingroup kanariv_media
 * @param root Root directory to traverse recursively.
 * @param mode Fingerprinting mode (Sampled vs Full).
 * @return Generator yielding one `Track` per audio file.
 * @details Uses `recursive_directory_iterator` with `skip_permission_denied`.
 * @par Thread safety
 * Not thread-safe with concurrent filesystem mutation; otherwise re-entrant.
 * @see ScanMode
 * @see scanTracksToVector
 */
KANARIV_API kanariv::utils::Generator<kanariv::types::Track>
scanTracks(const std::filesystem::path& root, ScanMode mode = ScanMode::Sampled);

/**
 * @brief Scans a directory and collects all tracks into a vector.
 * @ingroup kanariv_media
 * @param root Root directory.
 * @param mode Fingerprinting mode.
 * @return Vector of tracks, or `Error` (currently always succeeds, returns empty on missing root).
 * @par Thread safety
 * Re-entrant; not thread-safe with concurrent filesystem mutation.
 * @see scanTracks
 */
KANARIV_API std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
scanTracksToVector(const std::filesystem::path& root, ScanMode mode = ScanMode::Sampled);

} // namespace kanariv::media
