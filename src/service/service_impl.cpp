#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kanariv/config.hpp>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/ipc/shm_status.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/version.hpp>
#include <memory>
#include <optional>
#include <service/ipc_server.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <utils/log_sink.hpp>
#include <variant>

#include "config_detail.hpp"
#include "service_impl.hpp"
#include "service_paths.hpp"
#include "service_status.hpp"

namespace kanariv::service {

Service::~Service() {
    shutdown();
}

Service::ExpectedService Service::create(const ServiceConfig& cfg,
                                         kanariv::contracts::Storage storage) {
    // Host-supplied runtime paths are authoritative over the inherited fields.
    // Desktop callers leave RuntimePaths empty and keep the existing behavior.
    ServiceConfig resolved = cfg;
    if (!cfg.runtime.database.empty())
        resolved.dbPath = cfg.runtime.database;
    if (!cfg.runtime.configuration.empty())
        resolved.configPath = cfg.runtime.configuration;
    if (!cfg.runtime.socket.empty())
        resolved.socketPath = cfg.runtime.socket;

    if (auto dirs = cfg.runtime.ensureDirectories(); !dirs)
        return std::unexpected{dirs.error()};

    const bool inProcess = cfg.inProcess;
    const std::filesystem::path& dbPath = resolved.dbPath;

    std::string spStr;
    std::filesystem::path lockPath;
    std::filesystem::path pidPath;
    std::intptr_t lockFd = -1;
    std::error_code ec;

    if (!inProcess) {
        // Determine socket path: explicit override, else derived from the database.
        // Only an explicit socket re-keys the pid/lock files; a derived one
        // must keep the database-derived names clients already look for.
        const bool explicitSocket = !resolved.socketPath.empty();
        if (explicitSocket) {
            spStr = resolved.socketPath;
        } else {
            spStr = detail::socketPathForDb(dbPath);
        }
        resolved.socketPath = spStr;
        const std::string socketKey = explicitSocket ? spStr : std::string{};

        // Single-instance enforcement via flock lock file
        lockPath = detail::lockPathForSocket(dbPath, socketKey);
        if (!detail::tryAcquireLock(lockPath, lockFd)) {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::AlreadyExists, "service already running (lock held)")};
        }

        // Check for stale PID file
        pidPath = detail::pidPathForSocket(dbPath, socketKey);
        if (std::filesystem::exists(pidPath, ec)) {
            auto existingPid = detail::readPidFile(pidPath);
            if (existingPid && detail::checkPidAlive(*existingPid)) {
                // Process is alive, daemon already running
                detail::releaseLock(lockFd);
                return std::unexpected{
                    kanariv::utils::makeError(kanariv::utils::StatusCode::AlreadyExists,
                                              "service already running (pid alive)")};
            }
            // Stale PID - remove it
            std::filesystem::remove(pidPath, ec);
        }

