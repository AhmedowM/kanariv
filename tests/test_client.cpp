#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <mutex>
#include <optional>
#include <service/ipc_io.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#ifdef _WIN32
#include <kanariv/windows_fwd.hpp>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

using namespace kanariv::test_helpers;

namespace {

namespace ipc = kanariv::ipc;
namespace svcd = kanariv::service::detail;

using Clock = std::chrono::steady_clock;

/// @brief Poll interval for the test server loops (deadline/stop responsive).
constexpr std::chrono::milliseconds pollSlice{2};

#ifdef _WIN32
// Windows headers are intentionally not pulled in: `windows_fwd.hpp` declares
// what this file needs. These constants are local to the test server.
constexpr DWORD pipeNoWait = 0x00000001UL;
constexpr DWORD errorPipeConnected = 535UL;
constexpr DWORD errorPipeListening = 536UL;

std::wstring widenAscii(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}
#endif

std::string uniqueEndpoint(const std::filesystem::path& dir, std::string_view name) {
    static std::atomic<unsigned> counter{0};
    const auto tick = Clock::now().time_since_epoch().count();
    const std::string id = std::string{name} + "-" + std::to_string(tick % 1000000000) + "-" +
                           std::to_string(counter.fetch_add(1));
#ifdef _WIN32
    (void)dir;
    return "\\\\.\\pipe\\kanariv-test-client-" + id;
#else
    return (dir / ("kanariv-test-client-" + id + ".sock")).generic_string();
#endif
}

/** @brief One accepted connection owned by the fake daemon. */
class TestConnection {
  public:
    TestConnection() = default;
#ifdef _WIN32
    explicit TestConnection(HANDLE handle) : handle_(handle) {}
#else
    explicit TestConnection(int fd) : fd_(fd) {}
#endif
    TestConnection(TestConnection&& other) noexcept {
        adopt(other);
    }
    TestConnection& operator=(TestConnection&& other) noexcept {
        if (this != &other) {
            close();
            adopt(other);
        }
        return *this;
    }
    TestConnection(const TestConnection&) = delete;
    TestConnection& operator=(const TestConnection&) = delete;
    ~TestConnection() {
        close();
    }

    void close() noexcept {
#ifdef _WIN32
        if (handle_ && handle_ != invalidHandleValue)
            ::CloseHandle(handle_);
        handle_ = invalidHandleValue;
#else
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
#endif
    }

    bool setNonBlocking() noexcept {
#ifdef _WIN32
        return svcd::setNonBlocking(handle_);
#else
        return svcd::configureSocket(fd_);
#endif
    }

    svcd::IoStatus read(std::span<std::byte> out, std::stop_token st, Clock::time_point deadline) {
#ifdef _WIN32
        return svcd::readExact(handle_, out, st, deadline);
#else
        return svcd::readExact(fd_, out, st, deadline);
#endif
    }

    svcd::IoStatus write(std::span<const std::byte> data, std::stop_token st,
                         Clock::time_point deadline) {
#ifdef _WIN32
        return svcd::writeAll(handle_, data, st, deadline);
#else
        return svcd::writeAll(fd_, data, st, deadline);
#endif
    }

  private:
    void adopt(TestConnection& other) noexcept {
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = invalidHandleValue;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
    }

#ifdef _WIN32
    HANDLE handle_{invalidHandleValue};
#else
    int fd_{-1};
#endif
};

/**
 * @brief One-shot fake daemon used to exercise the client transport.
 * @details Listens on an explicit endpoint, reads exactly one request frame,
 * and either replies (optionally late), echoes a wrong id, or stalls until the
 * client closes. Deliberately bypasses IpcServer so the tests observe only the
 * client's wire behaviour.
 */
class FakeDaemon {
  public:
    enum class Mode {
        Echo,    ///< Reply with the request id after `replyDelay`.
        WrongId, ///< Reply with a different id.
        Stall,   ///< Never reply; wait for the client to close.
    };
    struct Plan {
        Mode mode{Mode::Echo};
        std::chrono::milliseconds replyDelay{0};
    };

    FakeDaemon(std::string endpoint, Plan plan)
        : endpoint_(std::move(endpoint)), plan_(plan), ready_(openListener()) {
        if (ready_)
            thread_ = std::jthread([this](std::stop_token st) { serve(st); });
    }

