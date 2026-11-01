#include <catch2/catch_test_macros.hpp>
#include <caudio/contracts/database.hpp>
#include <caudio/types/db.hpp>
#include <caudio/types/engine.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/protocol.hpp>
#include <caudio/ipc/result.hpp>
#include <caudio/ipc/shm_status.hpp>
#include <caudio/service/service.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/json.hpp>
#include <chrono>
#include <common.hpp>
#include <filesystem>
#include <stop_token>
#include <string>
#include <support/engine_test_helpers.hpp>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "../src/client/ipc_client.hpp"
using namespace caudio::ipc;
using namespace caudio::utils;
using namespace caudio::db;
using namespace caudio::engine;
using namespace caudio::service;
using namespace caudio::client;
using namespace caudio::test_helpers;

namespace {

template <class... Ts>
constexpr bool allCommandAlternatives(std::variant<Ts...>*) {
    return (CommandAlternative<Ts> && ...);
}

static_assert(allCommandAlternatives(static_cast<Command*>(nullptr)));
static_assert(CommandType<const Play&>);
static_assert(!CommandAlternative<const Play>);

} // namespace

TEST_CASE("daemon start/stop", "[ipc][cli]") {
    std::string suffix = "roundtrip";
    std::filesystem::path dir = tempDirPath("ipc_" + suffix);
    std::filesystem::path dbPath = dir / "library.db";
    std::error_code ec;

    ServiceConfig cfg;
    cfg.dbPath = dbPath;
    cfg.configPath = dir / "config.json";

    // Start daemon
    auto svcRes = openServiceWithDatabase(cfg.dbPath.generic_string(), cfg);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(svcRes.value());

    std::jthread svcThread([&](std::stop_token st) { (void)svc->run(st); });
    // wait for socket ready
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Connect client and send one command
    auto clientRes = IpcClient::connect(cfg.dbPath);
    REQUIRE(clientRes.has_value());
    auto client = std::move(clientRes.value());

    // Test status (no tracks yet)
    auto statusRes = client.send(StatusReq{});
    REQUIRE(statusRes.has_value());
    bool hasStatus = false;
    std::visit(
        [&](auto&& v) {
            using U = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<U, Status>) {
                hasStatus = true;
                REQUIRE(v.state == caudio::types::PlaybackState::Stopped);
                REQUIRE(v.q_size == 0);
            }
        },
        statusRes.value());
    REQUIRE(hasStatus);

    // Shutdown via new connection (Windows named pipes are single-use)
    auto clientRes2 = IpcClient::connect(cfg.dbPath);
    REQUIRE(clientRes2.has_value());
    auto client2 = std::move(clientRes2.value());
    auto shutdownRes = client2.send(Shutdown{});
    REQUIRE(shutdownRes.has_value());

    svcThread.request_stop();
    svcThread.join();
    svc.reset();

    // Verify cleanup
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
    std::filesystem::remove(cfg.configPath, ec);
}

TEST_CASE("bookmark protocol roundtrip", "[ipc]") {
    Command add{BookmarkAdd{.position_ms = 83000, .note = "note", .track_id = 42}};
    auto addJson = toJson(add);
    auto addBack = commandFromJson(addJson);
    REQUIRE(addBack.has_value());
    auto* addVal = std::get_if<BookmarkAdd>(&*addBack);
    REQUIRE(addVal != nullptr);
    CHECK(addVal->position_ms == 83000);
    CHECK(addVal->note == "note");
    CHECK(addVal->track_id == 42);

    Command list{BookmarkList{std::nullopt}};
    auto listJson = toJson(list);
    auto listBack = commandFromJson(listJson);
    REQUIRE(listBack.has_value());
    auto* listVal = std::get_if<BookmarkList>(&*listBack);
    REQUIRE(listVal != nullptr);
    CHECK(!listVal->track_id.has_value());

    Result res{Bookmarks{{
        caudio::types::Bookmark{
            .id = 1, .track_id = 42, .position_ms = 83000, .note = "note", .created = 1700},
    }}};
    auto resJson = toJson(res);
    auto resBack = resultFromJson(resJson);
    REQUIRE(resBack.has_value());
    auto* marks = std::get_if<Bookmarks>(&*resBack);
    REQUIRE(marks != nullptr);
    REQUIRE(marks->entries.size() == 1);
    CHECK(marks->entries[0].id == 1);
    CHECK(marks->entries[0].track_id == 42);
    CHECK(marks->entries[0].position_ms == 83000);
    CHECK(marks->entries[0].note == "note");
    CHECK(marks->entries[0].created == 1700);
}