        // Check for stale socket
#ifdef _WIN32
        if (!spStr.empty() && spStr.starts_with("\\\\")) {
            if (detail::probeSocketAlive(spStr)) {
                detail::releaseLock(lockFd);
                return std::unexpected{
                    kanariv::utils::makeError(kanariv::utils::StatusCode::AlreadyExists,
                                              "service already running (pipe alive)")};
            }
        } else
#endif
            if (!spStr.empty() && !spStr.starts_with("\\\\")) {
            std::filesystem::path sockP(spStr);
            if (std::filesystem::exists(sockP, ec)) {
                if (detail::probeSocketAlive(spStr)) {
                    detail::releaseLock(lockFd);
                    return std::unexpected{
                        kanariv::utils::makeError(kanariv::utils::StatusCode::AlreadyExists,
                                                  "service already running (socket alive)")};
                } else {
                    // Stale socket - remove
                    std::filesystem::remove(sockP, ec);
                }
            }
        }
    }

    // ensure db parent dirs exist before open (fixes "unable to open database file")
    if (!dbPath.empty()) {
        std::error_code ec2;
        auto parent = dbPath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec2);
    }

    // Log first: rotating file next to the db (see logPathFor), so db/engine
    // creation below already emit. A host-supplied log directory wins. An
    // in-process host without a log directory stays silent instead of
    // inferring a desktop location. A log that cannot be opened degrades to
    // silence — logging must never block startup.
    kanariv::utils::Logger::Callback logCb = [](kanariv::utils::LogLevel, std::string_view) {};
    std::filesystem::path logFile;
    if (!cfg.runtime.logs.empty()) {
        logFile = cfg.runtime.logs / "kanariv.log";
    } else if (!inProcess) {
        if (auto lp = kanariv::config::logPathFor(dbPath))
            logFile = *lp;
    }
    if (!logFile.empty()) {
        kanariv::utils::FileSinkOptions sinkOpts;
        sinkOpts.teeStderr = cfg.logToStderr;
        if (auto sink = kanariv::utils::FileLogSink::open(logFile, sinkOpts))
            logCb = sink->callback();
    }
    auto loggerPtr = std::make_unique<kanariv::utils::Logger>(
        std::move(logCb), static_cast<kanariv::utils::LogLevel>(std::clamp(
                              cfg.logLevel, std::to_underlying(kanariv::utils::LogLevel::Trace),
                              std::to_underlying(kanariv::utils::LogLevel::Error))));
    kanariv::utils::Logger* logger = loggerPtr.get();
    for (const auto& key : cfg.unknownKeys)
        logger->warn("config: ignoring unknown key '{}'", key);

    // create Engine
    kanariv::types::EngineConfig ecfg{};
    ecfg.logger = logger;
    ecfg.autoResumeOnPlay = cfg.autoResumeOnPlay;
    ecfg.replayGain = cfg.replayGain;
    ecfg.fadeInMs = cfg.fadeInMs;
    ecfg.fadeOutMs = cfg.fadeOutMs;
    ecfg.crossfadeMs = cfg.crossfadeMs;
    ecfg.interTrackGapMs = cfg.interTrackGapMs;
    ecfg.device = cfg.device;
    ecfg.deviceName = cfg.deviceName;
    auto engRes = kanariv::engine::Engine::create(ecfg);
    if (!engRes) {
        detail::releaseLock(lockFd);
        return std::unexpected{engRes.error()};
    }
    std::unique_ptr<kanariv::engine::Engine> eng = std::move(engRes.value());
    if (storage.complete()) {
        if (auto e = eng->attachStorage(storage); !e) {
            detail::releaseLock(lockFd);
            return std::unexpected{e.error()};
        }
    }

    // IPC server / PID / shared status exist only for the desktop daemon.
    std::unique_ptr<IpcServer> srvPtr;
    std::string shmName;
    std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter;
    std::string pidStr;
    if (!inProcess) {
        srvPtr = std::make_unique<IpcServer>();
        auto listenRes = srvPtr->listen(dbPath, spStr);
        if (!listenRes) {
            detail::releaseLock(lockFd);
            return std::unexpected{listenRes.error()};
        }

        // Create PID file with current PID
        try {
            auto parent = pidPath.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
            }
            std::ofstream pf(pidPath);
            if (pf) {
#ifdef _WIN32
                pidStr = std::to_string(::_getpid());
#else
                pidStr = std::to_string(::getpid());
#endif
                pf << pidStr << "\n";
            }
        } catch (...) {
        }

        // Create shared memory status block for event-driven status snapshots
        // Derive shm name via the canonical endpoint id (consistent with socket/pid/lock)
        shmName = kanariv::config::detail::endpointIdForDb(dbPath);
        auto shmRes = kanariv::ipc::ShmStatusWriter::create(shmName);
        if (!shmRes) {
            // Non-fatal: continue without shm (clients fall back to IPC status).
            loggerPtr->warn("shm status unavailable: {} (falling back to IPC status)",
                            shmRes.error().message);
        }
        if (shmRes)
            shmWriter = std::make_unique<kanariv::ipc::ShmStatusWriter>(std::move(*shmRes));
    }

    auto svc = std::unique_ptr<Service>(new Service(
        resolved, std::move(storage), std::move(eng), std::move(srvPtr), std::move(loggerPtr),
        pidPath, lockPath, spStr, lockFd, std::move(shmWriter), shmName));
    if (inProcess) {
        svc->impl_->logger_->info("service started in-process version={} db={}",
                                  kanariv::version::version(), dbPath.generic_string());
    } else {
        svc->impl_->logger_->info("daemon started version={} pid={} db={} socket={}",
                                  kanariv::version::version(), pidStr, dbPath.generic_string(),
                                  spStr);
    }
    return svc;
}

