/**
 * @file system.cpp
 * @brief History and audio-device command handlers.
 * @ingroup kanariv_app
 */

#include <app/app_impl.hpp>
#include <app/format_internal.hpp>
#include <kanariv/app/app.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/math.hpp>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <variant>

namespace kanariv::app {

AppResult App::status() {
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::StatusReq{}}));
}

AppResult App::info() {
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::Info{}}));
}

AppResult App::setVolume(const kanariv::ipc::VolumeSet& volume) {
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{volume}));
}

AppResult App::historyList(int limit) {
    kanariv::ipc::Command cmd{kanariv::ipc::HistoryList{limit}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::historyClear() {
    kanariv::ipc::Command cmd{kanariv::ipc::HistoryClear{}};
    return impl_->confirm(impl_->sendRaw(cmd), "History cleared");
}

AppResult App::bookmarkAdd(std::optional<double> position, std::string note,
                           std::optional<int64_t> track) {
    double pos = 0.0;
    if (position.has_value()) {
        pos = *position;
    } else {
        kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}});
        if (!ps)
            return std::unexpected{ps.error()};
        auto* st = std::get_if<kanariv::ipc::Status>(&*ps);
        if (!st) {
            if (auto* e = std::get_if<kanariv::utils::Error>(&*ps))
                return std::unexpected{*e};
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::State, "status unavailable")};
        }
        pos = st->pos;
    }
    if (pos < 0.0)
        pos = 0.0;
    int64_t posMs = (int64_t)std::llround(pos * 1000.0);
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkAdd{posMs, std::move(note), track}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    std::string line = std::format("Bookmark added at {}", kanariv::utils::formatClock(pos));
    return Outcome{
        .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
}

AppResult App::bookmarkList(std::optional<int64_t> track) {
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkList{track}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::bookmarkRemove(int64_t id) {
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkRemove{id}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Bookmark {} removed", id));
}

AppResult App::bookmarkGoto(int64_t id) {
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkGoto{id}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        std::string who = kanariv::app::trackWho(*st);
        std::string line =
            std::format("Resuming {} from {}", who, kanariv::utils::formatClock(st->pos));
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::lyricsSet(std::string lrc_text, bool is_synced, std::string source,
                         std::optional<std::string> track) {
    int64_t tid = 0;
    if (track.has_value()) {
        const auto& t = *track;
        bool numeric = !t.empty();
        for (char c : t) {
            if (!std::isdigit((unsigned char)c)) {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            try {
                tid = std::stoll(t);
            } catch (...) {
                tid = 0;
            }
        } else {
            kanariv::ipc::Command cmd{kanariv::ipc::TagGet{.path = t}};
            auto tgRes = impl_->sendRaw(cmd);
            if (!tgRes)
                return std::unexpected{tgRes.error()};
            if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*tgRes))
                tid = st->track.id;
            else
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::NotFound,
                    "track not found for '" + t + "'")};
        }
    } else {
        kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}});
        if (!ps)
            return std::unexpected{ps.error()};
        auto* st = std::get_if<kanariv::ipc::Status>(&*ps);
        if (!st) {
            if (auto* e = std::get_if<kanariv::utils::Error>(&*ps))
                return std::unexpected{*e};
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::State, "status unavailable")};
        }
        tid = st->track_id;
    }
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    kanariv::ipc::Command cmd{kanariv::ipc::LyricsSet{std::move(lrc_text), is_synced,
                                                      std::move(source), tid}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    return Outcome{
        .result = std::move(*res),
        .line = std::format("Lyrics set for track {}", tid),
        .toStderr = false,
        .failed = false};
}

AppResult App::lyricsGet(std::optional<std::string> track) {
    int64_t tid = 0;
    if (track.has_value()) {
        const auto& t = *track;
        bool numeric = !t.empty();
        for (char c : t) {
            if (!std::isdigit((unsigned char)c)) {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            try {
                tid = std::stoll(t);
            } catch (...) {
                tid = 0;
            }
        } else {
            kanariv::ipc::Command cmd{kanariv::ipc::TagGet{.path = t}};
            auto tgRes = impl_->sendRaw(cmd);
            if (!tgRes)
                return std::unexpected{tgRes.error()};
            if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*tgRes))
                tid = st->track.id;
            else
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::NotFound,
                    "track not found for '" + t + "'")};
        }
    } else {
        kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}});
        if (!ps)
            return std::unexpected{ps.error()};
        auto* st = std::get_if<kanariv::ipc::Status>(&*ps);
        if (!st) {
            if (auto* e = std::get_if<kanariv::utils::Error>(&*ps))
                return std::unexpected{*e};
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::State, "status unavailable")};
        }
        tid = st->track_id;
    }
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    kanariv::ipc::Command cmd{kanariv::ipc::LyricsGet{tid}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* ly = std::get_if<kanariv::ipc::Lyrics>(&*res)) {
        std::string line;
        if (!ly->lrc_text.empty())
            line = std::format("Lyrics for track {} ({}):\n{}", ly->track_id,
                               ly->is_synced ? "synced" : "plain", ly->lrc_text);
        else
            line = std::format("No lyrics text for track {}", ly->track_id);
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::lyricsRemove(std::optional<std::string> track) {
    int64_t tid = 0;
    if (track.has_value()) {
        const auto& t = *track;
        bool numeric = !t.empty();
        for (char c : t) {
            if (!std::isdigit((unsigned char)c)) {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            try {
                tid = std::stoll(t);
            } catch (...) {
                tid = 0;
            }
        } else {
            kanariv::ipc::Command cmd{kanariv::ipc::TagGet{.path = t}};
            auto tgRes = impl_->sendRaw(cmd);
            if (!tgRes)
                return std::unexpected{tgRes.error()};
            if (auto* st = std::get_if<kanariv::ipc::SingleTrack>(&*tgRes))
                tid = st->track.id;
            else
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::NotFound,
                    "track not found for '" + t + "'")};
        }
    } else {
        kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}});
        if (!ps)
            return std::unexpected{ps.error()};
        auto* st = std::get_if<kanariv::ipc::Status>(&*ps);
        if (!st) {
            if (auto* e = std::get_if<kanariv::utils::Error>(&*ps))
                return std::unexpected{*e};
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::State, "status unavailable")};
        }
        tid = st->track_id;
    }
    if (tid == 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "no current track")};
    kanariv::ipc::Command cmd{kanariv::ipc::LyricsRemove{tid}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    return Outcome{
        .result = std::move(*res),
        .line = std::format("Lyrics removed for track {}", tid),
        .toStderr = false,
        .failed = false};
}

AppResult App::deviceList() {
    kanariv::ipc::Command cmd{kanariv::ipc::DeviceList{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::deviceSet(const std::string& id) {
    kanariv::ipc::Command cmd{kanariv::ipc::DeviceSet{id}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Default device: {}", id));
}

AppResult App::deviceTest(std::optional<std::string> id, bool tone) {
    kanariv::ipc::Command cmd{kanariv::ipc::DeviceTest{id, tone}};
    std::string line =
        id.has_value() ? std::format("Device available: {}", *id) : "Default device available";
    if (tone)
        line += " (playing test tone)";
    return impl_->confirm(impl_->sendRaw(cmd), std::move(line));
}

} // namespace kanariv::app
