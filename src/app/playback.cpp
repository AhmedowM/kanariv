/**
 * @file playback.cpp
 * @brief Playback command handlers (transport, direct play, seek).
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders. Shared play
 * flow, direct file play, transport commands and seeking. Argument *syntax*
 * (time/volume grammar, globs) stays in the shell; these take parsed values.
 */

#include <algorithm>
#include <app/app_impl.hpp>
#include <app/format_internal.hpp>
#include <chrono>
#include <expected>
#include <format>
#include <kanariv/app/app.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/math.hpp>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::app {

constexpr double resumeToleranceSec = 0.5;
constexpr int busyRetryMs = 50;

auto App::Impl::transportWho(const std::expected<kanariv::ipc::Result, kanariv::utils::Error>& res)
    -> std::string {
    if (const auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        return kanariv::app::trackWho(*st);
    }
    return std::string{"unknown track"};
}

// Shared play flow (used by `play`, and by `resume` when stopped).
auto App::play() -> AppResult {
    // Prior state decides the wording (resumed vs fresh); one extra roundtrip.
    bool wasPaused = false;
    bool wasStopped = false;
    double priorPos = 0;
    int64_t priorTrack = 0;
    {
        kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
        if (auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}})) {
            if (auto* st = std::get_if<kanariv::ipc::Status>(&*ps)) {
                wasPaused = (st->state == kanariv::types::PlaybackState::Paused);
                wasStopped = (st->state == kanariv::types::PlaybackState::Stopped);
                priorPos = st->pos;
                priorTrack = st->track_id;
            }
        }
    }
    kanariv::ipc::Command cmd{kanariv::ipc::Play{}};
    auto res = impl_->sendPlay(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        // Session restore: after a restart the persisted track/position is
        // reported while stopped. The daemon seeks its decoder before output
        // starts, so the status Play returned is already at the resume point;
        // an explicit Seek then would only reset the ring a second time.
        bool restored = false;
        if (wasStopped && priorTrack != 0 && priorPos > 0.0 && st->track_id == priorTrack) {
            restored = st->pos >= priorPos - resumeToleranceSec &&
                       st->pos <= priorPos + resumeToleranceSec;
            if (!restored) {
                if (auto seekRes =
                        impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::Seek{priorPos}})) {
                    if (auto fresh =
                            impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::StatusReq{}})) {
                        if (std::holds_alternative<kanariv::ipc::Status>(*fresh)) {
                            res = std::move(fresh);
                        }
                    }
                    restored = true;
                }
            }
        }
        std::string who = impl_->transportWho(res);
        std::string line =
            wasPaused
                ? std::format("Resuming {} from {}", who, kanariv::utils::formatClock(priorPos))
            : restored
                ? std::format("Resuming {} from {}", who, kanariv::utils::formatClock(priorPos))
                : std::format("Playing {}", who);
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

auto App::playFiles(const std::vector<std::string>& files, bool save) -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::PlayFiles{.paths = files, .save = save}};
    auto res = impl_->sendPlay(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        std::string who = kanariv::app::trackWho(*st);
        std::string line = save ? std::format("Playing {}", who)
                                : std::format("Playing {} (temporary queue)", who);
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

auto App::pause() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::Pause{}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::State && e.message == "not playing") {
            return Outcome::warn("pause: nothing playing");
        }
        return std::unexpected{e};
    }
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        std::string who = impl_->transportWho(res);
        return Outcome{
            .result = std::move(*res),
            .line = std::format("Paused {} at {}", who, kanariv::utils::formatClock(st->pos)),
            .toStderr = false,
            .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

auto App::resume() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::Resume{}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::State && e.message == "not paused") {
            // Forgiving resume: playing -> warn; stopped -> play from cursor.
            bool playing = false;
            bool probed = false;
            kanariv::client::Client probe{impl_->config_.dbPath, impl_->config_.socketPath};
            if (auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}})) {
                if (auto* st = std::get_if<kanariv::ipc::Status>(&*ps)) {
                    probed = true;
                    playing = (st->state == kanariv::types::PlaybackState::Playing);
                }
            }
            if (probed && playing) {
                return Outcome::warn("resume: already playing");
            }
            if (!probed) {
                return std::unexpected{e};
            }
            return play();
        }
        return std::unexpected{e};
    }
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        // Hoisted: the line must be built before Outcome moves *res (the
        // braced list otherwise reads moved-from strings on some toolchains).
        std::string who = kanariv::app::trackWho(*st);
        std::string at = kanariv::utils::formatClock(st->pos);
        return Outcome{.result = std::move(*res),
                       .line = std::format("Resuming {} from {}", who, at),
                       .toStderr = false,
                       .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

auto App::restart() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::Restart{}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
    // Hoisted: read before Outcome moves *res (see resume()).
    std::string who = impl_->transportWho(res);
    return Outcome{.result = std::move(*res),
                   .line = std::format("Restarting {}", who),
                   .toStderr = false,
                   .failed = false};
}

