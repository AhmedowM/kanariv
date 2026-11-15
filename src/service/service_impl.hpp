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
#include <kanariv/ipc/shm_status.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <service/ipc_server.hpp>
#include <service/media_session.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::service {

std::optional<std::pair<std::vector<std::byte>, std::string>>
extractEmbeddedCoverArt(const std::filesystem::path& path, kanariv::utils::Logger* logger);

namespace detail {
/**
 * @brief Enqueues ids in transactions of at most kQueueBatchChunk (K5).
 * @return Success, or the first error; chunks committed before it stay.
 */
std::expected<void, kanariv::utils::Error>
enqueueInChunks(kanariv::contracts::IQueueStore& queues, int64_t qid, std::span<const int64_t> ids);
} // namespace detail

struct Service::Impl {
    Impl(const ServiceConfig& cfg, kanariv::contracts::Storage storage,
         std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
         std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
         std::filesystem::path lockPath, std::string socketPath, std::intptr_t lockFd,
         std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName);

    kanariv::utils::Expected<void> run(std::stop_token st);
    void shutdown();
    void updateShmStatus();
    /** Status snapshot; a pure read that does not touch the change sequence. */
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> statusResult();
    /** Status snapshot after a real state change: bumps the sequence, then reads. */
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> statusAfterChange();
    /**
     * @brief Status snapshot after a real state change, tagged with a transition.
     * @details Same as above, then stamps @p transition onto the Status result
     * ("started", "resumed", "already_playing") so clients word their output
     * from the reply instead of a second status probe.
     */
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    statusAfterChange(std::string_view transition);
    /** Bumps the status change sequence and wakes StatusWait waiters. */
    void noteStateChanged();
    /** Blocks until the sequence changes or the timeout elapses. */
    void waitForStatusChange(std::chrono::milliseconds timeout);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    dispatch(const kanariv::ipc::Command& cmd);

    /** Push current engine state to the OS media session. */
    void syncMediaSession();

    /// Visitor routing each Command alternative to its handle() overload.
    struct DispatchVisitor {
        Impl* self;
        template <typename T>
        auto operator()(const T& c) const {
            return self->handle(c);
        }
    };

    // Command handlers (defined in dispatch_*.cpp).
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkAdd& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkGoto& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkList& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkRemove& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LyricsGet& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LyricsSet& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LyricsRemove& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::ConfigExport& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::ConfigGet& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::ConfigImport& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::ConfigList&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::ConfigReset& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::ConfigSet& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::DeviceList&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::DeviceSet& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::DeviceTest& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::HistoryClear&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::HistoryList& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Info&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryAdd& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryList& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryMaintenance& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryRemove& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryScan& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibrarySearch& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryStats&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Next&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Pause&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Play&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlayFiles&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistDelete& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistCreate& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistAdd& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistExport& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistImport& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistList&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistLoad& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistRename& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistSave& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistTracks& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Prev&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Preview&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueAdd& qa);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueClear&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueCreate& qc);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueDelete& qd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueList&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueMove& qm);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueQueues&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueRemove& qr);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueRepeat& qr);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueShuffle& qs);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueSwitch& qs);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Restart&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Resume&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Seek& s);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::SeekBy& s);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::Shutdown&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::StatusReq&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::StatusWait&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Stop&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TagEdit& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TagGet& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TagBatchEdit& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TrackRate& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::VolumeSet& v);

    ServiceConfig config_;
    kanariv::contracts::Storage storage_;
    std::unique_ptr<kanariv::engine::Engine> engine_;
    std::unique_ptr<IpcServer> server_;
    std::unique_ptr<kanariv::utils::Logger> logger_;
    std::filesystem::path pidPath_;
    std::filesystem::path lockPath_;
    std::string socketPath_;
    std::intptr_t lockFd_{-1};
    std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter_;
    std::string shmName_;
    std::unique_ptr<kanariv::media_session::MediaSession> mediaSession_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
    /** Set once shutdown() starts; late media-key intents are rejected. */
    std::atomic<bool> shuttingDown_{false};

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