Service::Service(const ServiceConfig& cfg, kanariv::contracts::Storage storage,
                 std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
                 std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
                 std::filesystem::path lockPath, std::string socketPath, std::intptr_t lockFd,
                 std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName)
    : impl_(std::make_unique<Impl>(cfg, std::move(storage), std::move(eng), std::move(srv),
                                   std::move(logger), std::move(pidPath), std::move(lockPath),
                                   std::move(socketPath), lockFd, std::move(shmWriter),
                                   std::move(shmName))) {}

Service::Impl::Impl(const ServiceConfig& cfg, kanariv::contracts::Storage storage,
                    std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
                    std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
                    std::filesystem::path lockPath, std::string socketPath, std::intptr_t lockFd,
                    std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName)
    : config_(cfg), storage_(std::move(storage)), engine_(std::move(eng)), server_(std::move(srv)),
      logger_(std::move(logger)), pidPath_(std::move(pidPath)), lockPath_(std::move(lockPath)),
      socketPath_(std::move(socketPath)), lockFd_(lockFd), shmWriter_(std::move(shmWriter)),
      shmName_(std::move(shmName)) {
    // Create the OS media-session backend (internal, daemon-only). A missing
    // backend is not fatal: the daemon runs fine without OS integration.
    auto msRes = kanariv::media_session::MediaSession::create("kanariv", "", true, true);
    if (!msRes) {
        if (logger_)
            logger_->debug("media session unavailable: {}", msRes.error().message);
    } else {
        mediaSession_ = std::move(*msRes);
        mediaSession_->setLogger(logger_.get());
        // OS media-key intents run on backend threads. They are routed through
        // dispatch() instead of touching the engine directly, so there is
        // exactly one serialized writer into Engine/Database (S2/S8) and the
        // command handlers keep owning the shm/media-session refresh.
        mediaSession_->setCallbacks({
            .onPlay = [this] { (void)dispatch(kanariv::ipc::Play{}); },
            .onPause = [this] { (void)dispatch(kanariv::ipc::Pause{}); },
            .onStop = [this] { (void)dispatch(kanariv::ipc::Stop{}); },
            .onNext = [this] { (void)dispatch(kanariv::ipc::Next{}); },
            .onPrev = [this] { (void)dispatch(kanariv::ipc::Prev{}); },
            .onSeek = [this](double delta) { (void)dispatch(kanariv::ipc::SeekBy{delta}); },
            .onSeekTo = [this](double position) { (void)dispatch(kanariv::ipc::Seek{position}); },
            .onRepeat =
                [this](kanariv::types::RepeatMode repeat) {
                    (void)dispatch(kanariv::ipc::QueueRepeat{.mode = repeat});
                },
            .onShuffle =
                [this](bool shuffle) { (void)dispatch(kanariv::ipc::QueueShuffle{.on = shuffle}); },
            .onVolume =
                [this](float volume, bool muted) {
                    // VolumeSet already owns the pre-mute bookkeeping, so an OS
                    // unmute restores the level the CLI would have restored.
                    const float pct = std::clamp(volume, 0.0f, 1.0f) * 100.0f;
                    (void)dispatch(kanariv::ipc::VolumeSet{.level = pct, .mute = muted});
                },
        });
        // Engine track events (natural end / auto-advance) must refresh the
        // OS now-playing UI, which no IPC command triggers. They also bump the
        // status sequence: a track change is a real state change, so StatusWait
        // waiters must observe it (S5).
        (void)engine_->setCallbacks({
            .on_track_started = [this](int64_t) -> void {
                noteStateChanged();
                requestMediaSessionRefresh();
            },
            .on_track_ended = [this](int64_t, double) -> void {
                noteStateChanged();
                requestMediaSessionRefresh();
            },
        });
        // Activate the OS media session so media keys and now-playing UI work.
        if (auto e = mediaSession_->activate(); !e && logger_)
            logger_->debug("media session activate: {}", e.error().message);
        // Publishes on its own thread: a transport command must never pay for
        // a database lookup, a cover read or a Base64 encode (S7).
        mediaThread_ = std::thread([this] { mediaWorkerLoop(); });
    }
}

kanariv::utils::Expected<void> Service::run(std::stop_token st) {
    return impl_->run(st);
}

bool Service::running() const noexcept {
    return impl_->running_.load(std::memory_order_acquire);
}

bool Service::waitUntilRunning() const {
    return impl_->waitUntilRunning();
}

