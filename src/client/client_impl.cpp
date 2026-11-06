#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <future>
#include <kanariv/client/client.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ipc_client.hpp"

namespace kanariv::client {

Client::Client(Config cfg) : config_(std::move(cfg)) {}

Client::Client(std::filesystem::path dbPath) : config_{std::move(dbPath), {}} {}

Client::Client(std::filesystem::path dbPath, std::string_view socketPath)
    : config_{std::move(dbPath), std::string(socketPath.data(), socketPath.size())} {}

Client::Client(const kanariv::config::Config& cfg) : config_{cfg.dbPath, cfg.socketPath} {}

auto Client::config() const noexcept -> const Config& {
    return config_;
}

auto Client::dbPath() const noexcept -> std::filesystem::path {
    return config_.dbPath;
}

auto Client::probe() const -> bool {
    return IpcClient::connect(config_.dbPath, config_.socketPath).has_value();
}

auto Client::send(const kanariv::ipc::Command& cmd, std::chrono::milliseconds timeout)
    -> kanariv::utils::Expected<kanariv::ipc::Result> {
    // Use promise/future + jthread + stop_token for timeout handling.
    // This avoids C alarm and uses chrono::milliseconds + poll/select style wait.
    auto prom = std::make_shared<std::promise<kanariv::utils::Expected<kanariv::ipc::Result>>>();
    auto fut = prom->get_future();
    kanariv::ipc::Command cmdCopy = cmd;
    Config cfgCopy = config_;

    // RAII worker via make_unique per spec
    auto worker = std::make_unique<std::jthread>(
        [prom, cmdCopy = std::move(cmdCopy), cfgCopy](const std::stop_token& st) mutable -> void {
            if (st.stop_requested()) {
                try {
                    prom->set_value(std::unexpected{
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "cancelled")});
                } catch (...) {
                }
                return;
            }
            auto conn = IpcClient::connect(cfgCopy.dbPath, cfgCopy.socketPath);
            if (!conn) {
                try {
                    prom->set_value(std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::State, "daemon not running")});
                } catch (...) {
                }
                return;
            }
            if (st.stop_requested()) {
                try {
                    prom->set_value(std::unexpected{
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "cancelled")});
                } catch (...) {
                }
                return;
            }
            auto res = conn->send(cmdCopy);
            try {
                if (res) {
                    prom->set_value(*res);
                } else {
                    prom->set_value(std::unexpected{res.error()});
                }
            } catch (...) {
            }
        });

    // Handle timeout via chrono::milliseconds -- uses future::wait_for.
    if (fut.wait_for(timeout) == std::future_status::ready) {
        return fut.get();
    } else {
        worker->request_stop();
        // Avoid blocking join beyond timeout: move worker to background storage with reaping and
        // bounded size.
        static std::mutex bgMtx;
        static std::vector<std::unique_ptr<std::jthread>> bg;
        {
            std::scoped_lock lk(bgMtx);
            // Reap completed workers: jthread that has finished is still joinable until joined,
            // but if it has been joined/detached elsewhere it becomes !joinable(). Also check
            // stop_token as best-effort to prune. This prevents unbounded growth.
            bg.erase(std::remove_if(bg.begin(), bg.end(),
                                    [](const std::unique_ptr<std::jthread>& t) -> bool {
                                        return !t->joinable();
                                    }),
                     bg.end());
            // Enforce bounded size (audit B6: limit to 8 background workers)
            constexpr int maxBgWorkers = 8;
            if (bg.size() >= maxBgWorkers) {
                // Drop oldest without blocking join: detach (the worker was
                // stop-requested above) instead of release(), so no thread
                // object leaks; the thread itself runs out on its own.
                // All entries here are joinable (reaped above, mutex held).
                bg.front()->detach();
                bg.erase(bg.begin());
            }
            bg.push_back(std::move(worker));
        }
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "timeout")};
    }
}

auto Client::waitForStatus(std::chrono::milliseconds timeout)
    -> kanariv::utils::Expected<kanariv::ipc::Result> {
    return send(kanariv::ipc::Command{kanariv::ipc::StatusWait{
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count())}});
}

} // namespace kanariv::client