TEST_CASE("history protocol roundtrip", "[ipc]") {
    Result res{History{{
        caudio::types::HistoryEntry{.id = 1,
                                    .track_id = 42,
                                    .started_at = 1700000000000,
                                    .completed_at = 1700000000240,
                                    .position_ms = 240000,
                                    .completion_pct = 100.0,
                                    .queue_id = 1,
                                    .title = "Song",
                                    .artist = "Artist",
                                    .path = "/music/song.mp3",
                                    .duration = 240.0},
    }}};
    auto resJson = toJson(res);
    auto resBack = resultFromJson(resJson);
    REQUIRE(resBack.has_value());
    auto* history = std::get_if<History>(&*resBack);
    REQUIRE(history != nullptr);
    REQUIRE(history->entries.size() == 1);
    const auto& entry = history->entries[0];
    CHECK(entry.id == 1);
    CHECK(entry.track_id == 42);
    CHECK(entry.started_at == 1700000000000);
    CHECK(entry.completed_at == 1700000000240);
    CHECK(entry.position_ms == 240000);
    CHECK(entry.completion_pct == 100.0);
    CHECK(entry.queue_id == 1);
    CHECK(entry.title == "Song");
    CHECK(entry.artist == "Artist");
    CHECK(entry.path == "/music/song.mp3");
    CHECK(entry.duration == 240.0);
}

TEST_CASE("devices protocol roundtrip", "[ipc]") {
    Result res{Devices{{
        caudio::types::DeviceInfo{.id = "hw:0,0", .name = "Built-in Audio", .isDefault = true},
        caudio::types::DeviceInfo{.id = "hw:1,0", .name = "USB Headset", .isDefault = false},
    }}};
    auto resJson = toJson(res);
    REQUIRE(resJson["type"].snapshot().get<std::string>() == "Devices");
    auto resBack = resultFromJson(resJson);
    REQUIRE(resBack.has_value());
    auto* devices = std::get_if<Devices>(&*resBack);
    REQUIRE(devices != nullptr);
    REQUIRE(devices->devices.size() == 2);
    CHECK(devices->devices[0].id == "hw:0,0");
    CHECK(devices->devices[0].name == "Built-in Audio");
    CHECK(devices->devices[0].isDefault);
    CHECK(devices->devices[1].id == "hw:1,0");
    CHECK_FALSE(devices->devices[1].isDefault);
}

TEST_CASE("library stats detailed protocol roundtrip", "[ipc]") {
    caudio::types::Track track{};
    track.id = 7;
    track.path = "/music/song.mp3";
    track.title = "Song";
    track.duration = 240.0;
    Result res{
        caudio::types::LibraryStats{.tracks = 100,
                                    .queues = 5,
                                    .playlists = 20,
                                    .total_duration_ms = 36000000,
                                    .total_play_time_ms = 7200000,
                                    .top_tracks = std::vector<caudio::types::Track>{track}}};
    auto resJson = toJson(res);
    REQUIRE(resJson["type"].snapshot().get<std::string>() == "LibraryStats");
    auto resBack = resultFromJson(resJson);
    REQUIRE(resBack.has_value());
    auto* stats = std::get_if<caudio::types::LibraryStats>(&*resBack);
    REQUIRE(stats != nullptr);
    CHECK(stats->tracks == 100);
    CHECK(stats->queues == 5);
    CHECK(stats->playlists == 20);
    CHECK(stats->total_duration_ms == 36000000);
    CHECK(stats->total_play_time_ms == 7200000);
    REQUIRE(stats->top_tracks.has_value());
    REQUIRE(stats->top_tracks->size() == 1);
    CHECK((*stats->top_tracks)[0].id == 7);
    CHECK((*stats->top_tracks)[0].title == "Song");
}

