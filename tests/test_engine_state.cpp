#include <sqlite3.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <common.hpp>
#include <cstdint>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <support/engine_test_helpers.hpp>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::engine;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

TEST_CASE("engine state save and load roundtrip", "[engine_state]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_state_rt").string();
    {
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());
        for (int i = 0; i < 2; ++i) {
            kanariv::types::Track t;
            t.path = "state" + std::to_string(i) + ".wav";
            t.duration = 1.0;
            for (int b = 0; b < 32; ++b)
                t.fingerprint[b] = (uint8_t)(0xA0 + i * 16 + b);
            auto r = db->insertTrack(t);
            REQUIRE(r.has_value());
            REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
        }
        kanariv::types::EngineConfig cfg;
        cfg.allowSimulatedPlayback = true;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(eng->attachStorage(kanariv::db::makeStorage(
                                       std::shared_ptr<kanariv::db::Database>(std::move(db))))
                    .has_value());
        REQUIRE(eng->setShuffle(true).has_value());
        REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::All).has_value());
        REQUIRE(eng->setVolume(0.42f).has_value());
        REQUIRE(eng->play(1).has_value());
        // state should be persisted on shutdown
        eng->shutdown();
    }
    // verify raw engine_state values
    sqlite3* h = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(h,
                               "SELECT shuffle_enabled,repeat_mode,cursor_pos,volume,shuffle_perm "
                               "FROM engine_state WHERE id=1",
                               -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(st, 0) == 1);
    REQUIRE(sqlite3_column_int(st, 1) == (int)kanariv::types::RepeatMode::All);
    int64_t cursor = sqlite3_column_int64(st, 2);
    REQUIRE(cursor >= 1);
    double vol = sqlite3_column_double(st, 3);
    REQUIRE(vol > 0.41);
    REQUIRE(vol < 0.43);
    int blobBytes = sqlite3_column_bytes(st, 4);
    REQUIRE(blobBytes == (int)(2 * sizeof(int64_t)));
    sqlite3_finalize(st);
    sqlite3_close(h);
    // reopen via Engine::open and verify loaded
    auto e2Res =
        openEngineWithDatabase(dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false});
    REQUIRE(e2Res.has_value());
    auto eng2 = std::move(e2Res.value());
    REQUIRE(eng2->volume() > 0.41f);
    REQUIRE(eng2->volume() < 0.43f);
    // shuffle should still be true: try setShuffle(false) and check it toggles
    // we can verify via checking that next() uses shuffle perm (should still have perm)
    // Check by reopening raw again
    sqlite3* h2 = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h2, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st2 = nullptr;
    REQUIRE(sqlite3_prepare_v2(
                h2, "SELECT shuffle_enabled,repeat_mode,volume FROM engine_state WHERE id=1", -1,
                &st2, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st2) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(st2, 0) == 1);
    REQUIRE(sqlite3_column_int(st2, 1) == (int)kanariv::types::RepeatMode::All);
    REQUIRE(sqlite3_column_double(st2, 2) > 0.41);
    sqlite3_finalize(st2);
    sqlite3_close(h2);
    eng2.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("engine state open without file creates default", "[engine_state]") {
    std::string dbPath = tempDbPath("eng_state_default").string();
    auto eRes =
        openEngineWithDatabase(dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false});
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->volume() == 1.0f);
    // default repeat Off
    REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::Off).has_value());
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("engine state volume clamped 0-1", "[engine_state]") {
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->setVolume(2.0f).has_value());
    REQUIRE(eng->volume() == 1.0f);
    REQUIRE(eng->setVolume(-1.0f).has_value());
    REQUIRE(eng->volume() == 0.0f);
}

TEST_CASE("engine create emits lifecycle log", "[engine_state]") {
    std::vector<std::pair<LogLevel, std::string>> out;
    Logger logger(
        [&](LogLevel lvl, std::string_view msg) { out.emplace_back(lvl, std::string(msg)); },
        LogLevel::Trace);
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    cfg.logger = &logger;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].first == LogLevel::Debug);
    REQUIRE(out[0].second.find("engine created") != std::string::npos);
}

