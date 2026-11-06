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
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
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
#include <service/ipc_server.hpp>
#include <stop_token>
#include <string>

namespace kanariv::service {

struct Service::Impl {
    Impl(const ServiceConfig& cfg, std::shared_ptr<kanariv::contracts::IDatabase> db,
         std::unique_ptr<kanariv::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
         std::unique_ptr<kanariv::utils::Logger> logger, std::filesystem::path pidPath,
         std::string socketPath, std::intptr_t lockFd,
         std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter, std::string shmName);

    kanariv::utils::Expected<void> run(const std::stop_token& st);
    void shutdown();
    void updateShmStatus() const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> statusResult();
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    dispatch(const kanariv::ipc::Command& cmd);

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
    handle(const kanariv::ipc::BookmarkAdd& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkGoto& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkList& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::BookmarkRemove& cmd) const;
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
    handle(const kanariv::ipc::HistoryClear&) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::HistoryList& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::Info&) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryAdd& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryList& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryMaintenance& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryRemove& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryScan& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibrarySearch& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::LibraryStats&) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Next&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Pause&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Play&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlayFiles&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistDelete& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistCreate& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistAdd& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistExport& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistImport& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistList&) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistLoad& cmd);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistRename& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistSave& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::PlaylistTracks& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Prev&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Preview&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueAdd& qa);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueClear&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueCreate& qc);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueDelete& qd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueList&) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueMove& qm);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::QueueQueues&) const;
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
    handle(const kanariv::ipc::Shutdown&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::StatusReq&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::StatusWait&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error> handle(const kanariv::ipc::Stop&);
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TagEdit& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TagGet& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::TrackRate& cmd) const;
    std::expected<kanariv::ipc::Result, kanariv::utils::Error>
    handle(const kanariv::ipc::VolumeSet& v);

    ServiceConfig config_;
    std::shared_ptr<kanariv::contracts::IDatabase> db_;
    std::unique_ptr<kanariv::engine::Engine> engine_;
    std::unique_ptr<IpcServer> server_;
    std::unique_ptr<kanariv::utils::Logger> logger_;
    std::filesystem::path pidPath_;
    std::string socketPath_;
    std::intptr_t lockFd_{-1};
    std::unique_ptr<kanariv::ipc::ShmStatusWriter> shmWriter_;
    std::string shmName_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};

    // Status-change notification for StatusWait (long-poll style streaming).
    // Incremented on every status-changing operation; StatusWait blocks until
    // it changes or a timeout elapses.
    std::atomic<uint64_t> status_seq_{0};
    std::mutex status_mtx_;
    std::condition_variable status_cv_;
};

} // namespace kanariv::service
