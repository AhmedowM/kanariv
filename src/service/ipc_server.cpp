#include <array>
#include <atomic>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/windows_fwd.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <kanariv/config.hpp>

#include "ipc_server.hpp"

#ifdef _WIN32
namespace {
// Owner-only pipe DACL, so another local user cannot connect to our daemon
// (shutdown/queue control, library/history reads) without any protocol
// change: owner + built-in admins + system get full access, everyone else
// nothing. SDDL is constant; conversion cannot fail short of OOM, and then
// we fall back to default attributes (availability wins, documented).
constexpr wchar_t pipeSddl[] = L"D:(A;;GA;;;OW)(A;;GA;;;BA)(A;;GA;;;SY)";
// Layout-compatible with SECURITY_ATTRIBUTES (length, descriptor, inherit).
struct PipeSecurityAttributes {
    DWORD nLength{0};
    LPVOID descriptor{nullptr};
    BOOL inherit{0};
};
class PipeDacl {
  public:
    PipeDacl() {
        sa_.nLength = sizeof(sa_);
        LPVOID sd = nullptr;
        DWORD len = 0;
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(pipeSddl, sddlRevision1, &sd,
                                                                   &len) &&
            sd) {
            sa_.descriptor = sd;
        }
    }
    ~PipeDacl() {
        if (sa_.descriptor)
            ::LocalFree(sa_.descriptor);
    }
    PipeDacl(const PipeDacl&) = delete;
    PipeDacl& operator=(const PipeDacl&) = delete;
    LPVOID attributes() {
        return sa_.descriptor ? &sa_ : nullptr;
    }

  private:
    PipeSecurityAttributes sa_;
};
} // namespace
#endif

namespace kanariv::service {

// All acceptor state and threads live here so the public header stays free
// of std::thread/mutex/condition_variable/vector includes.
struct IpcServer::Impl {
    // One slot per connection. A connection thread marks its own slot done
    // on exit (retireClient); the accept loop sweeps done slots, so the
    // list holds only live threads and shutdown joins a bounded set.
    // Retire never erases/joins (shutdown may be joining concurrently);
    // erase/swap only happens with no other thread touching the list.
    struct ClientSlot {
        std::jthread thread;
        bool done{false};
    };

    kanariv::utils::Expected<void> listen(const std::filesystem::path& dbPath,
                                         std::string_view socketPathOverride);
    void run(std::stop_token st,
             std::function<std::expected<kanariv::ipc::Result, kanariv::utils::Error>(
                 const kanariv::ipc::Command&)>
                 dispatch);
    void retireClient();
    void shutdown();

    std::atomic<bool> running_{false};
    std::jthread acceptThread_;
    std::vector<ClientSlot> clients_;
    std::mutex clientsMtx_;
    std::mutex cvMtx_;
    std::condition_variable cv_;
    std::string socketPath_;
    std::stop_source stopSource_;
    // Listen handle / listen fd, shared between the accept loop (take/replace)
    // and shutdown/listen (take/close). Atomic take-over (exchange) gives
    // single ownership on every handoff -- no double-close on the shutdown
    // race. -1 = invalid on both platforms.
    std::atomic<std::intptr_t> pipeHandle_{-1};
};

IpcServer::IpcServer() : impl_(std::make_unique<Impl>()) {}

IpcServer::~IpcServer() {
    shutdown();
}

kanariv::utils::Expected<void> IpcServer::listen(const std::filesystem::path& dbPath,
                                                std::string_view socketPathOverride) {
    return impl_->listen(dbPath, socketPathOverride);
}

void IpcServer::run(std::stop_token st,
                    std::function<std::expected<kanariv::ipc::Result, kanariv::utils::Error>(
                        const kanariv::ipc::Command&)>
                        dispatch) {
    impl_->run(st, std::move(dispatch));
}

void IpcServer::retireClient() {
    impl_->retireClient();
}

void IpcServer::shutdown() {
    impl_->shutdown();
}

kanariv::utils::Expected<void> IpcServer::Impl::listen(const std::filesystem::path& dbPath,
                                                      std::string_view socketPathOverride) {
    kanariv::utils::Expected<std::string> sp;
    if (!socketPathOverride.empty()) {
        sp = std::string(socketPathOverride.data(), socketPathOverride.size());
    } else {
        sp = kanariv::config::socketPathFor(dbPath);
    }
    if (!sp)
        return std::unexpected{sp.error()};
    socketPath_ = *sp;
    running_.store(false);

#ifdef _WIN32
    // Atomic take-over: whoever exchanges non-null owns the close.
    HANDLE prev = reinterpret_cast<HANDLE>(pipeHandle_.exchange(-1));
    if (prev && prev != invalidHandleValue) {
        ::CloseHandle(prev);
    }
    std::string pathStr = socketPath_;
    std::wstring w;
    w.reserve(pathStr.size());
    for (char c : pathStr)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    PipeDacl dacl;
    HANDLE h = ::CreateNamedPipeW(w.c_str(), pipeAccessDuplex, pipeTypeByte | pipeWait,
                                  pipeUnlimited, 16384, 16384, 0, dacl.attributes());
    if (h == invalidHandleValue) {
        DWORD err = ::GetLastError();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Io, "CreateNamedPipeW failed: " + std::to_string(err))};
    }
    pipeHandle_.store(reinterpret_cast<std::intptr_t>(h));
    return {};
