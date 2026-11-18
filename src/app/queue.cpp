/**
 * @file queue.cpp
 * @brief Queue command handlers.
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders. Listing
 * with playback-order highlight, switching, creation/deletion, the three
 * add modes (playlist / id-or-search / paths), removal by id/pos/path,
 * move, clear, shuffle and repeat. Flag-combination validation travels with
 * the handlers; glob expansion uses the shared paths utilities.
 */

#include <algorithm>
#include <app/app_impl.hpp>
#include <app/detail.hpp>
#include <app/format_internal.hpp>
#include <app/paths_internal.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <kanariv/app/app.hpp>
#include <kanariv/app/paths.hpp>
#include <kanariv/client/output_formatter.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/parse.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::app {

namespace {

/** Rebuilds the QueueTracks JSON shape from a batch add report. */
kanariv::ipc::Result collectAddedTracks(const kanariv::ipc::Result& res) {
    kanariv::ipc::QueueTracks qt{};
    if (auto* ar = std::get_if<kanariv::ipc::QueueAddReport>(&res)) {
        for (const auto& item : ar->items) {
            if (item.track.id != 0)
                qt.tracks.push_back(item.track);
        }
    }
    return kanariv::ipc::Result{std::move(qt)};
}

bool anyItemFailed(const kanariv::ipc::QueueAddReport& report) {
    return std::any_of(report.items.begin(), report.items.end(),
                       [](const auto& item) { return item.status == "failed"; });
}

} // namespace

AppResult App::queueList() {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueQueues{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

std::expected<TrackList, kanariv::utils::Error> App::queueTracks(const std::string& order) {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueList{order}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    int64_t curId = 0;
    auto probe = impl_->makeClient();
    if (auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}})) {
        if (auto* st = std::get_if<kanariv::ipc::Status>(&*ps))
            curId = st->track_id;
    }
    return TrackList{std::move(*res), curId};
}

AppResult App::queueSwitch(std::int64_t qid) {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueSwitch{qid}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Switched to queue {}", qid));
}

