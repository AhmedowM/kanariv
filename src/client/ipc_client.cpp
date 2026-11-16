#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <kanariv/config.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "ipc_client.hpp"
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace kanariv::client {

namespace {

/// @brief Poll interval while a transfer waits for the peer.
constexpr std::chrono::milliseconds pollInterval{2};

/// @brief Longest single wait for a free named-pipe instance (Windows connect).
constexpr std::chrono::milliseconds connectPollSlice{20};

/// @brief Largest write request per syscall (a full pipe buffer may be smaller).
constexpr std::size_t writeChunk = 4096;

/// @brief Handle value that means "not connected" on both platforms.
constexpr std::intptr_t noHandle = -1;

/// @brief Outcome of a bounded transfer.
enum class IoStatus : std::uint8_t {
    Ok,      ///< The whole requested range was transferred.
    Closed,  ///< The peer closed the connection or the transport failed.
    Timeout, ///< The deadline elapsed before the transfer completed.
    Stopped, ///< The caller's stop token was requested.
};

bool deadlineExpired(IpcClient::Deadline deadline) noexcept {
    return deadline != IpcClient::Deadline::max() && std::chrono::steady_clock::now() >= deadline;
}

/**
 * @brief How long a single poll may wait before the deadline/stop token is re-checked.
 * @details Never zero so the wait cannot spin; never longer than `connectPollSlice`.
 */
std::chrono::milliseconds waitSlice(IpcClient::Deadline deadline) noexcept {
    if (deadline == IpcClient::Deadline::max())
        return connectPollSlice;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return std::chrono::milliseconds{1};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return std::clamp(left, std::chrono::milliseconds{1}, connectPollSlice);
}

kanariv::utils::Error transferError(IoStatus status) {
    switch (status) {
    case IoStatus::Stopped:
        return kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "cancelled");
    case IoStatus::Timeout:
        return kanariv::utils::makeError(kanariv::utils::StatusCode::Timeout, "timeout");
    case IoStatus::Ok:
    case IoStatus::Closed:
        break;
    }
    return kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "connection closed");
}

#ifdef _WIN32

/// @brief Reads exactly `out.size()` bytes, polling so the stop token and deadline are honoured.
IoStatus readExact(HANDLE pipe, std::span<std::byte> out, IpcClient::Deadline deadline,
                   std::stop_token st) {
    std::size_t got = 0;
    while (got < out.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        DWORD read = 0;
        const auto chunk = static_cast<DWORD>(out.size() - got);
        if (::ReadFile(pipe, out.data() + got, chunk, &read, nullptr) == 0) {
            // A nowait pipe reports "nothing buffered yet" as ERROR_NO_DATA;
            // every other failure means the connection is gone.
            if (::GetLastError() != ERROR_NO_DATA)
                return IoStatus::Closed;
            if (deadlineExpired(deadline))
                return IoStatus::Timeout;
            std::this_thread::sleep_for(pollInterval);
            continue;
        }
        if (read == 0)
            return IoStatus::Closed; // The daemon closed the pipe.
        got += static_cast<std::size_t>(read);
    }
    return IoStatus::Ok;
}

/// @brief Writes every byte of `data`, polling so the stop token and deadline are honoured.
IoStatus writeAll(HANDLE pipe, std::span<const std::byte> data, IpcClient::Deadline deadline,
                  std::stop_token st) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        const std::size_t chunk =
            (data.size() - sent < writeChunk) ? data.size() - sent : writeChunk;
        DWORD written = 0;
        if (::WriteFile(pipe, data.data() + sent, static_cast<DWORD>(chunk), &written, nullptr) ==
            0) {
            // A full pipe buffer surfaces as ERROR_NO_DATA; anything else is a
            // dead connection.
            if (::GetLastError() != ERROR_NO_DATA)
                return IoStatus::Closed;
            written = 0;
        }
        if (written == 0) {
            if (deadlineExpired(deadline))
                return IoStatus::Timeout;
            std::this_thread::sleep_for(pollInterval);
            continue;
        }
        sent += static_cast<std::size_t>(written);
    }
    return IoStatus::Ok;
}

#else

/// @brief Waits until `fd` is ready for `events`, the deadline expires, or the stop token fires.
IoStatus waitReady(int fd, short events, IpcClient::Deadline deadline, std::stop_token st) {
    while (true) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        if (deadlineExpired(deadline))
            return IoStatus::Timeout;
        pollfd probe{fd, events, 0};
        const int rc = ::poll(&probe, 1, static_cast<int>(waitSlice(deadline).count()));
        if (rc > 0)
            return IoStatus::Ok;
        if (rc == 0)
            continue; // Slice elapsed: re-check the stop token and deadline.
        if (errno == EINTR)
            continue;
        return IoStatus::Closed;
    }
}

