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
#include <engine/engine_access.hpp>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kanariv/config.hpp>
#include <kanariv/contracts/database.hpp>
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

using kanariv::engine::detail::ServiceAccess;

Service::~Service() {
    shutdown();
}

Service::ExpectedService Service::create(const ServiceConfig& cfg,
                                         std::shared_ptr<kanariv::contracts::IDatabase> db) {
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
        if (!resolved.socketPath.empty()) {
            spStr = resolved.socketPath;
        } else {
            spStr = detail::socketPathForDb(dbPath);
        }
        resolved.socketPath = spStr;

        // Single-instance enforcement via flock lock file
        lockPath = detail::lockPathForSocket(dbPath, spStr);
        if (!detail::tryAcquireLock(lockPath, lockFd)) {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::AlreadyExists, "service already running (lock held)")};
        }

        // Check for stale PID file
        pidPath = detail::pidPathForSocket(dbPath, spStr);
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

    // create Engine
    kanariv::types::EngineConfig ecfg{};
    ecfg.logger = logger;
    ecfg.autoResumeOnPlay = cfg.autoResumeOnPlay;
    auto engRes = kanariv::engine::Engine::create(ecfg);
    if (!engRes) {
        detail::releaseLock(lockFd);
        return std::unexpected{engRes.error()};
    }
    std::unique_ptr<kanariv::engine::Engine> eng = std::move(engRes.value());
    if (db) {
        if (auto e = eng->attachDatabase(db); !e) {
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
        // Derive shm name via canonical hex8 (consistent with socket/pid/lock)
        shmName = kanariv::config::detail_paths::hex8ForDb(dbPath);
        auto shmRes = kanariv::ipc::ShmStatusWriter::create(shmName);
        if (!shmRes) {
            // Non-fatal: continue without shm (clients fall back to IPC status).
            loggerPtr->warn("shm status unavailable: {} (falling back to IPC status)",
                            shmRes.error().message);
        }
        if (shmRes)
            shmWriter = std::make_unique<kanariv::ipc::ShmStatusWriter>(std::move(*shmRes));
    }

    auto svc = std::unique_ptr<Service>(
        new Service(resolved, std::move(db), std::move(eng), std::move(srvPtr),
                    std::move(loggerPtr), pidPath, spStr, lockFd, std::move(shmWriter), shmName));
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

Service::Service(const ServiceConfig& cfg, std::shared_ptr<kanariv::contracts::IDatabase> db,
                 std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
                 std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
                 std::string socketPath, std::intptr_t lockFd,
                 std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName)
    : impl_(std::make_unique<Impl>(cfg, std::move(db), std::move(eng), std::move(srv),
                                   std::move(logger), std::move(pidPath), std::move(socketPath),
                                   lockFd, std::move(shmWriter), std::move(shmName))) {}

Service::Impl::Impl(const ServiceConfig& cfg, std::shared_ptr<kanariv::contracts::IDatabase> db,
                    std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
                    std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
                    std::string socketPath, std::intptr_t lockFd,
                    std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName)
    : config_(cfg), db_(std::move(db)), engine_(std::move(eng)), server_(std::move(srv)),
      logger_(std::move(logger)), pidPath_(std::move(pidPath)), socketPath_(std::move(socketPath)),
      lockFd_(lockFd), shmWriter_(std::move(shmWriter)), shmName_(std::move(shmName)) {
    // Create the OS media-session backend (internal, daemon-only). A missing
    // backend is not fatal: the daemon runs fine without OS integration.
    auto msRes = kanariv::media_session::MediaSession::create("kanariv", "", true, true);
    if (!msRes) {
        if (logger_)
            logger_->debug("media session unavailable: {}", msRes.error().message);
    } else {
        mediaSession_ = std::move(*msRes);
        mediaSession_->setLogger(logger_.get());
        mediaSession_->setCallbacks({
            .onPlay =
                [this] {
                    (void)engine_->play(engine_->activeQueueId());
                    syncMediaSession();
                },
            .onPause =
                [this] {
                    (void)engine_->pause();
                    syncMediaSession();
                },
            .onStop =
                [this] {
                    (void)engine_->stop();
                    syncMediaSession();
                },
            .onNext =
                [this] {
                    (void)engine_->next();
                    syncMediaSession();
                },
            .onPrev =
                [this] {
                    (void)engine_->prev();
                    syncMediaSession();
                },
            .onSeek =
                [this](double delta) {
                    auto cur = engine_->position();
                    (void)engine_->seek(cur + delta);
                    syncMediaSession();
                },
            .onSeekTo =
                [this](double position) {
                    (void)engine_->seek(position);
                    syncMediaSession();
                },
            .onRepeat =
                [this](kanariv::types::RepeatMode repeat) {
                    (void)engine_->setRepeat(repeat);
                    syncMediaSession();
                },
            .onShuffle =
                [this](bool shuffle) {
                    (void)engine_->setShuffle(shuffle);
                    syncMediaSession();
                },
            .onVolume =
                [this](float volume, bool muted) {
                    const float current = engine_->volume();
                    if (muted || volume <= 0.0f) {
                        // Remember the pre-mute level so a later unmute (from
                        // the OS mixer) can restore it, mirroring VolumeSet.
                        if (current > 0.0f)
                            (void)ServiceAccess::setPreMuteVolume(*engine_, current);
                        (void)engine_->setVolume(0.0f);
                    } else {
                        (void)ServiceAccess::setPreMuteVolume(*engine_, volume);
                        (void)engine_->setVolume(volume);
                    }
                    syncMediaSession();
                },
        });
        // Engine track events (natural end / auto-advance) must refresh the
        // OS now-playing UI, which syncMediaSession() only does on explicit
        // user commands. Wire these up after media-session setup so natural
        // track end transitions SMTC to Stopped instead of staying on Playing.
        (void)engine_->setCallbacks({
            .on_track_started = [this](int64_t) -> void { syncMediaSession(); },
            .on_track_ended = [this](int64_t, double) -> void { syncMediaSession(); },
        });
        // Activate the OS media session so media keys and now-playing UI work.
        if (auto e = mediaSession_->activate(); !e && logger_)
            logger_->debug("media session activate: {}", e.error().message);
    }
}

kanariv::utils::Expected<void> Service::run(std::stop_token st) {
    return impl_->run(st);
}

bool Service::running() const noexcept {
    return impl_->running_.load(std::memory_order_acquire);
}

kanariv::utils::Expected<void> Service::Impl::run(std::stop_token st) {
    if (running_.exchange(true)) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "already running")};
    }
    // build dispatcher
    auto dispatcher = [this](const kanariv::ipc::Command& cmd)
        -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> { return this->dispatch(cmd); };
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
    return {};
}

