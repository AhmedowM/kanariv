#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <kanariv/config/runtime.hpp>
#include <kanariv/contracts/storage.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/host.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <optional>
#include <service/ipc_io.hpp>
#include <service/ipc_server.hpp>
#include <service/service_paths.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <kanariv/windows_fwd.hpp>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#endif

using namespace kanariv::service;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

namespace {

ServiceConfig inProcessConfig(const std::filesystem::path& dbPath,
                              const std::filesystem::path& logsDir) {
    ServiceConfig cfg;
    cfg.inProcess = true;
    cfg.logToStderr = false;
    cfg.runtime.database = dbPath;
    cfg.runtime.logs = logsDir;
    return cfg;
}

} // namespace

TEST_CASE("host create starts service in-process", "[service][host]") {
    auto dir = tempDirPath("host_start");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes.has_value());
    auto& host = *hostRes;
    REQUIRE(host->running());
    REQUIRE(host->ready());
    REQUIRE_FALSE(host->error().has_value());
    REQUIRE(host->service().running());
    host->stop();
    REQUIRE_FALSE(host->running());
}

TEST_CASE("host requestStop then wait", "[service][host]") {
    auto dir = tempDirPath("host_stop");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes.has_value());
    auto& host = *hostRes;
    REQUIRE(host->running());

    host->requestStop();
    REQUIRE(host->wait());
    REQUIRE_FALSE(host->running());
    REQUIRE_FALSE(host->error().has_value());
}

TEST_CASE("host stop then re-start is a fresh host", "[service][host]") {
    auto dir = tempDirPath("host_restart");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    {
        auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
        REQUIRE(hostRes.has_value());
        auto& host = *hostRes;
        REQUIRE(host->running());
        host->stop();
    }
    // Destructor calls stop(); a new host can bind the same in-process db path.
    auto hostRes2 = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes2.has_value());
    auto& host2 = *hostRes2;
    REQUIRE(host2->running());
    host2->stop();
}

TEST_CASE("host destroy stops worker without explicit stop", "[service][host]") {
    auto dir = tempDirPath("host_destroy");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    {
        auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
        REQUIRE(hostRes.has_value());
        auto& host = *hostRes;
        REQUIRE(host->running());
    }
    // Out of scope: destructor must join the worker and stop the loop.
}

TEST_CASE("host missing database path fails cleanly", "[service][host]") {
    auto dir = tempDirPath("host_fail");
    // A path whose parent is a file (not a directory) cannot be created.
    auto blocker = dir / "blocker";
    {
        std::ofstream f(blocker);
        REQUIRE(f);
    }
    auto badDb = blocker / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(badDb, logsDir));
    REQUIRE_FALSE(hostRes.has_value());
}

TEST_CASE("queue create maps repeatDefault to RepeatMode", "[service][queue][repeat]") {
    struct Case {
        kanariv::types::RepeatMode repeatDefault;
        kanariv::types::RepeatMode expected;
    };
    // Wire order is authoritative: "one" must store RepeatMode::One (2) and
    // "all" must store RepeatMode::All (1); the handler used to swap them.
    // Config::repeatDefault is already the parsed enum.
    const Case cases[] = {
        {kanariv::types::RepeatMode::One, kanariv::types::RepeatMode::One},
        {kanariv::types::RepeatMode::All, kanariv::types::RepeatMode::All},
        {kanariv::types::RepeatMode::Off, kanariv::types::RepeatMode::Off},
    };
    for (const auto& c : cases) {
        const std::string tag = std::to_string(static_cast<int>(c.repeatDefault));
        auto dir = tempDirPath("qrepeat_" + tag);
        auto dbPath = dir / "library.db";
        auto logsDir = dir / "logs";
        std::filesystem::create_directories(logsDir);

        auto cfg = inProcessConfig(dbPath, logsDir);
        cfg.repeatDefault = c.repeatDefault;

        auto dbRes = kanariv::db::Database::open(dbPath.generic_string());
        REQUIRE(dbRes.has_value());
        std::shared_ptr<kanariv::db::Database> db(std::move(*dbRes));

        auto svcRes = Service::create(cfg, kanariv::db::makeStorage(db));
        REQUIRE(svcRes.has_value());
        auto svc = std::move(*svcRes);

        auto res = svc->dispatchCommand(kanariv::ipc::QueueCreate{.name = "mapped"});
        REQUIRE(res.has_value());
        auto* created = std::get_if<kanariv::ipc::QueueCreated>(&*res);
        REQUIRE(created != nullptr);

        auto q = db->getQueue(created->id);
        REQUIRE(q.has_value());
        REQUIRE(q->repeat_mode == c.expected);
    }
}

