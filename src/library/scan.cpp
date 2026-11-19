#include <cstddef>
#include <cstdint>
#include <db/database_access.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/library/scan.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/fingerprint.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <kanariv/utils/time.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>

namespace kanariv::library {

namespace {

kanariv::media::ScanMode toMediaMode(kanariv::contracts::ScanMode mode) {
    return mode == kanariv::contracts::ScanMode::Full ? kanariv::media::ScanMode::Full
                                                      : kanariv::media::ScanMode::Sampled;
}

// Fills the tag and stream-parameter columns from a scan result, moving values
// out of it. Returns false when the extract failed or carried no usable field.
bool applyMetadata(kanariv::utils::Expected<kanariv::types::TrackMetadata>& meta,
                   kanariv::types::TrackRow& t) {
    if (!meta)
        return false;
    auto& m = *meta;
    bool hasMeta = false;
    if (!m.title.empty()) {
        t.title = std::move(m.title);
        hasMeta = true;
    }
    if (!m.artist.empty()) {
        t.artist = std::move(m.artist);
        hasMeta = true;
    }
    if (!m.album.empty()) {
        t.album = std::move(m.album);
        hasMeta = true;
    }
    if (!m.album_artist.empty()) {
        t.album_artist = std::move(m.album_artist);
        hasMeta = true;
    }
    if (!m.genre.empty()) {
        t.genre = std::move(m.genre);
        hasMeta = true;
    }
    if (m.year != 0) {
        t.year = m.year;
        hasMeta = true;
    }
    if (m.track_num != 0) {
        t.track_num = m.track_num;
        hasMeta = true;
    }
    if (m.disc_num != 0) {
        t.disc_num = m.disc_num;
        hasMeta = true;
    }
    if (m.duration > 0) {
        t.duration = m.duration;
        hasMeta = true;
    }
    if (m.sample_rate != 0) {
        t.sample_rate = static_cast<uint32_t>(m.sample_rate);
        hasMeta = true;
    }
    if (m.channels != 0) {
        t.channels = static_cast<uint32_t>(m.channels);
        hasMeta = true;
    }
    if (m.bitrate != 0) {
        t.bitrate = m.bitrate;
        hasMeta = true;
    }
    return hasMeta;
}

// A row whose tags could not be re-read must not keep the previous scan's text:
// those values may describe a different file that used to live at this path.
void clearMetadata(kanariv::types::TrackRow& t) {
    t.title.clear();
    t.artist.clear();
    t.album.clear();
    t.album_artist.clear();
    t.genre.clear();
    t.year = 0;
    t.track_num = 0;
    t.disc_num = 0;
    t.cover_art_path.clear();
    t.duration = 0;
    t.sample_rate = 0;
    t.channels = 0;
    t.bitrate = 0;
}

} // namespace

Scanner::Scanner(std::shared_ptr<kanariv::db::Database> db) : db_(std::move(db)) {}

kanariv::utils::Expected<void>
Scanner::scanLibrary(int64_t libraryId,
                     std::function<void(int64_t, int64_t, std::string_view)> progress,
                     kanariv::contracts::ScanMode mode) {
    if (libraryId == 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    auto& db = *db_;
    const auto mediaMode = toMediaMode(mode);
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
                 mode == kanariv::contracts::ScanMode::Sampled ? "sampled" : "full");
    int64_t scanned = 0;
    int64_t added = 0;
    int64_t updated = 0;
    int64_t failed = 0;
    constexpr size_t batchSize = 500;
    size_t batchPending = 0;
    // RAII: the lock is released and an open batch rolled back on every exit
    // path, including a failed BEGIN. No error path re-enters the mutex.
    std::unique_lock<std::mutex> batchLock;
    std::optional<kanariv::db::DbTransaction> batchTx;
    auto beginBatch = [&]() -> kanariv::utils::Expected<void> {
        if (batchTx)
            return {};
        batchLock = std::unique_lock<std::mutex>(kanariv::db::DatabaseAccess::mutex(db));
        if (!kanariv::db::DatabaseAccess::handleLocked(db)) {
            batchLock.unlock();
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
        }
        auto tx = kanariv::db::DbTransaction::begin(kanariv::db::DatabaseAccess::handleLocked(db));
        if (!tx) {
            batchLock.unlock();
            return std::unexpected{tx.error()};
        }
        batchTx.emplace(std::move(*tx));
        batchPending = 0;
        return {};
    };
    auto commitBatch = [&]() -> kanariv::utils::Expected<void> {
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

    for (auto trk : kanariv::media::scanTracks(root, mediaMode, onIssue)) {
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
        auto existingPath = kanariv::db::DatabaseAccess::findByPathLocked(db, trk.path);
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
        auto byFp = kanariv::db::DatabaseAccess::findByFingerprintLocked(db, trk.fingerprint);
        if (byFp) {
            kanariv::types::TrackRow upd = *byFp;
            upd.path = trk.path;
            upd.size = trk.size;
            upd.mtime = trk.mtime;
            upd.library_id = libraryId;
            auto meta = kanariv::media::extractMetadata(trk.path);
            if (!meta && db.logger())
                db.logger()->debug("scan metadata failed file={} err={}", trk.path,
                                   meta.error().message);
            if (!applyMetadata(meta, upd))
                clearMetadata(upd);
            if (kanariv::db::DatabaseAccess::updateTrackLocked(db, upd))
                updated++;
            else
                failed++;
            if (existingPath && existingPath->id != byFp->id) {
                if (existingPath->fingerprint == trk.fingerprint)
                    (void)kanariv::db::DatabaseAccess::deleteTrackLocked(db, existingPath->id);
            }
        } else if (existingPath) {
            kanariv::types::TrackRow upd = *existingPath;
            int64_t keepPlay = upd.play_count;
            int keepRating = upd.rating;
            int64_t keepAdded = upd.date_added;
            upd.fingerprint = trk.fingerprint;
            upd.size = trk.size;
            upd.mtime = trk.mtime;
            upd.library_id = libraryId;
            auto meta = kanariv::media::extractMetadata(trk.path);
            if (!meta && db.logger())
                db.logger()->debug("scan metadata failed file={} err={}", trk.path,
                                   meta.error().message);
            if (!applyMetadata(meta, upd))
                clearMetadata(upd);
            upd.dirty = false;
            upd.play_count = keepPlay;
            upd.rating = keepRating;
            upd.date_added = keepAdded;
            // replay_gain_track/album are deliberately left untouched: they are
            // owned by the explicit analysis pass (library::analyzeLibrary), so a
            // rescan never discards a measurement or resurrects the old heuristic.
            if (kanariv::db::DatabaseAccess::updateTrackLocked(db, upd))
                updated++;
            else {
                failed++;
                if (auto* lg = db.logger())
                    lg->warn("scan update failed file={}", trk.path);
            }
        } else {
            trk.library_id = libraryId;
            trk.date_added = kanariv::utils::unixMillis();
            if (kanariv::db::DatabaseAccess::insertTrackLocked(db, trk))
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

    // Album gain is not derived here: it is a property of the loudness analysis
    // pass (library::analyzeLibrary), which measures whole album groups. A scan
    // only discovers and upserts rows.

    auto libs2 = db.listLibraries();
    if (libs2) {
        for (auto& l : *libs2)
            if (l.id == libraryId) {
                l.last_scanned = kanariv::utils::unixMillis();
                {
                    std::unique_lock<std::mutex> lk(kanariv::db::DatabaseAccess::mutex(db));
                    // Single UPDATE, no explicit transaction: a failed write is
                    // reported without re-entering the (non-recursive) mutex.
                    if (auto stored = kanariv::db::DatabaseAccess::libraryUpdateLocked(db, l);
                        !stored) {
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

} // namespace kanariv::library