/// @brief Reads exactly `out.size()` bytes from a non-blocking socket.
IoStatus readExact(int fd, std::span<std::byte> out, IpcClient::Deadline deadline,
                   std::stop_token st) {
    std::size_t got = 0;
    while (got < out.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        const ::ssize_t n =
            ::recv(fd, reinterpret_cast<char*>(out.data()) + got, out.size() - got, 0);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0)
            return IoStatus::Closed; // Ordered shutdown by the peer.
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return IoStatus::Closed;
        const auto ready = waitReady(fd, POLLIN, deadline, st);
        if (ready != IoStatus::Ok)
            return ready;
    }
    return IoStatus::Ok;
}

/**
 * @brief Send flags that turn a peer-closed socket into an error instead of SIGPIPE.
 * @details A reused keep-alive connection can be closed by the daemon at any
 * moment; without this flag the default SIGPIPE disposition kills the client
 * before `EPIPE` can be returned to the caller.
 */
#if defined(MSG_NOSIGNAL)
inline constexpr int sendFlags = MSG_NOSIGNAL;
#else
inline constexpr int sendFlags = 0;
#endif

/// @brief Writes every byte of `data` to a non-blocking socket.
IoStatus writeAll(int fd, std::span<const std::byte> data, IpcClient::Deadline deadline,
                  std::stop_token st) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        const ::ssize_t n = ::send(fd, reinterpret_cast<const char*>(data.data()) + sent,
                                   data.size() - sent, sendFlags);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0)
            return IoStatus::Closed;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return IoStatus::Closed;
        const auto ready = waitReady(fd, POLLOUT, deadline, st);
        if (ready != IoStatus::Ok)
            return ready;
    }
    return IoStatus::Ok;
}

#endif

} // namespace

struct IpcClient::Impl {
    /** @brief Connected socket/named pipe path. */
    std::string socketPath;
    /** @brief True if using Windows named pipe, false for Unix socket. */
    bool isWinPipe{false};
    /** @brief Connected handle (pipe HANDLE on Windows, socket fd on POSIX).
     * Atomic so a cancelling thread may read it while the owner closes it.
     * `noHandle` = invalid on both platforms. */
    std::atomic<std::intptr_t> pipeHandle{noHandle};
    /** @brief Atomic request ID counter for framing. */
    std::atomic<uint32_t> nextId{0};

    /**
     * @brief Send raw framed data.
     * @param data Byte span to send (already framed with length prefix).
     * @return void on success, Error on send failure/timeout/cancellation.
     */
    kanariv::utils::Expected<void> rawSend(std::span<const std::byte> data, Deadline deadline,
                                           std::stop_token st);

    /**
     * @brief Receive one framed response.
     * Reads the 4-byte header through `kanariv::ipc` (which owns the layout and
     * the 16 MiB cap), then reads exactly the declared payload length.
     * @return JSON payload (without the length header), or an Error.
     */
    kanariv::utils::Expected<std::string> rawRecv(Deadline deadline, std::stop_token st);
};

IpcClient::IpcClient() : impl_(std::make_unique<Impl>()) {}

kanariv::utils::Expected<IpcClient> IpcClient::connect(const std::filesystem::path& dbPath,
                                                       std::string_view socketPathOverride,
                                                       Deadline deadline, std::stop_token st) {
    kanariv::utils::Expected<std::string> sp;
    if (!socketPathOverride.empty()) {
        sp = std::string(socketPathOverride.data(), socketPathOverride.size());
    } else {
        sp = kanariv::config::socketPathFor(dbPath);
    }
    if (!sp)
        return std::unexpected{sp.error()};
    std::string path = *sp;

#ifdef _WIN32
    std::wstring w;
    w.reserve(path.size());
    for (char c : path)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    HANDLE h = INVALID_HANDLE_VALUE;
    while (true) {
        if (st.stop_requested())
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "cancelled")};
        h = ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                          nullptr);
        if (h != INVALID_HANDLE_VALUE)
            break;
        const DWORD err = ::GetLastError();
        if (err != ERROR_PIPE_BUSY) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                          "CreateFileW connect failed: " + std::to_string(err))};
        }
        // A busy pipe means a live listener whose instances are all
        // momentarily taken (burst load or mid-teardown drain) -- NOT a dead
        // daemon. Wait for one in short slices so the caller's deadline bounds
        // the whole attempt (one fixed wait could outlast it).
        if (deadlineExpired(deadline)) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Timeout,
                                                             "connect timed out")};
        }
        (void)::WaitNamedPipeW(w.c_str(), static_cast<DWORD>(waitSlice(deadline).count()));
    }
    // Non-blocking byte mode: an idle or stalled daemon must not park our
    // transfer loops past the caller's deadline or stop token.
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (::SetNamedPipeHandleState(h, &mode, nullptr, nullptr) == 0) {
        const DWORD modeErr = ::GetLastError();
        ::CloseHandle(h);
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "SetNamedPipeHandleState failed: " +
                                                             std::to_string(modeErr))};
    }
    IpcClient c;
    c.impl_->pipeHandle.store(reinterpret_cast<std::intptr_t>(h));
    c.impl_->socketPath = std::move(path);
    c.impl_->isWinPipe = true;
    return c;
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "socket failed")};
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "socket path too long")};
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    // Non-blocking connect: a wedged daemon with a full backlog parks a
    // blocking connect(2) for tens of seconds, so poll against the deadline.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "fcntl failed")};
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
            ::close(fd);
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "connect failed")};
        }
        const auto ready = waitReady(fd, POLLOUT, deadline, st);
        if (ready != IoStatus::Ok) {
            ::close(fd);
            return std::unexpected{transferError(ready)};
        }
        int soError = 0;
        socklen_t soLen = sizeof(soError);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen) != 0 || soError != 0) {
            ::close(fd);
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "connect failed")};
        }
    }
