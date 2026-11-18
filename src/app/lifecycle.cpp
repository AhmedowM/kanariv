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
#include <kanariv/fwd.hpp>
#include <kanariv/utils/status_format.hpp>
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

#include <algorithm>
#include <app/app_impl.hpp>
#include <app/detail.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <kanariv/app/app.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::app {

namespace {
// True if the pid file names a live process. A file left by a killed daemon
// (crash/taskkill) names a dead pid -- callers clear it instead of waiting.
// Unreadable/garbage content is treated as live (conservative: same 2 s wait
// as before, never a wrong fast-path). PID reuse inside the 100 ms poll
// window is astronomically rare; worst case is the old wait, not corruption.
bool pidFileAlive(const std::filesystem::path& pidPath) {
    std::ifstream f(pidPath, std::ios::binary);
    if (!f)
        return true;
    long long pid = 0;
    f >> pid;
    if (!f || pid <= 0)
        return true;
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h) {
        ::CloseHandle(h);
        return true;
    }
    // No such process => dead; any other error (access denied) => alive.
    return ::GetLastError() != ERROR_INVALID_PARAMETER;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    // ESRCH = no such process => dead; EPERM and others => alive.
    return errno != ESRCH;
#endif
}

#ifndef _WIN32
// A8: resolve the executable path in the parent. The spawned child must not
// allocate or touch the filesystem between fork() and exec() (see the comment
// in spawnDaemon), so this runs before the fork.
std::string resolveExePath(const std::string& fallback) {
#ifdef __linux__
    char linkBuf[4096]{};
    if (ssize_t n = ::readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1); n > 0) {
        linkBuf[n] = '\0';
        return linkBuf;
    }
#elif defined(__APPLE__)
    char buf[4096]{};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return buf;
    std::vector<char> dyn(size);
    if (_NSGetExecutablePath(dyn.data(), &size) == 0)
        return dyn.data();
#else
    // BSD / other POSIX: try /proc/curproc/file then /proc/self/exe.
    char linkBuf[4096]{};
    if (ssize_t n = ::readlink("/proc/curproc/file", linkBuf, sizeof(linkBuf) - 1); n > 0) {
        linkBuf[n] = '\0';
        return linkBuf;
    }
    if (ssize_t n = ::readlink("/proc/self/exe", linkBuf, sizeof(linkBuf) - 1); n > 0) {
        linkBuf[n] = '\0';
        return linkBuf;
    }
#endif
    if (!fallback.empty())
        return fallback;
    return "./kanariv";
}
#endif
} // namespace

App::App(kanariv::config::Config cfg) : impl_(std::make_unique<Impl>()) {
    impl_->setConfig(std::move(cfg));
}

App::~App() = default;

void App::setConfig(kanariv::config::Config cfg) {
    impl_->setConfig(std::move(cfg));
}

void App::setArgv0(std::string argv0) {
    impl_->setArgv0(std::move(argv0));
}

kanariv::config::Config App::Impl::config() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return config_;
}

void App::Impl::setConfig(kanariv::config::Config cfg) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    config_ = std::move(cfg);
}

std::string App::Impl::argv0() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return argv0_;
}

void App::Impl::setArgv0(std::string argv0) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    argv0_ = std::move(argv0);
}

kanariv::client::Client App::Impl::makeClient() const {
    // One snapshot: dbPath and socketPath always come from the same config.
    const auto cfg = config();
    return kanariv::client::Client{cfg.dbPath, cfg.socketPath};
}

bool App::Impl::daemonReachable() const {
    return makeClient().probe();
}

std::filesystem::path App::Impl::pidPathForConfig() const {
    const auto cfg = config();
    // Canonical pid path -- single source via kanariv.config (hash of dbPath + XDG/LOCALAPPDATA)
    auto r = kanariv::config::pidPathFor(cfg.dbPath);
    if (r)
        return *r;
    // Fallback when canonical derivation fails: pid file next to the database.
    auto pp = cfg.dbPath.parent_path();
    if (pp.empty())
        pp = std::filesystem::current_path();
    return pp / "kanariv.pid";
}

