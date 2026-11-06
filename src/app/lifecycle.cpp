/**
 * @file lifecycle.cpp
 * @brief Daemon lifecycle orchestration (start / shutdown / spawn).
 * @ingroup kanariv_app
 * @details Moved verbatim out of the CLI shell (Phase 0): spawn a detached
 * daemon child, wait out a previous daemon's teardown, require a full
 * status round-trip before reporting success, and shut down cleanly.
 * Rendering still happens here; Phase 1 moves it to callers.
 */

#include <cstddef>
#include <kanariv/utils/result.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Granular API headers used directly (spawnDaemon): kept after windows.h.
// clang-format off
#include <errhandlingapi.h>
#include <handleapi.h>
#include <libloaderapi.h>
#include <minwindef.h>
#include <processthreadsapi.h>
// clang-format on
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#include <app/app_impl.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <kanariv/app/app.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::app {

constexpr int daemonProbeMaxAttempts = 20;
constexpr int daemonProbePollMs = 100;
constexpr int daemonReadyMaxAttempts = 50;
constexpr int daemonReadyTimeoutMs = 500;
constexpr int daemonShutdownTimeoutMs = 2000;
constexpr int daemonStartRetryDelayMs = 500;

namespace {
// True if the pid file names a live process. A file left by a killed daemon
// (crash/taskkill) names a dead pid -- callers clear it instead of waiting.
// Unreadable/garbage content is treated as live (conservative: same 2 s wait
// as before, never a wrong fast-path). PID reuse inside the 100 ms poll
// window is astronomically rare; worst case is the old wait, not corruption.
auto pidFileAlive(const std::filesystem::path& pidPath) -> bool {
    std::ifstream f(pidPath, std::ios::binary);
    if (!f) {
        return true;
    }
    long long pid = 0;
    f >> pid;
    if (!f || pid <= 0) {
        return true;
    }
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h != nullptr) {
        ::CloseHandle(h);
        return true;
    }
    // No such process => dead; any other error (access denied) => alive.
    return ::GetLastError() != ERROR_INVALID_PARAMETER;
#else
    if (::kill((pid_t)pid, 0) == 0)
        return true;
    // ESRCH = no such process => dead; EPERM and others => alive.
    return errno != ESRCH;
#endif
}
} // namespace

App::App(kanariv::config::Config cfg) : impl_(std::make_unique<Impl>()) {
    impl_->config_ = std::move(cfg);
}

App::~App() = default;

void App::setConfig(kanariv::config::Config cfg) {
    impl_->config_ = std::move(cfg);
}

void App::setArgv0(std::string argv0) {
    impl_->argv0_ = std::move(argv0);
}

void App::attachDaemonHost(std::function<AppResult(bool foreground)> host) {
    impl_->daemonHost_ = std::move(host);
}

void App::attachPreviewHost(std::function<AppResult(const std::string& file)> host) {
    impl_->previewHost_ = std::move(host);
}

auto App::Impl::pidPathForConfig() const -> std::filesystem::path {
    // Canonical pid path -- single source via kanariv.config (hash of dbPath + XDG/LOCALAPPDATA)
    auto r = kanariv::config::pidPathFor(config_.dbPath);
    if (r) {
        return *r;
    }
    // Fallback when canonical derivation fails: pid file next to the database.
    auto pp = config_.dbPath.parent_path();
    if (pp.empty()) {
        pp = std::filesystem::current_path();
    }
    return pp / "kanariv.pid";
}

