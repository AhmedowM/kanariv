#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <kanariv/config.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ipc_client.hpp"
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace kanariv::client {

struct IpcClient::Impl {
    /** @brief Connected socket/named pipe path. */
    std::string socketPath;
    /** @brief True if using Windows named pipe, false for Unix socket. */
    bool isWinPipe{false};
    /** @brief Connected handle (pipe HANDLE on Windows, socket fd on POSIX).
     * -1 = invalid on both platforms. */
    std::intptr_t pipeHandle{-1};
    /** @brief Atomic request ID counter for framing. */
    std::atomic<uint32_t> nextId{0};

    /**
     * @brief Send raw framed data.
     * @param data Byte span to send (already framed with length prefix).
     * @return void on success, Error on send failure or not connected.
     */
    auto rawSend(std::span<const std::byte> data) const -> kanariv::utils::Expected<void>;

    /**
     * @brief Receive a framed response.
     * Reads 4-byte BE length header, then reads exact payload length.
     * Enforces 16MB max frame size.
     * @return Payload bytes (without length header) on success, Error on failure.
     */
    auto rawRecv() const -> kanariv::utils::Expected<std::vector<std::byte>>;
};

IpcClient::IpcClient() : impl_(std::make_unique<Impl>()) {}

auto IpcClient::connect(const std::filesystem::path& dbPath, std::string_view socketPathOverride)
    -> kanariv::utils::Expected<IpcClient> {
    kanariv::utils::Expected<std::string> sp;
    if (!socketPathOverride.empty()) {
        sp = std::string(socketPathOverride.data(), socketPathOverride.size());
    } else {
        sp = kanariv::config::socketPathFor(dbPath);
    }
    if (!sp) {
        return std::unexpected{sp.error()};
    }
    std::string path = *sp;

#ifdef _WIN32
    std::wstring w;
    w.reserve(path.size());
    for (char c : path) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // A busy pipe means a live listener whose instances are all
            // momentarily taken (burst load or mid-teardown drain) -- NOT a
            // dead daemon. Wait briefly and try once more instead of
            // misreporting "daemon not running".
            constexpr DWORD waitNamedPipeTimeoutMs = 500;
            if (::WaitNamedPipeW(w.c_str(), waitNamedPipeTimeoutMs) != 0) {
                h = ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
                err = (h == INVALID_HANDLE_VALUE) ? ::GetLastError() : ERROR_SUCCESS;
            }
        }
        if (h == INVALID_HANDLE_VALUE) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                          "CreateFileW connect failed: " + std::to_string(err))};
        }
    }
    DWORD mode = PIPE_READMODE_BYTE;
    ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    IpcClient c;
    c.impl_->pipeHandle = reinterpret_cast<std::intptr_t>(h);
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
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "connect failed")};
    }
    IpcClient c;
    c.impl_->pipeHandle = fd;
    c.impl_->socketPath = std::move(path);
    c.impl_->isWinPipe = false;
    return c;
#endif
}

IpcClient::~IpcClient() {
    close();
}

IpcClient::IpcClient(IpcClient&& other) noexcept = default;

auto IpcClient::operator=(IpcClient&& other) noexcept -> IpcClient& = default;

auto IpcClient::send(const kanariv::ipc::Command& cmd)
    -> kanariv::utils::Expected<kanariv::ipc::Result> {
    if (!impl_) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
    uint32_t id = impl_->nextId.fetch_add(1) + 1;
    kanariv::ipc::IpcRequest req{.id = id, .cmd = cmd};
    std::string json = kanariv::ipc::serializeRequest(req);
    auto framed = kanariv::ipc::frame(json);

    if (auto e = impl_->rawSend(framed); !e) {
        return std::unexpected{e.error()};
    }

    auto raw = impl_->rawRecv();
    if (!raw) {
        return std::unexpected{raw.error()};
    }

    // rawRecv already stripped the 4-byte BE header; construct string directly -- do not
    // call deframe
    std::string replyStr;
    replyStr.reserve(raw->size());
    for (auto b : *raw) {
        replyStr.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    }

    auto repExp = kanariv::ipc::deserializeReply(replyStr);
    if (!repExp) {
        return std::unexpected{repExp.error()};
    }
    if (!repExp->result) {
        return std::unexpected{repExp->result.error()};
    }
    return *(repExp->result);
}

void IpcClient::close() noexcept {
    if (!impl_) {
        return;
    }
#ifdef _WIN32
    if (impl_->pipeHandle != -1) {
        ::CloseHandle(reinterpret_cast<HANDLE>(impl_->pipeHandle));
        impl_->pipeHandle = -1;
    }
#else
    if (impl_->pipeHandle >= 0) {
        ::close(static_cast<int>(impl_->pipeHandle));
        impl_->pipeHandle = -1;
    }
#endif
}

