#pragma once

/**
 * @file ipc_io.hpp
 * @brief Exact-length, cancellable pipe/socket transfers for the IPC server.
 * @ingroup kanariv_service
 * @details The accept loop reads one length-prefixed request and writes one
 * reply per connection. Byte-mode named pipes and stream sockets are allowed
 * to transfer fewer bytes than requested, so every transfer loops until the
 * requested byte count is reached, the peer closes, the caller's stop token
 * fires, or the deadline elapses -- a partial transfer is never fatal.
 *
 * Windows connections run in nonblocking pipe mode (`PIPE_NOWAIT`) so an idle
 * or non-reading client can never park a connection thread past the deadline
 * or a shutdown request; POSIX connections use short receive/send timeouts for
 * the same reason.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <thread>

#ifdef _WIN32
#include <kanariv/windows_fwd.hpp>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace kanariv::service::detail {

/**
 * @brief Outcome of an exact-length transfer.
 * @ingroup kanariv_service
 */
enum class IoStatus : std::uint8_t {
    Ok,      ///< The whole requested range was transferred.
    Closed,  ///< The peer closed the connection or the transport failed.
    Timeout, ///< The deadline elapsed before the transfer completed.
    Stopped, ///< The caller's stop token was requested.
};

/** @brief Largest read request issued per syscall. */
inline constexpr std::size_t kReadChunk = 64 * 1024;
/** @brief Largest write request issued per syscall (a full pipe buffer may be smaller). */
inline constexpr std::size_t kWriteChunk = 4096;
/** @brief Sleep between polls when the transfer would block. */
inline constexpr std::chrono::milliseconds kIoPollInterval{2};

#ifdef _WIN32

// Granular codes used by the transfer loops; distinct names keep them clear of
// the windows.h macros a consumer may have pulled in first.
inline constexpr DWORD kPipeNoWait = 0x00000001UL;     ///< PIPE_NOWAIT.
inline constexpr DWORD kErrorNoData = 232UL;           ///< ERROR_NO_DATA.
inline constexpr DWORD kErrorBrokenPipe = 109UL;       ///< ERROR_BROKEN_PIPE.
inline constexpr DWORD kErrorPipeNotConnected = 233UL; ///< ERROR_PIPE_NOT_CONNECTED.

/**
 * @brief Switches a connected pipe instance to nonblocking byte mode.
 * @ingroup kanariv_service
 * @param pipe Connected pipe instance owned by the connection thread.
 * @return true when the instance now polls instead of blocking.
 */
inline bool setNonBlocking(HANDLE pipe) noexcept {
    DWORD mode = pipeReadModeByte | kPipeNoWait;
    return ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != 0;
}

/**
 * @brief Reads exactly `out.size()` bytes from a pipe.
 * @ingroup kanariv_service
 * @param pipe Connected pipe instance in nonblocking byte mode.
 * @param out Destination span; must be fully filled for `IoStatus::Ok`.
 * @param st Stop token checked between polls.
 * @param deadline Absolute time after which `IoStatus::Timeout` is returned.
 * @return Transfer outcome; a short read keeps looping.
 */
inline IoStatus readExact(HANDLE pipe, std::span<std::byte> out, std::stop_token st,
                          std::chrono::steady_clock::time_point deadline) {
    std::size_t got = 0;
    while (got < out.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        const std::size_t chunk = (out.size() - got < kReadChunk) ? out.size() - got : kReadChunk;
        DWORD transferred = 0;
        if (::ReadFile(pipe, out.data() + got, static_cast<DWORD>(chunk), &transferred, nullptr) !=
            0) {
            if (transferred == 0)
                return IoStatus::Closed; // The client closed the pipe.
            got += static_cast<std::size_t>(transferred);
            continue;
        }
        // Nonblocking pipes report "nothing buffered yet" as ERROR_NO_DATA;
        // every other failure means the connection is gone.
        if (::GetLastError() != kErrorNoData)
            return IoStatus::Closed;
        if (std::chrono::steady_clock::now() >= deadline)
            return IoStatus::Timeout;
        std::this_thread::sleep_for(kIoPollInterval);
    }
    return IoStatus::Ok;
}

/**
 * @brief Writes every byte of `data` to a pipe.
 * @ingroup kanariv_service
 * @param pipe Connected pipe instance in nonblocking byte mode.
 * @param data Bytes to send.
 * @param st Stop token checked between polls.
 * @param deadline Absolute time after which `IoStatus::Timeout` is returned.
 * @return Transfer outcome; a short write keeps looping.
 */
inline IoStatus writeAll(HANDLE pipe, std::span<const std::byte> data, std::stop_token st,
                         std::chrono::steady_clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        const std::size_t chunk =
            (data.size() - sent < kWriteChunk) ? data.size() - sent : kWriteChunk;
        DWORD transferred = 0;
        if (::WriteFile(pipe, data.data() + sent, static_cast<DWORD>(chunk), &transferred,
                        nullptr) == 0) {
            // A full pipe buffer surfaces as ERROR_NO_DATA; anything else is a
            // dead connection.
            if (::GetLastError() != kErrorNoData)
                return IoStatus::Closed;
            transferred = 0;
        }
        if (transferred == 0) {
            if (std::chrono::steady_clock::now() >= deadline)
                return IoStatus::Timeout;
            std::this_thread::sleep_for(kIoPollInterval);
            continue;
        }
        sent += static_cast<std::size_t>(transferred);
    }
    return IoStatus::Ok;
}

#else

/** @brief Receive/send slice that keeps stop-token checks responsive. */
inline constexpr std::chrono::milliseconds kSocketTimeout{100};

/**
 * @brief Bounds blocking sends and receives on a connected socket.
 * @ingroup kanariv_service
 * @param fd Connected socket.
 * @return true when the socket timeouts were applied.
 */
inline bool configureSocket(int fd) noexcept {
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = static_cast<suseconds_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(kSocketTimeout).count());
    const bool recvOk = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    const bool sendOk = ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
    return recvOk && sendOk;
}

inline IoStatus readExact(int fd, std::span<std::byte> out, std::stop_token st,
                          std::chrono::steady_clock::time_point deadline) {
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
        if (std::chrono::steady_clock::now() >= deadline)
            return IoStatus::Timeout;
    }
    return IoStatus::Ok;
}

inline IoStatus writeAll(int fd, std::span<const std::byte> data, std::stop_token st,
                         std::chrono::steady_clock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (st.stop_requested())
            return IoStatus::Stopped;
        const ::ssize_t n =
            ::send(fd, reinterpret_cast<const char*>(data.data()) + sent, data.size() - sent, 0);
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
        if (std::chrono::steady_clock::now() >= deadline)
            return IoStatus::Timeout;
    }
    return IoStatus::Ok;
}

#endif

} // namespace kanariv::service::detail
