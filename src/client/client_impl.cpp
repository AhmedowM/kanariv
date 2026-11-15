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
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include "ipc_client.hpp"

namespace kanariv::client {

namespace {

/// @brief Budget for an ordinary command when the caller does not pick one.
constexpr std::chrono::milliseconds defaultSendTimeout{2000};

/// @brief Transport headroom added on top of a `StatusWait`'s own daemon timeout.
constexpr std::chrono::milliseconds statusWaitMargin{500};

/// @brief Budget for the liveness probe (one connect, no command).
constexpr std::chrono::milliseconds probeTimeout{400};

/**
 * @brief Budget for a command that the caller left at its default.
 * @details A few commands legitimately run for minutes (scanning a tree,
 * importing a playlist, batch tag writes, library maintenance). Aborting them
 * on the 2 s default would report a failure while the daemon keeps working on
 * a command the caller already treats as finished.
 */
std::chrono::milliseconds commandTimeout(const kanariv::ipc::Command& cmd) {
    if (std::holds_alternative<kanariv::ipc::LibraryScan>(cmd))
        return std::chrono::minutes{30};
    if (std::holds_alternative<kanariv::ipc::PlaylistImport>(cmd) ||
        std::holds_alternative<kanariv::ipc::LibraryAdd>(cmd) ||
        std::holds_alternative<kanariv::ipc::LibraryMaintenance>(cmd) ||
        std::holds_alternative<kanariv::ipc::TagBatchEdit>(cmd))
        return std::chrono::minutes{5};
    return defaultSendTimeout;
}

/**
 * @brief Canonicalizes a database path before it is used for endpoint derivation.
 * @details Socket/pid/lock names are a hash of the path spelling, so a relative
 * path and the same file spelled absolutely used to derive different endpoints
 * (a running daemon looked dead depending on the CWD). A relative path is
 * resolved once here; an absolute path keeps its spelling because the daemon
 * derives its endpoint from the configured spelling and the two must agree.
 * `weakly_canonical` tolerates a not-yet-created database (probe must work
 * before the first start); `absolute` is the fallback when even that fails.
 */
std::filesystem::path canonicalDbPath(const std::filesystem::path& dbPath) {
    if (dbPath.is_absolute())
        return dbPath;
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(dbPath, ec);
    if (!ec && !canonical.empty())
        return canonical;
    ec.clear();
    auto absolute = std::filesystem::absolute(dbPath, ec);
    if (!ec && !absolute.empty())
        return absolute;
    return dbPath;
}

Config canonicalConfig(Config cfg) {
    cfg.dbPath = canonicalDbPath(cfg.dbPath);
    return cfg;
}

kanariv::utils::Expected<kanariv::ipc::Result> cancelledError() {
    return std::unexpected{
        kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "cancelled")};
}

/**
 * @brief Hand-off point between `Client::send` and its worker thread.
 * @details Lets the caller cancel an in-flight command: once the worker has
 * published its connection, cancelling aborts the transport so a blocked
 * transfer returns promptly and the worker can be joined (never detached).
 */
struct TransportSlot {
    std::mutex mtx;
    std::shared_ptr<IpcClient> conn;
    bool cancelled{false};

    void attach(std::shared_ptr<IpcClient> published) {
        {
            std::lock_guard lk{mtx};
            conn = std::move(published);
            if (!cancelled)
                return;
        }
        abortTransport();
    }

    void cancel() {
        {
            std::lock_guard lk{mtx};
            cancelled = true;
        }
        abortTransport();
    }

  private:
    void abortTransport() {
        std::shared_ptr<IpcClient> open;
        {
            std::lock_guard lk{mtx};
            open = conn;
        }
        if (open)
            open->abort();
    }
};

} // namespace

Client::Client(Config cfg) : config_(canonicalConfig(std::move(cfg))) {}

Client::Client(std::filesystem::path dbPath) : Client(Config{std::move(dbPath)}) {}

Client::Client(std::filesystem::path dbPath, std::string_view socketPath)
    : Client(Config{std::move(dbPath), std::string(socketPath)}) {}

Client::Client(const kanariv::config::Config& cfg) : Client(Config{cfg.dbPath, cfg.socketPath}) {}

const Config& Client::config() const noexcept {
    return config_;
}

std::filesystem::path Client::dbPath() const noexcept {
    return config_.dbPath;
}

bool Client::probe() const {
    // Bounded: a wedged daemon must not stall "is it running?" on startup.
    return IpcClient::connect(config_.dbPath, config_.socketPath,
                              std::chrono::steady_clock::now() + probeTimeout)
        .has_value();
}

kanariv::utils::Expected<kanariv::ipc::Result> Client::send(const kanariv::ipc::Command& cmd,
                                                          std::chrono::milliseconds timeout) {
    const auto budget = timeout.count() > 0 ? timeout : commandTimeout(cmd);
    const auto deadline = std::chrono::steady_clock::now() + budget;

    // The command runs on a worker so the caller can enforce `budget`. The
    // promise is shared and every set_value is guarded, so a completion that
    // races the timeout cannot throw through the worker.
    auto prom = std::make_shared<std::promise<kanariv::utils::Expected<kanariv::ipc::Result>>>();
    auto fut = prom->get_future();
    auto slot = std::make_shared<TransportSlot>();
    kanariv::ipc::Command cmdCopy = cmd;
    Config cfgCopy = config_;

    std::jthread worker(
        [prom, slot, cmdCopy = std::move(cmdCopy), cfgCopy, deadline](std::stop_token st) mutable {
            const auto finish = [prom](kanariv::utils::Expected<kanariv::ipc::Result> value) {
                try {
                    prom->set_value(std::move(value));
                } catch (...) {
                }
            };
            if (st.stop_requested()) {
                finish(cancelledError());
                return;
            }
            auto conn = IpcClient::connect(cfgCopy.dbPath, cfgCopy.socketPath, deadline, st);
            if (!conn) {
                finish(st.stop_requested()
                           ? cancelledError()
                           : kanariv::utils::Expected<kanariv::ipc::Result>{
                                 std::unexpected{kanariv::utils::makeError(
                                     kanariv::utils::StatusCode::State, "daemon not running")}});
                return;
            }
            auto connPtr = std::make_shared<IpcClient>(std::move(*conn));
            slot->attach(connPtr);
            if (st.stop_requested()) {
                finish(cancelledError());
                return;
            }
            auto res = connPtr->send(cmdCopy, deadline, st);
            if (res)
                finish(std::move(*res));
            else
                finish(std::unexpected{res.error()});
        });

    if (fut.wait_for(budget) == std::future_status::ready)
        return fut.get();

    // Timeout: cancel deterministically. request_stop() ends the worker's I/O
    // loops (they poll the token and the same deadline), the slot aborts the
    // transport it may be blocked on, and the join is bounded by one poll
    // slice. No detached threads and no straggler list.
    worker.request_stop();
    slot->cancel();
    worker.join();
    return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "timeout")};
}

kanariv::utils::Expected<kanariv::ipc::Result>
Client::waitForStatus(std::chrono::milliseconds timeout) {
    // The daemon blocks for up to `timeout` before answering, so the transport
    // must outwait it by a margin -- otherwise every default call reports a
    // local timeout while the daemon is still waiting.
    const auto daemonWait = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    return send(kanariv::ipc::Command{kanariv::ipc::StatusWait{static_cast<int>(daemonWait)}},
                timeout + statusWaitMargin);
}

} // namespace kanariv::client