std::expected<void, std::uint32_t> App::Impl::spawnDaemon(const kanariv::config::Config& cfg) {
#ifdef _WIN32
    wchar_t exeBuf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        DWORD err = GetLastError();
        if (err == 0)
            err = 1;
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
        wCmd += L"\"";
    }
    // CreateProcessW requires mutable, null-terminated buffer
    std::vector<wchar_t> buf(wCmd.size() + 1, L'\0');
    for (size_t i = 0; i < wCmd.size(); ++i)
        buf[i] = wCmd[i];
    buf[wCmd.size()] = L'\0';
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Use DETACHED_PROCESS only - CREATE_NEW_CONSOLE | DETACHED_PROCESS is invalid
    // (ERROR_INVALID_PARAMETER 87)
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        return std::unexpected{static_cast<std::uint32_t>(err)};
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {};
#else
    // A8: between fork() and exec() only async-signal-safe calls are legal.
    // Another thread of this process may hold the malloc/iostream lock, and
    // that lock stays held in the child; std::string/vector/filesystem work
    // there can deadlock. Resolve the executable path, the config string and
    // the argv array here, then let the child only setsid/chdir/close/execv
    // over parent-built storage (never allocating or touching the filesystem).
    const std::string exePath = resolveExePath(argv0());
    std::string configPath;
    if (!cfg.configPath.empty())
        configPath = cfg.configPath.generic_string();
    char* execArgv[6];
    int execArgc = 0;
    execArgv[execArgc++] = const_cast<char*>(exePath.c_str());
    if (!configPath.empty()) {
        execArgv[execArgc++] = const_cast<char*>("--config");
        execArgv[execArgc++] = const_cast<char*>(configPath.c_str());
    }
    execArgv[execArgc++] = const_cast<char*>("--daemon");
    execArgv[execArgc++] = const_cast<char*>("--foreground");
    execArgv[execArgc] = nullptr;

    const char* const childExe = exePath.c_str();
    pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected{static_cast<std::uint32_t>(errno)};
    }
    if (pid > 0)
        return {};
    // child: async-signal-safe calls only until execv() replaces the image.
    if (setsid() < 0)
        _exit(1);
    (void)chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    execv(childExe, execArgv);
    _exit(1);
#endif
}

AppResult App::Impl::startDaemon(bool foreground, std::stop_token stop) {
    // One config snapshot for the whole start (probe, spawn, readiness).
    const auto cfg = config();
    kanariv::client::Client client{cfg.dbPath, cfg.socketPath};
    if (client.probe()) {
        return Outcome{std::nullopt, std::format("daemon already running at {}", cfg.socketPath),
                       false, false};
    }
    // Death-watch: a previous daemon may be mid-teardown (pipe already dead
    // but pid file present / lock still held while threads join). Spawning
    // into that window dies on the lock, so wait until the pipe is
    // unreachable AND no live daemon owns the pid file (2 s max). A pid
    // file left by a killed daemon is cleared immediately instead of
    // burning the full wait.
    {
        auto pidPath = pidPathForConfig();
        for (int i = 0; i < 20; ++i) {
            bool c = client.probe();
            std::error_code ec;
            bool pidExists = std::filesystem::exists(pidPath, ec);
            bool live = pidExists && pidFileAlive(pidPath);
            if (!c && !live) {
                if (pidExists)
                    std::filesystem::remove(pidPath, ec);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    // Ensure db parent dirs exist before trying to start service (foreground)
    {
        std::error_code ec2;
        auto parent = cfg.dbPath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec2);
    }
    if (foreground) {
        auto started = runForeground(stop);
        if (!started)
            return started;
        if (started->failed) {
            // runForeground reports its own failures as a failed Outcome;
            // lift them onto the single error channel so callers can detect a
            // failed start structurally.
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::State, started->line.value_or("daemon start failed"))};
        }
        return started;
    }
    auto spawnRes = spawnDaemon(cfg);
    if (!spawnRes) {
        std::uint32_t err = spawnRes.error();
        std::string what;
        try {
            what = std::system_category().message(err);
        } catch (...) {
            what = "unknown error";
        }
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::State,
            std::format("start failed: couldn't start daemon ({}: {})", err, what))};
    }
    // Readiness = full StatusReq round-trip (proves the accept loop and the
    // dispatcher are alive, not just that a pipe object exists -- a bare
    // connect can succeed against a dying daemon's draining instance).
    // The whole wait is bounded by one absolute budget (detail::
    // daemonReadyBudget, 5 s): each attempt times out at whatever remains,
    // so a slow spawn cannot stretch the documented deadline.
    const auto deadline = std::chrono::steady_clock::now() + detail::daemonReadyBudget;
    std::string lastDetail;
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto attempt = detail::readinessAttemptTimeout(remaining);
        if (attempt <= std::chrono::milliseconds::zero())
            break;
        kanariv::client::Client probe{cfg.dbPath, cfg.socketPath};
        if (auto ps = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}}, attempt)) {
            if (std::get_if<kanariv::ipc::Status>(&*ps)) {
                return Outcome{std::nullopt, std::format("daemon started at {}", cfg.socketPath),
                               false, false};
            }
            lastDetail = "unexpected reply";
        } else {
            lastDetail = ps.error().message;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero())
            break;
        std::this_thread::sleep_for(std::min(detail::daemonReadyPoll, left));
    }
    std::string msg =
        std::format("daemon start failed: socket not reachable at {}", cfg.socketPath);
    if (!lastDetail.empty())
        msg += std::format("\nlast error: {}", lastDetail);
    return std::unexpected{
        kanariv::utils::makeError(kanariv::utils::StatusCode::State, std::move(msg))};
}

