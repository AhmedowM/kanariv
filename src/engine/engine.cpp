#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "engine_access.hpp"
#include "engine_impl.hpp"

namespace kanariv::engine {

Engine::Engine(const kanariv::types::EngineConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

Engine::ExpectedVoid Engine::attachStorage(kanariv::contracts::Storage storage) {
    return impl_->attachStorage(std::move(storage));
}

void Engine::shutdown() {
    impl_->shutdown();
}

Engine::ExpectedVoid Engine::play(int64_t queueId) {
    return impl_->play(queueId);
}

Engine::ExpectedVoid Engine::playTrack(int64_t trackId) {
    return impl_->playTrack(trackId);
}

Engine::ExpectedVoid Engine::pause() {
    return impl_->pause();
}

Engine::ExpectedVoid Engine::resume() {
    return impl_->resume();
}

Engine::ExpectedVoid Engine::stop() {
    return impl_->stop();
}

Engine::ExpectedVoid Engine::seek(double seconds) {
    return impl_->seek(seconds);
}

Engine::ExpectedVoid Engine::setVolume(float g) {
    return impl_->setVolume(g);
}

float Engine::volume() const noexcept {
    return impl_->volume();
}

kanariv::types::PlaybackState Engine::state() const noexcept {
    return impl_->state();
}

double Engine::duration() const noexcept {
    return impl_->duration();
}

double Engine::position() const noexcept {
    return impl_->position();
}

int64_t Engine::currentTrackId() const noexcept {
    return impl_->currentTrackId();
}

bool Engine::shuffle() const noexcept {
    return impl_->shuffle();
}

kanariv::types::RepeatMode Engine::repeat() const noexcept {
    return impl_->repeat();
}

std::expected<kanariv::types::PlaybackStats, kanariv::utils::Error> Engine::stats() {
    return impl_->stats();
}

std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
Engine::listHistory(int limit) {
    return impl_->listHistory(limit);
}

std::expected<void, kanariv::utils::Error> Engine::clearHistory() {
    return impl_->clearHistory();
}

Engine::ExpectedVoid Engine::setShuffle(bool on) {
    return impl_->setShuffle(on);
}

Engine::ExpectedVoid Engine::toggleShuffle() {
    return impl_->setShuffle(std::nullopt);
}

Engine::ExpectedVoid Engine::setRepeat(kanariv::types::RepeatMode m) {
    return impl_->setRepeat(m);
}

int64_t Engine::activeQueueId() const noexcept {
    return impl_->activeQueueId();
}

Engine::ExpectedVoid Engine::switchQueue(int64_t qid) {
    return impl_->switchQueue(qid);
}

Engine::ExpectedVoid Engine::next() {
    return impl_->next();
}

Engine::ExpectedVoid Engine::prev() {
    return impl_->prev();
}

Engine::ExpectedVoid Engine::setCallbacks(const kanariv::types::EngineCallbacks& cbs) {
    return impl_->setCallbacks(cbs);
}

std::expected<kanariv::types::EngineEvent, kanariv::utils::Error> Engine::pollEvent() {
    return impl_->pollEvent();
}

Engine::ExpectedVoid Engine::drainEvents(kanariv::types::EngineEvent* buf, size_t cap, size_t* n) {
    return impl_->drainEvents(buf, cap, n);
}

std::vector<kanariv::types::EngineEvent> Engine::drainAll() {
    return impl_->drainAll();
}

Engine::ExpectedEngine Engine::create(const kanariv::types::EngineConfig& cfg) {
    auto e = std::unique_ptr<Engine>(new Engine(cfg));
    auto err = e->impl_->init();
    if (err)
        return std::unexpected(*err);
    if (cfg.logger)
        cfg.logger->debug("engine created monitor={}", cfg.enableMonitorThread);
    return e;
}

Engine::~Engine() {
    shutdown();
}

void Engine::Impl::shutdown() {
    // stop monitor
    monRun_.store(false, std::memory_order_release);
    monCv_.notify_all();
    if (monitorThread_.joinable()) {
        monitorThread_.request_stop();
        monCv_.notify_all();
        monitorThread_.join();
    }
    // stop decode
    decodeRun_.store(false, std::memory_order_release);
    decodeCv_.notify_all();
    if (decodeThread_.joinable()) {
        decodeThread_.request_stop();
        decodeCv_.notify_all();
        decodeThread_.join();
    }
    // persist state (threads are gone; take the queue lock so saveStateLocked's
    // precondition holds and the state_ snapshot is consistent)
    if (hasDb()) {
        std::lock_guard<std::timed_mutex> qlk(queueMutex_);
        state_.cursorPos = static_cast<int64_t>(queue_.cursor);
        saveStateBestEffort();
    }
    if (output_) {
        stopOutputBestEffort();
        output_.reset();
    }
    decoder_.reset();
    reader_.reset();
    ring_.reset();
    resetCrossfadeLocked();
}

Engine::Impl::Impl(const kanariv::types::EngineConfig& cfg) : cfg_(cfg), state_{}, queue_{} {
    cfg_.pollMs = cfg.pollMs ? cfg.pollMs : 10;
    cfg_.gaplessMs = cfg.gaplessMs ? cfg.gaplessMs : 300;
    cfg_.historyThresholdPct = cfg.historyThresholdPct ? cfg.historyThresholdPct : 60;
    cfg_.historyThresholdSecs = cfg.historyThresholdSecs ? cfg.historyThresholdSecs : 90;
    state_.volume = 1.0f;
    volume_.store(1.0f, std::memory_order_relaxed);
    state_.repeatMode = kanariv::types::RepeatMode::Off;
    queue_.repeat = kanariv::types::RepeatMode::Off;
    queue_.queue_id = 1;
    transition(kanariv::types::PlaybackState::Stopped, "init");
    hasCurrent_.store(false, std::memory_order_release);
    markedPlayed_.store(false, std::memory_order_release);
    resetPlaybackAccounting();
    gaplessArmed_.store(false, std::memory_order_release);
    lastProgressMs_.store(0, std::memory_order_release);
    fadeSample_.store(0, std::memory_order_release);
    fadeInSamples_ = 0;
    fadeOutSamples_ = 0;
    fadeOutStart_ = 0;
    interTrackGapArmed_.store(false, std::memory_order_release);
    interTrackGapStartMs_ = 0;
    crossfadeActive_ = false;
    crossfadePending_ = false;
    crossfadeSamples_ = 0;
    crossfadeSample_ = 0;
    // callbacks from config
    callbacks_ = cfg.callbacks;
}

std::optional<kanariv::utils::Error> Engine::Impl::init() {
    // start decode thread
    decodeRun_.store(true, std::memory_order_release);
    decodeThread_ = std::jthread([this](std::stop_token st) { decodeLoop(st); });
    // start monitor thread if enabled
    if (cfg_.enableMonitorThread) {
        monRun_.store(true, std::memory_order_release);
        monitorThread_ = std::jthread([this](std::stop_token st) { monitorLoop(st); });
    }
    return std::nullopt;
}

bool Engine::Impl::hasDb() const noexcept {
    return storage_.complete() && storage_.queues->isOpen();
}

// EngineAccess forwarders -- defined here where Impl is complete.
Engine::ExpectedVoid detail::EngineAccess::suspendCurrentTrack(Engine& eng) {
    return eng.impl_->suspendCurrentTrack();
}

Engine::ExpectedVoid detail::EngineAccess::resumeAfterTagWrite(Engine& eng,
                                                               const kanariv::types::TrackRow& t,
                                                               double pos, bool wasPaused) {
    return eng.impl_->resumeAfterTagWrite(t, pos, wasPaused);
}

float detail::EngineAccess::preMuteVolume(const Engine& eng) noexcept {
    return eng.impl_->preMuteVolume();
}

Engine::ExpectedVoid detail::EngineAccess::setPreMuteVolume(Engine& eng, float g) {
    return eng.impl_->setPreMuteVolume(g);
}

std::vector<int64_t> detail::EngineAccess::shufflePermFor(Engine& eng, int64_t qid) {
    return eng.impl_->shufflePermFor(qid);
}

void detail::EngineAccess::noteEnqueued(Engine& eng, int64_t qid, std::size_t count) {
    eng.impl_->noteEnqueued(qid, count);
}

void detail::EngineAccess::noteQueueMoved(Engine& eng, int64_t qid, std::size_t from,
                                          std::size_t to) {
    eng.impl_->noteQueueMoved(qid, from, to);
}

} // namespace kanariv::engine
