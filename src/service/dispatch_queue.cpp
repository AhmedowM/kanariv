#include <algorithm>
#include <kanariv/db/database.hpp>
#include <kanariv/db/search.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <expected>
#include <filesystem>
#include <media/fingerprint.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "service_impl.hpp"

namespace kanariv::service {

namespace detail {

double durationFromDecoder(const std::filesystem::path& path) noexcept;

inline std::expected<kanariv::types::Track, kanariv::utils::Error>
ensureLibraryTrack(kanariv::contracts::IDatabase& db, const std::filesystem::path& p) {
    auto fpRes = kanariv::media::internal::computeFingerprint(p);
    if (!fpRes)
        return std::unexpected{fpRes.error()};
    kanariv::types::Track t;
    t.path = p.generic_string();
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
    auto ins = db.insertTrack(t);
    if (ins) {
        t.id = *ins;
        return t;
    }
    if (ins.error().code == kanariv::utils::StatusCode::AlreadyExists) {
        if (auto existing = db.findByFingerprint(t.fingerprint))
            return std::move(*existing);
        if (auto byPath = db.findByPath(t.path))
            return std::move(*byPath);
    }
    return std::unexpected{ins.error()};
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
        auto q = db_->getQueue(qid);
        if (!q)
            return std::unexpected{q.error()};
    }
    auto items = db_->listQueueItems(qid);
    if (!items)
        return std::unexpected{items.error()};
    std::vector<kanariv::types::Track> tracks;
    tracks.reserve(items->size());
    for (auto& it : *items) {
        auto tr = db_->getTrack(it.track_id);
        if (tr)
            tracks.push_back(std::move(*tr));
    }
    if (ql.order != "added") {
        // Playback (shuffle) order: perm holds queue positions. A stale perm
        // (queue mutated after shuffle) contributes its valid entries first;
        // anything missing follows in insertion order.
        auto perm = ServiceAccess::shufflePermFor(*engine_, qid);
        if (!perm.empty()) {
            std::vector<kanariv::types::Track> ordered;
            ordered.reserve(tracks.size());
            std::vector<char> seen(tracks.size(), 0);
            for (int64_t p : perm) {
                if (p >= 0 && static_cast<std::size_t>(p) < tracks.size() && !seen[static_cast<std::size_t>(p)]) {
                    ordered.push_back(tracks[static_cast<std::size_t>(p)]);
                    seen[static_cast<std::size_t>(p)] = 1;
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
    auto qs = db_->listQueues();
    if (!qs)
        return std::unexpected{qs.error()};
    int64_t active = engine_->activeQueueId();
    kanariv::ipc::Queues out{};
    for (auto& q : *qs) {
        auto items = db_->listQueueItems(q.id);
        std::size_t n = items ? items->size() : 0;
        out.entries.push_back(kanariv::ipc::QueueEntry{.id=q.id, .name=q.name, .tracks=n, .active=q.id == active, .temp=q.temp});
    }
    return Result{std::move(out)};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueCreate& qc) {
    if (qc.name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "queue create: missing name")};
    auto id = db_->createQueue(qc.name);
    if (!id)
        return std::unexpected{id.error()};
    // Apply the configured default repeat mode to the new queue.
    kanariv::db::Database& db = dynamic_cast<kanariv::db::Database&>(*db_);
    if (config_.repeatDefault == "one") {
        if (auto e = db.setQueueRepeat(*id, 1); !e)
            return std::unexpected{e.error()};
    } else if (config_.repeatDefault == "all") {
        if (auto e = db.setQueueRepeat(*id, 2); !e)
            return std::unexpected{e.error()};
    }
    return Result{kanariv::ipc::QueueCreated{.id=*id, .name=qc.name}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueDelete& qd) {
    if (qd.qid == engine_->activeQueueId())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "cannot delete the active queue")};
    auto q = db_->getQueue(qd.qid);
    if (!q)
        return std::unexpected{q.error()};
    auto r = db_->deleteQueue(qd.qid);
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
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Unsupported,
                                                            "unsupported file type: " + ps)};
    }
    std::vector<int64_t> ids;
    ids.reserve(cmd.paths.size());
    for (auto& ps : cmd.paths) {
        auto trRes = detail::ensureLibraryTrack(*db_, std::filesystem::path(ps));
        if (!trRes)
            return std::unexpected{trRes.error()};
        ids.push_back(trRes->id);
    }
    std::string first = std::filesystem::path(cmd.paths.front()).filename().generic_string();
    auto qid = db_->createQueue("temp: " + first);
    if (!qid)
        return std::unexpected{qid.error()};
    if (!cmd.save) {
        auto tr = db_->setQueueTemp(*qid, true);
        if (!tr)
            return std::unexpected{tr.error()};
    }
    auto er = db_->queueEnqueueBatch(*qid, ids);
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
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueSwitch& qs) {
    auto q = db_->getQueue(qs.qid);
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
    return statusResult();
}