    ~FakeDaemon() {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
        conn_.close();
        closeListener();
#ifndef _WIN32
        // Endpoint filesystem entry is ours; unlink it after the listener died.
        std::error_code ec;
        std::filesystem::remove(endpoint_, ec);
#endif
    }

    FakeDaemon(const FakeDaemon&) = delete;
    FakeDaemon& operator=(const FakeDaemon&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return ready_;
    }
    [[nodiscard]] bool requestSeen() const noexcept {
        return requestSeen_.load();
    }
    [[nodiscard]] bool clientClosed() const noexcept {
        return clientClosed_.load();
    }
    [[nodiscard]] std::string requestJson() const {
        std::lock_guard lk{mtx_};
        return requestJson_;
    }

  private:
    bool openListener() {
#ifdef _WIN32
        const std::wstring w = widenAscii(endpoint_);
        // Nonblocking listener so a client that never arrives cannot park the
        // worker in ConnectNamedPipe past shutdown.
        listener_ = ::CreateNamedPipeW(w.c_str(), pipeAccessDuplex,
                                       pipeTypeByte | pipeReadModeByte | pipeNoWait, 1, 65536,
                                       65536, 0, nullptr);
        return listener_ != invalidHandleValue;
#else
        listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener_ < 0)
            return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (endpoint_.size() >= sizeof(addr.sun_path)) {
            closeListener();
            return false;
        }
        std::memcpy(addr.sun_path, endpoint_.c_str(), endpoint_.size() + 1);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener_, 1) != 0) {
            closeListener();
            return false;
        }
        return true;
#endif
    }

    void closeListener() noexcept {
#ifdef _WIN32
        if (listener_ && listener_ != invalidHandleValue)
            ::CloseHandle(listener_);
        listener_ = invalidHandleValue;
#else
        if (listener_ >= 0)
            ::close(listener_);
        listener_ = -1;
#endif
    }

    bool waitForClient(std::stop_token st) {
        const auto deadline = Clock::now() + std::chrono::seconds{10};
        while (!st.stop_requested() && Clock::now() < deadline) {
#ifdef _WIN32
            if (::ConnectNamedPipe(listener_, nullptr) != 0) {
                conn_ = TestConnection{listener_};
                listener_ = invalidHandleValue;
                return true;
            }
            const DWORD err = ::GetLastError();
            if (err == errorPipeConnected) {
                conn_ = TestConnection{listener_};
                listener_ = invalidHandleValue;
                return true;
            }
            if (err != errorPipeListening)
                return false;
            std::this_thread::sleep_for(pollSlice);
#else
            pollfd probe{listener_, POLLIN, 0};
            const int rc = ::poll(&probe, 1, static_cast<int>(pollSlice.count()));
            if (rc > 0) {
                const int fd = ::accept(listener_, nullptr, nullptr);
                if (fd < 0)
                    return false;
                conn_ = TestConnection{fd};
                return true;
            }
            if (rc < 0 && errno != EINTR)
                return false;
#endif
        }
        return false;
    }

    std::optional<std::string> readFrame(std::stop_token st) {
        const auto deadline = Clock::now() + std::chrono::seconds{10};
        std::array<std::byte, ipc::kFrameHeaderBytes> header{};
        if (conn_.read(header, st, deadline) != svcd::IoStatus::Ok)
            return std::nullopt;
        const auto length = ipc::decodeFrameLength(header);
        if (!length)
            return std::nullopt;
        std::string payload(*length, '\0');
        std::span<std::byte> out{reinterpret_cast<std::byte*>(payload.data()), payload.size()};
        if (conn_.read(out, st, deadline) != svcd::IoStatus::Ok)
            return std::nullopt;
        return payload;
    }

    void serve(std::stop_token st) {
        if (!waitForClient(st) || !conn_.setNonBlocking())
            return;
        auto request = readFrame(st);
        if (!request)
            return;

        std::uint32_t requestId = 0;
        {
            std::lock_guard lk{mtx_};
            requestJson_ = *request;
        }
        if (auto parsed = ipc::deserializeRequest(*request))
            requestId = parsed->id;
        requestSeen_.store(true);

        if (plan_.mode == Mode::Stall) {
            // The client must close its end on timeout; read until EOF.
            std::array<std::byte, 1> sink{};
            clientClosed_.store(conn_.read(sink, st, Clock::now() + std::chrono::seconds{30}) ==
                                svcd::IoStatus::Closed);
            return;
        }

        const auto replyAt = Clock::now() + plan_.replyDelay;
        while (!st.stop_requested() && Clock::now() < replyAt)
            std::this_thread::sleep_for(pollSlice);
        if (st.stop_requested())
            return;

        ipc::IpcReply reply;
        reply.id = plan_.mode == Mode::WrongId ? requestId + 1 : requestId;
        reply.result = ipc::Result{ipc::Empty{}};
        auto framed = ipc::frame(ipc::serializeReply(reply));
        if (framed)
            (void)conn_.write(*framed, st, Clock::now() + std::chrono::seconds{5});
    }

    std::string endpoint_;
    Plan plan_;