TEST_CASE("engine state shuffle toggle clears perm", "[engine_state]") {
    std::string dbPath = tempDbPath("eng_state_toggle").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "tog" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xB0 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(
                   kanariv::db::makeStorage(std::shared_ptr<kanariv::db::Database>(std::move(db))))
                .has_value());
    REQUIRE(eng->setShuffle(true).has_value());
    // verify blob exists
    sqlite3* h = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(h, "SELECT shuffle_perm FROM engine_state WHERE id=1", -1, &st,
                               nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    REQUIRE(sqlite3_column_bytes(st, 0) == 2 * sizeof(int64_t));
    sqlite3_finalize(st);
    sqlite3_close(h);
    REQUIRE(eng->setShuffle(false).has_value());
    sqlite3* h2 = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h2, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_prepare_v2(h2,
                               "SELECT shuffle_perm,shuffle_enabled FROM engine_state WHERE id=1",
                               -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    REQUIRE(sqlite3_column_bytes(st, 0) == 0); // null blob
    REQUIRE(sqlite3_column_int(st, 1) == 0);
    sqlite3_finalize(st);
    sqlite3_close(h2);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("engine persists resume position across restart", "[engine_state]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_state_pos").string();
    int64_t trackId = 0;
    {
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());
        kanariv::types::Track t;
        t.path = "pos.wav";
        t.duration = 10.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xC0 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        trackId = *r;
        REQUIRE(db->queueEnqueue(1, trackId, -1).has_value());

        kanariv::types::EngineConfig cfg;
        cfg.allowSimulatedPlayback = true;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(eng->attachStorage(kanariv::db::makeStorage(
                                       std::shared_ptr<kanariv::db::Database>(std::move(db))))
                    .has_value());
        REQUIRE(eng->play(1).has_value());
        REQUIRE(eng->seek(4.0).has_value());
        REQUIRE(eng->position() > 3.9);
        eng->shutdown();
    }
    {
        sqlite3* h = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* st = nullptr;
        REQUIRE(sqlite3_prepare_v2(h, "SELECT position_ms,current_track_id FROM engine_state", -1,
                                   &st, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(st) == SQLITE_ROW);
        int64_t posMs = sqlite3_column_int64(st, 0);
        REQUIRE(posMs >= 3900);
        REQUIRE(posMs <= 4500);
        REQUIRE(sqlite3_column_int64(st, 1) == trackId);
        sqlite3_finalize(st);
        sqlite3_close(h);
    }
    {
        auto eRes = openEngineWithDatabase(
            dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false});
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(eng->state() == kanariv::types::PlaybackState::Stopped);
        REQUIRE(eng->currentTrackId() == trackId);
        REQUIRE(eng->position() > 3.9);
        REQUIRE(eng->position() <= 4.5);
    }
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("concurrent transport commands keep the engine usable", "[engine_state][concurrency]") {
    // E2 stress: every public transport/state call must stay safe when racing
    // the monitor and decode threads. Results are ignored per call (Busy is a
    // documented outcome under contention); the contract checked here is
    // liveness -- the engine still accepts a stop and a fresh play afterwards.
    std::string dbPath = tempDbPath("eng_state_stress").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto sharedDb = std::shared_ptr<Database>(std::move(dbRes.value()));
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "stress" + std::to_string(i) + ".wav";
        t.duration = 30.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0xE0 + i * 16 + b);
        auto r = sharedDb->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(sharedDb->queueEnqueue(1, *r, -1).has_value());
    }
    // Simulated playback keeps the stress test headless: nonexistent files
    // exercise the timer path, the locking is the same as with audio.
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(kanariv::db::makeStorage(sharedDb)).has_value());

    constexpr int kThreads = 4;
    constexpr int kIterations = 100;
    std::atomic<bool> go{false};
    std::atomic<int> calls{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIterations; ++i) {
                switch ((i + t * 3) % 11) {
                case 0:
                    (void)eng->play(1);
                    break;
                case 1:
                    (void)eng->next();
                    break;
                case 2:
                    (void)eng->prev();
                    break;
                case 3:
                    (void)eng->stop();
                    break;
                case 4:
                    (void)eng->pause();
                    break;
                case 5:
                    (void)eng->resume();
                    break;
                case 6:
                    (void)eng->setVolume((i % 10) / 10.0f);
                    break;
                case 7:
                    (void)eng->setShuffle((i % 2) == 0);
                    break;
                case 8:
                    (void)eng->setRepeat(i % 3 == 0   ? kanariv::types::RepeatMode::Off
                                         : i % 3 == 1 ? kanariv::types::RepeatMode::All
                                                      : kanariv::types::RepeatMode::One);
                    break;
                case 9:
                    (void)eng->position();
                    (void)eng->duration();
                    break;
                default:
                    (void)eng->currentTrackId();
                    (void)eng->state();
                    break;
                }
                calls.fetch_add(1, std::memory_order_relaxed);
                // Read-side calls run concurrently with every writer above.
                (void)eng->state();
                (void)eng->currentTrackId();
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads)
        th.join();
    REQUIRE(calls.load() == kThreads * kIterations);
    // Still usable after the storm: stop, start again, stop again.
    REQUIRE(eng->stop().has_value());
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Stopped);
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Playing);
    REQUIRE(eng->stop().has_value());
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("explicit stop clears the resume position", "[engine_state]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_state_stop_pos").string();
    {
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());
        kanariv::types::Track t;
        t.path = "stop_pos.wav";
        t.duration = 10.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xD0 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());

        kanariv::types::EngineConfig cfg;
        cfg.allowSimulatedPlayback = true;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(eng->attachStorage(kanariv::db::makeStorage(
                                       std::shared_ptr<kanariv::db::Database>(std::move(db))))
                    .has_value());
        REQUIRE(eng->play(1).has_value());
        REQUIRE(eng->seek(4.0).has_value());
        REQUIRE(eng->stop().has_value());
        REQUIRE(eng->position() == 0.0);
        eng->shutdown();
    }
    {
        auto eRes = openEngineWithDatabase(
            dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false});
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(eng->position() == 0.0);
    }
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}