// Forward declarations for QueueAdd helpers.
static std::expected<kanariv::types::Track, kanariv::utils::Error> addTrackByFilePath(
    kanariv::contracts::IDatabase& db, const std::filesystem::path& p);
static bool alreadyQueued(kanariv::contracts::IDatabase& db, int64_t qid,
                          int64_t trackId);
static std::expected<kanariv::types::Track, kanariv::utils::Error> addTrackByIdOrPath(
    kanariv::contracts::IDatabase& db, const std::string& query);
static void batchEnqueueTracks(kanariv::contracts::IDatabase& db, kanariv::engine::Engine& engine,
                               int64_t qid, std::vector<kanariv::types::Track>& toAdd);

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueAdd& qa) {
    int64_t qid = engine_->activeQueueId();
    auto q = db_->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    if (!qa.search) {
        std::filesystem::path p(qa.query);
        auto single = addTrackByFilePath(*db_, p);
        if (kanariv::media::hasAudioExt(p)) {
            if (!single)
                return std::unexpected{single.error()};
            if (!alreadyQueued(*db_, qid, single->id)) {
                auto eq = db_->queueEnqueue(qid, single->id);
                if (!eq)
                    return std::unexpected{eq.error()};
                ServiceAccess::noteEnqueued(*engine_, qid, 1);
            }
            updateShmStatus();
            std::vector<kanariv::types::Track> v;
            v.push_back(std::move(*single));
            return Result{QueueTracks{std::move(v)}};
        }
    }
    std::vector<kanariv::types::Track> toAdd;
    if (qa.search) {
        auto sr = kanariv::db::search(*db_, qa.query, 50);
        if (!sr)
            return std::unexpected{sr.error()};
        toAdd = std::move(*sr);
        if (toAdd.empty()) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                            "no matches for: " + qa.query)};
        }
    } else {
        auto single = addTrackByIdOrPath(*db_, qa.query);
        if (!single)
            return std::unexpected{single.error()};
        toAdd.push_back(std::move(*single));
    }
    batchEnqueueTracks(*db_, *engine_, qid, toAdd);
    updateShmStatus();
    return Result{QueueTracks{std::move(toAdd)}};
}

// Forward declarations for QueueRemove helpers.
static std::expected<int64_t, kanariv::utils::Error> parseIdOrIndex(const std::string& idOrIndex);
static bool removeByIdOrPosition(kanariv::contracts::IDatabase& db, int64_t qid, int64_t val);
static std::vector<kanariv::types::Track> collectQueueTracks(
    kanariv::contracts::IDatabase& db, const std::vector<kanariv::types::QueueItem>& items);

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueRemove& qr) {
    int64_t qid = engine_->activeQueueId();
    auto q = db_->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    // parse idOrIndex
    auto val = parseIdOrIndex(qr.idOrIndex);
    if (!val)
        return std::unexpected{val.error()};

    // try as position first
    if (!removeByIdOrPosition(*db_, qid, *val))
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                      "queue item not found")};

    auto items = db_->listQueueItems(qid);
    if (!items)
        return std::unexpected{items.error()};
    std::vector<kanariv::types::Track> tracks = collectQueueTracks(*db_, *items);
    updateShmStatus();
    return Result{QueueTracks{std::move(tracks)}};
}