#ifdef _WIN32
    HANDLE listener_{invalidHandleValue};
#else
    int listener_{-1};
#endif
    bool ready_{false};
    std::jthread thread_;
    TestConnection conn_;
    mutable std::mutex mtx_;
    std::string requestJson_;
    std::atomic<bool> requestSeen_{false};
    std::atomic<bool> clientClosed_{false};
};

/** @brief Restores the process working directory when a test changes it. */
struct CurrentPathGuard {
    std::filesystem::path original{std::filesystem::current_path()};
    ~CurrentPathGuard() {
        std::error_code ec;
        std::filesystem::current_path(original, ec);
    }
};

} // namespace

TEST_CASE("client reports a reply id mismatch as corrupt", "[client][c4]") {
    auto dir = tempDirPath("client_wrong_id");
    const auto endpoint = uniqueEndpoint(dir, "wrong-id");
    FakeDaemon daemon(endpoint, {.mode = FakeDaemon::Mode::WrongId});
    REQUIRE(daemon.ok());

    kanariv::client::Client client(dir / "library.db", endpoint);
    auto res =
        client.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}}, std::chrono::seconds{5});
    REQUIRE_FALSE(res.has_value());
    INFO(res.error().message);
    REQUIRE(res.error().code == kanariv::utils::StatusCode::Corrupt);
}

TEST_CASE("waitForStatus forwards its timeout and outwaits the daemon", "[client][c1]") {
    auto dir = tempDirPath("client_status_wait");
    const auto endpoint = uniqueEndpoint(dir, "status-wait");
    // Answer later than the 2 s transport default but inside the 500 ms margin.
    FakeDaemon daemon(
        endpoint, {.mode = FakeDaemon::Mode::Echo, .replyDelay = std::chrono::milliseconds{2200}});
    REQUIRE(daemon.ok());

    kanariv::client::Client client(dir / "library.db", endpoint);
    const auto start = Clock::now();
    auto res = client.waitForStatus(std::chrono::milliseconds{2000});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);

    REQUIRE(res.has_value());
    REQUIRE(elapsed >= std::chrono::milliseconds{2100});
    REQUIRE(elapsed < std::chrono::milliseconds{5000});

    // The daemon's own wait must receive the caller's timeout, not the default.
    auto observed = kanariv::ipc::deserializeRequest(daemon.requestJson());
    REQUIRE(observed.has_value());
    const auto* wait = std::get_if<kanariv::ipc::StatusWait>(&observed->cmd);
    REQUIRE(wait != nullptr);
    REQUIRE(wait->timeout_ms == 2000);
}

TEST_CASE("send times out and closes a stalled connection", "[client][c2]") {
    auto dir = tempDirPath("client_stall");
    const auto endpoint = uniqueEndpoint(dir, "stall");
    FakeDaemon daemon(endpoint, {.mode = FakeDaemon::Mode::Stall});
    REQUIRE(daemon.ok());

    kanariv::client::Client client(dir / "library.db", endpoint);
    const auto start = Clock::now();
    auto res = client.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}},
                           std::chrono::milliseconds{200});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);

    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error().code == kanariv::utils::StatusCode::Timeout);
    REQUIRE(res.error().message == "timeout");
    // Bounded: the stalled peer must not hold the call past its own budget
    // (the worker is stopped and joined, never detached).
    REQUIRE(elapsed >= std::chrono::milliseconds{150});
    REQUIRE(elapsed < std::chrono::milliseconds{2000});
    REQUIRE(daemon.requestSeen());

    // The transport is closed as part of the cancellation, so the peer sees EOF.
    busyWaitUntil([&daemon] { return daemon.clientClosed(); }, std::chrono::seconds{3},
                  std::chrono::milliseconds{5});
    REQUIRE(daemon.clientClosed());
}

