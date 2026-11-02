#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOTE: on Windows the real <windows.h> comes first on purpose. Hand-rolled
// block first: clang-format must not sort it.
// clang-format off
#ifdef _WIN32
#include <windows.h>
// Granular API headers used directly; kept after windows.h (Windows-only:
// none of these exist on POSIX).
#include <errhandlingapi.h>
#include <fileapi.h>
#include <handleapi.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <namedpipeapi.h>
#include <processthreadsapi.h>
#include <synchapi.h>
#include <winerror.h>
#include <winnt.h>
#endif
// clang-format on
#include "service_paths.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#endif

#include <kanariv/config.hpp>

#include "config_detail.hpp"

namespace kanariv::service::detail {
std::filesystem::path pidPathForSocket(const std::filesystem::path& dbPath,
                                       const std::string& /*socketPath*/) {
    auto r = kanariv::config::pidPathFor(dbPath);
    if (r)
        return *r;
    auto pp = dbPath.parent_path();
    if (pp.empty())
        pp = std::filesystem::current_path();
    return pp / "kanariv.pid";
}

std::filesystem::path lockPathForSocket(const std::filesystem::path& dbPath,
                                        const std::string& /*socketPath*/) {
    auto r = kanariv::config::lockPathFor(dbPath);
    if (r)
        return *r;
    std::string hex = kanariv::config::detail_paths::hex8ForDb(dbPath);
    auto pidPath = pidPathForSocket(dbPath, "");
    return pidPath.parent_path() / ("kanariv-" + hex + ".lock");
}

std::string socketPathForDb(const std::filesystem::path& dbPath) {
    auto r = kanariv::config::socketPathFor(dbPath);
    if (r)
        return *r;
    // Fallback uses canonical hex8 encoding (consistent with primary socketPathFor)
    std::string hex = kanariv::config::detail_paths::hex8ForDb(dbPath);
#ifdef _WIN32
    return std::string(R"(\\.\pipe\kanariv-)") + hex;
#else
    auto pp = dbPath.parent_path();
    if (pp.empty())
        pp = std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(pp, ec);
    return (pp / ("kanariv-" + hex + ".sock")).generic_string();
#endif
}

bool probeSocketAlive(const std::string& sp) {
#ifdef _WIN32
    if (sp.empty())
        return false;
    std::wstring w;
    w.reserve(sp.size());
    for (char c : sp)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        return true;
    }
    DWORD err = ::GetLastError();
    if (err == 231 /*ERROR_PIPE_BUSY*/) {
        // Pipe exists but all instances busy - treat as alive
        return true;
    }
    if (err == 2 /*ERROR_FILE_NOT_FOUND*/ || err == 109 /*ERROR_BROKEN_PIPE*/) {
        return false;
    }
    // For other errors, try WaitNamedPipe to confirm pipe exists
    if (::WaitNamedPipeW(w.c_str(), 0)) {
        return true;
    }
    return false;
#else
    if (sp.empty())
        return false;
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sp.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::memcpy(addr.sun_path, sp.c_str(), sp.size() + 1);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
    return rc == 0;
#endif
}

bool tryAcquireLock(const std::filesystem::path& lockPath, std::intptr_t& outFd) {
#ifdef _WIN32
    std::error_code ec;
    auto parent = lockPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::wstring wpath;
    wpath.reserve(lockPath.native().size());
    for (wchar_t c : lockPath.native())
        wpath.push_back(c);

    HANDLE h =
        ::CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ, // allow readers, deny writers
                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outFd = -1;
        return false;
    }
    // Try to lock the first byte exclusively, non-blocking
    OVERLAPPED ov{};
    ov.Offset = 0;
    ov.OffsetHigh = 0;
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        ::CloseHandle(h);
        outFd = -1;
        return false; // ERROR_LOCK_VIOLATION (33) or other
    }
    outFd = reinterpret_cast<intptr_t>(h);
    return true;
#else
    std::error_code ec;
    auto parent = lockPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    int fd = ::open(lockPath.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (fd < 0)
        return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    outFd = fd;
    return true;
#endif
}

void releaseLock(std::intptr_t fd) {
    if (fd < 0)
        return;
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
    OVERLAPPED ov{};
    ov.Offset = 0;
    ov.OffsetHigh = 0;
    ::UnlockFileEx(h, 0, 1, 0, &ov);
    ::CloseHandle(h);
#else
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
}

bool checkPidAlive(int pid) {
    if (pid <= 0)
        return false;
#ifndef _WIN32
    return ::kill(pid, 0) == 0;
#else
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    DWORD wait = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return wait == WAIT_TIMEOUT;
#endif
}

std::optional<int> readPidFile(const std::filesystem::path& pidPath) {
    std::error_code ec;
    if (!std::filesystem::exists(pidPath, ec))
        return std::nullopt;
    std::ifstream in(pidPath);
    if (!in)
        return std::nullopt;
    int pid = 0;
    in >> pid;
    if (in.fail())
        return std::nullopt;
    return pid;
}

std::filesystem::path resolveConfigPath(const std::filesystem::path& configPath,
                                        const std::filesystem::path& dbPath) {
    if (!configPath.empty())
        return configPath;
    if (!dbPath.empty()) {
        auto pp = dbPath.parent_path();
        if (!pp.empty())
            return pp / "config.json";
    }
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    if (ec)
        tmp = std::filesystem::path("/tmp");
    return tmp / "kanariv" / "config.json";
}

} // namespace kanariv::service::detail