TEST_CASE("host exposes service for direct commands", "[service][host]") {
    auto dir = tempDirPath("host_cmd");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes.has_value());
    auto& host = *hostRes;
    // The wrapped service is reachable for host-driven command dispatch.
    REQUIRE(host->service().running());
    host->stop();
}

TEST_CASE("runtime paths empty is a no-op", "[service][runtime]") {
    kanariv::config::RuntimePaths empty;
    REQUIRE(empty.empty());
    REQUIRE(empty.ensureDirectories().has_value());
}

TEST_CASE("runtime paths ensure directories", "[service][runtime]") {
    auto dir = tempDirPath("runtime");
    kanariv::config::RuntimePaths rp;
    rp.database = dir / "sub" / "library.db";
    rp.logs = dir / "logs";
    rp.cache = dir / "cache";
    rp.importedMedia = dir / "imported";
    REQUIRE(rp.ensureDirectories().has_value());
    REQUIRE(std::filesystem::is_directory(dir / "logs"));
    REQUIRE(std::filesystem::is_directory(dir / "cache"));
    REQUIRE(std::filesystem::is_directory(dir / "imported"));
    REQUIRE(std::filesystem::exists(dir / "sub")); // database parent created
}

namespace {

namespace ipc = kanariv::ipc;
namespace svcd = kanariv::service::detail;

using TestClock = std::chrono::steady_clock;

/**
 * @brief Minimal raw transport connection used by the framing-level tests.
 * @details Deliberately bypasses IpcClient so a test can split a header across
 * writes and observe what the server does with a partial transfer (S1).
 */
class RawConnection {
  public:
    RawConnection() = default;
#ifdef _WIN32
    explicit RawConnection(HANDLE handle) : handle_(handle) {}
#else
    explicit RawConnection(int fd) : fd_(fd) {}
#endif
    RawConnection(RawConnection&& other) noexcept {
        adopt(other);
    }
    RawConnection& operator=(RawConnection&& other) noexcept {
        if (this != &other) {
            close();
            adopt(other);
        }
        return *this;
    }
    RawConnection(const RawConnection&) = delete;
    RawConnection& operator=(const RawConnection&) = delete;
    ~RawConnection() {
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

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return handle_ && handle_ != invalidHandleValue;
#else
        return fd_ >= 0;
#endif
    }

    bool makeNonBlocking() noexcept {
#ifdef _WIN32
        return svcd::setNonBlocking(handle_);
#else
        return svcd::configureSocket(fd_);
#endif
    }

    svcd::IoStatus read(std::span<std::byte> out, std::stop_token st,
                        TestClock::time_point deadline) {
#ifdef _WIN32
        return svcd::readExact(handle_, out, st, deadline);
#else
        return svcd::readExact(fd_, out, st, deadline);
#endif
    }

    svcd::IoStatus write(std::span<const std::byte> data, std::stop_token st,
                         TestClock::time_point deadline) {
#ifdef _WIN32
        return svcd::writeAll(handle_, data, st, deadline);
#else
        return svcd::writeAll(fd_, data, st, deadline);
#endif
    }