AppResult App::queueCreate(const std::string& name) {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueCreate{name}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* qc = std::get_if<kanariv::ipc::QueueCreated>(&*res)) {
        // Hoisted: copy before Outcome moves *res (moved-from strings read
        // empty on some toolchains; see App::resume).
        std::int64_t id = qc->id;
        std::string qName = qc->name;
        return Outcome{.result = std::move(*res),
                       .line = std::format("Created queue {} '{}'", id, qName),
                       .toStderr = false,
                       .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::queueDelete(std::int64_t qid) {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueDelete{qid}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Deleted queue {}", qid));
}

BatchResult App::queueAdd(const std::vector<std::string>& paths, const std::string& id, bool search,
                          std::int64_t playlist, bool replace, bool recursive) {
    bool hasId = !id.empty();
    bool hasPlaylist = playlist != 0;
    if (replace && !hasPlaylist)
        return BatchReport::fail("queue add: --replace needs --playlist");
    if (hasPlaylist && (hasId || search || !paths.empty()))
        return BatchReport::fail("queue add: --playlist takes no PATH, --id, or --search");
    if (hasId && (!paths.empty() || search))
        return BatchReport::fail("queue add: --id takes no PATH or --search");
    if (search && paths.size() != 1)
        return BatchReport::fail("queue add: --search takes exactly one query");
    if (hasId && !kanariv::app::isNumeric(id))
        return BatchReport::fail("queue add: --id needs a numeric library id");
    if (!hasId && !search && !hasPlaylist && paths.empty())
        return BatchReport::fail("queue add: need a PATH, --id ID, or --search QUERY");
    if (hasPlaylist && replace) {
        auto clr = impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::QueueClear{}});
        if (!clr)
            return std::unexpected{clr.error()};
    }
    BatchReport rep;
    if (hasPlaylist) {
        kanariv::ipc::Command tcmd{kanariv::ipc::PlaylistTracks{playlist}};
        auto tres = impl_->sendRaw(tcmd);
        if (!tres)
            return std::unexpected{tres.error()};
        auto* pd = std::get_if<kanariv::ipc::PlaylistData>(&*tres);
        if (!pd) {
            detail::appendBlock(rep.out, kanariv::client::formatResult(*tres));
            rep.exitCode = 0;
            return rep;
        }
        if (pd->tracks.empty())
            return BatchReport::fail(std::format("queue add: playlist {} has no tracks", playlist));
        std::vector<std::string> queries;
        queries.reserve(pd->tracks.size());
        for (auto& t : pd->tracks)
            queries.push_back(std::to_string(t.id));
        kanariv::ipc::Command cmd{
            kanariv::ipc::QueueAddBatch{.queries = std::move(queries), .search = false}};
        auto res = impl_->sendRaw(cmd);
        if (!res)
            return std::unexpected{res.error()};
        int added = detail::printAddReportText(*res, rep.out, rep.err);
        rep.json = collectAddedTracks(*res);
        detail::countLineText(added, rep.out);
        rep.exitCode = (added == 0) ? 1 : 0;
        return rep;
    }
    if (hasId || search) {
        kanariv::ipc::Command cmd{
            kanariv::ipc::QueueAdd{.query = hasId ? id : paths[0], .search = search}};
        auto res = impl_->sendRaw(cmd);
        if (!res)
            return std::unexpected{res.error()};
        bool failed = false;
        if (auto* ar = std::get_if<kanariv::ipc::QueueAddReport>(&*res))
            failed = anyItemFailed(*ar);
        int added = detail::printAddReportText(*res, rep.out, rep.err);
        rep.json = collectAddedTracks(*res);
        detail::countLineText(added, rep.out);
        rep.exitCode = (added == 0 && failed) ? 1 : 0;
        return rep;
    }
    // PATH mode: expand globs/folders client-side.
    std::vector<std::string> files;
    std::vector<std::string> unmatched;
    for (auto& tok : paths)
        kanariv::app::expandAddToken(tok, recursive, files, unmatched);
    bool hardFail = false;
    for (auto& u : unmatched) {
        if (kanariv::app::isNumeric(u)) {
            detail::emitLine(rep.err, std::format("queue add: '{}' is not a file (use --id for "
                                                  "library ids)",
                                                  u));
            hardFail = true;
        } else if (kanariv::app::hasGlobChars(u)) {
            detail::emitLine(rep.err, std::format("No files matched: {}", u));
        } else {
            std::error_code ec;
            if (std::filesystem::is_directory(u, ec) && !ec)
                detail::emitLine(rep.err, std::format("No files matched: {}", u));
            else {
                detail::emitLine(rep.err, std::format("queue add: no such file: {}", u));
                hardFail = true;
            }
        }
    }
    int added = 0;
    if (!files.empty()) {
        kanariv::ipc::Command cmd{
            kanariv::ipc::QueueAddBatch{.queries = std::move(files), .search = false}};
        auto res = impl_->sendRaw(cmd);
        if (!res)
            return std::unexpected{res.error()};
        if (auto* ar = std::get_if<kanariv::ipc::QueueAddReport>(&*res))
            hardFail = hardFail || anyItemFailed(*ar);
        added = detail::printAddReportText(*res, rep.out, rep.err);
        rep.json = collectAddedTracks(*res);
    } else {
        rep.json = kanariv::ipc::Result{kanariv::ipc::QueueTracks{}};
    }
    detail::countLineText(added, rep.out);
    rep.exitCode = (added == 0 && hardFail) ? 1 : 0;
    return rep;
}