#else
    int prevFd = pipeHandle_.exchange(-1);
    if (prevFd >= 0) {
        ::close(prevFd);
    }
    std::error_code ec;
    auto parent = std::filesystem::path(socketPath_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    if (!parent.empty() && !ec) {
        // Owner-only daemon dir: Linux ignores socket-file modes for access
        // control, so the containing directory is the real gate (notably the
        // /tmp fallback; XDG_RUNTIME_DIR is 0700 already). Best-effort.
        std::error_code ec2;
        std::filesystem::permissions(parent, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec2);
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "socket failed")};
    }
    std::string sockStr = socketPath_;
    ::unlink(sockStr.c_str());
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockStr.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "socket path too long")};
    }
    std::memcpy(addr.sun_path, sockStr.c_str(), sockStr.size() + 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "bind failed")};
    }
    // Owner-only socket file, best-effort: honored on Darwin/BSD, ignored
    // (harmlessly) on Linux, where the directory gate above applies.
    (void)::fchmod(fd, S_IRWXU);
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "listen failed")};
    }
    pipeHandle_.store(fd);
    return {};
#endif
}

void IpcServer::Impl::run(std::stop_token st,
                          std::function<std::expected<kanariv::ipc::Result, kanariv::utils::Error>(
                              const kanariv::ipc::Command&)>
                              dispatch) {
    if (running_.exchange(true)) {
        return;
    }
    stopSource_ = std::stop_source{};
    acceptThread_ = std::jthread([this, st, dispatch](std::stop_token jt) {
        (void)jt;
        while (!st.stop_requested() && !stopSource_.get_token().stop_requested() &&
               running_.load()) {
#ifdef _WIN32
            HANDLE listenH = reinterpret_cast<HANDLE>(pipeHandle_.load());
            if (pipeHandle_.load() == -1) {
                // No listening instance: a follow-up create failed, or a bad
                // instance was dropped below. Re-create one so the daemon
                // cannot stay deaf with no listener (D-10).
                if (!socketPath_.empty()) {
                    std::string pathStr = socketPath_;
                    std::wstring w;
                    w.reserve(pathStr.size());
                    for (char c : pathStr)
                        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                    PipeDacl dacl;
                    HANDLE h =
                        ::CreateNamedPipeW(w.c_str(), pipeAccessDuplex, pipeTypeByte | pipeWait,
                                           pipeUnlimited, 16384, 16384, 0, dacl.attributes());
                    if (h != invalidHandleValue) {
                        pipeHandle_.store(reinterpret_cast<std::intptr_t>(h));
                        continue;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            BOOL connected = ::ConnectNamedPipe(listenH, nullptr);
            if (connected == 0) {
                DWORD err = ::GetLastError();
                if (err == 535 /*ERROR_PIPE_CONNECTED*/) {
                } else {
                    if (st.stop_requested() || stopSource_.get_token().stop_requested())
                        break;
                    // A client may connect and vanish before we reach
                    // ConnectNamedPipe; the instance then returns ERROR_NO_DATA
                    // (or a broken-pipe variant) forever. Retrying the same
                    // handle spins without ever accepting, leaving a live but
                    // deaf daemon (D-10). Reset the instance so it is reusable.
                    if (err == 232 /*ERROR_NO_DATA*/ || err == 109 /*ERROR_BROKEN_PIPE*/ ||
                        err == 233 /*ERROR_PIPE_NOT_CONNECTED*/) {
                        if (!::DisconnectNamedPipe(listenH)) {
                            // Reset failed: drop the instance; the top of the
                            // loop rebuilds a fresh listening one.
                            HANDLE stale = reinterpret_cast<HANDLE>(pipeHandle_.exchange(-1));
                            if (stale && stale != invalidHandleValue)
                                ::CloseHandle(stale);
                        }
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
            }
            // Take ownership of the connected instance. Shutdown may take it
            // concurrently -- whoever exchanges non-null owns the handle, so
            // a close here can never double-close. (Only this loop creates
            // instances, so a non-null take is always the connected one.)
            HANDLE clientHandle = reinterpret_cast<HANDLE>(pipeHandle_.exchange(-1));
            if (!clientHandle || clientHandle == invalidHandleValue) {
                // Lost the race with shutdown; loop back to the stop check.
                continue;
            }
            {
                std::string pathStr = socketPath_;
                std::wstring w;
                w.reserve(pathStr.size());
                for (char c : pathStr)
                    w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
                PipeDacl nextDacl;
                HANDLE next =
                    ::CreateNamedPipeW(w.c_str(), pipeAccessDuplex, pipeTypeByte | pipeWait,
                                       pipeUnlimited, 16384, 16384, 0, nextDacl.attributes());
                if (next != invalidHandleValue) {
                    pipeHandle_.store(reinterpret_cast<std::intptr_t>(next));
                }
                // else: stay without a listener; the loop re-checks stop
                // flags and shutdown's post-join drain closes strays.
            }
            {
                std::lock_guard<std::mutex> lk(clientsMtx_);
                clients_.emplace_back();
                clients_.back().thread =
                    std::jthread([this, clientHandle, dispatch](std::stop_token ct) {
                        (void)ct;
                        // Self-retire on every exit path (including
                        // exceptions): the slot is swept by the accept loop.
                        struct RetireGuard {
                            Impl* self;
                            ~RetireGuard() {
                                self->retireClient();
                            }
                        } retire{this};
                        try {
                            std::array<std::byte, 4> hdr{};
                            DWORD r = 0;
                            BOOL ok = ::ReadFile(clientHandle, hdr.data(), 4, &r, nullptr);
                            if (ok == 0 || r != 4) {
                                ::CloseHandle(clientHandle);
                                return;
                            }
                            std::uint32_t len =
                                (static_cast<std::uint32_t>(std::to_underlying(hdr[0])) << 24) |
                                (static_cast<std::uint32_t>(std::to_underlying(hdr[1])) << 16) |
                                (static_cast<std::uint32_t>(std::to_underlying(hdr[2])) << 8) |
                                static_cast<std::uint32_t>(std::to_underlying(hdr[3]));
                            if (len > 16 * 1024 * 1024) {
                                ::CloseHandle(clientHandle);
                                return;
                            }
                            std::vector<std::byte> payload(len);
                            if (len > 0) {
                                DWORD rr = 0;
                                ok = ::ReadFile(clientHandle, payload.data(), len, &rr, nullptr);
                                if (ok == 0 || rr != len) {
                                    ::CloseHandle(clientHandle);
                                    return;
                                }
                            }
                            std::string reqStr;
                            reqStr.reserve(payload.size());
                            for (auto b : payload)
                                reqStr.push_back(static_cast<char>(static_cast<unsigned char>(b)));
                            auto reqExp = kanariv::ipc::deserializeRequest(reqStr);
                            kanariv::ipc::IpcReply reply;
                            if (!reqExp) {
                                reply.id = 0;
                                reply.result = std::unexpected{reqExp.error()};
                            } else {
                                reply.id = reqExp->id;
                                auto resExp = dispatch(reqExp->cmd);
                                if (!resExp)
                                    reply.result = std::unexpected{resExp.error()};
                                else
                                    reply.result = *resExp;
                            }
                            std::string repJson = kanariv::ipc::serializeReply(reply);
                            auto framed = kanariv::ipc::frame(repJson);
                            DWORD w2 = 0;
                            BOOL okW = ::WriteFile(clientHandle, framed.data(),
                                                   static_cast<DWORD>(framed.size()), &w2, nullptr);
                            if (!okW) {
                                ::CloseHandle(clientHandle);
                                return;
                            }
                            ::FlushFileBuffers(clientHandle);
                            ::DisconnectNamedPipe(clientHandle);
                            ::CloseHandle(clientHandle);
                        } catch (const std::exception& e) {
                            // Last-resort fallback: a thrown handler used to
                            // just close the pipe, so the client could only
                            // report `ReadFile hdr failed: 109`
                            // (ERROR_BROKEN_PIPE). Try to answer with the
                            // cause first.
                            try {
                                kanariv::ipc::IpcReply errReply;
                                errReply.id = 0;
                                errReply.result = std::unexpected{kanariv::utils::makeError(
                                    kanariv::utils::StatusCode::Internal,
                                    std::string("server error: ") + e.what())};
                                std::string errJson = kanariv::ipc::serializeReply(errReply);
                                auto errFrame = kanariv::ipc::frame(errJson);
                                DWORD w3 = 0;
                                (void)::WriteFile(clientHandle, errFrame.data(),
                                                 static_cast<DWORD>(errFrame.size()), &w3, nullptr);
                                (void)::FlushFileBuffers(clientHandle);
                                (void)::DisconnectNamedPipe(clientHandle);
                            } catch (...) {
                            }
                            ::CloseHandle(clientHandle);
                        } catch (...) {
                            ::CloseHandle(clientHandle);
                        }
                    });
                // Reap exited connections: the list holds live threads only.
                std::erase_if(clients_, [](const ClientSlot& s) { return s.done; });
            }
#else
            int listenSnapshot = static_cast<int>(pipeHandle_.load());
            if (listenSnapshot < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listenSnapshot, &rfds);
            timeval tv{0, 100000};
            int sel = ::select(listenSnapshot + 1, &rfds, nullptr, nullptr, &tv);
            if (sel < 0) {
                if (errno == EINTR)
                    continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (sel == 0)
                continue;
            if (!FD_ISSET(listenSnapshot, &rfds))
                continue;
            int cfd = ::accept(listenSnapshot, nullptr, nullptr);
            if (cfd < 0) {
                if (errno == EINTR)
                    continue;
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(clientsMtx_);
                clients_.emplace_back();
                clients_.back().thread = std::jthread([this, cfd, dispatch](std::stop_token ct) {
                    (void)ct;
                    // Self-retire on every exit path (including
                    // exceptions): the slot is swept by the accept loop.
                    struct RetireGuard {
                        Impl* self;
                        ~RetireGuard() {
                            self->retireClient();
                        }
                    } retire{this};
                    try {
                        std::array<std::byte, 4> hdr{};
                        auto recvExact = [cfd](std::span<std::byte> out) -> bool {
                            std::size_t got = 0;
                            while (got < out.size()) {
                                ::ssize_t n = ::recv(cfd, reinterpret_cast<char*>(out.data()) + got,
                                                     out.size() - got, 0);
                                if (n <= 0) {
                                    if (n < 0 && errno == EINTR)
                                        continue;
                                    return false;
                                }
                                got += static_cast<std::size_t>(n);
                            }
                            return true;
                        };
                        auto sendAll = [cfd](std::span<const std::byte> data) -> bool {
                            std::size_t sent = 0;
                            while (sent < data.size()) {
                                ::ssize_t n =
                                    ::send(cfd, reinterpret_cast<const char*>(data.data()) + sent,
                                           data.size() - sent, 0);
                                if (n < 0) {
                                    if (errno == EINTR)
                                        continue;
                                    return false;
                                }
                                sent += static_cast<std::size_t>(n);
                            }
                            return true;
                        };
                        if (!recvExact(std::span<std::byte>(hdr))) {
                            ::close(cfd);
                            return;
                        }
                        std::uint32_t len =
                            (static_cast<std::uint32_t>(std::to_underlying(hdr[0])) << 24) |
                            (static_cast<std::uint32_t>(std::to_underlying(hdr[1])) << 16) |
                            (static_cast<std::uint32_t>(std::to_underlying(hdr[2])) << 8) |
                            static_cast<std::uint32_t>(std::to_underlying(hdr[3]));
                        if (len > 16 * 1024 * 1024) {
                            ::close(cfd);
                            return;
                        }
                        std::vector<std::byte> payload(len);
                        if (len > 0 && !recvExact(std::span<std::byte>(payload))) {
                            ::close(cfd);
                            return;
                        }
                        std::string reqStr;
                        reqStr.reserve(payload.size());
                        for (auto b : payload)
                            reqStr.push_back(static_cast<char>(static_cast<unsigned char>(b)));
                        auto reqExp = kanariv::ipc::deserializeRequest(reqStr);
                        kanariv::ipc::IpcReply reply;
                        if (!reqExp) {
                            reply.id = 0;
                            reply.result = std::unexpected{reqExp.error()};
                        } else {
                            reply.id = reqExp->id;
                            auto resExp = dispatch(reqExp->cmd);
                            if (!resExp)
                                reply.result = std::unexpected{resExp.error()};
                            else
                                reply.result = *resExp;
                        }
                        std::string repJson = kanariv::ipc::serializeReply(reply);
                        auto framed = kanariv::ipc::frame(repJson);
                        if (!sendAll(framed)) {
                            ::close(cfd);
                            return;
                        }
                        ::close(cfd);
                    } catch (const std::exception& e) {
                        // Answer with the cause instead of silently closing
                        // (matches the Windows branch above).
                        try {
                            kanariv::ipc::IpcReply errReply;
                            errReply.id = 0;
                            errReply.result = std::unexpected{kanariv::utils::makeError(
                                kanariv::utils::StatusCode::Internal,
                                std::string("server error: ") + e.what())};
                            std::string errJson = kanariv::ipc::serializeReply(errReply);
                            auto errFrame = kanariv::ipc::frame(errJson);
                            std::size_t sent = 0;
                            while (sent < errFrame.size()) {
                                ::ssize_t n = ::send(cfd,
                                                     reinterpret_cast<const char*>(
                                                         errFrame.data()) +
                                                         sent,
                                                     errFrame.size() - sent, 0);
                                if (n <= 0) {
                                    if (n < 0 && errno == EINTR)
                                        continue;
                                    break;
                                }
                                sent += static_cast<std::size_t>(n);
                            }
                        } catch (...) {
                        }
                        ::close(cfd);
                    } catch (...) {
                        ::close(cfd);
                    }
                });
                // Reap exited connections: the list holds live threads only.
                std::erase_if(clients_, [](const ClientSlot& s) { return s.done; });
            }
#endif
            {
                std::unique_lock<std::mutex> lk(cvMtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(10));
            }
        }
    });
}

void IpcServer::Impl::retireClient() {
    // Marks the CALLING connection thread done. Never erases/joins/detaches:
    // shutdown may be iterating the list concurrently (it never holds
    // clientsMtx_ while joining, so this brief lock cannot deadlock it).
    const auto me = std::this_thread::get_id();
    std::lock_guard<std::mutex> lk(clientsMtx_);
    for (auto& s : clients_) {
        if (s.thread.get_id() == me) {
            s.done = true;
            break;
        }
    }
}

void IpcServer::Impl::shutdown() {
    bool was = running_.exchange(false);
    (void)was;
    stopSource_.request_stop();
    cv_.notify_all();
#ifdef _WIN32
    // Release a synchronous ConnectNamedPipe the accept thread may be blocked
    // in. CloseHandle from another thread does NOT reliably abort a pending
    // synchronous ConnectNamedPipe, so we instead satisfy it with a throwaway
    // client connection while the listen instance is still open. The loop then
    // re-checks stop_requested and exits. This must run BEFORE the take-over
    // below: once the listen handle is closed no instance is left to connect
    // to, which is exactly the race that hung shutdown (D-10). Retry briefly
    // since the accept loop may be between iterations when we start.
    if (!socketPath_.empty()) {
        std::string pathStr = socketPath_;
        std::wstring w;
        w.reserve(pathStr.size());
        for (char c : pathStr)
            w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        for (int attempt = 0; attempt < 40; ++attempt) {
            HANDLE h = ::CreateFileW(w.c_str(), genericRead | genericWrite, 0, nullptr,
                                     openExisting, 0, nullptr);
            if (h != invalidHandleValue) {
                ::CloseHandle(h);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // Take-over (see listen()): if the accept loop is mid-handshake it owns
    // its handle; anything left here is ours to close.
    HANDLE cur = reinterpret_cast<HANDLE>(pipeHandle_.exchange(-1));
    if (cur && cur != invalidHandleValue) {
        ::CloseHandle(cur);
    }
#else
    int listenCur = static_cast<int>(pipeHandle_.exchange(-1));
    if (listenCur >= 0) {
        ::close(listenCur);
        if (!socketPath_.empty()) {
            std::string s = socketPath_;
            ::unlink(s.c_str());
        }
    }
#endif
    if (acceptThread_.joinable()) {
        acceptThread_.request_stop();
        acceptThread_.join();
    }
#ifdef _WIN32
    // Post-join drain: the accept loop may have stored a fresh listen
    // instance after our pre-join take. Exchange is idempotent.
    HANDLE leftover = reinterpret_cast<HANDLE>(pipeHandle_.exchange(-1));
    if (leftover && leftover != invalidHandleValue) {
        ::CloseHandle(leftover);
    }
#endif
    // NOTE: the accept thread is already joined above, so no new entries
    // appear from here on. Joins run WITHOUT clientsMtx_: a finishing
    // connection only flips its done flag under that lock (retireClient),
    // and holding it across joins would deadlock exactly that path.
    // retireClient never erases/joins, so unlocked iteration is safe.
    {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        for (auto& s : clients_) {
            if (s.thread.joinable())
                s.thread.request_stop();
        }
    }
    for (auto& s : clients_) {
        if (s.thread.joinable())
            s.thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        clients_.clear();
    }
    cv_.notify_all();
}

} // namespace kanariv::service
