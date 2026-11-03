#include <chrono>
#include <cmath>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <expected>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
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

// Forward declarations for VolumeSet helpers.
static auto clampUnit(float v) -> float;
static auto rememberOrSetPreMute(kanariv::engine::Engine& engine, float cur, float target)
    -> std::expected<void, kanariv::utils::Error>;
using kanariv::ipc::StatusWait;
using kanariv::ipc::Stop;
using kanariv::ipc::VolumeSet;

// Play command: start/resume playback of active queue
auto Service::Impl::handle(const kanariv::ipc::Play&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    int64_t aq = engine_->activeQueueId();
    auto r = engine_->play(aq);
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::Pause&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto r = engine_->pause();
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::Resume&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto r = engine_->resume();
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::Restart&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    // restart: seek to 0, ensure playing
    auto r = engine_->seek(0.0);
    if (!r) {
        // if no track, try play
        int64_t aq = engine_->activeQueueId();
        auto pr = engine_->play(aq);
        if (!pr) {
            return std::unexpected{pr.error()};
        }
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

auto Service::Impl::handle(const kanariv::ipc::Stop&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto r = engine_->stop();
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::Next&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto r = engine_->next();
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::Prev&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto r = engine_->prev();
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::Seek& s)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    if (!std::isfinite(s.seconds) || s.seconds < 0) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "seek: invalid seconds")};
    }
    auto r = engine_->seek(s.seconds);
    if (!r) {
        return std::unexpected{r.error()};
    }
    updateShmStatus();
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::StatusReq&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::StatusWait& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    // Capture the current status sequence, then wait until it changes (a
    // status-changing operation incremented it) or the timeout elapses.
    uint64_t seq = status_seq_.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lk(status_mtx_);
    status_cv_.wait_for(lk, std::chrono::milliseconds(cmd.timeout_ms), [this, seq] -> bool {
        return status_seq_.load(std::memory_order_acquire) != seq;
    });
    return statusResult();
}

auto Service::Impl::handle(const kanariv::ipc::VolumeSet& v)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
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
        target = clampUnit(lvl / 100.0f);
        if (auto pr = rememberOrSetPreMute(*engine_, cur, target); !pr) {
            return std::unexpected{pr.error()};
        }
        hasTarget = true;
    }
    if (v.deltaPct.has_value()) {
        int d = *v.deltaPct;
        float curPct = cur * 100.0f;
        float np = curPct + static_cast<float>(d);
        target = clampUnit(np / 100.0f);
        if (auto pr = rememberOrSetPreMute(*engine_, cur, target); !pr) {
            return std::unexpected{pr.error()};
        }
        hasTarget = true;
    }
    if (v.mute.has_value()) {
        if (*v.mute) {
            if (auto pr = rememberOrSetPreMute(*engine_, cur, 0.0f); !pr) {
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
        if (!r) {
            return std::unexpected{r.error()};
        }
    }
    updateShmStatus();
    kanariv::ipc::VolumeInfo vi{.vol = engine_->volume(), .muted = engine_->volume() == 0.0f};
    return Result{vi};
}

// Clamps a value to the [0, 1] range.
static auto clampUnit(float v) -> float {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

// Remembers the current volume as pre-mute when targeting zero; otherwise sets
// pre-mute volume to the target. Returns error on failure.
static auto rememberOrSetPreMute(kanariv::engine::Engine& engine, float cur, float target)
    -> std::expected<void, kanariv::utils::Error> {
    if (target <= 0.0f) {
        if (cur > 0.0f) {
            if (auto pr = ServiceAccess::setPreMuteVolume(engine, cur); !pr) {
                return std::unexpected{pr.error()};
            }
        }
    } else {
        if (auto pr = ServiceAccess::setPreMuteVolume(engine, target); !pr) {
            return std::unexpected{pr.error()};
        }
    }
    return {};
}

} // namespace kanariv::service