auto IpcClient::Impl::rawSend(std::span<const std::byte> data) const
    -> kanariv::utils::Expected<void> {
#ifdef _WIN32
    if (pipeHandle == -1) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
    std::size_t sent = 0;
    while (sent < data.size()) {
        DWORD w = 0;
        BOOL ok = ::WriteFile(reinterpret_cast<HANDLE>(pipeHandle),
                              reinterpret_cast<const char*>(data.data()) + sent,
                              static_cast<DWORD>(data.size() - sent), &w, nullptr);
        if (ok == 0) {
            DWORD err = ::GetLastError();
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Io, "WriteFile failed: " + std::to_string(err))};
        }
        sent += w;
    }
    return {};
#else
    if (pipeHandle < 0) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
    std::size_t sent = 0;
    while (sent < data.size()) {
        ::ssize_t n =
            ::send(static_cast<int>(pipeHandle), reinterpret_cast<const char*>(data.data()) + sent,
                   data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "send failed")};
        }
        sent += static_cast<std::size_t>(n);
    }
    return {};
#endif
}

auto IpcClient::Impl::rawRecv() const -> kanariv::utils::Expected<std::vector<std::byte>> {
#ifdef _WIN32
    if (pipeHandle == -1) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
    std::array<std::byte, 4> hdr{};
    std::size_t got = 0;
    while (got < 4) {
        DWORD r = 0;
        BOOL ok = ::ReadFile(reinterpret_cast<HANDLE>(pipeHandle),
                             reinterpret_cast<char*>(hdr.data()) + got, 4 - static_cast<DWORD>(got),
                             &r, nullptr);
        if (ok == 0) {
            DWORD err = ::GetLastError();
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Io, "ReadFile hdr failed: " + std::to_string(err))};
        }
        if (r == 0) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "pipe closed")};
        }
        got += r;
    }
    constexpr std::uint32_t frameLenShift3 = 24;
    constexpr std::uint32_t frameLenShift2 = 16;
    constexpr std::uint32_t frameLenShift1 = 8;
    constexpr std::size_t maxFrameSizeBytes = 16 * 1024 * 1024;
    constexpr std::uint32_t frameByteIndex3 = 3;
    std::uint32_t len = (static_cast<std::uint32_t>(std::to_underlying(hdr[0])) << frameLenShift3) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        (static_cast<std::uint32_t>(std::to_underlying(hdr[1])) << frameLenShift2) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        (static_cast<std::uint32_t>(std::to_underlying(hdr[2])) << frameLenShift1) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        static_cast<std::uint32_t>(std::to_underlying(hdr[frameByteIndex3])); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (len > maxFrameSizeBytes) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too large")};
    }
    std::vector<std::byte> payload(len);
    got = 0;
    while (got < len) {
        DWORD r = 0;
        BOOL ok = ::ReadFile(reinterpret_cast<HANDLE>(pipeHandle),
                             reinterpret_cast<char*>(payload.data()) + got,
                             static_cast<DWORD>(len - got), &r, nullptr);
        if (ok == 0) {
            DWORD err = ::GetLastError();
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Io, "ReadFile payload failed: " + std::to_string(err))};
        }
        if (r == 0) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "pipe closed")};
        }
        got += r;
    }
    return payload;
#else
    if (pipeHandle < 0) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "not connected")};
    }
    std::array<std::byte, 4> hdr{};
    std::size_t got = 0;
    while (got < 4) {
        ::ssize_t n = ::recv(static_cast<int>(pipeHandle),
                             reinterpret_cast<char*>(hdr.data()) + got, 4 - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "recv hdr failed")};
        }
        if (n == 0)
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "peer closed")};
        got += static_cast<std::size_t>(n);
    }
    constexpr std::uint32_t frameLenShift3 = 24;
    constexpr std::uint32_t frameLenShift2 = 16;
    constexpr std::uint32_t frameLenShift1 = 8;
    constexpr std::size_t maxFrameSizeBytes = 16 * 1024 * 1024;
    constexpr std::uint32_t frameByteIndex3 = 3;
    std::uint32_t len = (static_cast<std::uint32_t>(std::to_underlying(hdr[0])) << frameLenShift3) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        (static_cast<std::uint32_t>(std::to_underlying(hdr[1])) << frameLenShift2) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        (static_cast<std::uint32_t>(std::to_underlying(hdr[2])) << frameLenShift1) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        static_cast<std::uint32_t>(std::to_underlying(hdr[frameByteIndex3])); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (len > maxFrameSizeBytes) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too large")};
    }
    std::vector<std::byte> payload(len);
    got = 0;
    while (got < len) {
        ::ssize_t n = ::recv(static_cast<int>(pipeHandle),
                             reinterpret_cast<char*>(payload.data()) + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "recv payload failed")};
        }
        if (n == 0)
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "peer closed")};
        got += static_cast<std::size_t>(n);
    }
    return payload;
#endif
}

} // namespace kanariv::client
