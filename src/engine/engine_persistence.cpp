#include <expected>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine_impl.hpp"

namespace kanariv::engine {

Engine::ExpectedVoid Engine::Impl::attachStorage(kanariv::contracts::Storage storage) {
    if (!storage.complete() || !storage.queues->isOpen())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db"));
    // loadState() writes the queue-locked state_/queue_ fields, and publishing
    // storage_ first would let a monitor tick observe (and persist) a
    // half-loaded state: take the queue lock across the attach.
    std::lock_guard<std::timed_mutex> qlk(queueMutex_);
    storage_ = std::move(storage);
    if (auto ec = loadState())
        return std::unexpected(*ec);
    return {};
}

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Engine::Impl::stats() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    auto ds = storage_.library->getStats();
    if (!ds)
        return std::unexpected(ds.error());
    return *ds;
}

std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
Engine::Impl::listHistory(int limit) {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    return storage_.history->listHistory(limit);
}

std::expected<void, kanariv::utils::Error> Engine::Impl::clearHistory() {
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    return storage_.history->clearHistory();
}

std::optional<kanariv::utils::Error> Engine::Impl::loadState() {
    if (!hasDb())
        return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db");
    auto loaded = storage_.state->load();
    if (!loaded)
        return loaded.error();
    state_.shuffleEnabled.store(loaded->shuffle_enabled ? 1 : 0, std::memory_order_relaxed);
    state_.repeatMode.store(loaded->repeat_mode, std::memory_order_relaxed);
    state_.cursorPos.store(loaded->cursor_pos, std::memory_order_relaxed);
    state_.volume.store(loaded->volume, std::memory_order_relaxed);
    state_.preMuteVolume.store(loaded->pre_mute_volume, std::memory_order_relaxed);
    state_.positionMs.store(loaded->position_ms, std::memory_order_relaxed);
    volume_.store(loaded->volume, std::memory_order_relaxed);
    preMute_.store(loaded->pre_mute_volume, std::memory_order_relaxed);
    queue_.perm = std::move(loaded->shuffle_perm);
    queue_.shuffle.store(loaded->shuffle_enabled, std::memory_order_relaxed);
    queue_.repeat.store(loaded->repeat_mode, std::memory_order_relaxed);
    const int64_t cursor = loaded->cursor_pos < 0 ? 0 : loaded->cursor_pos;
    queue_.cursor = static_cast<size_t>(cursor);
    // The record names its queue instead of storing a row id, so a database
    // rebuild or import finds it again. An unknown name falls back to default.
    int64_t qid = 1;
    if (const auto queues = storage_.queues->listQueues(); queues) {
        for (const auto& q : *queues) {
            if (q.name == loaded->queue_name) {
                qid = q.id;
                break;
            }
        }
    }
    queue_.queue_id.store(qid, std::memory_order_relaxed);
    state_.activeQueueId.store(qid, std::memory_order_relaxed);
    auto queueExists = storage_.queues->queueExists(qid);
    if (!queueExists || !*queueExists) {
        queue_.queue_id.store(1, std::memory_order_relaxed);
        state_.activeQueueId.store(1, std::memory_order_relaxed);
    }
    // A persisted permutation holds queue *indices*, so it is only meaningful
    // for the queue it was written for. If the queue shrank or was replaced
    // while the daemon was down, drop it and restart at the head instead of
    // fetching removed entries (or failing the first play with NotFound).
    if (auto counted = storage_.queues->queueCount(queue_.queue_id.load(std::memory_order_relaxed));
        counted) {
        const std::size_t queueLen = *counted;
        if (!queue_.shuffle.load(std::memory_order_relaxed)) {
            queue_.perm.clear();
        } else if (queue_.perm.size() != queueLen) {
            queue_.perm.clear();
            queue_.cursor = 0;
        }
        if (queue_.cursor >= (queue_.perm.empty() ? queueLen : queue_.perm.size()))
            queue_.cursor = 0;
    } else if (!queue_.perm.empty() && queue_.cursor >= queue_.perm.size()) {
        queue_.cursor = 0;
    }
    // The optional id is a hint; the path is the identity.
    int64_t trackId = 0;
    std::string trackPath = loaded->current_path;
    if (loaded->current_track_id) {
        if (auto track = storage_.tracks->getTrack(*loaded->current_track_id); track) {
            trackId = track->id;
            trackPath = track->path;
        }
    }
    if (trackId <= 0 && !trackPath.empty()) {
        if (auto track = storage_.tracks->findByPath(trackPath); track) {
            trackId = track->id;
            trackPath = track->path;
        }
    }
    state_.currentTrackId.store(trackId, std::memory_order_release);
    state_.currentPath = std::move(trackPath);
    return std::nullopt;
}