kanariv::utils::Expected<void> Service::Impl::run(std::stop_token st) {
    auto signalLoopExit = [this] {
        {
            std::lock_guard<std::mutex> lk(lifeMtx_);
            loopExited_ = true;
        }
        lifeCv_.notify_all();
    };
    if (running_.exchange(true)) {
        signalLoopExit();
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "already running")};
    }
    {
        // Publish readiness under the lock the waiter reads it under, so a
        // waiter cannot miss the notification between predicate and sleep.
        std::lock_guard<std::mutex> lk(lifeMtx_);
        ready_ = true;
    }
    lifeCv_.notify_all();
    // build dispatcher
    auto dispatcher = [this](const kanariv::ipc::Command& cmd)
        -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
        return this->dispatch(cmd);
    };
    if (server_)
        server_->run(st, dispatcher);
    // block until stop requested
    while (!st.stop_requested() && !shutdownRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (logger_)
        logger_->info("daemon stopping");
    // Tear down here (not only in the destructor): pid/lock/socket release
    // must be sequenced before run() returns so a racing starter observes a
    // truthful state. shutdown() is idempotent; the destructor re-entry is
    // harmless.
    this->shutdown();
    signalLoopExit();
    return {};
}

bool Service::Impl::waitUntilRunning() const {
    std::unique_lock<std::mutex> lk(lifeMtx_);
    lifeCv_.wait(lk, [this] { return ready_ || loopExited_; });
    return ready_ && !loopExited_;
}

void Service::shutdown() {
    impl_->shutdown();
}

void Service::Impl::shutdown() {
    bool was = running_.exchange(false);
    (void)was;
    shutdownRequested_.store(true, std::memory_order_release);
    // Reject late media-key intents and stop the OS media session BEFORE the
    // engine is torn down: an intent arriving in that window would otherwise
    // dispatch into a shut-down engine (S8). Clearing the callbacks stops new
    // intents; deactivate() runs after releasing mediaMtx_ because it joins
    // the backend thread, which may be parked inside a callback waiting for
    // that lock (dispatch rejects it via shuttingDown_).
    shuttingDown_.store(true, std::memory_order_release);
    if (engine_)
        (void)engine_->setCallbacks({});
    // Stop the media worker before touching the backend it publishes to: no
    // request can be queued after the engine callbacks are cleared, and the
    // worker checks the stop flag before every publish.
    {
        std::lock_guard<std::mutex> lk{mediaReqMtx_};
        mediaStop_ = true;
    }
    mediaReqCv_.notify_all();
    if (mediaThread_.joinable())
        mediaThread_.join();
    if (mediaSession_) {
        {
            std::lock_guard<std::mutex> lk(mediaMtx_);
            mediaSession_->setCallbacks({});
        }
        (void)mediaSession_->deactivate();
    }
    if (server_)
        server_->shutdown();
    if (engine_)
        engine_->shutdown();
    // Drop ephemeral direct-play queues (their tracks stay in the library).
    if (storage_.queues) {
        if (auto qs = storage_.queues->listQueues()) {
            for (auto& q : *qs) {
                if (q.temp)
                    (void)storage_.queues->deleteQueue(q.id);
            }
        }
    }
    // cleanup pid file and socket
    try {
        std::error_code ec;
        if (!pidPath_.empty() && std::filesystem::exists(pidPath_, ec)) {
            std::filesystem::remove(pidPath_, ec);
        }
        if (!socketPath_.empty()) {
            std::string s = socketPath_;
            if (!s.starts_with("\\\\") && !s.empty()) {
                std::filesystem::path sp(s);
                if (std::filesystem::exists(sp, ec)) {
                    std::filesystem::remove(sp, ec);
                }
            }
        }
        // Remove the lock file while the lock is still held: releasing first
        // would let another process acquire the lock and then have the live
        // file deleted underneath it, admitting a third "exclusive" holder.
        // Windows keeps the file open without FILE_SHARE_DELETE, so the remove
        // fails there and the empty file stays -- harmless, because the lock
        // is the handle, not the name.
        if (lockFd_ >= 0) {
            std::error_code rmEc;
            if (!lockPath_.empty())
                std::filesystem::remove(lockPath_, rmEc);
            detail::releaseLock(lockFd_);
            lockFd_ = -1;
        }
    } catch (...) {
    }
    // shm handle will be cleaned up via RAII
}

kanariv::contracts::Storage Service::storage() noexcept {
    return impl_->storage_;
}

