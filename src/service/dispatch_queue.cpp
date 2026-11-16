#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <expected>
#include <filesystem>
#include <kanariv/contracts/queue_store.hpp>
#include <kanariv/contracts/track_store.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/fingerprint.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/parse.hpp>
#include <kanariv/utils/result.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>

#include "service_impl.hpp"

namespace kanariv::service {

namespace detail {

double durationFromDecoder(const std::filesystem::path& path) noexcept;

/** Strips leading/trailing ASCII whitespace. */
inline std::string_view trimAscii(std::string_view s) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

inline std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
ensureLibraryTrack(kanariv::contracts::ITrackStore& tracks, const std::filesystem::path& p) {
    auto fpRes = kanariv::media::internal::computeFingerprint(p);
    if (!fpRes)
        return std::unexpected{fpRes.error()};
    kanariv::types::TrackRow t;
    t.path = kanariv::utils::detail::pathToUtf8(p);
    t.fingerprint = *fpRes;
    t.duration = durationFromDecoder(p);
    std::error_code ec2;
    auto sz = std::filesystem::file_size(p, ec2);
    if (!ec2)
        t.size = static_cast<int64_t>(sz);
    auto ftime = std::filesystem::last_write_time(p, ec2);
    if (!ec2)
        t.mtime = static_cast<int64_t>(ftime.time_since_epoch().count());
    if (auto meta = kanariv::media::extractMetadata(t.path); meta) {
        t.title = std::move(meta->title);
        t.artist = std::move(meta->artist);
        t.album = std::move(meta->album);
        t.album_artist = std::move(meta->album_artist);
        t.genre = std::move(meta->genre);
        t.year = meta->year;
        t.track_num = meta->track_num;
        t.disc_num = meta->disc_num;
        if (meta->duration > 0)
            t.duration = meta->duration;
        t.sample_rate = static_cast<uint32_t>(meta->sample_rate);
        t.channels = static_cast<uint32_t>(meta->channels);
        t.bitrate = meta->bitrate;
    }
    auto ins = tracks.insertTrack(t);
    if (ins) {
        t.id = *ins;
        return t;
    }
    if (ins.error().code == kanariv::utils::StatusCode::AlreadyExists) {
        if (auto existing = tracks.findByFingerprint(t.fingerprint))
            return std::move(*existing);
        if (auto byPath = tracks.findByPath(t.path))
            return std::move(*byPath);
    }
    return std::unexpected{ins.error()};
}

std::expected<void, kanariv::utils::Error> enqueueInChunks(kanariv::contracts::IQueueStore& queues,
                                                           int64_t qid,
                                                           std::span<const int64_t> ids) {
    for (std::size_t off = 0; off < ids.size(); off += kanariv::contracts::queueBatchChunk) {
        const std::size_t n = std::min(kanariv::contracts::queueBatchChunk, ids.size() - off);
        if (auto r = queues.queueEnqueueBatch(qid, ids.subspan(off, n)); !r)
            return std::unexpected{r.error()};
    }
    return {};
}

/** One parsed `#pos:<n>` / `#id:<n>` selector (A2a grammar). */
struct QueueSelector final {
    int64_t number{0};
    bool byTrackId{false};
};

std::expected<QueueSelector, kanariv::utils::Error> parseQueueSelector(std::string_view raw) {
    //   "#pos:<n>" = zero-based queue position, "#id:<n>" = track id. A bare
    //   number used to mean "position", which silently removed a different row
    //   when the client meant an id; it is rejected instead of guessed.
    std::string_view spec = trimAscii(raw);
    QueueSelector out;
    if (spec.starts_with("#id:")) {
        out.byTrackId = true;
        spec.remove_prefix(4);
    } else if (spec.starts_with("#pos:")) {
        spec.remove_prefix(5);
    } else {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                      "ambiguous idOrIndex (use #pos:<n> or #id:<n>)")};
    }
    const auto number = kanariv::utils::parseId(spec);
    if (!number) {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "invalid idOrIndex (use #pos:<n> or #id:<n>)")};
    }
    out.number = *number;
    return out;
}

/**
 * @brief Resolves one QueueAdd query to library tracks.
 * @details Audio path, library id and path lookup behave as the direct-play
 * branch; anything else falls back to a library search capped at 50 matches.
 */