// Parses idOrIndex into a signed integer. Whitespace-trimmed; negative allowed.
// Returns InvalidArg when not a pure integer.
static std::expected<int64_t, kanariv::utils::Error> parseIdOrIndex(const std::string& idOrIndex) {
    std::string s = idOrIndex;
    s.erase(0, s.find_first_not_of(" \t\n\r"));
    s.erase(s.find_last_not_of(" \t\n\r") + 1);
    if (s.empty())
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                      "invalid idOrIndex: empty")};
    bool allDigit = true;
    std::size_t off = (s[0] == '-' ? 1 : 0);
    for (std::size_t i = off; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            allDigit = false;
            break;
        }
    if (!allDigit)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                      "invalid idOrIndex: not an integer")};
    try {
        return std::stoll(s);
    } catch (...) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                      "invalid idOrIndex: out of range")};
    }
}

// Ensures the track at path p is in the library and returns it.
static std::expected<kanariv::types::Track, kanariv::utils::Error> addTrackByFilePath(
    kanariv::contracts::IDatabase& db, const std::filesystem::path& p) {
    std::error_code ec;
    bool exists = std::filesystem::exists(p, ec);
    if (!ec && !exists)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NotFound, "track not found: " + p.string())};
    auto trRes = detail::ensureLibraryTrack(db, p);
    if (!trRes)
        return std::unexpected{trRes.error()};
    return std::move(*trRes);
}

// Checks whether track_id is already present in queue qid.
static bool alreadyQueued(kanariv::contracts::IDatabase& db, int64_t qid,
                          int64_t trackId) {
    if (auto items = db.listQueueItems(qid)) {
        for (auto& it : *items) {
            if (it.track_id == trackId)
                return true;
        }
    }
    return false;
}

// Adds a single track by parsing the query as an int id, then falling back to
// findByPath, then FTS search.
static std::expected<kanariv::types::Track, kanariv::utils::Error> addTrackByIdOrPath(
    kanariv::contracts::IDatabase& db, const std::string& query) {
    // try parse as int id
    bool parsed = false;
    int64_t id = 0;
    try {
        std::string s = query;
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        s.erase(s.find_last_not_of(" \t\n\r") + 1);
        if (!s.empty()) {
            bool isNum = true;
            std::size_t off = (s[0] == '-' ? 1 : 0);
            for (std::size_t i = off; i < s.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
                    isNum = false;
                    break;
                }
            if (isNum) {
                id = std::stoll(s);
                parsed = true;
            }
        }
    } catch (...) {
    }
    if (parsed && id != 0) {
        auto tr = db.getTrack(id);
        if (!tr)
            return std::unexpected{tr.error()};
        return std::move(*tr);
    }
    // try findByPath
    auto tr = db.findByPath(query);
    if (tr)
        return std::move(*tr);
    // fallback to search via FTS (covers LIKE)
    auto sr = kanariv::db::search(db, query, 50);
    if (!sr)
        return std::unexpected{sr.error()};
    if (sr->empty())
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::NotFound, "track not found: " + query)};
    return std::move(sr->front());
}

// Batch enqueues tracks in a single transaction, skipping already-queued ids.
static void batchEnqueueTracks(kanariv::contracts::IDatabase& db, kanariv::engine::Engine& engine,
                               int64_t qid, std::vector<kanariv::types::Track>& toAdd) {
    std::vector<int64_t> fresh;
    fresh.reserve(toAdd.size());
    std::vector<int64_t> have;
    if (auto items = db.listQueueItems(qid)) {
        have.reserve(items->size());
        for (auto& it : *items)
            have.push_back(it.track_id);
    }
    for (auto& t : toAdd) {
        if (std::find(have.begin(), have.end(), t.id) == have.end()) {
            fresh.push_back(t.id);
            have.push_back(t.id);
        }
    }
    if (!fresh.empty()) {
        auto er = db.queueEnqueueBatch(qid, fresh);
        if (!er)
            throw std::runtime_error(er.error().message);
        ServiceAccess::noteEnqueued(engine, qid, fresh.size());
    }
}