auto App::Impl::spawnDaemon(const kanariv::config::Config& cfg)
    -> std::expected<void, std::uint32_t> {
#ifdef _WIN32
    wchar_t exeBuf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        DWORD err = GetLastError();
        if (err == 0) {
            err = 1;
        }
        return std::unexpected{static_cast<std::uint32_t>(err)};
    }
    // Build mutable wide command line: "<exePath>" --daemon --foreground [--config "<configPath>"]
    std::wstring wCmd = L"\"";
    wCmd += exeBuf;
    wCmd += L"\" --daemon --foreground";
    if (!cfg.configPath.empty()) {
        // configPath may contain forward slashes; quoting protects both slash styles
        std::wstring cfgW = cfg.configPath.wstring();
        wCmd += L" --config \"";
        wCmd += cfgW;
        wCmd += L'\"';
    }
    // CreateProcessW requires mutable, null-terminated buffer
    std::vector<wchar_t> buf(wCmd.size() + 1, L'\0');
    for (size_t i = 0; i < wCmd.size(); ++i) {
        buf[i] = wCmd[i]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    buf[wCmd.size()] =
        L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Use DETACHED_PROCESS only - CREATE_NEW_CONSOLE | DETACHED_PROCESS is invalid
    // (ERROR_INVALID_PARAMETER 87)
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (ok == 0) {
        DWORD err = GetLastError();
        return std::unexpected{static_cast<std::uint32_t>(err)};
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {};
#else
    pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected{static_cast<std::uint32_t>(errno)};
    }
    if (pid > 0)
        return {};
    // child
    if (setsid() < 0)
        _exit(1);
    if (chdir("/") != 0) {
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    std::string exePath;
#ifdef __linux__
    {
        char linkBuf[4096]{};
        ssize_t n = readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1);
        if (n > 0) {
            linkBuf[n] = '\0';
            exePath = linkBuf;
        } else {
            exePath = argv0_.empty() ? "/proc/self/exe" : argv0_;
        }
    }
#elif defined(__APPLE__)
    {
        char buf[4096]{};
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            exePath = buf;
        } else {
            std::vector<char> dyn(size);
            if (_NSGetExecutablePath(dyn.data(), &size) == 0) {
                exePath = dyn.data();
            } else if (!argv0_.empty()) {
                exePath = argv0_;
            } else {
                exePath = "./kanariv";
            }
        }
    }
#else
    // BSD / other POSIX: try /proc/curproc/file then fallback to argv0
    {
        char linkBuf[4096]{};
        ssize_t n = readlink("/proc/curproc/file", linkBuf, sizeof(linkBuf) - 1);
        if (n > 0) {
            linkBuf[n] = '\0';
            exePath = linkBuf;
        } else {
            n = readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1);
            if (n > 0) {
                linkBuf[n] = '\0';
                exePath = linkBuf;
            } else if (!argv0_.empty()) {
                exePath = argv0_;
            } else {
                exePath = "./kanariv";
            }
        }
    }
#endif
    if (!cfg.configPath.empty()) {
        std::string cfgStr = cfg.configPath.generic_string();
        execl(exePath.c_str(), exePath.c_str(), "--config", cfgStr.c_str(), "--daemon",
              "--foreground", nullptr);
    } else {
        execl(exePath.c_str(), exePath.c_str(), "--daemon", "--foreground", nullptr);
    }
    _exit(1);
#endif
}

auto App::Impl::startDaemon(bool foreground) -> AppResult {
    kanariv::client::Client client{config_.dbPath, config_.socketPath};
    if (client.probe()) {
        return Outcome{.result = std::nullopt,
                       .line = std::format("daemon already running at {}", config_.socketPath),
                       .toStderr = false,
                       .failed = false};
    }
    // Death-watch: a previous daemon may be mid-teardown (pipe already dead
    // but pid file present / lock still held while threads join). Spawning
    // into that window dies on the lock, so wait until the pipe is
    // unreachable AND no live daemon owns the pid file (2 s max). A pid
    // file left by a killed daemon is cleared immediately instead of
    // burning the full wait.
    {
        auto pidPath = pidPathForConfig();
        for (int i = 0; i < daemonProbeMaxAttempts; ++i) {
            bool c = client.probe();
            std::error_code ec;
            bool pidExists = std::filesystem::exists(pidPath, ec);
            bool live = pidExists && pidFileAlive(pidPath);
            if (!c && !live) {
                if (pidExists) {
                    std::filesystem::remove(pidPath, ec);
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(daemonProbePollMs));
        }
    }
    // Ensure db parent dirs exist before trying to start service (foreground)
    {
        std::error_code ec2;
        auto parent = config_.dbPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec2);
        }
    }
    if (foreground) {
        if (daemonHost_) {
            return daemonHost_(foreground);
        }
        return Outcome::fail("daemon host not configured");
    } else {
        auto spawnRes = spawnDaemon(config_);
        if (!spawnRes) {
            std::uint32_t err = spawnRes.error();
            std::string what;
            try {
                what = std::system_category().message(err);
            } catch (...) {
                what = "unknown error";
            }
            return Outcome::fail(
                std::format("start failed: couldn't start daemon ({}: {})", err, what));
        }
        // Readiness = full StatusReq round-trip (proves the accept loop and the
        // dispatcher are alive, not just that a pipe object exists -- a bare
        // connect can succeed against a dying daemon's draining instance).
        // Budget 5 s: a fresh spawn on a slow/Debug box can exceed the old
        // 1.5 s connect-only budget.
        std::string lastDetail;
        for (int i = 0; i < daemonReadyMaxAttempts; ++i) {
            kanariv::client::Client probe{config_.dbPath, config_.socketPath};
            if (auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}},
                                     std::chrono::milliseconds{daemonReadyTimeoutMs})) {
                if (std::get_if<kanariv::ipc::Status>(&*ps) != nullptr) {
                    return Outcome{.result = std::nullopt,
                                   .line = std::format("daemon started at {}", config_.socketPath),
                                   .toStderr = false,
                                   .failed = false};
                }
                lastDetail = "unexpected reply";
            } else {
                lastDetail = ps.error().message;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(daemonProbePollMs));
        }
        std::string msg =
            std::format("daemon start failed: socket not reachable at {}", config_.socketPath);
        if (!lastDetail.empty()) {
            msg += std::format("\nlast error: {}", lastDetail);
        }
        return Outcome::fail(std::move(msg));
    }
}