  private:
    void adopt(RawConnection& other) noexcept {
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

#ifdef _WIN32
std::wstring widenAscii(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}
#endif

std::string uniqueTestSocket(const std::filesystem::path& dir, const std::string& name) {
    static std::atomic<unsigned> counter{0};
    const auto tick = TestClock::now().time_since_epoch().count();
    const std::string id =
        name + "-" + std::to_string(tick % 1000000000) + "-" + std::to_string(counter.fetch_add(1));
#ifdef _WIN32
    (void)dir;
    return "\\\\.\\pipe\\kanariv-test-" + id;
#else
    return (dir / ("kanariv-test-" + id + ".sock")).generic_string();
#endif
}

RawConnection connectRaw(const std::string& path,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
    const auto deadline = TestClock::now() + timeout;
#ifdef _WIN32
    const std::wstring w = widenAscii(path);
    while (TestClock::now() < deadline) {
        HANDLE h = ::CreateFileW(w.c_str(), genericRead | genericWrite, 0, nullptr, openExisting, 0,
                                 nullptr);
        if (h != invalidHandleValue)
            return RawConnection{h};
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
#else
    while (TestClock::now() < deadline) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return RawConnection{};
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() < sizeof(addr.sun_path)) {
            std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                return RawConnection{fd};
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
#endif
    return RawConnection{};
}

bool writeFrame(RawConnection& conn, std::span<const std::byte> bytes, bool splitHeader) {
    const auto deadline = TestClock::now() + std::chrono::seconds{5};
    if (!splitHeader)
        return conn.write(bytes, std::stop_token{}, deadline) == svcd::IoStatus::Ok;
    // Split inside the 4-byte length prefix: the server used to assume one
    // ReadFile returns the whole header (S1).
    constexpr std::size_t kSplit = 2;
    if (conn.write(bytes.first(kSplit), std::stop_token{}, deadline) != svcd::IoStatus::Ok)
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    return conn.write(bytes.subspan(kSplit), std::stop_token{}, deadline) == svcd::IoStatus::Ok;
}

std::optional<ipc::IpcReply>
readReply(RawConnection& conn,
          std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
    const auto deadline = TestClock::now() + timeout;
    std::array<std::byte, 4> hdr{};
    if (conn.read(hdr, std::stop_token{}, deadline) != svcd::IoStatus::Ok)
        return std::nullopt;
    const std::uint32_t len = (static_cast<std::uint32_t>(std::to_underlying(hdr[0])) << 24) |
                              (static_cast<std::uint32_t>(std::to_underlying(hdr[1])) << 16) |
                              (static_cast<std::uint32_t>(std::to_underlying(hdr[2])) << 8) |
                              static_cast<std::uint32_t>(std::to_underlying(hdr[3]));
    if (len == 0 || len > ipc::kMaxFrameBytes)
        return std::nullopt;
    std::vector<std::byte> payload(len);
    if (conn.read(payload, std::stop_token{}, deadline) != svcd::IoStatus::Ok)
        return std::nullopt;
    std::string json(payload.size(), '\0');
    std::memcpy(json.data(), payload.data(), payload.size());
    auto reply = ipc::deserializeReply(json);
    if (!reply)
        return std::nullopt;
    return std::move(*reply);
}

std::expected<ipc::Result, kanariv::utils::Error> okDispatch(const ipc::Command&) {
    return ipc::Result{ipc::Empty{}};
}

/** @brief Stops and joins the server even when an assertion fails. */
struct ServerGuard {
    kanariv::service::IpcServer& server;
    std::stop_source& stop;
    ~ServerGuard() {
        stop.request_stop();
        server.shutdown();
    }
};

} // namespace

TEST_CASE("ipc server reassembles a split request header", "[service][ipc][s1]") {
    auto dir = tempDirPath("ipc_split_header");
    auto socketPath = uniqueTestSocket(dir, "split");
    kanariv::service::IpcServer server;
    REQUIRE(server.listen(dir / "library.db", socketPath).has_value());
    std::stop_source stop;
    server.run(stop.get_token(), okDispatch);
    ServerGuard guard{server, stop};

    auto conn = connectRaw(socketPath);
    REQUIRE(conn.valid());
    REQUIRE(conn.makeNonBlocking());

    const auto framed = ipc::frame(
        ipc::serializeRequest(ipc::IpcRequest{.id = 7, .cmd = ipc::Command{ipc::StatusReq{}}}));
    REQUIRE(framed.has_value());
    REQUIRE(writeFrame(conn, *framed, true));

    auto reply = readReply(conn);
    REQUIRE(reply.has_value());
    REQUIRE(reply->id == 7);
    REQUIRE(reply->result.has_value());
    REQUIRE(std::holds_alternative<ipc::Empty>(*reply->result));
}

TEST_CASE("ipc server reads a payload larger than one pipe buffer", "[service][ipc][s1]") {
    auto dir = tempDirPath("ipc_large_payload");
    auto socketPath = uniqueTestSocket(dir, "large");
    kanariv::service::IpcServer server;
    REQUIRE(server.listen(dir / "library.db", socketPath).has_value());
    std::stop_source stop;
    server.run(stop.get_token(), okDispatch);
    ServerGuard guard{server, stop};

    auto conn = connectRaw(socketPath);
    REQUIRE(conn.valid());
    REQUIRE(conn.makeNonBlocking());

    // Invalid JSON on purpose: only transport reassembly is under test. A
    // single-ReadFile server either parses a truncated request or drops the
    // connection without answering.
    const std::string payload(200000, ' ');
    const auto framed = ipc::frame(payload);
    REQUIRE(framed.has_value());
    REQUIRE(writeFrame(conn, *framed, false));

    auto reply = readReply(conn, std::chrono::seconds{10});
    REQUIRE(reply.has_value());
    REQUIRE_FALSE(reply->result.has_value());
}

TEST_CASE("ipc exact read honours its deadline and stop token", "[service][ipc][s3]") {
    auto dir = tempDirPath("ipc_read_deadline");
    auto socketPath = uniqueTestSocket(dir, "deadline");
    kanariv::service::IpcServer server;
    REQUIRE(server.listen(dir / "library.db", socketPath).has_value());
    std::stop_source stop;
    server.run(stop.get_token(), okDispatch);
    ServerGuard guard{server, stop};

    auto conn = connectRaw(socketPath);
    REQUIRE(conn.valid());
    REQUIRE(conn.makeNonBlocking());

    // The server sends nothing until a request arrives, so this read can only
    // end at the deadline (S3: an idle peer must not park the thread).
    std::array<std::byte, 8> buf{};
    const auto start = TestClock::now();
    const auto outcome = conn.read(buf, std::stop_token{}, start + std::chrono::milliseconds{80});
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(TestClock::now() - start);
    REQUIRE(outcome == svcd::IoStatus::Timeout);
    REQUIRE(elapsed >= std::chrono::milliseconds{50});
    REQUIRE(elapsed < std::chrono::milliseconds{2000});

    std::stop_source cancelled;
    cancelled.request_stop();
    REQUIRE(conn.read(buf, cancelled.get_token(), TestClock::now() + std::chrono::seconds{5}) ==
            svcd::IoStatus::Stopped);
}

TEST_CASE("ipc server refuses connections beyond its cap", "[service][ipc][s3]") {
    auto dir = tempDirPath("ipc_client_cap");
    auto socketPath = uniqueTestSocket(dir, "cap");
    kanariv::service::IpcServer server;
    REQUIRE(server.listen(dir / "library.db", socketPath).has_value());
    std::stop_source stop;
    server.run(stop.get_token(), okDispatch);
    ServerGuard guard{server, stop};

    // 32 idle clients occupy every slot; the 33rd must be refused instead of
    // spawning an unbounded number of threads (S3).
    std::vector<RawConnection> idle;
    for (std::size_t i = 0; i < 32; ++i) {
        auto conn = connectRaw(socketPath);
        REQUIRE(conn.valid());
        idle.push_back(std::move(conn));
    }
    auto extra = connectRaw(socketPath);
    REQUIRE(extra.valid());
    REQUIRE(extra.makeNonBlocking());

    auto reply = readReply(extra);
    REQUIRE(reply.has_value());
    REQUIRE_FALSE(reply->result.has_value());
    REQUIRE(reply->result.error().code == kanariv::utils::StatusCode::Busy);
}

TEST_CASE("ipc server shutdown joins an idle client promptly", "[service][ipc][s3]") {
    auto dir = tempDirPath("ipc_shutdown_idle");
    auto socketPath = uniqueTestSocket(dir, "shutdown");
    kanariv::service::IpcServer server;
    REQUIRE(server.listen(dir / "library.db", socketPath).has_value());
    std::stop_source stop;
    server.run(stop.get_token(), okDispatch);
    ServerGuard guard{server, stop};

    auto conn = connectRaw(socketPath);
    REQUIRE(conn.valid());
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const auto start = TestClock::now();
    stop.request_stop();
    server.shutdown();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(TestClock::now() - start);
    REQUIRE(elapsed < std::chrono::milliseconds{1500});
}

TEST_CASE("service without a database reports status and rejects db commands", "[service][s4]") {
    auto dir = tempDirPath("service_nodb");
    auto cfg = inProcessConfig(dir / "library.db", dir / "logs");
    std::filesystem::create_directories(cfg.runtime.logs);

    auto svcRes = Service::create(cfg);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(*svcRes);
    REQUIRE_FALSE(svc->storage().complete());

    // Status is a pure engine read and must work without a database (S4).
    auto statusRes = svc->dispatchCommand(kanariv::ipc::StatusReq{});
    REQUIRE(statusRes.has_value());
    auto* status = std::get_if<kanariv::ipc::Status>(&*statusRes);
    REQUIRE(status != nullptr);
    REQUIRE(status->state == kanariv::types::PlaybackState::Stopped);

    // Library/queue commands must fail cleanly instead of dereferencing null.
    auto listRes = svc->dispatchCommand(kanariv::ipc::QueueList{});
    REQUIRE_FALSE(listRes.has_value());
    REQUIRE(listRes.error().code == kanariv::utils::StatusCode::State);

    auto volumeRes = svc->dispatchCommand(kanariv::ipc::VolumeSet{.level = 40.0f});
    REQUIRE(volumeRes.has_value());
}

TEST_CASE("status reads do not wake a waiting StatusWait", "[service][s5]") {
    auto dir = tempDirPath("service_status_read");
    auto cfg = inProcessConfig(dir / "library.db", dir / "logs");
    std::filesystem::create_directories(cfg.runtime.logs);
    auto svcRes = Service::create(cfg);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(*svcRes);

    std::atomic<long long> elapsedMs{0};
    std::atomic<bool> waitOk{false};
    std::jthread waiter([&] {
        const auto start = TestClock::now();
        auto res = svc->dispatchCommand(kanariv::ipc::StatusWait{.timeout_ms = 300});
        waitOk.store(res.has_value());
        elapsedMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(TestClock::now() - start)
                .count());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    for (int i = 0; i < 5; ++i) {
        auto res = svc->dispatchCommand(kanariv::ipc::StatusReq{});
        REQUIRE(res.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    waiter.join();
    REQUIRE(waitOk.load());
    // Five reads must not have ended the wait early: it ran out its timeout.
    REQUIRE(elapsedMs.load() >= 200);
}

TEST_CASE("a volume change wakes a waiting StatusWait", "[service][s5]") {
    auto dir = tempDirPath("service_status_wake");
    auto cfg = inProcessConfig(dir / "library.db", dir / "logs");
    std::filesystem::create_directories(cfg.runtime.logs);
    auto svcRes = Service::create(cfg);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(*svcRes);

    std::atomic<long long> elapsedMs{0};
    std::atomic<bool> waitOk{false};
    std::atomic<float> seenVolume{-1.0f};
    std::jthread waiter([&] {
        const auto start = TestClock::now();
        auto res = svc->dispatchCommand(kanariv::ipc::StatusWait{.timeout_ms = 4000});
        if (res) {
            waitOk.store(true);
            if (auto* status = std::get_if<kanariv::ipc::Status>(&*res))
                seenVolume.store(status->vol);
        }
        elapsedMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(TestClock::now() - start)
                .count());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    auto volumeRes = svc->dispatchCommand(kanariv::ipc::VolumeSet{.level = 42.0f});
    REQUIRE(volumeRes.has_value());
    waiter.join();
    REQUIRE(waitOk.load());
    REQUIRE(elapsedMs.load() < 3000);
    REQUIRE(seenVolume.load() > 0.40f);
    REQUIRE(seenVolume.load() < 0.44f);
}

TEST_CASE("queue remove distinguishes positions from track ids", "[service][queue][s6]") {
    auto dir = tempDirPath("queue_remove_s6");
    std::filesystem::create_directories(dir / "logs");
    auto dbRes = kanariv::db::Database::open((dir / "library.db").generic_string());
    REQUIRE(dbRes.has_value());
    std::shared_ptr<kanariv::db::Database> db(std::move(*dbRes));

    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "track" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<std::uint8_t>(i * 10 + b);
        auto id = db->insertTrack(t);
        REQUIRE(id.has_value());
        ids.push_back(*id);
        REQUIRE(db->queueEnqueue(1, *id, -1).has_value());
    }

    auto svcRes = Service::create(inProcessConfig(dir / "library.db", dir / "logs"),
                                  kanariv::db::makeStorage(db));
    REQUIRE(svcRes.has_value());
    auto svc = std::move(*svcRes);

    const auto remove = [&](std::string spec) {
        return svc->dispatchCommand(kanariv::ipc::QueueRemove{.idOrIndex = std::move(spec)});
    };
    const auto remaining = [](const auto& res) -> std::vector<int64_t> {
        std::vector<int64_t> out;
        auto* tracks = std::get_if<kanariv::ipc::QueueTracks>(&*res);
        if (tracks == nullptr)
            return out;
        for (const auto& t : tracks->tracks)
            out.push_back(t.id);
        return out;
    };

    // A bare number is ambiguous and must be rejected, never guessed (A2a):
    // interpreting a track id as a slot used to remove the wrong row.
    auto bare = remove("1");
    REQUIRE_FALSE(bare.has_value());
    REQUIRE(bare.error().code == kanariv::utils::StatusCode::InvalidArg);

    // Zero-based position removes the named slot; the rejected command above
    // left the queue untouched, so position 1 is still ids[1].
    auto first = remove("#pos:1");
    REQUIRE(first.has_value());
    REQUIRE(remaining(first) == std::vector<int64_t>{ids[0], ids[2]});

    // Explicit track id removes the named track, not the same-numbered slot.
    REQUIRE(db->queueEnqueue(1, ids[1], -1).has_value());
    auto byId = remove("#id:" + std::to_string(ids[1]));
    REQUIRE(byId.has_value());
    REQUIRE(remaining(byId) == std::vector<int64_t>{ids[0], ids[2]});

    auto byPos = remove("#pos:0");
    REQUIRE(byPos.has_value());
    REQUIRE(remaining(byPos) == std::vector<int64_t>{ids[2]});

    auto invalid = remove("not-a-number");
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == kanariv::utils::StatusCode::InvalidArg);

    auto missing = remove("#id:999999");
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == kanariv::utils::StatusCode::NotFound);
}

TEST_CASE("queue batch commands report per-item outcomes", "[service][queue][a2b]") {
    auto dir = tempDirPath("queue_batch_a2b");
    std::filesystem::create_directories(dir / "logs");
    auto dbRes = kanariv::db::Database::open((dir / "library.db").generic_string());
    REQUIRE(dbRes.has_value());
    std::shared_ptr<kanariv::db::Database> db(std::move(*dbRes));

    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "track" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<std::uint8_t>(i * 10 + b);
        auto id = db->insertTrack(t);
        REQUIRE(id.has_value());
        ids.push_back(*id);
    }

    auto svcRes = Service::create(inProcessConfig(dir / "library.db", dir / "logs"),
                                  kanariv::db::makeStorage(db));
    REQUIRE(svcRes.has_value());
    auto svc = std::move(*svcRes);

    const auto queueIds = [&]() {
        std::vector<int64_t> out;
        auto res = svc->dispatchCommand(kanariv::ipc::QueueList{"added"});
        if (!res)
            return out;
        if (auto* qt = std::get_if<kanariv::ipc::QueueTracks>(&*res))
            for (const auto& t : qt->tracks)
                out.push_back(t.id);
        return out;
    };

    // One request for the whole batch: outcomes stay in request order, an
    // already-queued id is reported, never duplicated, and an unknown id fails
    // without aborting the rest.
    auto add = svc->dispatchCommand(
        kanariv::ipc::QueueAddBatch{.queries = {std::to_string(ids[0]), std::to_string(ids[1]),
                                                "999999", std::to_string(ids[0])},
                                    .search = false});
    REQUIRE(add.has_value());
    auto* addReport = std::get_if<kanariv::ipc::QueueAddReport>(&*add);
    REQUIRE(addReport != nullptr);
    REQUIRE(addReport->items.size() == 4);
    REQUIRE(addReport->items[0].query == std::to_string(ids[0]));
    REQUIRE(addReport->items[0].status == "added");
    REQUIRE(addReport->items[0].track.id == ids[0]);
    REQUIRE(addReport->items[1].status == "added");
    REQUIRE(addReport->items[2].status == "failed");
    REQUIRE(addReport->items[2].track.id == 0);
    REQUIRE(addReport->items[2].detail == "not found");
    REQUIRE(addReport->items[3].status == "duplicate");
    REQUIRE(queueIds() == std::vector<int64_t>{ids[0], ids[1]});

    // Re-adding the same queries is all duplicates and creates no rows.
    auto again = svc->dispatchCommand(
        kanariv::ipc::QueueAddBatch{.queries = {std::to_string(ids[1])}, .search = false});
    REQUIRE(again.has_value());
    auto* againReport = std::get_if<kanariv::ipc::QueueAddReport>(&*again);
    REQUIRE(againReport != nullptr);
    REQUIRE(againReport->items.size() == 1);
    REQUIRE(againReport->items[0].status == "duplicate");
    REQUIRE(queueIds() == std::vector<int64_t>{ids[0], ids[1]});

    // Selectors apply in request order; a missing id and a malformed selector
    // are reported per item and leave the rest of the batch intact.
    auto rem = svc->dispatchCommand(
        kanariv::ipc::QueueRemoveBatch{.selectors = {"#pos:0", "#id:999999", "nonsense"}});
    REQUIRE(rem.has_value());
    auto* remReport = std::get_if<kanariv::ipc::QueueRemoveReport>(&*rem);
    REQUIRE(remReport != nullptr);
    REQUIRE(remReport->items.size() == 3);
    REQUIRE(remReport->items[0].status == "removed");
    REQUIRE(remReport->items[0].track.id == ids[0]);
    REQUIRE(remReport->items[1].status == "missing");
    REQUIRE(remReport->items[2].status == "failed");
    REQUIRE_FALSE(remReport->items[2].detail.empty());
    REQUIRE(queueIds() == std::vector<int64_t>{ids[1]});
}

TEST_CASE("explicit sockets give daemons independent pid and lock files", "[service][s9]") {
    auto dir = tempDirPath("service_explicit_socket");
    auto dbPath = dir / "library.db";

    auto cfg1 = inProcessConfig(dbPath, dir / "logs");
    cfg1.inProcess = false;
    cfg1.socketPath = uniqueTestSocket(dir, "one");
    cfg1.configPath = dir / "config1.json";
    auto cfg2 = cfg1;
    cfg2.socketPath = uniqueTestSocket(dir, "two");
    cfg2.configPath = dir / "config2.json";

    const auto pid1 = svcd::pidPathForSocket(dbPath, cfg1.socketPath);
    const auto pid2 = svcd::pidPathForSocket(dbPath, cfg2.socketPath);
    REQUIRE(pid1 != pid2);
    REQUIRE(svcd::lockPathForSocket(dbPath, cfg1.socketPath) !=
            svcd::lockPathForSocket(dbPath, cfg2.socketPath));

    // Same database, different explicit sockets: both must start (before S9
    // the second one lost the database-derived lock and failed AlreadyExists).
    auto svc1 = Service::create(cfg1);
    REQUIRE(svc1.has_value());
    auto svc2 = Service::create(cfg2);
    REQUIRE(svc2.has_value());
    REQUIRE(std::filesystem::exists(pid1));
    REQUIRE(std::filesystem::exists(pid2));
}