TEST_CASE("a scan keeps its long default deadline", "[client][c2]") {
    auto dir = tempDirPath("client_scan_default");
    const auto endpoint = uniqueEndpoint(dir, "scan-default");
    // Longer than the ordinary 2 s default; a LibraryScan must still wait.
    FakeDaemon daemon(
        endpoint, {.mode = FakeDaemon::Mode::Echo, .replyDelay = std::chrono::milliseconds{2300}});
    REQUIRE(daemon.ok());

    kanariv::client::Client client(dir / "library.db", endpoint);
    const auto start = Clock::now();
    auto res = client.send(kanariv::ipc::Command{kanariv::ipc::LibraryScan{}},
                           std::chrono::milliseconds{0});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);

    REQUIRE(res.has_value());
    REQUIRE(elapsed >= std::chrono::milliseconds{2200});
    REQUIRE(elapsed < std::chrono::milliseconds{5000});
}

TEST_CASE("transport policy classifies long-running commands", "[client][c2]") {
    using kanariv::ipc::TransportPolicy;
    namespace ipc = kanariv::ipc;

    // Interactive is the fallback, so unlisted commands cannot be given a
    // longer budget by accident.
    CHECK(ipc::transportPolicyFor(ipc::Command{ipc::Play{}}) == TransportPolicy::Interactive);
    CHECK(ipc::transportPolicyFor(ipc::Command{ipc::StatusReq{}}) == TransportPolicy::Interactive);
    CHECK(ipc::transportPolicyFor(ipc::Command{ipc::Shutdown{}}) == TransportPolicy::Interactive);

    // One request carrying bulk payloads.
    for (const auto& cmd : {ipc::Command{ipc::QueueAddBatch{}},
                            ipc::Command{ipc::QueueRemoveBatch{}},
                            ipc::Command{ipc::PlaylistExport{}},
                            ipc::Command{ipc::PlaylistImport{}},
                            ipc::Command{ipc::LibraryAdd{}},
                            ipc::Command{ipc::LibraryMaintenance{}},
                            ipc::Command{ipc::TagBatchEdit{}}})
        CHECK(ipc::transportPolicyFor(cmd) == TransportPolicy::Batch);

    // A tree walk outlives even the batch budget.
    CHECK(ipc::transportPolicyFor(ipc::Command{ipc::LibraryScan{}}) ==
          TransportPolicy::LongRunning);

    CHECK(ipc::transportDeadline(TransportPolicy::Interactive) == std::chrono::seconds{2});
    CHECK(ipc::transportDeadline(TransportPolicy::Batch) == std::chrono::minutes{5});
    CHECK(ipc::transportDeadline(TransportPolicy::LongRunning) == std::chrono::minutes{30});
}

TEST_CASE("probe returns false quickly when nothing listens", "[client][c3]") {
    auto dir = tempDirPath("client_probe_missing");
    const auto endpoint = uniqueEndpoint(dir, "no-listener");
    kanariv::client::Client client(dir / "library.db", endpoint);

    const auto start = Clock::now();
    REQUIRE_FALSE(client.probe());
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    REQUIRE(elapsed < std::chrono::milliseconds{1000});
}

TEST_CASE("relative database paths resolve like absolute ones", "[client][c6]") {
    auto dir = tempDirPath("client_relative_db");
    {
        std::ofstream f(dir / "library.db");
        REQUIRE(f);
    }
    const kanariv::client::Client absoluteClient(dir / "library.db");

    CurrentPathGuard guard;
    std::filesystem::current_path(dir);
    const kanariv::client::Client relativeClient(std::filesystem::path{"library.db"});

    REQUIRE(relativeClient.dbPath().is_absolute());
    REQUIRE(relativeClient.dbPath().filename() == "library.db");

    std::error_code ec;
    const bool same =
        std::filesystem::equivalent(relativeClient.dbPath(), absoluteClient.dbPath(), ec);
    REQUIRE_FALSE(ec);
    REQUIRE(same);
}