AppResult App::startDaemon(bool foreground, std::stop_token stop) {
    return impl_->startDaemon(foreground, stop);
}

AppResult App::shutdownDaemon() {
    const auto cfg = impl_->config();
    kanariv::client::Client client{cfg.dbPath, cfg.socketPath};
    auto cmd = kanariv::ipc::Command{kanariv::ipc::Shutdown{}};
    auto res = client.send(cmd);
    if (!res && !client.probe()) {
        // Structurally unreachable: there is nothing to shut down.
        return Outcome::fail("shutdown: daemon not running");
    }
    // Even if send failed against a reachable daemon, attempt to poll pid file.
    // poll pid file and socket until daemon exits
    auto pidPath = impl_->pidPathForConfig();
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::error_code ec;
        bool pidExists = std::filesystem::exists(pidPath, ec);
        bool conn = client.probe();
        if (!conn && !pidExists)
            break;
        if (!conn) {
            // socket gone, check pid file stale
            if (!pidExists)
                break;
        }
    }
    bool conn = client.probe();
    if (conn) {
        return Outcome::fail(std::format("shutdown: daemon still running at {}", cfg.socketPath));
    }
    return Outcome{std::nullopt, "daemon stopped", false, false};
}

kanariv::utils::Expected<kanariv::ipc::Result>
App::Impl::sendRaw(const kanariv::ipc::Command& cmd, std::chrono::milliseconds timeout) {
    kanariv::client::Client client = makeClient();
    auto res = client.send(cmd, timeout);
    if (!res) {
        // Classify the failure structurally: a failed liveness probe means the
        // daemon is gone, so report the canonical unreachable-daemon error.
        // A reachable daemon's error is propagated untouched.
        if (!client.probe())
            return std::unexpected{detail::daemonDownError()};
        return std::unexpected{res.error()};
    }
    return res;
}

kanariv::utils::Expected<kanariv::ipc::Result>
App::Impl::sendPlay(const kanariv::ipc::Command& cmd) {
    auto res = sendRaw(cmd);
    if (!res && !daemonReachable()) {
        // Autostart only when the daemon is actually down; a reachable
        // daemon's error is propagated untouched.
        if (!startDaemon(false, {})) {
            // A failed start gets one 500 ms grace period before the single
            // retry; the retry's error is what the caller sees.
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
            return sendRaw(cmd);
        }
        return sendRaw(cmd);
    }
    return res;
}

AppResult Outcome::warn(std::string message) {
    return Outcome{std::nullopt, std::move(message), true, false};
}

AppResult Outcome::fail(std::string message) {
    return Outcome{std::nullopt, std::move(message), true, true};
}

BatchResult BatchReport::fail(std::string message) {
    return BatchReport{std::nullopt, "", std::move(message), 1};
}

AppResult App::Impl::confirm(kanariv::utils::Expected<kanariv::ipc::Result>&& res,
                             std::optional<std::string> line) {
    auto owned = std::move(res);
    if (!owned)
        return std::unexpected{owned.error()};
    return Outcome{std::move(*owned), std::move(line), false};
}

} // namespace kanariv::app
