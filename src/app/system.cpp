/**
 * @file system.cpp
 * @brief History and audio-device command handlers.
 * @ingroup kanariv_app
 */

#include <algorithm>
#include <app/app_impl.hpp>
#include <app/format_internal.hpp>
#include <cmath>
#include <format>
#include <kanariv/app/app.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/math.hpp>
#include <optional>
#include <string>
#include <variant>

namespace kanariv::app {

constexpr double msToSecDivisor = 1000.0;

auto App::status() -> AppResult {
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::StatusReq{}}));
}

auto App::info() -> AppResult {
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::Info{}}));
}

auto App::setVolume(const kanariv::ipc::VolumeSet& volume) -> AppResult {
    return impl_->confirm(impl_->sendRaw(kanariv::ipc::Command{volume}));
}

auto App::historyList(int limit) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::HistoryList{limit}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

auto App::historyClear() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::HistoryClear{}};
    return impl_->confirm(impl_->sendRaw(cmd), "History cleared");
}

auto App::bookmarkAdd(std::optional<double> position, std::string note,
                      std::optional<int64_t> track) -> AppResult {
    double pos = 0.0;
    if (position.has_value()) {
        pos = *position;
    } else {
        kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}});
        if (!ps) {
            return std::unexpected{ps.error()};
        }
        auto* st = std::get_if<kanariv::ipc::Status>(&*ps);
        if (st == nullptr) {
            if (auto* e = std::get_if<kanariv::utils::Error>(&*ps)) {
                return std::unexpected{*e};
            }
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::State, "status unavailable")};
        }
        pos = st->pos;
    }
    pos = std::max(pos, 0.0);
    auto posMs = static_cast<int64_t>(std::llround(pos * msToSecDivisor));
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkAdd{
        .position_ms = posMs, .note = std::move(note), .track_id = track}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
    std::string line = std::format("Bookmark added at {}", kanariv::utils::formatClock(pos));
    return Outcome{
        .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
}

auto App::bookmarkList(std::optional<int64_t> track) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkList{track}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

auto App::bookmarkRemove(int64_t id) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkRemove{id}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Bookmark {} removed", id));
}

auto App::bookmarkGoto(int64_t id) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::BookmarkGoto{id}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
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

auto App::deviceList() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::DeviceList{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

auto App::deviceSet(const std::string& id) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::DeviceSet{id}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Default device: {}", id));
}

auto App::deviceTest(std::optional<std::string> id, bool tone) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::DeviceTest{.id = id, .tone = tone}};
    std::string line =
        id.has_value() ? std::format("Device available: {}", *id) : "Default device available";
    if (tone) {
        line += " (playing test tone)";
    }
    return impl_->confirm(impl_->sendRaw(cmd), std::move(line));
}

} // namespace kanariv::app