TEST_CASE("library stats summary protocol roundtrip", "[ipc]") {
    Result res{caudio::types::LibraryStats{.tracks = 100,
                                           .queues = 5,
                                           .playlists = 20,
                                           .total_duration_ms = 36000000,
                                           .total_play_time_ms = 7200000,
                                           .top_tracks = std::nullopt}};
    auto resJson = toJson(res);
    REQUIRE(resJson["type"].snapshot().get<std::string>() == "LibraryStats");
    CHECK_FALSE(resJson.contains("top_tracks"));
    auto resBack = resultFromJson(resJson);
    REQUIRE(resBack.has_value());
    auto* stats = std::get_if<caudio::types::LibraryStats>(&*resBack);
    REQUIRE(stats != nullptr);
    CHECK(stats->tracks == 100);
    CHECK(stats->total_duration_ms == 36000000);
    CHECK_FALSE(stats->top_tracks.has_value());
}

TEST_CASE("library stats command roundtrip", "[ipc]") {
    auto summary = toJson(Command{LibraryStats{}});
    CHECK(summary["type"].snapshot().get<std::string>() == "LibraryStats");
    CHECK_FALSE(summary.contains("top_tracks"));

    auto detailed = toJson(Command{LibraryStats{5}});
    REQUIRE(detailed.contains("top_tracks"));
    CHECK(detailed["top_tracks"].snapshot().get<int>() == 5);
    auto detailedBack = commandFromJson(detailed);
    REQUIRE(detailedBack.has_value());
    auto* detailedCmd = std::get_if<LibraryStats>(&*detailedBack);
    REQUIRE(detailedCmd != nullptr);
    REQUIRE(detailedCmd->top_tracks.has_value());
    CHECK(*detailedCmd->top_tracks == 5);

    Json legacy;
    legacy["type"] = "LibraryStatsDetailed";
    legacy["top_tracks"] = 3;
    auto legacyBack = commandFromJson(legacy);
    REQUIRE(legacyBack.has_value());
    auto* legacyCmd = std::get_if<LibraryStats>(&*legacyBack);
    REQUIRE(legacyCmd != nullptr);
    REQUIRE(legacyCmd->top_tracks.has_value());
    CHECK(*legacyCmd->top_tracks == 3);
}

TEST_CASE("shm status reader/writer roundtrip", "[ipc][shm]") {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count() % 100000000;
    const std::string name = "test-" + std::to_string(suffix);
    auto writerRes = caudio::ipc::ShmStatusWriter::create(name);
    if (!writerRes)
        WARN(writerRes.error().message);
    REQUIRE(writerRes.has_value());
    auto& writer = *writerRes;

    caudio::ipc::ShmStatusUpdate update{};
    update.position = 12.5;
    update.volume = 0.5f;
    update.muted = true;
    update.state = 2;
    update.track_id = 42;
    update.title = "Song";
    update.artist = "Artist";
    writer.publish(update);
    writer.setDuration(180.0);
    writer.setQueueSize(3);

    auto readerRes = caudio::ipc::ShmStatusReader::openReadOnly(name);
    if (!readerRes)
        WARN(readerRes.error().message);
    REQUIRE(readerRes.has_value());
    const auto snapshot = readerRes->snapshot();
    CHECK(snapshot.position == 12.5);
    CHECK(snapshot.duration == 180.0);
    CHECK(snapshot.volume == 0.5f);
    CHECK(snapshot.muted);
    CHECK(snapshot.state == 2);
    CHECK(snapshot.track_id == 42);
    CHECK(snapshot.queueSize == 3);
    CHECK(std::string{snapshot.title} == "Song");
    CHECK(std::string{snapshot.artist} == "Artist");
    CHECK(writer.valid());
    CHECK(readerRes->valid());
}