std::expected<void, kanariv::utils::Error>
Engine::Impl::saveStateLocked(std::optional<int64_t> livePositionMs) {
    // @pre queueMutex_ held: state_/queue_.perm are queue-lock-only. The live
    // position needs decodeMtx_ (transport counters) when the caller cannot
    // supply it, so take it briefly then -- callers must not already hold it.
    if (!hasDb())
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "no db"));
    kanariv::types::EngineState persisted;
    persisted.queue_name = "default";
    if (auto queue = storage_.queues->getQueue(queue_.queue_id.load(std::memory_order_relaxed));
        queue && !queue->name.empty())
        persisted.queue_name = queue->name;
    persisted.cursor_pos = static_cast<int64_t>(queue_.cursor);
    persisted.current_path = state_.currentPath;
    const int64_t trackId = state_.currentTrackId.load(std::memory_order_relaxed);
    if (trackId > 0)
        persisted.current_track_id = trackId;
    persisted.shuffle_enabled = queue_.shuffle.load(std::memory_order_relaxed);
    persisted.repeat_mode = queue_.repeat.load(std::memory_order_relaxed);
    persisted.shuffle_perm = queue_.perm;
    // volume_ is the authoritative value: setVolume() may skip the queue lock
    // while it is contended, and the atomic is still current.
    persisted.volume = volume_.load(std::memory_order_relaxed);
    persisted.pre_mute_volume = preMute_.load(std::memory_order_relaxed);
    if (livePositionMs) {
        persisted.position_ms = *livePositionMs;
    } else {
        std::lock_guard<std::mutex> lk(decodeMtx_);
        persisted.position_ms = hasCurrent_.load(std::memory_order_acquire)
                                    ? static_cast<int64_t>(currentPositionLocked() * 1000.0)
                                    : state_.positionMs.load(std::memory_order_relaxed);
    }
    if (persisted.position_ms < 0)
        persisted.position_ms = 0;
    auto result = storage_.state->save(persisted);
    if (result) {
        state_.positionMs = persisted.position_ms;
        state_.shuffleEnabled.store(persisted.shuffle_enabled ? 1 : 0, std::memory_order_relaxed);
        state_.repeatMode.store(persisted.repeat_mode, std::memory_order_relaxed);
        state_.cursorPos.store(persisted.cursor_pos, std::memory_order_relaxed);
        state_.activeQueueId.store(queue_.queue_id.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
    }
    return result;
}

void Engine::Impl::saveStateBestEffort(std::optional<int64_t> livePositionMs) {
    if (auto r = saveStateLocked(livePositionMs); !r && cfg_.logger)
        cfg_.logger->warn("state persist failed: {}", r.error());
}

void Engine::Impl::startOutputBestEffort() {
    if (auto r = output_->start(); !r && cfg_.logger)
        cfg_.logger->warn("audio start failed: {}", r.error());
}

void Engine::Impl::stopOutputBestEffort() {
    if (auto r = output_->stop(); !r && cfg_.logger)
        cfg_.logger->warn("audio stop failed: {}", r.error());
}

} // namespace kanariv::engine
