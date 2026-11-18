#pragma once

/**
 * @file service_impl.hpp
 * @brief Service::Impl definition shared by the service implementation TUs (internal).
 * @ingroup kanariv_service
 * @details Holds component ownership, the command dispatcher and every handle()
 * overload so the public service/core.hpp stays free of the IPC/db/json/engine
 * header weight. Installed nowhere; internal to src/service.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <service/art_cache.hpp>
#include <service/ipc_server.hpp>
#include <service/media_session.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kanariv::service {

std::optional<std::pair<std::vector<std::byte>, std::string>>
extractEmbeddedCoverArt(const std::filesystem::path& path, kanariv::utils::Logger* logger);

namespace detail {
/**
 * @brief Enqueues ids in transactions of at most queueBatchChunk (K5).
 * @return Success, or the first error; chunks committed before it stay.
 */
kanariv::utils::Expected<void> enqueueInChunks(kanariv::contracts::IQueueStore& queues, int64_t qid,
                                               std::span<const int64_t> ids);
} // namespace detail

struct Service::Impl {
    Impl(const ServiceConfig& cfg, kanariv::contracts::Storage storage,
         std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
         std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
         std::filesystem::path lockPath, std::string socketPath, std::intptr_t lockFd);

    kanariv::utils::Expected<void> run(std::stop_token st);
    /** Blocks until run() publishes readiness or exits; see Service::waitUntilRunning. */
    bool waitUntilRunning() const;
    void shutdown();
    /** Status snapshot; a pure read that does not touch the change sequence. */
    kanariv::utils::Expected<kanariv::ipc::Result> statusResult();
    /** Status snapshot after a real state change: bumps the sequence, then reads. */
    kanariv::utils::Expected<kanariv::ipc::Result> statusAfterChange();
    /**
     * @brief Status snapshot after a real state change, tagged with a transition.
     * @details Same as above, then stamps @p transition onto the Status result
     * ("started", "resumed", "already_playing") so clients word their output
     * from the reply instead of a second status probe.
     */
    kanariv::utils::Expected<kanariv::ipc::Result> statusAfterChange(std::string_view transition);
    /** Bumps the status change sequence and wakes StatusWait waiters. */
    void noteStateChanged();
    /** Blocks until the sequence changes or the timeout elapses. */
    void waitForStatusChange(std::chrono::milliseconds timeout);
    kanariv::utils::Expected<kanariv::ipc::Result> dispatch(const kanariv::ipc::Command& cmd);

    /**
     * @brief Requests a media-session refresh (never blocks the caller).
     * @details The worker coalesces requests: it publishes the current engine
     * state once per burst, so a transport command only pays for a wake-up,
     * not for a database lookup and cover-art encoding.
     */
    void requestMediaSessionRefresh();
    /// Worker loop: waits for a request, publishes, repeats while behind.
    void mediaWorkerLoop();
    /// Publishes engine state and encoded art to the OS media session.
    void publishMediaSession();
    /**
     * @brief Encodes cover art for a track, reusing the stat-validated cache.
     * @return Encoded art, or nullopt when the track has none (or a cached miss).
     */
    std::optional<ArtCacheEntry> artworkFor(const kanariv::types::TrackRow& track);

    /// Visitor routing each Command alternative to its handle() overload.
    struct DispatchVisitor {
        Impl* self;
        template <typename T>
        auto operator()(const T& c) const {
            return self->handle(c);
        }
    };

    // Command handlers (defined in dispatch_*.cpp).
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::BookmarkAdd& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::BookmarkGoto& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::BookmarkList& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::BookmarkRemove& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LyricsGet& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LyricsSet& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LyricsRemove& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::ConfigExport& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::ConfigGet& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::ConfigImport& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::ConfigList&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::ConfigReset& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::ConfigSet& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::DeviceList&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::DeviceSet& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::DeviceTest& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::HistoryClear&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::HistoryList& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Info&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibraryAdd& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibraryAnalyze& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibraryList& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result>
    handle(const kanariv::ipc::LibraryMaintenance& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibraryRemove& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibraryScan& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibrarySearch& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::LibraryStats&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Next&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Pause&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Play&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlayFiles&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistDelete& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistCreate& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistAdd& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistExport& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistImport& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistList&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistLoad& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistRename& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistSave& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::PlaylistTracks& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Prev&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Preview&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueAdd& qa);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueAddBatch& qb);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueClear&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueCreate& qc);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueDelete& qd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueList&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueMove& qm);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueQueues&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueRemove& qr);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueRemoveBatch& qb);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueRepeat& qr);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueShuffle& qs);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::QueueSwitch& qs);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Restart&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Resume&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Seek& s);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::SeekBy& s);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Shutdown&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::StatusReq&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::StatusWait&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::Stop&);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::TagEdit& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::TagGet& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::TagBatchEdit& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::TrackRate& cmd);
    kanariv::utils::Expected<kanariv::ipc::Result> handle(const kanariv::ipc::VolumeSet& v);

    ServiceConfig config_;
    kanariv::contracts::Storage storage_;
    std::unique_ptr<kanariv::engine::Engine> engine_;
    std::unique_ptr<IpcServer> server_;
    std::unique_ptr<kanariv::utils::Logger> logger_;
    std::filesystem::path pidPath_;
    std::filesystem::path lockPath_;
    std::string socketPath_;
    std::intptr_t lockFd_{-1};
    std::unique_ptr<kanariv::media_session::MediaSession> mediaSession_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
    /** Set once shutdown() starts; late media-key intents are rejected. */
    std::atomic<bool> shuttingDown_{false};
    // Startup handshake for Host::create(): run() publishes readiness or exit
    // under lifeMtx_ and notifies, so the host blocks instead of polling
    // running() every millisecond.
    mutable std::mutex lifeMtx_;
    mutable std::condition_variable lifeCv_;
    bool ready_{false};
    bool loopExited_{false};

    // Media-session worker: owns every mediaSession_ update and the art cache.
    // Requests are counted, so a burst collapses into one publish per wake-up.
    std::thread mediaThread_;
    std::mutex mediaReqMtx_;
    std::condition_variable mediaReqCv_;
    std::uint64_t mediaReqSeq_{0};
    std::uint64_t mediaDoneSeq_{0};
    bool mediaStop_{false};
    ArtCache artCache_;

    // Serializes command dispatch: one writer into Engine/Database at a time,
    // matching the documented IPC model. StatusWait waits outside this lock.
    std::mutex dispatchMtx_;
    // Guards calls into the OS media-session backend, so dispatch and engine
    // track callbacks never update it concurrently.
    std::mutex mediaMtx_;

    // Status-change notification for StatusWait (long-poll style streaming).
    // Bumped by actual state changes only (engine events and mutating
    // commands); pure reads never bump it, so waiters do not wake each other.
    std::atomic<uint64_t> status_seq_{0};
    std::mutex status_mtx_;
    std::condition_variable status_cv_;
};

} // namespace kanariv::service