TEST_CASE("command roundtrip - volume", "[ipc][cli]") {
    std::string suffix = "volume";
    std::filesystem::path dir = tempDirPath("ipc_" + suffix);
    std::filesystem::path dbPath = dir / "library.db";
    std::error_code ec;

    ServiceConfig cfg;
    cfg.dbPath = dbPath;
    cfg.configPath = dir / "config.json";

    // Start daemon
    auto svcRes = openServiceWithDatabase(cfg.dbPath.generic_string(), cfg);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(svcRes.value());

    std::jthread svcThread([&](std::stop_token st) { (void)svc->run(st); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Connect and send volume command
    auto clientRes = IpcClient::connect(cfg.dbPath);
    REQUIRE(clientRes.has_value());
    auto client = std::move(clientRes.value());

    // Test volume
    auto volRes = client.send(VolumeSet{.level = 75.0f});
    REQUIRE(volRes.has_value());
    bool hasVol = false;
    std::visit(
        [&](auto&& v) {
            using U = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<U, VolumeInfo>) {
                hasVol = true;
                // Volume is stored as 0.0-1.0, input is 0-100
                REQUIRE(v.vol == 0.75f);
            }
        },
        volRes.value());
    REQUIRE(hasVol);

    // Shutdown
    auto clientRes2 = IpcClient::connect(cfg.dbPath);
    REQUIRE(clientRes2.has_value());
    auto client2 = std::move(clientRes2.value());
    (void)client2.send(Shutdown{});

    svcThread.request_stop();
    svcThread.join();
    svc.reset();

    // Cleanup
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
    std::filesystem::remove(cfg.configPath, ec);
}

TEST_CASE("command roundtrip - status", "[ipc][cli]") {
    std::string suffix = "status";
    std::filesystem::path dir = tempDirPath("ipc_" + suffix);
    std::filesystem::path dbPath = dir / "library.db";
    std::error_code ec;

    ServiceConfig cfg;
    cfg.dbPath = dbPath;
    cfg.configPath = dir / "config.json";

    // Start daemon
    auto svcRes = openServiceWithDatabase(cfg.dbPath.generic_string(), cfg);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(svcRes.value());

    std::jthread svcThread([&](std::stop_token st) { (void)svc->run(st); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Connect and send status command
    auto clientRes = IpcClient::connect(cfg.dbPath);
    REQUIRE(clientRes.has_value());
    auto client = std::move(clientRes.value());

    // Test status
    auto statusRes = client.send(StatusReq{});
    REQUIRE(statusRes.has_value());
    bool hasStatus = false;
    std::visit(
        [&](auto&& v) {
            using U = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<U, Status>) {
                hasStatus = true;
                REQUIRE(v.state == caudio::types::PlaybackState::Stopped);
                REQUIRE(v.q_size == 0);
            }
        },
        statusRes.value());
    REQUIRE(hasStatus);

    // Shutdown
    auto clientRes2 = IpcClient::connect(cfg.dbPath);
    REQUIRE(clientRes2.has_value());
    auto client2 = std::move(clientRes2.value());
    (void)client2.send(Shutdown{});

    svcThread.request_stop();
    svcThread.join();
    svc.reset();

    // Cleanup
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
    std::filesystem::remove(cfg.configPath, ec);
}

TEST_CASE("track rate and smart playlist protocol roundtrip", "[ipc]") {
    // TrackRate command roundtrip.
    Command rate{TrackRate{.track_id = 7, .rating = 5}};
    auto rateJson = toJson(rate);
    auto rateBack = commandFromJson(rateJson);
    REQUIRE(rateBack.has_value());
    auto* rateVal = std::get_if<TrackRate>(&*rateBack);
    REQUIRE(rateVal != nullptr);
    CHECK(rateVal->track_id == 7);
    CHECK(rateVal->rating == 5);

    // PlaylistCreate with smart_query roundtrip.
    Command pl{PlaylistCreate{.name = "top rated", .smart_query = "rating=5"}};
    auto plJson = toJson(pl);
    auto plBack = commandFromJson(plJson);
    REQUIRE(plBack.has_value());
    auto* plVal = std::get_if<PlaylistCreate>(&*plBack);
    REQUIRE(plVal != nullptr);
    CHECK(plVal->name == "top rated");
    CHECK(plVal->smart_query == "rating=5");
}

TEST_CASE("daemon track rate and smart playlist roundtrip", "[ipc][cli]") {
    std::string suffix = "trackrate";
    std::filesystem::path dir = tempDirPath("ipc_" + suffix);
    std::filesystem::path dbPath = dir / "library.db";
    std::error_code ec;

    ServiceConfig cfg;
    cfg.dbPath = dbPath;
    cfg.configPath = dir / "config.json";

    // Open the database once and share it with the service so the test can
    // insert tracks directly while the daemon runs (WAL allows this).
    auto dbRes = caudio::db::Database::open(dbPath.generic_string());
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    caudio::types::Track t{};
    t.path = "/tmp/track_rate.mp3";
    t.title = "Rate Me";
    t.artist = "Test Artist";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(b * 3);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    int64_t trackId = *ins;

    std::shared_ptr<caudio::contracts::IDatabase> sharedDb(std::move(db));
    auto svcRes = caudio::service::Service::create(cfg, sharedDb);
    REQUIRE(svcRes.has_value());
    auto svc = std::move(svcRes.value());

    std::jthread svcThread([&](std::stop_token st) { (void)svc->run(st); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Rate the track via IPC (new connection per request on named pipes).
    {
        auto rateClientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(rateClientRes.has_value());
        auto rateClient = std::move(rateClientRes.value());
        auto rateRes = rateClient.send(TrackRate{.track_id = trackId, .rating = 5});
        REQUIRE(rateRes.has_value());
        CHECK(std::holds_alternative<Empty>(*rateRes));
    }

    // Create a smart playlist via IPC.
    int64_t plId = 0;
    {
        auto plClientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(plClientRes.has_value());
        auto plClient = std::move(plClientRes.value());
        auto plRes = plClient.send(PlaylistCreate{.name = "top rated", .smart_query = "rating=5"});
        REQUIRE(plRes.has_value());
        auto* pc = std::get_if<PlaylistCreated>(&*plRes);
        REQUIRE(pc != nullptr);
        CHECK(pc->name == "top rated");
        plId = pc->id;
    }

    // List tracks on the smart playlist (new connection per request).
    {
        auto plTracksClientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(plTracksClientRes.has_value());
        auto plTracksClient = std::move(plTracksClientRes.value());
        auto plTracksRes = plTracksClient.send(PlaylistTracks{.pid = plId});
        REQUIRE(plTracksRes.has_value());
        auto* pd = std::get_if<PlaylistData>(&*plTracksRes);
        REQUIRE(pd != nullptr);
        REQUIRE(pd->tracks.size() == 1);
        CHECK(pd->tracks[0].id == trackId);
        CHECK(pd->tracks[0].rating == 5);
    }

    // Shutdown (new connection).
    {
        auto shutdownClientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(shutdownClientRes.has_value());
        auto shutdownClient = std::move(shutdownClientRes.value());
        (void)shutdownClient.send(Shutdown{});
    }

    svcThread.request_stop();
    svcThread.join();
    svc.reset();

    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
    std::filesystem::remove(cfg.configPath, ec);
}

TEST_CASE("daemon library list rating filter and maintenance", "[ipc][cli]") {
    std::string suffix = "libmaint";
    std::filesystem::path dir = tempDirPath("ipc_" + suffix);
    std::filesystem::path dbPath = dir / "library.db";
    std::error_code ec;

    auto dbRes = caudio::db::Database::open(dbPath.generic_string());
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    // Insert tracks: some rated, some not.
    caudio::types::Track t1{};
    t1.path = "/tmp/track_a.mp3";
    t1.title = "Track A";
    t1.rating = 5;
    for (int b = 0; b < 32; ++b)
        t1.fingerprint[b] = (uint8_t)(b + 1);
    auto ins1 = db->insertTrack(t1);
    REQUIRE(ins1.has_value());

    caudio::types::Track t2{};
    t2.path = "/tmp/track_b.mp3";
    t2.title = "Track B";
    t2.rating = 0;
    for (int b = 0; b < 32; ++b)
        t2.fingerprint[b] = (uint8_t)(b + 2);
    auto ins2 = db->insertTrack(t2);
    REQUIRE(ins2.has_value());

    ServiceConfig cfg;
    cfg.dbPath = dbPath;
    cfg.configPath = dir / "config.json";

    auto svcRes = caudio::service::Service::create(cfg, std::move(db));
    REQUIRE(svcRes.has_value());
    auto svc = std::move(svcRes.value());

    std::jthread svcThread([&](std::stop_token st) { (void)svc->run(st); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // LibraryList with rating=5 should return only track A.
    {
        auto clientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(clientRes.has_value());
        auto client = std::move(clientRes.value());
        auto listRes = client.send(LibraryList{.limit = 50, .rating = 5});
        REQUIRE(listRes.has_value());
        auto* tracks = std::get_if<Tracks>(&*listRes);
        REQUIRE(tracks != nullptr);
        REQUIRE(tracks->tracks.size() == 1);
        CHECK(tracks->tracks[0].rating == 5);
    }

    // LibraryMaintenance report.
    {
        auto clientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(clientRes.has_value());
        auto client = std::move(clientRes.value());
        auto repRes = client.send(LibraryMaintenance{.action = "report",
                                                     .dry_run = true,
                                                     .library_id = 1});
        REQUIRE(repRes.has_value());
        auto* rep = std::get_if<LibraryMaintenanceReport>(&*repRes);
        REQUIRE(rep != nullptr);
        CHECK(rep->tracks_checked == 2);
        // Both tracks reference non-existent paths, so both should be missing.
        CHECK(rep->missing_tracks.size() == 2);
    }

    // LibraryMaintenance prune (dry_run=false) should delete missing tracks.
    {
        auto clientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(clientRes.has_value());
        auto client = std::move(clientRes.value());
        auto repRes = client.send(LibraryMaintenance{.action = "prune",
                                                     .dry_run = false,
                                                     .library_id = 1});
        REQUIRE(repRes.has_value());
        CHECK(std::holds_alternative<Empty>(*repRes));
    }

    // Verify tracks are gone.
    {
        auto clientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(clientRes.has_value());
        auto client = std::move(clientRes.value());
        auto listRes = client.send(LibraryList{.limit = 50});
        REQUIRE(listRes.has_value());
        auto* tracks = std::get_if<Tracks>(&*listRes);
        REQUIRE(tracks != nullptr);
        CHECK(tracks->tracks.empty());
    }

    // Shutdown (new connection).
    {
        auto shutdownClientRes = IpcClient::connect(cfg.dbPath);
        REQUIRE(shutdownClientRes.has_value());
        auto shutdownClient = std::move(shutdownClientRes.value());
        (void)shutdownClient.send(Shutdown{});
    }

    svcThread.request_stop();
    svcThread.join();
    svc.reset();

    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
    std::filesystem::remove(cfg.configPath, ec);
}