void Service::shutdown() {
    impl_->shutdown();
}

void Service::Impl::shutdown() {
    bool was = running_.exchange(false);
    (void)was;
    shutdownRequested_.store(true, std::memory_order_release);
    if (server_)
        server_->shutdown();
    if (engine_)
        engine_->shutdown();
    // Drop ephemeral direct-play queues (their tracks stay in the library).
    if (db_) {
        if (auto qs = db_->listQueues()) {
            for (auto& q : *qs) {
                if (q.temp)
                    (void)db_->deleteQueue(q.id);
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
        // cleanup lock file
        if (lockFd_ >= 0) {
            detail::releaseLock(lockFd_);
            lockFd_ = -1;
            auto lockPath = detail::lockPathForSocket(config_.dbPath, socketPath_);
            std::filesystem::remove(lockPath, ec);
        }
    } catch (...) {
    }
    // Tear down OS media session.
    if (mediaSession_)
        (void)mediaSession_->deactivate();
    // shm handle will be cleaned up via RAII
}

kanariv::contracts::IDatabase& Service::db() noexcept {
    return *impl_->db_;
}

kanariv::utils::Expected<void>
Service::attachDatabase(std::shared_ptr<kanariv::contracts::IDatabase> db) {
    if (!db || !db->isOpen()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db")};
    }
    impl_->db_ = std::move(db);
    return engine().attachDatabase(impl_->db_);
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
    if (!shmWriter_ || !shmWriter_->valid())
        return;
    auto& eng = *engine_;
    int64_t track_id = eng.currentTrackId();
    // Single track fetch (C-2): title/artist/duration come from one lookup.
    std::string title, artist;
    double duration = 0.0;
    if (track_id != 0) {
        if (auto tr = db_->getTrack(track_id); tr) {
            title = tr->title;
            artist = tr->artist;
            duration = tr->duration;
        }
    }
    // O(1) queue size via COUNT(*) (C-2): the old listQueueItems() materialized
    // the full vector of items just for .size().
    size_t qSize = 0;
    {
        int64_t aq = eng.activeQueueId();
        qSize = db_->queueCount(aq);
    }
    kanariv::ipc::ShmStatusUpdate u{};
    u.position = eng.position();
    u.volume = eng.volume();
    u.muted = (eng.volume() == 0.0f);
    u.state = static_cast<int>(eng.state());
    u.track_id = track_id;
    u.title = title;
    u.artist = artist;
    shmWriter_->publish(u);
    shmWriter_->setQueueSize(qSize);
    if (track_id != 0) {
        shmWriter_->setDuration(duration);
    }
}

std::expected<kanariv::ipc::Result, kanariv::utils::Error> Service::Impl::statusResult() {
    auto st = detail::buildStatus(*engine_, *db_);
    if (!st)
        return std::unexpected{st.error()};
    // Bump the status sequence and wake any StatusWait waiters so they
    // observe the latest status.
    status_seq_.fetch_add(1, std::memory_order_release);
    status_cv_.notify_all();
    return kanariv::ipc::Result{*st};
}

void Service::Impl::syncMediaSession() {
    if (!mediaSession_ || !mediaSession_->active())
        return;

    try {
        if (logger_)
            logger_->debug("media session sync started track={}", engine_->currentTrackId());

        int64_t track_id = engine_->currentTrackId();
        std::string title, artist;
        std::optional<std::string> album, artworkPath, artworkBase64;
        std::string artworkMime;
        double duration = 0.0;

        if (track_id != 0) {
            if (auto tr = db_->getTrack(track_id); tr) {
                title = tr->title;
                artist = tr->artist;
                album = tr->album;
                artworkPath = tr->cover_art_path;
                duration = tr->duration;

                if (artworkPath) {
                    std::ifstream ifs(*artworkPath, std::ios::binary);
                    if (ifs) {
                        std::string content((std::istreambuf_iterator<char>(ifs)),
                                            std::istreambuf_iterator<char>());
                        if (!content.empty()) {
                            std::vector<std::byte> bytes;
                            bytes.reserve(content.size());
                            for (char value : content)
                                bytes.push_back(static_cast<std::byte>(value));
                            artworkBase64 = kanariv::utils::Base64::encode(
                                std::span<const std::byte>(bytes.data(), bytes.size()));
                            artworkMime = kanariv::utils::Base64::mimeTypeFromPath(*artworkPath);
                            if (logger_)
                                logger_->debug(
                                    "media session artwork encoded from cover file bytes={}",
                                    bytes.size());
                        }
                    }
                }

                if (!artworkBase64 && !tr->path.empty()) {
                    if (auto embedded = extractEmbeddedCoverArt(tr->path, logger_.get())) {
                        artworkBase64 = kanariv::utils::Base64::encode(std::span<const std::byte>(
                            embedded->first.data(), embedded->first.size()));
                        artworkMime = embedded->second;
                        if (logger_)
                            logger_->debug("media session artwork encoded from embedded bytes={}",
                                           embedded->first.size());
                    }
                }
            }
        }

        // Best-effort: a failed OS update must never break playback dispatch.
        if (logger_)
            logger_->debug("media session sending metadata artwork={}", artworkBase64.has_value());
        // When playback stops at the end of the queue the engine reports track
        // id 0. Keep the last now-playing item so the OS card shows the track
        // that just finished with a Stopped/play state instead of blanking.
        if (track_id != 0) {
            if (auto e = mediaSession_->setMetadata(
                    kanariv::media_session::Metadata{.title = title,
                                                     .artist = artist,
                                                     .album = album,
                                                     .artworkPath = artworkPath,
                                                     .artworkBase64 = artworkBase64,
                                                     .artworkMime = artworkMime,
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
            logger_->error("media session sync failed: {}", error.what());
    } catch (...) {
        if (logger_)
            logger_->error("media session sync failed with unknown exception");
    }
}

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
    return std::visit(DispatchVisitor{this}, cmd);
}

} // namespace kanariv::service
