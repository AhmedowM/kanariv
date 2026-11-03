#pragma once

/**
 * @file scan.hpp
 * @brief Database-side scan helpers (persistence half of library scanning).
 * @ingroup kanariv_db
 * @details Library scanning is split at the persistence seam:
 * - `kanariv::media` owns the pure, DB-free half: filesystem walk,
 *   extension filter, BLAKE3 fingerprinting and metadata extraction
 *   (`kanariv::media::scanTracks`, `kanariv::media::scanTracksToVector`).
 * - `kanariv::db` keeps the DB-coupled half: `scanLibrary` (batched upsert
 *   of the discovered tracks under the database mutex).
 */

#include <cstdint>
#include <expected>
#include <functional>
#include <kanariv/fwd.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/scan.hpp>
#include <string_view>

namespace kanariv::db {

/**
 * @brief Scans a library root and upserts tracks into the database.
 * @ingroup kanariv_db
 * @param db Database to update.
 * @param libraryId Library id whose `path` is the scan root.
 * @param progress Optional callback `progress(scanned, total, path)` invoked per file.
 * @param mode Discovery mode passed to `kanariv::media::scanTracks`.
 * @return Success or `Error` with `StatusCode::InvalidArg` if libraryId==0,
 * `StatusCode::NotFound` if library or path not found, `StatusCode::Busy` if
 * `BEGIN IMMEDIATE` fails, or `StatusCode::Internal` on commit failure.
 * @details Batching: holds `Database::mutex()` as `unique_lock<shared_mutex>`
 * for `batchSize` (500) files, wrapped in `BEGIN IMMEDIATE / COMMIT`, so
 * concurrent `listQueueItems` never sees partial state and a crash leaves DB
 * consistent per batch. Deduplication: early-exit on `path+size+mtime` match;
 * otherwise fingerprint dedup via `findByFingerprintLocked`/`findByPathLocked`.
 * Discovery (walk, fingerprint, tags) is done by `kanariv::media::scanTracks`.
 * @par Thread safety
 * Thread-safe: internally acquires `db.mutex()` per batch. Caller must not
 * hold `db.mutex()` across the call.
 * @par Lock ordering
 * Acquires `db.mutex()` (dbMutex_) exclusively per batch; inside the batch
 * uses `*Locked` helpers that assume the lock is held and additionally take
 * `cacheMutex_` internally.
 * @see kanariv::media::scanTracks
 * @see Database::findByPathLocked
 * @see Database::findByFingerprintLocked
 */
KANARIV_API std::expected<void, kanariv::utils::Error>
scanLibrary(contracts::IDatabase& db, int64_t libraryId,
            std::function<void(int64_t, int64_t, std::string_view)> progress = {},
            kanariv::media::ScanMode mode = kanariv::media::ScanMode::Sampled);

} // namespace kanariv::db