kanariv::utils::Expected<void> Service::attachStorage(kanariv::contracts::Storage storage) {
    if (!storage.complete() || !storage.queues->isOpen()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db")};
    }
    impl_->storage_ = std::move(storage);
    return engine().attachStorage(impl_->storage_);
}

kanariv::utils::Expected<kanariv::ipc::Result>
Service::dispatchCommand(const kanariv::ipc::Command& cmd) noexcept {
    return impl_->dispatch(cmd);
}

/**
 * @brief Access the audio engine instance.
 * @return Reference to the Engine.
 */
kanariv::engine::Engine& Service::engine() noexcept {
    return *impl_->engine_;
}
/**
 * @brief Access the IPC server instance.
 * @return Reference to the IpcServer.
 */
IpcServer& Service::server() noexcept {
    return *impl_->server_;
}
/**
 * @brief Get the shared memory name used for status block.
 * @return Shared memory name (hash of dbPath).
 */
const std::string& Service::shmName() const noexcept {
    return impl_->shmName_;
}
/**
 * @brief Get the shared memory status writer (if available).
 * @return Pointer to ShmStatusWriter or nullptr if SHM creation failed.
 */
kanariv::ipc::ShmStatusWriter* Service::shmWriter() noexcept {
    return impl_->shmWriter_.get();
}

void Service::Impl::updateShmStatus() {
    if (!shmWriter_ || !shmWriter_->valid() || !engine_)
        return;
    auto& eng = *engine_;
    int64_t track_id = eng.currentTrackId();
    // Single track fetch (C-2): title/artist/duration come from one lookup.
    // A database-less service still publishes engine state (S4).
    std::string title, artist;
    double duration = 0.0;
    if (storage_.tracks && track_id != 0) {
        if (auto tr = storage_.tracks->getTrack(track_id); tr) {
            title = tr->title;
            artist = tr->artist;
            duration = tr->duration;
        }
    }
    // O(1) queue size via COUNT(*) (C-2): the old listQueueItems() materialized
    // the full vector of items just for .size().
    size_t qSize = 0;
    if (storage_.queues) {
        int64_t aq = eng.activeQueueId();
        if (auto cnt = storage_.queues->queueCount(aq); cnt)
            qSize = *cnt;
    }
    kanariv::ipc::ShmStatusUpdate u{};
    u.position = eng.position();
    u.duration = duration;
    u.volume = eng.volume();
    u.muted = (eng.volume() == 0.0f);
    u.state = static_cast<int>(eng.state());
    u.track_id = track_id;
    u.queueSize = qSize;
    u.title = title;
    u.artist = artist;
    // One seqlock critical section for the whole snapshot (duration and queue
    // size used to be published through separate, non-sequenced setters).
    shmWriter_->publish(u);
}

void Service::Impl::noteStateChanged() {
    // Called only for real state changes: mutating command handlers and engine
    // track events. Reads must never bump the sequence, or every StatusReq
    // would wake every StatusWait waiter (S5).
    status_seq_.fetch_add(1, std::memory_order_release);
    status_cv_.notify_all();
}

