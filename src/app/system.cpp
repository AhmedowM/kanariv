/**
 * @file system.cpp
 * @brief History and audio-device command handlers.
 * @ingroup caudio_app
 */

#include <app/app_impl.hpp>
#include <app/format_internal.hpp>
#include <caudio/app/app.hpp>
#include <caudio/client/client.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/math.hpp>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <variant>

namespace caudio::app {

AppResult App::status() {
    return impl_->confirm(impl_->sendRaw(caudio::ipc::Command{caudio::ipc::StatusReq{}}));
}

AppResult App::info() {
    return impl_->confirm(impl_->sendRaw(caudio::ipc::Command{caudio::ipc::Info{}}));
}

AppResult App::setVolume(const caudio::ipc::VolumeSet& volume) {
    return impl_->confirm(impl_->sendRaw(caudio::ipc::Command{volume}));
}

AppResult App::historyList(int limit) {
    caudio::ipc::Command cmd{caudio::ipc::HistoryList{limit}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::historyClear() {
    caudio::ipc::Command cmd{caudio::ipc::HistoryClear{}};
    return impl_->confirm(impl_->sendRaw(cmd), "History cleared");
}

AppResult App::bookmarkAdd(std::optional<double> position, std::string note,
                           std::optional<int64_t> track) {
    double pos = 0.0;
    if (position.has_value()) {
        pos = *position;
    } else {
        caudio::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        auto ps = probe.send(caudio::ipc::Command{caudio::ipc::StatusReq{}});
        if (!ps)
            return std::unexpected{ps.error()};
        auto* st = std::get_if<caudio::ipc::Status>(&*ps);
        if (!st) {
            if (auto* e = std::get_if<caudio::utils::Error>(&*ps))
                return std::unexpected{*e};
            return std::unexpected{
                caudio::utils::makeError(caudio::utils::StatusCode::State, "status unavailable")};
        }
        pos = st->pos;
    }
    if (pos < 0.0)
        pos = 0.0;
    int64_t posMs = (int64_t)std::llround(pos * 1000.0);
    caudio::ipc::Command cmd{caudio::ipc::BookmarkAdd{posMs, std::move(note), track}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    std::string line = std::format("Bookmark added at {}", caudio::utils::formatClock(pos));
    return Outcome{
        .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
}

AppResult App::bookmarkList(std::optional<int64_t> track) {
    caudio::ipc::Command cmd{caudio::ipc::BookmarkList{track}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::bookmarkRemove(int64_t id) {
    caudio::ipc::Command cmd{caudio::ipc::BookmarkRemove{id}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Bookmark {} removed", id));
}

AppResult App::bookmarkGoto(int64_t id) {
    caudio::ipc::Command cmd{caudio::ipc::BookmarkGoto{id}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* st = std::get_if<caudio::ipc::Status>(&*res)) {
        std::string who = caudio::app::trackWho(*st);
        std::string line =
            std::format("Resuming {} from {}", who, caudio::utils::formatClock(st->pos));
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::deviceList() {
    caudio::ipc::Command cmd{caudio::ipc::DeviceList{}};
    return impl_->confirm(impl_->sendRaw(cmd));
}

AppResult App::deviceSet(const std::string& id) {
    caudio::ipc::Command cmd{caudio::ipc::DeviceSet{id}};
    return impl_->confirm(impl_->sendRaw(cmd), std::format("Default device: {}", id));
}

AppResult App::deviceTest(std::optional<std::string> id, bool tone) {
    caudio::ipc::Command cmd{caudio::ipc::DeviceTest{id, tone}};
    std::string line =
        id.has_value() ? std::format("Device available: {}", *id) : "Default device available";
    if (tone)
        line += " (playing test tone)";
    return impl_->confirm(impl_->sendRaw(cmd), std::move(line));
}

} // namespace caudio::app