// Removes a queue item by position first; if not found, looks up track_id and
// removes by position. Returns true on success.
static bool removeByIdOrPosition(kanariv::contracts::IDatabase& db, int64_t qid,
                                 int64_t val) {
    auto rm = db.queueRemove(qid, val);
    if (rm)
        return true;
    if (rm.error().code != kanariv::utils::StatusCode::NotFound)
        return false;
    // Not found as position: try track_id lookup.
    auto items = db.listQueueItems(qid);
    if (!items)
        return false;
    int64_t pos = -1;
    for (auto& it : *items) {
        if (it.track_id == val) {
            pos = it.position;
            break;
        }
    }
    if (pos < 0)
        return false;
    auto rm2 = db.queueRemove(qid, pos);
    return static_cast<bool>(rm2);
}

// Collects Track objects from queue items (skipping missing tracks).
static std::vector<kanariv::types::Track> collectQueueTracks(
    kanariv::contracts::IDatabase& db, const std::vector<kanariv::types::QueueItem>& items) {
    std::vector<kanariv::types::Track> tracks;
    for (auto& it : items) {
        auto tr = db.getTrack(it.track_id);
        if (tr)
            tracks.push_back(std::move(*tr));
    }
    return tracks;
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueMove& qm) {
    // QueueMove: reorder within queue via playlistReorder? For queue we lack direct
    // move. Simulate via remove+enqueue: fetch items, reorder vector, clear and
    // re-enqueue
    int64_t qid = engine_->activeQueueId();
    auto q = db_->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    auto items = db_->listQueueItems(qid);
    if (!items)
        return std::unexpected{items.error()};
    if (qm.from >= items->size() || qm.to >= items->size()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "move out of range")};
    }
    // collect track_ids in order
    std::vector<int64_t> ids;
    ids.reserve(items->size());
    for (auto& it : *items)
        ids.push_back(it.track_id);
    int64_t mv = ids[qm.from];
    ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(qm.from));
    ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(qm.to), mv);
    // Transactional clear+enqueue: single BEGIN IMMEDIATE/COMMIT so concurrent
    // listQueueItems never sees empty
    {
        auto r = db_->queueReplaceAll(qid, ids);
        if (!r)
            return std::unexpected{r.error()};
    }
    auto nitems = db_->listQueueItems(qid);
    if (!nitems)
        return std::unexpected{nitems.error()};
    std::vector<kanariv::types::Track> tracks;
    for (auto& it : *nitems) {
        auto tr = db_->getTrack(it.track_id);
        if (tr)
            tracks.push_back(std::move(*tr));
    }
    updateShmStatus();
    return Result{QueueTracks{std::move(tracks)}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueClear&) {
    int64_t qid = engine_->activeQueueId();
    auto q = db_->getQueue(qid);
    if (!q)
        return std::unexpected{q.error()};
    auto r = db_->queueClear(qid);
    if (!r)
        return std::unexpected{r.error()};
    if (config_.clearStopsPlayback) {
        if (auto e = engine_->stop(); !e)
            return std::unexpected{e.error()};
    }
    updateShmStatus();
    return Result{QueueTracks{{}}};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueShuffle& qs) {
    bool on;
    if (qs.on.has_value()) {
        on = *qs.on;
    } else {
        // toggle: get current state and flip
        on = !engine_->shuffle();
    }
    auto r = engine_->setShuffle(on);
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::QueueRepeat& qr) {
    if (qr.mode.has_value()) {
        auto r = engine_->setRepeat(*qr.mode);
        if (!r)
            return std::unexpected{r.error()};
    }
    // if no arg provided, just show current (statusResult already includes it)
    updateShmStatus();
    return statusResult();
}

} // namespace kanariv::service
