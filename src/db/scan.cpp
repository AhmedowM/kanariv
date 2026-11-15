#include <chrono>
#include <cstddef>
#include <cstdint>
#include <db/database_access.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <kanariv/db/database.hpp>
#include <kanariv/db/scan.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <media/fingerprint.hpp>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>

namespace kanariv::db {

std::expected<void, kanariv::utils::Error>
scanLibrary(contracts::IDatabase& db_, int64_t libraryId,
            std::function<void(int64_t, int64_t, std::string_view)> progress,
            kanariv::media::ScanMode mode) {
    Database& db = dynamic_cast<Database&>(db_);
    if (libraryId == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    auto libs = db.listLibraries();
    if (!libs)
        return std::unexpected{libs.error()};
    std::string libPath;
    for (auto& l : *libs)
        if (l.id == libraryId)
            libPath = l.path;
    if (libPath.empty()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "library not found")};
    }
    std::filesystem::path root = kanariv::utils::detail::pathFromUtf8(libPath);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "path not found")};
    if (auto* lg = db.logger())
        lg->info("scan started root={} mode={}", libPath,
                 mode == kanariv::media::ScanMode::Sampled ? "sampled" : "full");
    int64_t scanned = 0;
    int64_t added = 0;
    int64_t updated = 0;
    int64_t failed = 0;
    constexpr size_t batchSize = 500;
    size_t batchPending = 0;
    // RAII: the lock is released and an open batch rolled back on every exit
    // path, including a failed BEGIN. No error path re-enters the mutex.
    std::unique_lock<std::shared_mutex> batchLock;
    std::optional<DbTransaction> batchTx;
    auto beginBatch = [&]() -> std::expected<void, kanariv::utils::Error> {
        if (batchTx)
            return {};
        batchLock = std::unique_lock<std::shared_mutex>(DatabaseAccess::mutex(db));
        if (!DatabaseAccess::handleLocked(db)) {
            batchLock.unlock();
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
        }
        auto tx = DbTransaction::begin(DatabaseAccess::handleLocked(db));
        if (!tx) {
            batchLock.unlock();
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, tx.error().message)};
        }
        batchTx.emplace(std::move(*tx));
        batchPending = 0;
        return {};
    };
    auto commitBatch = [&]() -> std::expected<void, kanariv::utils::Error> {
        if (!batchTx)
            return {};
        auto committed = batchTx->commit();
        batchTx.reset(); // rolls back on COMMIT failure
        batchLock.unlock();
        batchPending = 0;
        return committed;
    };

    // Per-file discovery problems are logged as they happen (M8): a skipped file
    // is counted as failed so the final summary does not claim a clean scan.
    auto onIssue = [&db, &failed](const kanariv::media::ScanIssue& issue) {
        auto* lg = db.logger();
        if (issue.kind == kanariv::media::ScanIssueKind::WalkError) {
            if (lg)
                lg->error("scan walk error: {} ({})", issue.path, issue.message);
            return;
        }
        failed++;
        if (lg)
            lg->warn("scan skipped file={} reason={}", issue.path, issue.message);
    };

    for (auto trk : kanariv::media::scanTracks(root, mode, onIssue)) {
        // Backstop (M8): an all-zero fingerprint is the "unfingerprinted" sentinel
        // and collides with itself in the UNIQUE index, which would make every
        // later file look like a duplicate of the first. Never persist one.
        if (kanariv::media::internal::isZeroFingerprint(trk.fingerprint)) {
            failed++;
            scanned++;
            if (auto* lg = db.logger())
                lg->error("scan: refusing to store all-zero fingerprint file={}", trk.path);
            if (progress)
                progress(scanned, 0, trk.path);
            continue;
        }
        if (!batchTx) {
            auto b = beginBatch();
            if (!b)
                return std::unexpected{b.error()};
        }
        auto existingPath = DatabaseAccess::findByPathLocked(db, trk.path);
        if (existingPath && existingPath->size == trk.size && existingPath->mtime == trk.mtime) {
            scanned++;
            batchPending++;
            if (progress)
                progress(scanned, 0, trk.path);
            if (batchPending >= batchSize) {
                auto c = commitBatch();
                if (!c)
                    return std::unexpected{c.error()};
            }
            continue;
        }
        auto byFp = DatabaseAccess::findByFingerprintLocked(db, trk.fingerprint);
        if (byFp) {
            kanariv::types::Track upd = *byFp;
            upd.path = trk.path;
            upd.size = trk.size;
            upd.mtime = trk.mtime;
            upd.library_id = libraryId;
            upd.deleted_at = 0;
            bool hasMeta = false;
            auto meta = kanariv::media::extractMetadata(trk.path);
            if (!meta) {
                if (auto* lg = db.logger())
                    lg->debug("scan metadata failed file={} err={}", trk.path,
                              meta.error().message);
            }
            if (meta) {
                if (!meta->title.empty()) {
                    upd.title = std::move(meta->title);
                    hasMeta = true;
                }
                if (!meta->artist.empty()) {
                    upd.artist = std::move(meta->artist);
                    hasMeta = true;
                }
                if (!meta->album.empty()) {
                    upd.album = std::move(meta->album);
                    hasMeta = true;
                }
                if (!meta->album_artist.empty()) {
                    upd.album_artist = std::move(meta->album_artist);
                    hasMeta = true;
                }
                if (!meta->genre.empty()) {
                    upd.genre = std::move(meta->genre);
                    hasMeta = true;
                }
                if (meta->year != 0) {
                    upd.year = meta->year;
                    hasMeta = true;
                }
                if (meta->track_num != 0) {
                    upd.track_num = meta->track_num;
                    hasMeta = true;
                }
                if (meta->disc_num != 0) {
                    upd.disc_num = meta->disc_num;
                    hasMeta = true;
                }
                if (meta->duration > 0) {
                    upd.duration = meta->duration;
                    hasMeta = true;
                }
                if (meta->sample_rate != 0) {
                    upd.sample_rate = static_cast<uint32_t>(meta->sample_rate);
                    hasMeta = true;
                }
                if (meta->channels != 0) {
                    upd.channels = static_cast<uint32_t>(meta->channels);
                    hasMeta = true;
                }
                if (meta->bitrate != 0) {
                    upd.bitrate = meta->bitrate;
                    hasMeta = true;
                }
            }
            if (!hasMeta) {
                upd.title.clear();
                upd.artist.clear();
                upd.album.clear();
                upd.album_artist.clear();
                upd.genre.clear();
                upd.year = 0;
                upd.track_num = 0;
                upd.disc_num = 0;
                upd.cover_art_path.clear();
                upd.duration = 0;
                upd.sample_rate = 0;
                upd.channels = 0;
                upd.bitrate = 0;
            }
            if (DatabaseAccess::updateTrackLocked(db, upd))
                updated++;
            else
                failed++;
            if (existingPath && existingPath->id != byFp->id) {
                if (existingPath->fingerprint == trk.fingerprint)
                    (void)DatabaseAccess::deleteTrackLocked(db, existingPath->id);
            }
        } else if (existingPath) {
            kanariv::types::Track upd = *existingPath;
            int64_t keepPlay = upd.play_count;
            int keepRating = upd.rating;
            int64_t keepAdded = upd.date_added;
            upd.fingerprint = trk.fingerprint;
            upd.size = trk.size;
            upd.mtime = trk.mtime;
            upd.library_id = libraryId;
            upd.deleted_at = 0;
            bool hasMeta = false;
            auto meta = kanariv::media::extractMetadata(trk.path);
            if (!meta) {
                if (auto* lg = db.logger())
                    lg->debug("scan metadata failed file={} err={}", trk.path,
                              meta.error().message);
            }
            if (meta) {
                if (!meta->title.empty()) {
                    upd.title = std::move(meta->title);
                    hasMeta = true;
                }
                if (!meta->artist.empty()) {
                    upd.artist = std::move(meta->artist);
                    hasMeta = true;
                }
                if (!meta->album.empty()) {
                    upd.album = std::move(meta->album);
                    hasMeta = true;
                }
                if (!meta->album_artist.empty()) {
                    upd.album_artist = std::move(meta->album_artist);
                    hasMeta = true;
                }
                if (!meta->genre.empty()) {
                    upd.genre = std::move(meta->genre);
                    hasMeta = true;
                }
                if (meta->year != 0) {
                    upd.year = meta->year;
                    hasMeta = true;
                }
                if (meta->track_num != 0) {
                    upd.track_num = meta->track_num;
                    hasMeta = true;
                }
                if (meta->disc_num != 0) {
                    upd.disc_num = meta->disc_num;
                    hasMeta = true;
                }
                if (meta->duration > 0) {
                    upd.duration = meta->duration;
                    hasMeta = true;
                }
                if (meta->sample_rate != 0) {
                    upd.sample_rate = static_cast<uint32_t>(meta->sample_rate);
                    hasMeta = true;
                }
                if (meta->channels != 0) {
                    upd.channels = static_cast<uint32_t>(meta->channels);
                    hasMeta = true;
                }
                if (meta->bitrate != 0) {
                    upd.bitrate = meta->bitrate;
                    hasMeta = true;
                }
            }
            if (!hasMeta) {
                upd.title.clear();
                upd.artist.clear();
                upd.album.clear();
                upd.album_artist.clear();
                upd.genre.clear();
                upd.year = 0;
                upd.track_num = 0;
                upd.disc_num = 0;
                upd.cover_art_path.clear();
                upd.duration = 0;
                upd.sample_rate = 0;
                upd.channels = 0;
                upd.bitrate = 0;
            }
            upd.dirty = false;
            upd.play_count = keepPlay;
            upd.rating = keepRating;
            upd.date_added = keepAdded;
            // replay_gain_track/album are deliberately left untouched: the scanner
            // no longer synthesises gain values (M10), and the previous write was a
            // mis-scaled loudness heuristic, not ReplayGain.
            if (DatabaseAccess::updateTrackLocked(db, upd))
                updated++;
            else {
                failed++;
                if (auto* lg = db.logger())
                    lg->warn("scan update failed file={}", trk.path);
            }
        } else {
            trk.library_id = libraryId;
            if (DatabaseAccess::insertTrackLocked(db, trk))
                added++;
            else {
                failed++;
                if (auto* lg = db.logger())
                    lg->warn("scan insert failed file={}", trk.path);
            }
        }
        scanned++;
        batchPending++;
        if (progress)
            progress(scanned, 0, trk.path);
        if (batchPending >= batchSize) {
            auto c = commitBatch();
            if (!c)
                return std::unexpected{c.error()};
        }
    }
    if (batchTx) {
        auto c = commitBatch();
        if (!c)
            return std::unexpected{c.error()};
    }
    if (auto* lg = db.logger())
        lg->info("scan finished root={} scanned={} added={} updated={} failed={}", libPath, scanned,
                 added, updated, failed);

    // Album gain is no longer derived here (M10): the old block averaged the
    // per-track values the scanner had synthesised from a loudness heuristic and
    // wrote the result into replay_gain_album as if it were ReplayGain. The
    // columns are reserved for tag-imported values, which nothing writes yet.

    auto libs2 = db.listLibraries();
    if (libs2) {
        for (auto& l : *libs2)
            if (l.id == libraryId) {
                l.last_scanned = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                {
                    std::unique_lock<std::shared_mutex> lk(DatabaseAccess::mutex(db));
                    // Single UPDATE, no explicit transaction: a failed write is
                    // reported without re-entering the (non-recursive) mutex.
                    if (auto stored = DatabaseAccess::libraryUpdateLocked(db, l); !stored) {
                        if (auto* lg = db.logger())
                            lg->warn("scan: recording last_scanned failed: {}",
                                     stored.error().message);
                    }
                }
                break;
            }
    }
    return {};
}

} // namespace kanariv::db