#if defined(SO_NOSIGPIPE)
    // Platforms without send(MSG_NOSIGNAL) need the socket-level fallback so a
    // write to a peer-closed keep-alive connection cannot raise SIGPIPE.
    int noSigpipe = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif
    IpcClient c;
    c.impl_->pipeHandle.store(fd);
    c.impl_->socketPath = std::move(path);
    c.impl_->isWinPipe = false;
    return c;
#endif
}

IpcClient::~IpcClient() {
    close();
}

IpcClient::IpcClient(IpcClient&& other) noexcept = default;

IpcClient& IpcClient::operator=(IpcClient&& other) noexcept = default;

kanariv::utils::Expected<kanariv::ipc::Result>
IpcClient::send(const kanariv::ipc::Command& cmd, Deadline deadline, std::stop_token st) {
    if (!impl_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    const uint32_t id = impl_->nextId.fetch_add(1) + 1;
    kanariv::ipc::IpcRequest req{id, cmd};
    std::string json = kanariv::ipc::serializeRequest(req);
    auto framed = kanariv::ipc::frame(json);
    if (!framed)
        return std::unexpected{framed.error()};

    if (auto e = impl_->rawSend(*framed, deadline, st); !e)
        return std::unexpected{e.error()};

    auto payload = impl_->rawRecv(deadline, st);
    if (!payload)
        return std::unexpected{payload.error()};

    auto repExp = kanariv::ipc::deserializeReply(*payload);
    if (!repExp)
        return std::unexpected{repExp.error()};
    // The id exists to match a reply to its request; a mismatch is a protocol
    // desync (stale or crossed frame), never a result to act on.
    if (repExp->id != id) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "reply id mismatch")};
    }
    if (!repExp->result)
        return std::unexpected{repExp->result.error()};
    return *(repExp->result);
}

void IpcClient::close() noexcept {
    if (!impl_)
        return;
    const std::intptr_t handle = impl_->pipeHandle.exchange(noHandle);
    if (handle == noHandle)
        return;
#ifdef _WIN32
    ::CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

void IpcClient::abort() noexcept {
    if (!impl_)
        return;
    const std::intptr_t handle = impl_->pipeHandle.load();
    if (handle == noHandle)
        return;
#ifdef _WIN32
    // Backstop only: transfers already poll a non-blocking pipe.
    (void)::CancelIoEx(reinterpret_cast<HANDLE>(handle), nullptr);
#else
    // Wakes a blocked send/recv on another thread; the handle itself stays
    // owned here and is closed by close()/the destructor.
    (void)::shutdown(static_cast<int>(handle), SHUT_RDWR);
#endif
}

kanariv::utils::Expected<void> IpcClient::Impl::rawSend(std::span<const std::byte> data,
                                                        Deadline deadline, std::stop_token st) {
    const std::intptr_t handle = pipeHandle.load();
    if (handle == noHandle) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
#ifdef _WIN32
    const auto status = writeAll(reinterpret_cast<HANDLE>(handle), data, deadline, st);
#else
    const auto status = writeAll(static_cast<int>(handle), data, deadline, st);
#endif
    if (status != IoStatus::Ok)
        return std::unexpected{transferError(status)};
    return {};
}

kanariv::utils::Expected<std::string> IpcClient::Impl::rawRecv(Deadline deadline,
                                                               std::stop_token st) {
    const std::intptr_t handle = pipeHandle.load();
    if (handle == noHandle) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
    std::array<std::byte, kanariv::ipc::frameHeaderBytes> header{};
#ifdef _WIN32
    const auto pipe = reinterpret_cast<HANDLE>(handle);
    const auto headerStatus = readExact(pipe, header, deadline, st);
#else
    const int fd = static_cast<int>(handle);
    const auto headerStatus = readExact(fd, header, deadline, st);
#endif
    if (headerStatus != IoStatus::Ok)
        return std::unexpected{transferError(headerStatus)};
    // The ipc layer owns the header layout and the 16 MiB cap: never honour a
    // peer-claimed length before it has been validated.
    const auto length = kanariv::ipc::decodeFrameLength(header);
    if (!length)
        return std::unexpected{length.error()};
    std::string payload(*length, '\0');
    std::span<std::byte> out{reinterpret_cast<std::byte*>(payload.data()), payload.size()};
#ifdef _WIN32
    const auto payloadStatus = readExact(pipe, out, deadline, st);
#else
    const auto payloadStatus = readExact(fd, out, deadline, st);
#endif
    if (payloadStatus != IoStatus::Ok)
        return std::unexpected{transferError(payloadStatus)};
    return payload;
}

} // namespace kanariv::client