BatchResult App::queueRemove(const std::string& id, const std::string& pos,
                             const std::vector<std::string>& paths, bool recursive) {
    bool hasId = !id.empty();
    bool hasPos = !pos.empty();
    bool hasPaths = !paths.empty();
    int modes = (hasId ? 1 : 0) + (hasPos ? 1 : 0) + (hasPaths ? 1 : 0);
    if (modes == 0)
        return BatchReport::fail("queue remove: need PATH, --id ID, or --pos POS");
    if (modes > 1)
        return BatchReport::fail("queue remove: PATH, --id, and --pos are exclusive");
    if ((hasId && !kanariv::app::isNumeric(id)) || (hasPos && !kanariv::app::isNumeric(pos)))
        return BatchReport::fail("queue remove: --id and --pos need numeric values");
    // Snapshot insertion-order positions with labels for output.
    struct RemTarget {
        std::size_t pos{0};
        kanariv::types::TrackRow track{};
    };
    std::vector<RemTarget> all;
    {
        auto probe = impl_->makeClient();
        auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::QueueList{"added"}});
        if (!ps)
            return std::unexpected{ps.error()};
        if (auto* qt = std::get_if<kanariv::ipc::QueueTracks>(&*ps)) {
            for (std::size_t i = 0; i < qt->tracks.size(); ++i)
                all.push_back(RemTarget{.pos = i, .track = qt->tracks[i]});
        } else {
            BatchReport rep;
            detail::appendBlock(rep.out, kanariv::client::formatResult(*ps));
            rep.exitCode = 0;
            return rep;
        }
    }
    BatchReport rep;
    std::vector<RemTarget> targets;
    bool hardFail = false;
    auto parseNum = [&](const std::string& s, long long& out) {
        if (const auto parsed = kanariv::utils::parseId(s)) {
            out = *parsed;
            return true;
        }
        detail::emitLine(rep.err, std::format("queue remove: invalid number: {}", s));
        return false;
    };
    if (hasPos) {
        long long p = 0;
        if (!parseNum(pos, p))
            return BatchReport{.json = std::nullopt,
                               .out = std::move(rep.out),
                               .err = std::move(rep.err),
                               .exitCode = 1};
        if (p < 0 || static_cast<std::size_t>(p) >= all.size())
            return BatchReport{.json = std::nullopt,
                               .out = "",
                               .err = std::format("queue remove: position out of range: {}", p),
                               .exitCode = 1};
        targets.push_back(all[static_cast<std::size_t>(p)]);
    } else if (hasId) {
        long long idv = 0;
        if (!parseNum(id, idv))
            return BatchReport{.json = std::nullopt,
                               .out = std::move(rep.out),
                               .err = std::move(rep.err),
                               .exitCode = 1};
        for (auto& t : all) {
            if (t.track.id == idv)
                targets.push_back(t);
        }
        if (targets.empty())
            return BatchReport{.json = std::nullopt,
                               .out = "",
                               .err =
                                   std::format("queue remove: track {} is not in the queue", idv),
                               .exitCode = 1};
    } else {
        std::vector<std::string> files;
        std::vector<std::string> unmatched;
        for (auto& tok : paths)
            kanariv::app::expandAddToken(tok, recursive, files, unmatched);
        for (auto& u : unmatched) {
            if (kanariv::app::isNumeric(u)) {
                detail::emitLine(rep.err, std::format("queue remove: '{}' is not a file (use "
                                                      "--id/--pos for ids)",
                                                      u));
                hardFail = true;
            } else if (kanariv::app::hasGlobChars(u)) {
                detail::emitLine(rep.err, std::format("No files matched: {}", u));
            } else {
                std::error_code ec;
                if (std::filesystem::is_directory(u, ec) && !ec)
                    detail::emitLine(rep.err, std::format("No files matched: {}", u));
                else {
                    detail::emitLine(rep.err, std::format("queue remove: no such file: {}", u));
                    hardFail = true;
                }
            }
        }
        std::vector<bool> taken(all.size(), false);
        for (auto& f : files) {
            std::string key = kanariv::app::pathKey(f);
            bool found = false;
            for (std::size_t i = 0; i < all.size(); ++i) {
                if (!taken[i] && kanariv::app::pathKey(all[i].track.path) == key) {
                    targets.push_back(all[i]);
                    taken[i] = true;
                    found = true;
                }
            }
            if (!found) {
                detail::emitLine(rep.err, std::format("queue remove: not in queue: {}", f));
                hardFail = true;
            }
        }
    }
    // Remove descending so positions stay valid.
    std::sort(targets.begin(), targets.end(),
              [](const RemTarget& a, const RemTarget& b) { return a.pos > b.pos; });
    int removed = 0;
    std::vector<kanariv::types::TrackRow> removedTracks;
    std::vector<std::string> selectors;
    selectors.reserve(targets.size());
    for (auto& t : targets) {
        // Always send the discriminator (A2a): a bare number would be resolved
        // as a position and could delete a different row. `#id:` is resolved
        // server-side under the queue lock, so an --id removal cannot be
        // retargeted by a concurrent change; `#pos:` targets stay valid because
        // the batch applies selectors in request order and they are sorted
        // descending, so earlier removals never shift a later target.
        selectors.push_back(hasId ? std::format("#id:{}", t.track.id)
                                  : std::format("#pos:{}", t.pos));
    }
    if (!selectors.empty()) {
        kanariv::ipc::Command cmd{
            kanariv::ipc::QueueRemoveBatch{.selectors = std::move(selectors)}};
        auto res = impl_->sendRaw(cmd);
        if (!res)
            return std::unexpected{res.error()};
        removed = detail::printRemoveReportText(*res, rep.out, rep.err);
        if (auto* rr = std::get_if<kanariv::ipc::QueueRemoveReport>(&*res)) {
            for (auto& item : rr->items) {
                if (item.status == "removed")
                    removedTracks.push_back(item.track);
            }
        }
    }
    rep.json = kanariv::ipc::Result{kanariv::ipc::QueueTracks{std::move(removedTracks)}};
    rep.exitCode = (removed == 0) ? ((targets.empty() && !hardFail) ? 0 : 1) : 0;
    return rep;
}