void Service::Impl::waitForStatusChange(std::chrono::milliseconds timeout) {
    const uint64_t seq = status_seq_.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lk(status_mtx_);
    status_cv_.wait_for(lk, timeout,
                        [this, seq] { return status_seq_.load(std::memory_order_acquire) != seq; });
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error> Service::Impl::statusResult() {
    // Pure read: does not bump the status sequence (S5).
    auto st = detail::buildStatus(*engine_, storage_);
    if (!st)
        return std::unexpected{st.error()};
    return kanariv::ipc::Result{*st};
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error> Service::Impl::statusAfterChange() {
    noteStateChanged();
    return statusResult();
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::statusAfterChange(std::string_view transition) {
    auto res = statusAfterChange();
    if (res && !transition.empty()) {
        if (auto* st = std::get_if<kanariv::ipc::Status>(&*res))
            st->transition = transition;
    }
    return res;
}

void Service::Impl::requestMediaSessionRefresh() {
    if (!mediaSession_)
        return;
    {
        std::lock_guard<std::mutex> lk{mediaReqMtx_};
        if (mediaStop_)
            return;
        ++mediaReqSeq_;
    }
    mediaReqCv_.notify_one();
}

void Service::Impl::mediaWorkerLoop() {
    for (;;) {
        std::uint64_t requested = 0;
        {
            std::unique_lock lk{mediaReqMtx_};
            mediaReqCv_.wait(lk, [this] { return mediaStop_ || mediaReqSeq_ != mediaDoneSeq_; });
            if (mediaStop_)
                return;
            requested = mediaReqSeq_;
        }
        publishMediaSession();
        {
            std::lock_guard lk{mediaReqMtx_};
            if (requested > mediaDoneSeq_)
                mediaDoneSeq_ = requested;
        }
    }
}

std::optional<ArtCacheEntry> Service::Impl::artworkFor(const kanariv::types::Track& track) {
    std::filesystem::path source;
    bool embedded = false;
    if (!track.cover_art_path.empty())
        source = track.cover_art_path;
    else if (!track.path.empty()) {
        source = track.path;
        embedded = true;
    } else {
        return std::nullopt;
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(source, ec);
    if (ec)
        return std::nullopt;
    const auto size = std::filesystem::file_size(source, ec);
    if (ec)
        return std::nullopt;

    if (auto hit = artCache_.get(track.id, source, mtime, size))
        return hit;

    ArtCacheEntry entry;
    if (embedded) {
        if (auto art = extractEmbeddedCoverArt(source, logger_.get())) {
            entry.base64 = kanariv::utils::Base64::encode(
                std::span<const std::byte>(art->first.data(), art->first.size()));
            entry.mime = art->second;
        }
    } else {
        std::ifstream ifs(source, std::ios::binary);
        if (ifs) {
            const std::string content((std::istreambuf_iterator<char>(ifs)),
                                      std::istreambuf_iterator<char>());
            if (!content.empty()) {
                entry.base64 = kanariv::utils::Base64::encode(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(content.data()), content.size()));
                entry.mime = kanariv::utils::Base64::mimeTypeFromPath(source.string());
            }
        }
    }
    // Misses are cached too: a track without art is demuxed once, not per
    // command, and the stat check still invalidates it when the file changes.
    artCache_.put(track.id, source, mtime, size, entry);
    if (!entry.base64)
        return std::nullopt;
    return entry;
}

void Service::Impl::publishMediaSession() {
    std::lock_guard<std::mutex> lk(mediaMtx_);
    if (!mediaSession_ || !mediaSession_->active() || !engine_)
        return;

    try {
        const int64_t track_id = engine_->currentTrackId();
        std::string title, artist;
        std::optional<std::string> album, artworkPath;
        std::optional<ArtCacheEntry> art;
        double duration = 0.0;

        if (track_id != 0 && storage_.tracks) {
            if (auto tr = storage_.tracks->getTrack(track_id); tr) {
                title = tr->title;
                artist = tr->artist;
                album = tr->album;
                if (!tr->cover_art_path.empty())
                    artworkPath = tr->cover_art_path;
                duration = tr->duration;
                art = artworkFor(*tr);
            }
        }

        // Best-effort: a failed OS update must never break playback dispatch.
        // When playback stops at the end of the queue the engine reports track
        // id 0. Keep the last now-playing item so the OS card shows the track
        // that just finished with a Stopped/play state instead of blanking.
        if (track_id != 0) {
            if (auto e = mediaSession_->setMetadata(
                    kanariv::media_session::Metadata{.title = title,
                                                     .artist = artist,
                                                     .album = album,
                                                     .artworkPath = artworkPath,
                                                     .artworkBase64 = art ? art->base64
                                                                          : std::nullopt,
                                                     .artworkMime = art ? art->mime : std::string{},
                                                     .trackId = track_id});
                !e && logger_)
                logger_->warn("media session setMetadata failed: {}", e.error().message);
        }

        if (auto e =
                mediaSession_->setPlaybackState(engine_->state(), engine_->position(), duration);
            !e && logger_)
            logger_->warn("media session setPlaybackState failed: {}", e.error().message);

        if (auto e = mediaSession_->setPlaybackMode(engine_->shuffle(), engine_->repeat());
            !e && logger_)
            logger_->warn("media session setPlaybackMode failed: {}", e.error().message);

        if (auto e = mediaSession_->setVolume(engine_->volume(), engine_->volume() == 0.0f);
            !e && logger_)
            logger_->warn("media session setVolume failed: {}", e.error().message);
    } catch (const std::exception& error) {
        if (logger_)
            logger_->error("media session publish failed: {}", error.what());
    } catch (...) {
        if (logger_)
            logger_->error("media session publish failed with unknown exception");
    }
}

namespace {

/**
 * @brief Reports whether a command can run without an attached database.
 * @details The transport commands and config/device inspection only touch the
 * engine or the config file; everything else reads or writes library data and
 * must fail cleanly instead of dereferencing a null database (S4).
 */
bool commandRunsWithoutDatabase(const kanariv::ipc::Command& cmd) {
    return std::holds_alternative<kanariv::ipc::Play>(cmd) ||
           std::holds_alternative<kanariv::ipc::Pause>(cmd) ||
           std::holds_alternative<kanariv::ipc::Resume>(cmd) ||
           std::holds_alternative<kanariv::ipc::Restart>(cmd) ||
           std::holds_alternative<kanariv::ipc::Stop>(cmd) ||
           std::holds_alternative<kanariv::ipc::Next>(cmd) ||
           std::holds_alternative<kanariv::ipc::Prev>(cmd) ||
           std::holds_alternative<kanariv::ipc::Seek>(cmd) ||
           std::holds_alternative<kanariv::ipc::SeekBy>(cmd) ||
           std::holds_alternative<kanariv::ipc::VolumeSet>(cmd) ||
           std::holds_alternative<kanariv::ipc::StatusReq>(cmd) ||
           std::holds_alternative<kanariv::ipc::StatusWait>(cmd) ||
           std::holds_alternative<kanariv::ipc::Shutdown>(cmd) ||
           std::holds_alternative<kanariv::ipc::ConfigList>(cmd) ||
           std::holds_alternative<kanariv::ipc::ConfigGet>(cmd) ||
           std::holds_alternative<kanariv::ipc::ConfigSet>(cmd) ||
           std::holds_alternative<kanariv::ipc::ConfigReset>(cmd) ||
           std::holds_alternative<kanariv::ipc::ConfigExport>(cmd) ||
           std::holds_alternative<kanariv::ipc::ConfigImport>(cmd) ||
           std::holds_alternative<kanariv::ipc::DeviceList>(cmd) ||
           std::holds_alternative<kanariv::ipc::DeviceSet>(cmd) ||
           std::holds_alternative<kanariv::ipc::DeviceTest>(cmd);
}

} // namespace

std::expected<kanariv::ipc::Result, kanariv::utils::Error>
Service::Impl::dispatch(const kanariv::ipc::Command& cmd) {
    // Per-command trace at debug: what the daemon was asked to do. Guarded
    // by enabled() (dump() walks the whole command) and try/catch (a log
    // line must never break dispatch on adversarial input).
    if (logger_ && logger_->enabled(kanariv::utils::LogLevel::Debug)) {
        try {
            logger_->debug("dispatch {}", kanariv::ipc::toJson(cmd).dump());
        } catch (...) {
        }
    }
    // A handler that throws (e.g. a filesystem call on a path outside the
    // ANSI code page) must surface as an error reply, not escape into the
    // connection thread -- which used to close the pipe and leave the client
    // with a bare `ReadFile hdr failed: 109` (ERROR_BROKEN_PIPE).
    try {
        if (shuttingDown_.load(std::memory_order_acquire)) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                                             "service is shutting down")};
        }
        // StatusWait blocks on a condition variable; holding the dispatch lock
        // while waiting would serialize every other client behind a long poll.
        // The snapshot is taken under the lock (so it cannot race a mutating
        // handler) but does not bump the sequence (S5).
        if (const auto* wait = std::get_if<kanariv::ipc::StatusWait>(&cmd)) {
            waitForStatusChange(std::chrono::milliseconds(wait->timeout_ms));
            std::lock_guard<std::mutex> lk(dispatchMtx_);
            return statusResult();
        }
        // Serialized dispatch (S2): one writer into Engine/Database at a time,
        // matching the documented IPC model and keeping the status sequence a
        // single ordered stream for StatusWait.
        std::lock_guard<std::mutex> lk(dispatchMtx_);
        if (!storage_.complete() && !commandRunsWithoutDatabase(cmd)) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::State,
                                          "database unavailable: command requires a database")};
        }
        return std::visit(DispatchVisitor{this}, cmd);
    } catch (const std::exception& e) {
        // The reply text is fixed: what() can embed user paths, locale text or
        // library internals, and clients branch on the status code, not the
        // message. The log keeps the detail.
        if (logger_)
            logger_->error("dispatch failed: {}", e.what());
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, "internal error handling command")};
    } catch (...) {
        if (logger_)
            logger_->error("dispatch failed with unknown exception");
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal, "internal error handling command")};
    }
}

} // namespace kanariv::service
