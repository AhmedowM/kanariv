#include <sqlite3.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <chrono>
#include <common.hpp>
#include <cstdint>
#include <engine/history.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
using namespace kanariv::db;
using namespace kanariv::engine;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

TEST_CASE("shouldMarkPlayed 60pct 90s thresholds", "[engine_history]") {
    // detail::shouldMarkPlayed is in engine namespace detail
    using kanariv::engine::detail::shouldMarkPlayed;
    // duration 100s, accumulated playback 59s -> not marked (59%)
    REQUIRE(shouldMarkPlayed(100.0, 59.0, false, 60, 90) == false);
    // 61% accumulated playback is true
    REQUIRE(shouldMarkPlayed(100.0, 61.0, false, 60, 90) == true);
    // already marked -> false even if threshold met
    REQUIRE(shouldMarkPlayed(100.0, 80.0, true, 60, 90) == false);
    // 91s accumulated playback with duration long and pct not met
    REQUIRE(shouldMarkPlayed(1000.0, 91.0, false, 60, 90) == true);
    // pct 60 with duration 0 -> only secs matters; 90s is true
    REQUIRE(shouldMarkPlayed(0.0, 90.0, false, 60, 90) == true);
    // short duration 10s, 6.5s accumulated playback -> true via pct
    REQUIRE(shouldMarkPlayed(10.0, 6.5, false, 60, 90) == true);
    // accumulated playback less than both thresholds -> false
    REQUIRE(shouldMarkPlayed(200.0, 10.0, false, 60, 90) == false);
}

TEST_CASE("shouldMarkPlayed explicit 60 90 thresholds", "[engine_history]") {
    using kanariv::engine::detail::shouldMarkPlayed;
    REQUIRE(shouldMarkPlayed(100.0, 61.0, false, 60, 90) == true);
    REQUIRE(shouldMarkPlayed(100.0, 30.0, false, 60, 90) == false);
    REQUIRE(shouldMarkPlayed(100.0, 80.0, true, 60, 90) == false);
}

TEST_CASE("CAS exactly-once markedPlayed atomic", "[engine_history]") {
    std::atomic<bool> marked{false};
    bool e = false;
    REQUIRE(marked.compare_exchange_strong(e, true));
    // second CAS with expected false should fail because already true
    e = false;
    REQUIRE(!marked.compare_exchange_strong(e, true));
    REQUIRE(marked.load());
    // reset and succeed again
    marked.store(false);
    e = false;
    REQUIRE(marked.compare_exchange_strong(e, true));
}

TEST_CASE("history insert increments play_count and last_played", "[engine_history]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_hist_inc").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "hist.wav";
    t.duration = 10.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(0x60 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    int64_t tid = *ins;
    auto before = db->getTrack(tid);
    REQUIRE(before.has_value());
    REQUIRE(before->play_count == 0);
    // ensure queue has track for engine play
    REQUIRE(db->queueEnqueue(1, tid, -1).has_value());
    // engine with small thresholds: 10% and 1 sec, poll 10ms
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.historyThresholdPct = 10;
    cfg.historyThresholdSecs = 1;
    cfg.gaplessMs = 300;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    const int64_t wallLo = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    REQUIRE(eng->play(1).has_value());
    // wait deterministically for history to be marked (threshold 1s or 10% of 10s=1s, so ~1s)
    // poll every 50ms with overall timeout 2000ms, breaking early when condition met
    auto checkMarked = [&]() -> bool {
        sqlite3* h = nullptr;
        if (sqlite3_open_v2(dbPath.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            return false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(h, "SELECT play_count,last_played FROM tracks WHERE id=?", -1, &st,
                               nullptr) != SQLITE_OK) {
            sqlite3_close(h);
            return false;
        }
        sqlite3_bind_int64(st, 1, tid);
        bool ok = false;
        if (sqlite3_step(st) == SQLITE_ROW) {
            int64_t pc = sqlite3_column_int64(st, 0);
            int64_t lp = sqlite3_column_int64(st, 1);
            if (pc >= 1 && lp > 0)
                ok = true;
        }
        sqlite3_finalize(st);
        sqlite3_close(h);
        return ok;
    };
    // Seeking beyond the threshold must not count as listened playback.
    REQUIRE(eng->seek(5.0).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE_FALSE(checkMarked());
    busyWaitUntil(checkMarked, std::chrono::milliseconds(2000), std::chrono::milliseconds(50));
    REQUIRE(checkMarked());
    // Stored timestamps must be wall-clock (regression: the monotonic clock
    // rendered every history date as 1970-01-01). Bounds come from the test's
    // own clock, so this holds at any machine uptime.
    const int64_t wallHi = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    auto wallPlausible = [&](int64_t v) { return v >= wallLo - 5000 && v <= wallHi + 5000; };
    sqlite3* h4 = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h4, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st4 = nullptr;
    REQUIRE(sqlite3_prepare_v2(h4, "SELECT last_played FROM tracks WHERE id=?", -1, &st4,
                               nullptr) == SQLITE_OK);
    sqlite3_bind_int64(st4, 1, tid);
    REQUIRE(sqlite3_step(st4) == SQLITE_ROW);
    REQUIRE(wallPlausible(sqlite3_column_int64(st4, 0)));
    sqlite3_finalize(st4);
    sqlite3_stmt* st5 = nullptr;
    REQUIRE(sqlite3_prepare_v2(h4, "SELECT started_at, completed_at FROM history WHERE track_id=?",
                               -1, &st5, nullptr) == SQLITE_OK);
    sqlite3_bind_int64(st5, 1, tid);
    REQUIRE(sqlite3_step(st5) == SQLITE_ROW);
    REQUIRE(wallPlausible(sqlite3_column_int64(st5, 0)));
    REQUIRE(wallPlausible(sqlite3_column_int64(st5, 1)));
    sqlite3_finalize(st5);
    sqlite3_close(h4);
    // also check exactly-once: after marking, play_count should stay 1 not 2 (monitor interval
    // 10ms) poll stability: wait 300ms then verify still 1
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sqlite3* h2 = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h2, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st2 = nullptr;
    REQUIRE(sqlite3_prepare_v2(h2, "SELECT play_count FROM tracks WHERE id=?", -1, &st2, nullptr) ==
            SQLITE_OK);
    sqlite3_bind_int64(st2, 1, tid);
    REQUIRE(sqlite3_step(st2) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int64(st2, 0) == 1);
    sqlite3_finalize(st2);
    sqlite3_close(h2);
    // also history row exists
    sqlite3* h3 = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h3, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st3 = nullptr;
    REQUIRE(sqlite3_prepare_v2(h3, "SELECT COUNT(*) FROM history WHERE track_id=?", -1, &st3,
                               nullptr) == SQLITE_OK);
    sqlite3_bind_int64(st3, 1, tid);
    REQUIRE(sqlite3_step(st3) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int64(st3, 0) == 1);
    sqlite3_finalize(st3);
    sqlite3_close(h3);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}
