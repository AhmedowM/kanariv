#pragma once

/**
 * @file service_impl.hpp
 * @brief Service::Impl definition shared by the service implementation TUs (internal).
 * @ingroup caudio_service
 * @details Holds component ownership, the command dispatcher and every handle()
 * overload so the public service/core.hpp stays free of the IPC/db/json/engine
 * header weight. Installed nowhere; internal to src/service.
 */

#include <atomic>
#include <caudio/engine/engine.hpp>
#include <caudio/fwd.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/result.hpp>
#include <caudio/ipc/shm_status.hpp>
#include <caudio/service/service.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/log.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <service/ipc_server.hpp>
#include <stop_token>
#include <string>

namespace caudio::service {

struct Service::Impl {
    Impl(const ServiceConfig& cfg, std::shared_ptr<caudio::contracts::IDatabase> db,
         std::unique_ptr<caudio::engine::Engine> eng, std::unique_ptr<IpcServer> srv,
         std::unique_ptr<caudio::utils::Logger> logger, std::filesystem::path pidPath,
         std::string socketPath, std::intptr_t lockFd,
         std::unique_ptr<caudio::ipc::ShmStatusWriter> shmWriter, std::string shmName);

    caudio::utils::Expected<void> run(std::stop_token st);
    void shutdown();
    void updateShmStatus();
    std::expected<caudio::ipc::Result, caudio::utils::Error> statusResult();
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    dispatch(const caudio::ipc::Command& cmd);

    /// Visitor routing each Command alternative to its handle() overload.
    struct DispatchVisitor {
        Impl* self;
        template <typename T>
        auto operator()(const T& c) const {
            return self->handle(c);
        }
    };

    // Command handlers (defined in dispatch_*.cpp).
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::BookmarkAdd& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::BookmarkGoto& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::BookmarkList& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::BookmarkRemove& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::ConfigExport& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::ConfigGet& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::ConfigImport& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::ConfigList&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::ConfigReset& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::ConfigSet& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::DeviceList&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::DeviceSet& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::DeviceTest& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::HistoryClear&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::HistoryList& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Info&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibraryAdd& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibraryList& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibraryMaintenance& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibraryRemove& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibraryScan& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibrarySearch& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::LibraryStats&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Next&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Pause&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Play&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::PlayFiles&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistDelete& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistCreate& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistAdd& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistExport& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistImport& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistList&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistLoad& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistRename& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistSave& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::PlaylistTracks& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Prev&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Preview&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueAdd& qa);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::QueueClear&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueCreate& qc);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueDelete& qd);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::QueueList&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueMove& qm);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueQueues&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueRemove& qr);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueRepeat& qr);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueShuffle& qs);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::QueueSwitch& qs);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Restart&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Resume&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Seek& s);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Shutdown&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::StatusReq&);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::Stop&);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::TagEdit& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error> handle(const caudio::ipc::TagGet& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::TrackRate& cmd);
    std::expected<caudio::ipc::Result, caudio::utils::Error>
    handle(const caudio::ipc::VolumeSet& v);

    ServiceConfig config_;
    std::shared_ptr<caudio::contracts::IDatabase> db_;
    std::unique_ptr<caudio::engine::Engine> engine_;
    std::unique_ptr<IpcServer> server_;
    std::unique_ptr<caudio::utils::Logger> logger_;
    std::filesystem::path pidPath_;
    std::string socketPath_;
    std::intptr_t lockFd_{-1};
    std::unique_ptr<caudio::ipc::ShmStatusWriter> shmWriter_;
    std::string shmName_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
};

} // namespace caudio::service