auto App::stop() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::Stop{}};
    return impl_->confirm(impl_->sendRaw(cmd), "Stopped");
}

auto App::next() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::Next{}};
    auto res = impl_->sendRaw(cmd);
    if (!res && res.error().code == kanariv::utils::StatusCode::Busy) {
        // Transient queue-lock contention (racing gapless advance or a
        // sibling command): retry once after a beat before surfacing it.
        std::this_thread::sleep_for(std::chrono::milliseconds(busyRetryMs));
        res = impl_->sendRaw(cmd);
    }
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::Busy) {
            return Outcome::fail("next: engine busy, retry");
        }
        return std::unexpected{e};
    }
    // Hoisted: read before Outcome moves *res (see resume()).
    std::string who = impl_->transportWho(res);
    return Outcome{.result = std::move(*res),
                   .line = std::format("Playing {}", who),
                   .toStderr = false,
                   .failed = false};
}

auto App::prev() -> AppResult {
    kanariv::ipc::Command cmd{kanariv::ipc::Prev{}};
    auto res = impl_->sendRaw(cmd);
    if (!res && res.error().code == kanariv::utils::StatusCode::Busy) {
        // Same transient contention as next(): retry once (see above).
        std::this_thread::sleep_for(std::chrono::milliseconds(busyRetryMs));
        res = impl_->sendRaw(cmd);
    }
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::Busy) {
            return Outcome::fail("prev: engine busy, retry");
        }
        if (e.code == kanariv::utils::StatusCode::NotFound && e.message == "at start") {
            return Outcome::warn("prev: at queue start");
        }
        return std::unexpected{e};
    }
    // Hoisted: read before Outcome moves *res (see resume()).
    std::string who = impl_->transportWho(res);
    return Outcome{.result = std::move(*res),
                   .line = std::format("Playing {}", who),
                   .toStderr = false,
                   .failed = false};
}

auto App::seek(double target, bool isRelative) -> AppResult {
    if (isRelative) {
        kanariv::client::Client client{impl_->config_.dbPath, impl_->config_.socketPath};
        auto sres = client.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}});
        double pos = 0;
        bool hasPos = false;
        if (sres) {
            if (auto* ps = std::get_if<kanariv::ipc::Status>(&*sres)) {
                pos = ps->pos;
                hasPos = true;
            }
        }
        if (hasPos) {
            target = pos + target;
            target = std::max<double>(target, 0);
        } else {
            target = std::max<double>(target, 0);
        }
    }
    kanariv::ipc::Command cmd{kanariv::ipc::Seek{target}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        return std::unexpected{res.error()};
    }
    return Outcome{.result = std::move(*res),
                   .line = std::nullopt,
                   .toStderr = false,
                   .failed = false,
                   .quiet = true};
}

auto App::previewFile(const std::string& file) -> AppResult {
    if (impl_->previewHost_) {
        return impl_->previewHost_(file);
    }
    return Outcome::fail("preview host not configured");
}

} // namespace kanariv::app
