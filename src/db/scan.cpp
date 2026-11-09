#include <sqlite3.h>

#include <kanariv/db/database.hpp>
#include <kanariv/db/scan.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <db/database_access.hpp>
#include <db/stmt_helpers.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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
    std::unique_lock<std::shared_mutex> batchLock;
    bool inTx = false;
    auto beginBatch = [&]() -> std::expected<void, kanariv::utils::Error> {
        if (inTx)
            return {};
        batchLock = std::unique_lock<std::shared_mutex>(DatabaseAccess::mutex(db));
        if (!DatabaseAccess::handleLocked(db))
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
        char* err = nullptr;
        internal::SqliteErrGuard guard{err};
        int rc = sqlite3_exec(DatabaseAccess::handleLocked(db), "BEGIN IMMEDIATE", nullptr, nullptr,
                              &err);
        if (rc != SQLITE_OK) {
            batchLock.unlock();
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Busy, err ? std::string(err) : "BEGIN failed")};
        }
        inTx = true;
        batchPending = 0;
        return {};
    };
    auto commitBatch = [&]() -> std::expected<void, kanariv::utils::Error> {
        if (!inTx)
            return {};
        char* err = nullptr;
        internal::SqliteErrGuard guard{err};
        int rc = sqlite3_exec(DatabaseAccess::handleLocked(db), "COMMIT", nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            sqlite3_exec(DatabaseAccess::handleLocked(db), "ROLLBACK", nullptr, nullptr, nullptr);
            batchLock.unlock();
            inTx = false;
            batchPending = 0;
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Internal, err ? std::string(err) : "commit failed")};
        }
        batchLock.unlock();
        inTx = false;
        batchPending = 0;
        return {};
    };
    auto rollbackBatch = [&]() {
        if (!inTx)
            return;
        sqlite3_exec(DatabaseAccess::handleLocked(db), "ROLLBACK", nullptr, nullptr, nullptr);
        batchLock.unlock();
        inTx = false;
        batchPending = 0;
    };

    for (auto trk : kanariv::media::scanTracks(root, mode)) {
        if (!inTx) {
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
                if (!c) {
                    rollbackBatch();
                    return std::unexpected{c.error()};
                }
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
            if (auto rg = kanariv::media::computeReplayGainTrack(trk.path); rg)
                upd.replayGainTrack = *rg;
            if (DatabaseAccess::updateTrackLocked(db, upd))
                updated++;
            else {
                failed++;
                if (auto* lg = db.logger())
                    lg->warn("scan update failed file={}", trk.path);
            }
        } else {
            trk.library_id = libraryId;
            if (auto rg = kanariv::media::computeReplayGainTrack(trk.path); rg)
                trk.replayGainTrack = *rg;
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
            if (!c) {
                rollbackBatch();
                return std::unexpected{c.error()};
            }
        }
    }
    if (inTx) {
        auto c = commitBatch();
        if (!c) {
            rollbackBatch();
            return std::unexpected{c.error()};
        }
    }
    if (auto* lg = db.logger())
        lg->info("scan finished root={} scanned={} added={} updated={} failed={}", libPath, scanned,
                 added, updated, failed);

    // Album gain: average the per-track gains of all tracks sharing an album
    // name, then write replay_gain_album back to each track.
    {
        std::unique_lock<std::shared_mutex> lk(DatabaseAccess::mutex(db));
        if (DatabaseAccess::handleLocked(db)) {
            char* err = nullptr;
            internal::SqliteErrGuard guard{err};
            int rc = sqlite3_exec(DatabaseAccess::handleLocked(db), "BEGIN IMMEDIATE",
                                  nullptr, nullptr, &err);
            if (rc == SQLITE_OK) {
                sqlite3_stmt* st = nullptr;
                const char* sql =
                    "SELECT id, album, replay_gain_track FROM tracks "
                    "WHERE library_id=? AND album!='' AND replay_gain_track!=0";
                if (sqlite3_prepare_v2(DatabaseAccess::handleLocked(db), sql, -1, &st,
                                       nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(st, 1, libraryId);
                    struct AlbumGain { double sum; int count; };
                    std::map<std::string, AlbumGain> albums;
                    while (sqlite3_step(st) == SQLITE_ROW) {
                        int64_t id = sqlite3_column_int64(st, 0);
                        const char* album = (const char*)sqlite3_column_text(st, 1);
                        double rg = sqlite3_column_double(st, 2);
                        if (album && *album) {
                            auto& ag = albums[std::string(album)];
                            ag.sum += rg;
                            ag.count++;
                        }
                        (void)id;
                    }
                    sqlite3_finalize(st);
                    // Second pass: update each track with its album average.
                    for (auto& [album, ag] : albums) {
                        if (ag.count == 0)
                            continue;
                        double avg = ag.sum / (double)ag.count;
                        sqlite3_stmt* ust = nullptr;
                        if (sqlite3_prepare_v2(DatabaseAccess::handleLocked(db),
                                               "UPDATE tracks SET replay_gain_album=? "
                                               "WHERE library_id=? AND album=? AND replay_gain_track!=0",
                                               -1, &ust, nullptr) == SQLITE_OK) {
                            sqlite3_bind_double(ust, 1, avg);
                            sqlite3_bind_int64(ust, 2, libraryId);
                            sqlite3_bind_text(ust, 3, album.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(ust);
                            sqlite3_finalize(ust);
                        }
                    }
                }
                sqlite3_exec(DatabaseAccess::handleLocked(db), "COMMIT", nullptr, nullptr, nullptr);
            }
        }
    }

    auto libs2 = db.listLibraries();
    if (libs2) {
        for (auto& l : *libs2)
            if (l.id == libraryId) {
                l.last_scanned = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                {
                    std::unique_lock<std::shared_mutex> lk(DatabaseAccess::mutex(db));
                    char* err = nullptr;
                    internal::SqliteErrGuard guard{err};
                    int rc = sqlite3_exec(DatabaseAccess::handleLocked(db), "BEGIN IMMEDIATE",
                                          nullptr, nullptr, &err);
                    if (rc == SQLITE_OK) {
                        (void)DatabaseAccess::libraryUpdateLocked(db, l);
                        char* cErr = nullptr;
                        internal::SqliteErrGuard cGuard{cErr};
                        rc = sqlite3_exec(DatabaseAccess::handleLocked(db), "COMMIT", nullptr,
                                          nullptr, &cErr);
                        if (rc != SQLITE_OK)
                            sqlite3_exec(DatabaseAccess::handleLocked(db), "ROLLBACK", nullptr,
                                         nullptr, nullptr);
                    } else {
                        (void)db.libraryUpdate(l);
                    }
                }
                break;
            }
    }
    return {};
}

} // namespace kanariv::db