auto App::startDaemon(bool foreground) -> AppResult {
    return impl_->startDaemon(foreground);
}

auto App::shutdownDaemon() -> AppResult {
    kanariv::client::Client client{impl_->config_.dbPath, impl_->config_.socketPath};
    auto cmd = kanariv::ipc::Command{kanariv::ipc::Shutdown{}};
    auto res = client.send(cmd, std::chrono::milliseconds{daemonShutdownTimeoutMs});
    if (!res) {
        // if daemon not running, report
        if (res.error().code == kanariv::utils::StatusCode::State) {
            return Outcome::fail("shutdown: daemon not running");
        }
        // even if send failed, attempt to poll pid file
    }
    // poll pid file and socket until daemon exits
    auto pidPath = impl_->pidPathForConfig();
    for (int i = 0; i < daemonProbeMaxAttempts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(daemonProbePollMs));
        std::error_code ec;
        bool pidExists = std::filesystem::exists(pidPath, ec);
        bool conn = client.probe();
        if (!conn && !pidExists) {
            break;
        }
        if (!conn) {
            // socket gone, check pid file stale
            if (!pidExists) {
                break;
            }
        }
    }
    bool conn = client.probe();
    if (conn) {
        return Outcome::fail(
            std::format("shutdown: daemon still running at {}", impl_->config_.socketPath));
    }
    return Outcome{
        .result = std::nullopt, .line = "daemon stopped", .toStderr = false, .failed = false};
}

auto App::Impl::sendRaw(const kanariv::ipc::Command& cmd, std::chrono::milliseconds timeout) const
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    kanariv::client::Client client{config_.dbPath, config_.socketPath};
    auto res = client.send(cmd, timeout);
    if (!res) {
        return std::unexpected{res.error()};
    }
    if (std::holds_alternative<kanariv::utils::Error>(*res)) {
        return std::unexpected{std::get<kanariv::utils::Error>(*res)};
    }
    return std::move(*res);
}

auto App::Impl::sendPlay(const kanariv::ipc::Command& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto res = sendRaw(cmd);
    if (!res) {
        const auto& e = res.error();
        if (e.code == kanariv::utils::StatusCode::State && e.message == "daemon not running") {
            if (!startDaemon(false)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(daemonStartRetryDelayMs));
                auto retry = sendRaw(cmd);
                if (retry) {
                    return retry;
                }
                return std::unexpected{retry.error()};
            }
            return sendRaw(cmd);
        }
    }
    return res;
}

auto Outcome::warn(std::string message) -> AppResult {
    return Outcome{
        .result = std::nullopt, .line = std::move(message), .toStderr = true, .failed = false};
}

auto Outcome::fail(std::string message) -> AppResult {
    return Outcome{
        .result = std::nullopt, .line = std::move(message), .toStderr = true, .failed = true};
}

auto BatchReport::fail(std::string message) -> BatchResult {
    return BatchReport{.json = std::nullopt, .out = "", .err = std::move(message), .exitCode = 1};
}

auto App::Impl::confirm(std::expected<kanariv::ipc::Result, kanariv::utils::Error>&& res,
                        std::optional<std::string> line) -> AppResult {
    auto owned = std::move(res);
    if (!owned) {
        return std::unexpected{owned.error()};
    }
    return Outcome{.result = std::move(*owned), .line = std::move(line), .toStderr = false};
}

} // namespace kanariv::app
