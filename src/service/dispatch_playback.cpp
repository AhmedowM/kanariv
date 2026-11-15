#include <kanariv/types/engine.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include "service_impl.hpp"

namespace kanariv::service {

using kanariv::engine::detail::ServiceAccess;
using namespace kanariv::ipc;
using kanariv::ipc::Next;
using kanariv::ipc::Pause;
using kanariv::ipc::Play;
using kanariv::ipc::Prev;
using kanariv::ipc::Restart;
using kanariv::ipc::Resume;
using kanariv::ipc::Seek;
using kanariv::ipc::SeekBy;
using kanariv::ipc::StatusReq;
using kanariv::ipc::StatusWait;
using kanariv::ipc::Stop;
using kanariv::ipc::VolumeSet;

// Play command: start/resume playback of active queue
std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Play&) {
    const auto before = engine_->state();
    const int64_t beforeTrack = engine_->currentTrackId();
    const double beforePos = engine_->position();
    int64_t aq = engine_->activeQueueId();
    auto r = engine_->play(aq);
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    // Classify the transition here so clients can word the reply without a
    // second status probe (which raced other clients): a pause resumes in
    // place; a stop replays the persisted track only when the engine landed
    // back on it at (approximately) the persisted position.
    const bool resumed =
        before == kanariv::types::PlaybackState::Paused ||
        (before == kanariv::types::PlaybackState::Stopped && beforeTrack != 0 &&
         engine_->currentTrackId() == beforeTrack &&
         std::abs(engine_->position() - beforePos) < 0.5);
    return statusAfterChange(resumed ? "resumed" : "started");
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Pause&) {
    auto r = engine_->pause();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Resume&) {
    // Forgiving resume, resolved server-side: Playing is a no-op success the
    // reply marks "already_playing" (clients used to warn), and Stopped
    // replays the persisted track instead of failing State.
    const auto before = engine_->state();
    if (before == kanariv::types::PlaybackState::Playing) {
        updateShmStatus();
        syncMediaSession();
        return statusAfterChange("already_playing");
    }
    const bool wasPaused = before == kanariv::types::PlaybackState::Paused;
    auto r = wasPaused ? engine_->resume() : engine_->play(engine_->activeQueueId());
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange(wasPaused ? "resumed" : "started");
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Restart&) {
    // restart: seek to 0, ensure playing
    auto r = engine_->seek(0.0);
    if (!r) {
        // if no track, try play
        int64_t aq = engine_->activeQueueId();
        auto pr = engine_->play(aq);
        if (!pr)
            return std::unexpected{pr.error()};
        updateShmStatus();
        syncMediaSession();
        return statusAfterChange();
    }
    // ensure playing
    if (engine_->state() == kanariv::types::PlaybackState::Paused) {
        (void)engine_->resume();
    }
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Stop&) {
    auto r = engine_->stop();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Next&) {
    auto r = engine_->next();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Prev&) {
    auto r = engine_->prev();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Seek& s) {
    if (!std::isfinite(s.seconds) || s.seconds < 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "seek: invalid seconds")};
    }
    auto r = engine_->seek(s.seconds);
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::SeekBy& s) {
    if (!std::isfinite(s.delta)) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "seek: invalid delta")};
    }
    // Resolve the base and clamp under the dispatch lock, so a relative seek
    // cannot race another client's transport command between two roundtrips.
    double target = engine_->position() + s.delta;
    if (target < 0.0)
        target = 0.0;
    auto r = engine_->seek(target);
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    syncMediaSession();
    // Mutations are real state changes: bump the status sequence so
    // StatusWait waiters observe them (S5).
    return statusAfterChange();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::StatusReq&) {
    // Pure read: never bumps the status sequence, so polling clients cannot
    // wake StatusWait waiters (S5).
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::StatusWait& cmd) {
    // Normally intercepted by dispatch() so the wait does not hold the
    // dispatch mutex; kept consistent for direct callers (S5).
    waitForStatusChange(std::chrono::milliseconds(cmd.timeout_ms));
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::VolumeSet& v) {
    float cur = engine_->volume();
    float target = cur;
    bool hasTarget = false;
    // Absolute 0 counts as mute (it remembers the restore level); any
    // explicit non-zero level becomes the new restore level.
    if (v.level.has_value()) {
        float lvl = *v.level;
        if (!std::isfinite(lvl)) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                            "volume: invalid level")};
        }
        // clamp 0-100 -> 0.0-1.0
        if (lvl < 0.0f)
            lvl = 0.0f;
        if (lvl > 100.0f)
            lvl = 100.0f;
        target = lvl / 100.0f;
        if (target <= 0.0f) {
            if (cur > 0.0f) {
                if (auto pr = ServiceAccess::setPreMuteVolume(*engine_, cur); !pr)
                    return std::unexpected{pr.error()};
            }
            target = 0.0f;
        } else {
            if (auto pr = ServiceAccess::setPreMuteVolume(*engine_, target); !pr)
                return std::unexpected{pr.error()};
        }
        hasTarget = true;
    }
    if (v.deltaPct.has_value()) {
        int d = *v.deltaPct;
        float curPct = cur * 100.0f;
        float np = curPct + static_cast<float>(d);
        if (np < 0.0f)
            np = 0.0f;
        if (np > 100.0f)
            np = 100.0f;
        target = np / 100.0f;
        if (target <= 0.0f) {
            if (cur > 0.0f) {
                if (auto pr = ServiceAccess::setPreMuteVolume(*engine_, cur); !pr)
                    return std::unexpected{pr.error()};
            }
            target = 0.0f;
        } else {
            if (auto pr = ServiceAccess::setPreMuteVolume(*engine_, target); !pr)
                return std::unexpected{pr.error()};
        }
        hasTarget = true;
    }
    if (v.mute.has_value()) {
        if (*v.mute) {
            if (cur > 0.0f) {
                if (auto pr = ServiceAccess::setPreMuteVolume(*engine_, cur); !pr)
                    return std::unexpected{pr.error()};
            }
            target = 0.0f;
            hasTarget = true;
        } else {
            if (cur == 0.0f && !hasTarget) {
                target = ServiceAccess::preMuteVolume(*engine_);
                hasTarget = true;
            }
        }
    }
    if (hasTarget) {
        auto r = engine_->setVolume(target);
        if (!r)
            return std::unexpected{r.error()};
    }
    updateShmStatus();
    syncMediaSession();
    if (hasTarget)
        noteStateChanged();
    kanariv::ipc::VolumeInfo vi{engine_->volume(), engine_->volume() == 0.0f};
    return Result{vi};
}

} // namespace kanariv::service