std::expected<std::vector<kanariv::types::TrackRow>, kanariv::utils::Error>
resolveAddQuery(kanariv::contracts::Storage& storage, std::string_view query, bool search) {
    if (search) {
        auto sr = storage.library->searchTracks(query, 50);
        if (!sr)
            return std::unexpected{sr.error()};
        if (sr->empty()) {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::NotFound, "no matches for: " + std::string(query))};
        }
        return std::move(*sr);
    }
    std::filesystem::path p(query);
    std::error_code ec;
    const bool exists = std::filesystem::exists(p, ec);
    if (!ec && kanariv::media::hasAudioExt(p)) {
        if (!exists) {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::NotFound, "track not found: " + std::string(query))};
        }
        auto trRes = ensureLibraryTrack(*storage.tracks, p);
        if (!trRes)
            return std::unexpected{trRes.error()};
        std::vector<kanariv::types::TrackRow> single;
        single.reserve(1);
        single.push_back(std::move(*trRes));
        return single;
    }
    // A numeric query is a library id (paths are resolved below).
    const auto id = kanariv::utils::parseId(trimAscii(query));
    if (id && *id != 0) {
        auto tr = storage.tracks->getTrack(*id);
        if (!tr)
            return std::unexpected{tr.error()};
        std::vector<kanariv::types::TrackRow> single;
        single.reserve(1);
        single.push_back(std::move(*tr));
        return single;
    }
    if (auto tr = storage.tracks->findByPath(query)) {
        std::vector<kanariv::types::TrackRow> single;
        single.reserve(1);
        single.push_back(std::move(*tr));
        return single;
    }
    auto sr = storage.library->searchTracks(query, 50);
    if (!sr)
        return std::unexpected{sr.error()};
    if (sr->empty()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "track not found: " + std::string(query))};
    }
    return std::move(*sr);
}

double durationFromDecoder(const std::filesystem::path& path) noexcept {
    try {
        auto readerRes = kanariv::media::FileReader::open(path);
        if (!readerRes)
            return 0.0;
        auto& readerPtr = readerRes.value();
        auto decRes = kanariv::media::Decoder::open(*readerPtr);
        if (!decRes)
            return 0.0;
        auto& decPtr = decRes.value();
        std::uint32_t sr = decPtr->sampleRate();
        std::uint64_t frames = decPtr->totalFrames();
        if (sr == 0)
            return 0.0;
        return static_cast<double>(frames) / static_cast<double>(sr);
    } catch (...) {
        return 0.0;
    }
}

} // namespace detail

