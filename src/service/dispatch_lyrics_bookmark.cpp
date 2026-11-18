/**
 * @file dispatch_lyrics_bookmark.cpp
 * @brief History, bookmark and lyrics commands.
 * @ingroup kanariv_service
 */

#include <cstdint>
#include <expected>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <string>
#include <utility>

#include "service_impl.hpp"

namespace kanariv::service {

using namespace kanariv::ipc;

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::HistoryList& cmd) {
    int limit = cmd.limit.value_or(50);
    if (limit <= 0)
        limit = 50;
    auto hist = engine_->listHistory(limit);
    if (!hist)
        return std::unexpected{hist.error()};
    return Result{kanariv::ipc::History{std::move(*hist)}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::HistoryClear&) {
    auto r = engine_->clearHistory();
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::BookmarkList& cmd) {
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    auto list = storage_.tracks->listBookmarks(tid);
    if (!list)
        return std::unexpected{list.error()};
    return Result{kanariv::ipc::Bookmarks{std::move(*list)}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::BookmarkAdd& cmd) {
    if (cmd.position_ms < 0)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "bookmark: negative position")};
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    if (auto tr = storage_.tracks->getTrack(tid); !tr)
        return std::unexpected{tr.error()};
    auto r = storage_.tracks->bookmarkAdd(tid, cmd.position_ms, cmd.note);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::BookmarkRemove& cmd) {
    auto r = storage_.tracks->bookmarkRemove(cmd.id);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::BookmarkGoto& cmd) {
    auto b = storage_.tracks->bookmarkGet(cmd.id);
    if (!b)
        return std::unexpected{b.error()};
    auto pr = engine_->playTrack(b->track_id);
    if (!pr)
        return std::unexpected{pr.error()};
    if (auto sr = engine_->seek(static_cast<double>(b->position_ms) / 1000.0); !sr)
        return std::unexpected{sr.error()};
    return statusAfterChange();
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::LyricsGet& cmd) {
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    auto l = storage_.tracks->lyricsGet(tid);
    if (!l)
        return std::unexpected{l.error()};
    return Result{kanariv::ipc::Lyrics{l->id, l->track_id, std::move(l->lrc_text), l->is_synced,
                                       std::move(l->source), l->fetched_at}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::LyricsSet& cmd) {
    if (cmd.lrc_text.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "lyrics: empty text")};
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    if (auto tr = storage_.tracks->getTrack(tid); !tr)
        return std::unexpected{tr.error()};
    auto r = storage_.tracks->lyricsSet(tid, cmd.lrc_text, cmd.is_synced, cmd.source);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::Impl::handle(const kanariv::ipc::LyricsRemove& cmd) {
    int64_t tid = cmd.track_id.value_or(engine_->currentTrackId());
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    auto r = storage_.tracks->lyricsRemove(tid);
    if (!r)
        return std::unexpected{r.error()};
    return Result{Empty{}};
}
} // namespace kanariv::service
