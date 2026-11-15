#pragma once

/**
 * @file scan.hpp
 * @brief DB-coupled library scanning: discover files and upsert them.
 * @ingroup kanariv_library
 * @details Library scanning is split at the persistence seam:
 * - `kanariv::media` owns the pure, DB-free half: filesystem walk,
 *   extension filter, BLAKE3 fingerprinting and metadata extraction
 *   (`kanariv::media::scanTracks`, `kanariv::media::scanTracksToVector`).
 * - `kanariv::library` owns the orchestration: `Scanner` walks a configured
 *   library root and upserts the discovered tracks in bounded transactions.
 *   It is the only component that needs both the media stack and the store.
 */

#include <cstdint>
#include <expected>
#include <functional>
#include <kanariv/contracts/library_scan.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <string_view>

namespace kanariv::library {

/**
 * @brief Scans library roots and persists the discovered tracks.
 * @ingroup kanariv_library
 * @details Implements `contracts::ILibraryScanner` over one open database.
 * The scanner keeps the database alive for as long as it lives; it holds the
 * store's write lock only per batch, so concurrent readers never observe a
 * half-applied scan.
 * @par Thread safety
 * Thread-safe: batches take the database lock internally. The caller must not
 * hold the database lock across `scanLibrary`.
 */
class KANARIV_API Scanner final : public contracts::ILibraryScanner {
  public:
    /**
     * @brief Creates a scanner over an open database.
     * @param db Database to scan into; must not be null.
     */
    explicit Scanner(std::shared_ptr<kanariv::db::Database> db);

    /**
     * @brief Scans a library root and upserts the discovered tracks.
     * @param libraryId Library identifier whose `path` is the scan root.
     * @param progress Optional callback `progress(scanned, total, path)` per file.
     * @param mode Discovery mode.
     * @return Success or `Error` with `StatusCode::InvalidArg` if libraryId==0,
     * `StatusCode::NotFound` if library or path not found, `StatusCode::Busy` if
     * `BEGIN IMMEDIATE` fails, or `StatusCode::Io`/`Internal` on failure.
     * @details Batches: holds the database write lock for 500 files wrapped in
     * `BEGIN IMMEDIATE / COMMIT`, so concurrent reads never see partial state
     * and a crash leaves the database consistent per batch. Deduplication:
     * early exit on `path+size+mtime` match; otherwise fingerprint dedup.
     */
    std::expected<void, kanariv::utils::Error>
    scanLibrary(int64_t libraryId,
                std::function<void(int64_t, int64_t, std::string_view)> progress = {},
                contracts::ScanMode mode = contracts::ScanMode::Sampled) override;

  private:
    std::shared_ptr<kanariv::db::Database> db_;
};

} // namespace kanariv::library
