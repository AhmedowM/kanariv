/**
 * @file playback.cpp
 * @brief Playback command handlers (transport, direct play, seek).
 * @ingroup kanariv_app
 * @details Phase 1: returns outcome data; the frontend renders. Shared play
 * flow, direct file play, transport commands and seeking. Argument *syntax*
 * (time/volume grammar, globs) stays in the shell; these take parsed values.
 */

#include <app/app_impl.hpp>
#include <app/format_internal.hpp>
#include <chrono>
#include <expected>
#include <format>
#include <kanariv/app/app.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/math.hpp>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::app {

std::string App::Impl::transportWho(const kanariv::utils::Expected<kanariv::ipc::Result>& res) {
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res))
        return kanariv::app::trackWho(*st);
    return std::string{"unknown track"};
}

// Shared play flow (used by `play`, and by `resume` when stopped).
AppResult App::play() {
    kanariv::ipc::Command cmd{kanariv::ipc::Play{}};
    auto res = impl_->sendPlay(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        // The daemon classifies the transition, so the wording needs no
        // status probe and no corrective re-seek: a pause resumes in place,
        // and a restored session already starts at the persisted position.
        std::string who = kanariv::app::trackWho(*st);
        std::string line =
            st->transition == "resumed"
                ? std::format("Resuming {} from {}", who, kanariv::utils::formatClock(st->pos))
                : std::format("Playing {}", who);
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::playFiles(const std::vector<std::string>& files, bool save) {
    kanariv::ipc::Command cmd{kanariv::ipc::PlayFiles{files, save}};
    auto res = impl_->sendPlay(cmd);
    if (!res)
        return std::unexpected{res.error()};
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

AppResult App::pause() {
    kanariv::ipc::Command cmd{kanariv::ipc::Pause{}};
    auto res = impl_->sendRaw(cmd);
    if (!res) {
        const auto& e = res.error();
        // Engine::pause and Player::pause report State only when playback is
        // not Playing; against a reachable daemon that is the idle no-op.
        // An unreachable daemon's transport error must surface unchanged.
        if (e.code == kanariv::utils::StatusCode::State && impl_->daemonReachable())
            return Outcome::warn("pause: nothing playing");
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

AppResult App::resume() {
    kanariv::ipc::Command cmd{kanariv::ipc::Resume{}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
        // The daemon resolves every prior state: Playing comes back as
        // "already_playing" (a warn, not an error), Paused as "resumed", and
        // Stopped as "started" once it replays the persisted track.
        if (st->transition == "already_playing")
            return Outcome::warn("resume: already playing");
        // Hoisted: the line must be built before Outcome moves *res (the
        // braced list otherwise reads moved-from strings on some toolchains).
        std::string who = kanariv::app::trackWho(*st);
        std::string at = kanariv::utils::formatClock(st->pos);
        std::string line = st->transition == "started"
                               ? std::format("Playing {}", who)
                               : std::format("Resuming {} from {}", who, at);
        return Outcome{
            .result = std::move(*res), .line = std::move(line), .toStderr = false, .failed = false};
    }
    return Outcome{
        .result = std::move(*res), .line = std::nullopt, .toStderr = false, .failed = false};
}

AppResult App::restart() {
    kanariv::ipc::Command cmd{kanariv::ipc::Restart{}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    // Hoisted: read before Outcome moves *res (see resume()).
    std::string who = impl_->transportWho(res);
    return Outcome{.result = std::move(*res),
                   .line = std::format("Restarting {}", who),
                   .toStderr = false,
                   .failed = false};
}

AppResult App::stop() {
    kanariv::ipc::Command cmd{kanariv::ipc::Stop{}};
    return impl_->confirm(impl_->sendRaw(cmd), "Stopped");
}

AppResult App::next() {
    kanariv::ipc::Command cmd{kanariv::ipc::Next{}};
    auto res = impl_->sendRaw(cmd);
    if (!res && res.error().code == kanariv::utils::StatusCode::Busy) {
        // Transient queue-lock contention (racing gapless advance or a
        // sibling command): retry once after a beat before surfacing it.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        res = impl_->sendRaw(cmd);
    }
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::Busy)
            return Outcome::fail("next: engine busy, retry");
        return std::unexpected{e};
    }
    // Hoisted: read before Outcome moves *res (see resume()).
    std::string who = impl_->transportWho(res);
    return Outcome{.result = std::move(*res),
                   .line = std::format("Playing {}", who),
                   .toStderr = false,
                   .failed = false};
}

AppResult App::prev() {
    kanariv::ipc::Command cmd{kanariv::ipc::Prev{}};
    auto res = impl_->sendRaw(cmd);
    if (!res && res.error().code == kanariv::utils::StatusCode::Busy) {
        // Same transient contention as next(): retry once (see above).
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        res = impl_->sendRaw(cmd);
    }
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::Busy)
            return Outcome::fail("prev: engine busy, retry");
        if (e.code == kanariv::utils::StatusCode::NotFound) {
            // queuePrevLocked reports NotFound at queue start and for an
            // empty queue ("empty queue"); only a non-empty queue makes the
            // warning correct, so ask the daemon for its size (structural).
            if (auto st = impl_->sendRaw(kanariv::ipc::Command{kanariv::ipc::StatusReq{}})) {
                if (auto* status = std::get_if<kanariv::ipc::Status>(&*st);
                    status != nullptr && status->q_size > 0)
                    return Outcome::warn("prev: at queue start");
            }
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

AppResult App::seek(double target, bool isRelative) {
    // A relative seek stays one command: the daemon resolves the base
    // position and clamps at zero, so no client-side status read can race
    // another transport command between the two roundtrips.
    kanariv::ipc::Command cmd = isRelative ? kanariv::ipc::Command{kanariv::ipc::SeekBy{target}}
                                           : kanariv::ipc::Command{kanariv::ipc::Seek{target}};
    auto res = impl_->sendRaw(cmd);
    if (!res)
        return std::unexpected{res.error()};
    return Outcome{.result = std::move(*res),
                   .line = std::nullopt,
                   .toStderr = false,
                   .failed = false,
                   .quiet = true};
}

} // namespace kanariv::app