using kanariv::engine::detail::ServiceAccess;
using namespace kanariv::ipc;
using kanariv::ipc::QueueAdd;
using kanariv::ipc::QueueClear;
using kanariv::ipc::QueueList;
using kanariv::ipc::QueueMove;
using kanariv::ipc::QueueQueues;
using kanariv::ipc::QueueRemove;
using kanariv::ipc::QueueRepeat;
using kanariv::ipc::QueueShuffle;
using kanariv::ipc::QueueSwitch;

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueList& ql) {
    if (ql.order != "playback" && ql.order != "added" && !ql.order.empty())
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg, "queue list: invalid order (added|playback)")};
    int64_t qid = engine_->activeQueueId();
    // validation: queue exists
    {
        auto q = storage_.queues->getQueue(qid);
        if (!q)
            return std::unexpected{q.error()};
    }
    auto fetched = storage_.queues->tracksForQueue(qid);
    if (!fetched)
        return std::unexpected{fetched.error()};
    std::vector<kanariv::types::TrackRow> tracks = std::move(*fetched);
    if (ql.order != "added") {
        // Playback (shuffle) order: perm holds queue positions. A stale perm
        // (queue mutated after shuffle) contributes its valid entries first;
        // anything missing follows in insertion order.
        auto perm = ServiceAccess::shufflePermFor(*engine_, qid);
        if (!perm.empty()) {
            std::vector<kanariv::types::TrackRow> ordered;
            ordered.reserve(tracks.size());
            std::vector<char> seen(tracks.size(), 0);
            for (int64_t p : perm) {
                if (p >= 0 && (std::size_t)p < tracks.size() && !seen[(std::size_t)p]) {
                    ordered.push_back(tracks[(std::size_t)p]);
                    seen[(std::size_t)p] = 1;
                }
            }
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                if (!seen[i])
                    ordered.push_back(tracks[i]);
            }
            tracks = std::move(ordered);
        }
    }
    return Result{QueueTracks{std::move(tracks)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueQueues&) {
    auto qs = storage_.queues->listQueues();
    if (!qs)
        return std::unexpected{qs.error()};
    int64_t active = engine_->activeQueueId();
    kanariv::ipc::Queues out{};
    for (auto& q : *qs) {
        auto n = storage_.queues->queueCount(q.id);
        out.entries.push_back(
            kanariv::ipc::QueueEntry{q.id, q.name, n ? *n : 0, q.id == active, q.temp});
    }
    return Result{std::move(out)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueCreate& qc) {
    if (qc.name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "queue create: missing name")};
    auto id = storage_.queues->createQueue(qc.name);
    if (!id)
        return std::unexpected{id.error()};
    // Apply the configured default repeat mode to the new queue. `repeatDefault` is
    // already parsed; the stored integer is the wire value of RepeatMode
    // (Off=0, All=1, One=2).
    const auto mode = config_.repeatDefault;
    if (mode != kanariv::types::RepeatMode::Off) {
        if (auto e = storage_.queues->setQueueRepeat(*id, mode); !e)
            return std::unexpected{e.error()};
    }
    return Result{kanariv::ipc::QueueCreated{*id, qc.name}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueDelete& qd) {
    if (qd.qid == engine_->activeQueueId())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "cannot delete the active queue")};
    auto q = storage_.queues->getQueue(qd.qid);
    if (!q)
        return std::unexpected{q.error()};
    auto r = storage_.queues->deleteQueue(qd.qid);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::PlayFiles& cmd) {
    if (cmd.paths.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "play files: missing paths")};
    // Validate everything first: no half-built queues on failure.
    for (auto& ps : cmd.paths) {
        std::filesystem::path p(ps);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec) || ec)
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                             "track not found: " + ps)};
        if (!kanariv::media::hasAudioExt(p))
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Unsupported, "unsupported file type: " + ps)};
    }
    std::vector<int64_t> ids;
    ids.reserve(cmd.paths.size());
    for (auto& ps : cmd.paths) {
        auto trRes =
            detail::ensureLibraryTrack(*storage_.tracks, kanariv::utils::detail::pathFromUtf8(ps));
        if (!trRes)
            return std::unexpected{trRes.error()};
        ids.push_back(trRes->id);
    }
    std::string first = kanariv::utils::detail::pathToUtf8(
        kanariv::utils::detail::pathFromUtf8(cmd.paths.front()).filename());
    auto qid = storage_.queues->createQueue("temp: " + first);
    if (!qid)
        return std::unexpected{qid.error()};
    if (!cmd.save) {
        auto tr = storage_.queues->setQueueTemp(*qid, true);
        if (!tr)
            return std::unexpected{tr.error()};
    }
    auto er = detail::enqueueInChunks(*storage_.queues, *qid, ids);
    if (!er)
        return std::unexpected{er.error()};
    ServiceAccess::noteEnqueued(*engine_, *qid, ids.size());
    // Stop first: play() while playing restarts the current track instead
    // of starting the new queue.
    auto stp = engine_->stop();
    if (!stp)
        return std::unexpected{stp.error()};
    auto sw = engine_->switchQueue(*qid);
    if (!sw)
        return std::unexpected{sw.error()};
    auto pr = engine_->play(*qid);
    if (!pr)
        return std::unexpected{pr.error()};
    updateShmStatus();
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueSwitch& qs) {
    auto q = storage_.queues->getQueue(qs.qid);
    if (!q) {
        if (q.error().message.empty())
            return std::unexpected{kanariv::utils::makeError(
                q.error().code, "queue not found: " + std::to_string(qs.qid))};
        return std::unexpected{q.error()};
    }
    auto sw = engine_->switchQueue(qs.qid);
    if (!sw)
        return std::unexpected{sw.error()};
    updateShmStatus();
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueAdd& qa) {
    int64_t qid = engine_->activeQueueId();
    auto q = storage_.queues->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    // A single query is one outcome for the whole command: an unresolvable
    // query stays a command-level error (the batch form reports it per item
    // because one bad entry must not hide the others).
    auto resolved = detail::resolveAddQuery(storage_, qa.query, qa.search);
    if (!resolved)
        return std::unexpected{resolved.error()};
    std::vector<int64_t> have;
    if (auto items = storage_.queues->listQueueItems(qid)) {
        have.reserve(items->size());
        for (auto& it : *items)
            have.push_back(it.track_id);
    }
    // Same report shape as QueueAddBatch (S10): the client renders
    // added/duplicate from one vocabulary in every mode.
    QueueAddReport report;
    std::vector<int64_t> fresh;
    fresh.reserve(resolved->size());
    for (auto& t : *resolved) {
        QueueAddItem item;
        item.query = qa.query;
        const bool queued = std::find(have.begin(), have.end(), t.id) != have.end();
        item.status = queued ? "duplicate" : "added";
        if (!queued) {
            have.push_back(t.id);
            fresh.push_back(t.id);
        }
        item.track = std::move(t);
        report.items.push_back(std::move(item));
    }
    if (!fresh.empty()) {
        auto er = detail::enqueueInChunks(*storage_.queues, qid, fresh);
        if (!er)
            return std::unexpected{er.error()};
        ServiceAccess::noteEnqueued(*engine_, qid, fresh.size());
        noteStateChanged();
    }
    updateShmStatus();
    return Result{std::move(report)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueAddBatch& qb) {
    int64_t qid = engine_->activeQueueId();
    auto q = storage_.queues->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    std::vector<int64_t> have;
    if (auto items = storage_.queues->listQueueItems(qid)) {
        have.reserve(items->size());
        for (auto& it : *items)
            have.push_back(it.track_id);
    }
    QueueAddReport report;
    std::vector<int64_t> fresh;
    for (const auto& query : qb.queries) {
        auto resolved = detail::resolveAddQuery(storage_, query, qb.search);
        if (!resolved) {
            QueueAddItem item;
            item.query = query;
            item.status = "failed";
            // NotFound means the query matched nothing; the client echoes the
            // query, so "not found" is the whole detail. Anything else keeps
            // its own message.
            item.detail = resolved.error().code == kanariv::utils::StatusCode::NotFound
                              ? std::string{"not found"}
                              : resolved.error().message;
            report.items.push_back(std::move(item));
            continue;
        }
        for (auto& t : *resolved) {
            QueueAddItem item;
            item.query = query;
            const bool queued = std::find(have.begin(), have.end(), t.id) != have.end();
            item.status = queued ? "duplicate" : "added";
            if (!queued) {
                have.push_back(t.id);
                fresh.push_back(t.id);
            }
            item.track = std::move(t);
            report.items.push_back(std::move(item));
        }
    }
    if (!fresh.empty()) {
        auto er = detail::enqueueInChunks(*storage_.queues, qid, fresh);
        if (!er) {
            // K5: chunks committed before the failure stay. Re-read the queue
            // so each item reports what actually landed.
            const auto landed = storage_.queues->listQueueItems(qid);
            std::size_t landedCount = 0;
            for (auto& item : report.items) {
                if (item.status != "added")
                    continue;
                const bool present =
                    landed && std::any_of(landed->begin(), landed->end(), [&](const auto& row) {
                        return row.track_id == item.track.id;
                    });
                if (present) {
                    ++landedCount;
                    continue;
                }
                item.status = "failed";
                item.detail = er.error().message;
            }
            if (landedCount == 0)
                return std::unexpected{er.error()};
            ServiceAccess::noteEnqueued(*engine_, qid, landedCount);
            noteStateChanged();
        } else {
            ServiceAccess::noteEnqueued(*engine_, qid, fresh.size());
            noteStateChanged();
        }
    }
    updateShmStatus();
    return Result{std::move(report)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueRemove& qr) {
    int64_t qid = engine_->activeQueueId();
    auto q = storage_.queues->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    auto selector = detail::parseQueueSelector(qr.idOrIndex);
    if (!selector) {
        return std::unexpected{kanariv::utils::makeError(
            selector.error().code, "queue remove: " + selector.error().message)};
    }
    int64_t index = selector->number;
    if (selector->byTrackId) {
        auto found = storage_.queues->queueIndexOf(qid, selector->number);
        if (!found)
            return std::unexpected{found.error()};
        if (!*found)
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                             "queue item not found")};
        index = static_cast<int64_t>(**found);
    }
    auto removedRes = storage_.queues->queueRemove(qid, index);
    if (!removedRes)
        return std::unexpected{removedRes.error()};
    auto tracks = storage_.queues->tracksForQueue(qid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    updateShmStatus();
    noteStateChanged();
    return Result{QueueTracks{std::move(*tracks)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueRemoveBatch& qb) {
    int64_t qid = engine_->activeQueueId();
    auto q = storage_.queues->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    QueueRemoveReport report;
    bool changed = false;
    // Selectors apply in request order, each against the queue as left by the
    // previous one; positions therefore shift between entries.
    for (const auto& raw : qb.selectors) {
        QueueRemoveItem item;
        item.selector = raw;
        auto selector = detail::parseQueueSelector(raw);
        if (!selector) {
            item.status = "failed";
            item.detail = selector.error().message;
            report.items.push_back(std::move(item));
            continue;
        }
        auto tracks = storage_.queues->tracksForQueue(qid);
        if (!tracks)
            return std::unexpected{tracks.error()};
        int64_t index = selector->number;
        if (selector->byTrackId) {
            auto found = storage_.queues->queueIndexOf(qid, selector->number);
            if (!found)
                return std::unexpected{found.error()};
            if (!*found) {
                item.status = "missing";
                report.items.push_back(std::move(item));
                continue;
            }
            index = static_cast<int64_t>(**found);
        }
        if (index < 0 || static_cast<std::size_t>(index) >= tracks->size()) {
            item.status = "missing";
            report.items.push_back(std::move(item));
            continue;
        }
        item.track = (*tracks)[static_cast<std::size_t>(index)];
        auto removedRes = storage_.queues->queueRemove(qid, index);
        if (!removedRes) {
            item.track = kanariv::types::TrackRow{};
            item.status = removedRes.error().code == kanariv::utils::StatusCode::NotFound
                              ? "missing"
                              : "failed";
            item.detail = removedRes.error().message;
            report.items.push_back(std::move(item));
            continue;
        }
        item.status = "removed";
        changed = true;
        report.items.push_back(std::move(item));
    }
    if (changed) {
        updateShmStatus();
        noteStateChanged();
    }
    return Result{std::move(report)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueMove& qm) {
    int64_t qid = engine_->activeQueueId();
    auto q = storage_.queues->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    auto moved = storage_.queues->queueMove(qid, qm.from, qm.to);
    if (!moved)
        return std::unexpected{moved.error()};
    // Playback order is engine state: the row moved, so let the engine remap
    // its permutation before the client reads the playback order back.
    ServiceAccess::noteQueueMoved(*engine_, qid, qm.from, qm.to);
    auto tracks = storage_.queues->tracksForQueue(qid);
    if (!tracks)
        return std::unexpected{tracks.error()};
    updateShmStatus();
    noteStateChanged();
    return Result{QueueTracks{std::move(*tracks)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueClear&) {
    int64_t qid = engine_->activeQueueId();
    auto q = storage_.queues->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    auto r = storage_.queues->queueClear(qid);
    if (!r)
        return std::unexpected{r.error()};
    if (config_.clearStopsPlayback) {
        if (auto e = engine_->stop(); !e)
            return std::unexpected{e.error()};
    }
    updateShmStatus();
    noteStateChanged();
    return Result{QueueTracks{{}}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueShuffle& qs) {
    auto r = qs.on ? engine_->setShuffle(*qs.on) : engine_->toggleShuffle();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueRepeat& qr) {
    if (qr.mode.has_value()) {
        auto r = engine_->setRepeat(*qr.mode);
        if (!r)
            return std::unexpected{r.error()};
    }
    updateShmStatus();
    // Without a mode this is a read of the current repeat setting, so it must
    // not bump the change sequence (S5).
    if (qr.mode.has_value())
        return statusAfterChange();
    return statusResult();
}

} // namespace kanariv::service