AppResult App::queueMove(std::size_t from, std::size_t to) {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueMove{.from = from, .to = to}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    std::size_t n = 0;
    if (auto* qt = std::get_if<kanariv::ipc::QueueTracks>(&*res))
        n = qt->tracks.size();
    return Outcome{.result = std::move(*res),
                   .line = std::format("Moved to position {}. Queue: {} tracks", to, n),
                   .toStderr = false,
                   .failed = false};
}

AppResult App::queueClear() {
    kanariv::ipc::Command cmd{kanariv::ipc::QueueClear{}};
    return impl_->confirm(impl_->sendRaw(cmd), "Queue cleared");
}

AppResult App::queueShuffle(const std::string& mode) {
    std::optional<bool> on;
    if (mode == "on")
        on = true;
    else if (mode == "off")
        on = false;
    else if (!mode.empty())
        return Outcome::fail(std::format("shuffle: invalid mode '{}' (expected on|off)", mode));
    kanariv::ipc::Command cmd{kanariv::ipc::QueueShuffle{on}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    // Report the resulting state (the daemon answers Status).
    bool stateOn = on.value_or(false);
    bool known = on.has_value();
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        stateOn = st->shuffle;
        known = true;
    }
    if (!known)
        return Outcome{.result = std::move(*res),
                       .line = "Shuffle toggled",
                       .toStderr = false,
                       .failed = false};
    return Outcome{.result = std::move(*res),
                   .line = std::format("Shuffle: {}", stateOn ? "on" : "off"),
                   .toStderr = false,
                   .failed = false};
}

AppResult App::queueRepeat(const std::string& mode) {
    using RM = kanariv::types::RepeatMode;
    std::optional<RM> m;
    if (mode == "off")
        m = RM::Off;
    else if (mode == "one")
        m = RM::One;
    else if (mode == "all")
        m = RM::All;
    else if (!mode.empty())
        return Outcome::fail(std::format("repeat: invalid mode '{}' (expected off|one|all)", mode));
    else {
        // Bare repeat cycles off -> all -> one -> off.
        auto probe = impl_->makeClient();
        if (auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}})) {
            if (auto* st = std::get_if<kanariv::ipc::Status>(&*ps)) {
                if (st->repeat == RM::Off)
                    m = RM::All;
                else if (st->repeat == RM::All)
                    m = RM::One;
                else
                    m = RM::Off;
            }
        }
        if (!m.has_value())
            return Outcome::fail("repeat: could not read current mode");
    }
    kanariv::ipc::Command cmd{kanariv::ipc::QueueRepeat{m}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    RM finalMode = *m;
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res))
        finalMode = st->repeat;
    return Outcome{.result = std::move(*res),
                   .line = std::format("Repeat: {}", finalMode == RM::All   ? "all"
                                                     : finalMode == RM::One ? "one"
                                                                            : "off"),
                   .toStderr = false,
                   .failed = false};
}

} // namespace kanariv::app
