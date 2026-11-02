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
using kanariv::ipc::StatusReq;
using kanariv::ipc::StatusWait;
using kanariv::ipc::Stop;
using kanariv::ipc::VolumeSet;

// Play command: start/resume playback of active queue
std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Play&) {
    int64_t aq = engine_->activeQueueId();
    auto r = engine_->play(aq);
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Pause&) {
    auto r = engine_->pause();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Resume&) {
    auto r = engine_->resume();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
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
        return statusResult();
    }
    // ensure playing
    if (engine_->state() == kanariv::types::PlaybackState::Paused) {
        (void)engine_->resume();
    }
    updateShmStatus();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Stop&) {
    auto r = engine_->stop();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Next&) {
    auto r = engine_->next();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::Prev&) {
    auto r = engine_->prev();
    if (!r)
        return std::unexpected{r.error()};
    updateShmStatus();
    return statusResult();
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
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::StatusReq&) {
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::handle(const kanariv::ipc::StatusWait& cmd) {
    // Capture the current status sequence, then wait until it changes (a
    // status-changing operation incremented it) or the timeout elapses.
    uint64_t seq = status_seq_.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lk(status_mtx_);
    status_cv_.wait_for(lk, std::chrono::milliseconds(cmd.timeout_ms), [this, seq] {
        return status_seq_.load(std::memory_order_acquire) != seq;
    });
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
    kanariv::ipc::VolumeInfo vi{engine_->volume(), engine_->volume() == 0.0f};
    return Result{vi};
}

} // namespace kanariv::service
