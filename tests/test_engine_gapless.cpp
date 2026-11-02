#include <catch2/catch_test_macros.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <chrono>
#include <common.hpp>
#include <cstdint>
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

TEST_CASE("gapless CAS arms once via Engine", "[engine_gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless_cas").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "cas" + std::to_string(i) + ".wav";
        t.duration = 0.8;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0xC8 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    // gapless should arm once when remaining <=0.3 (pos>=0.5) and trigger next via engineTick
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(2000),
                  std::chrono::milliseconds(50));
    REQUIRE(eng->currentTrackId() != first);
    // ensure CAS prevented double trigger: wait 300ms and verify still on second track only
    int64_t second = eng->currentTrackId();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(eng->currentTrackId() == second);
    // verify gapless armed flag implicitly via exactly one advancement (queue now has second track)
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("gapless 300ms lookahead triggers next once", "[engine_gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "gap" + std::to_string(i) + ".wav";
        t.duration = 0.8;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0xC0 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    // duration 0.8, gap 0.3 -> when remaining <=0.3 (pos>=0.5) should arm and trigger next
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(2000),
                  std::chrono::milliseconds(50));
    int64_t cur = eng->currentTrackId();
    // should have advanced to second track via gapless
    REQUIRE(cur != first);
    // ensure only once: poll 300ms, should not advance again (queue now empty or last track)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int64_t cur2 = eng->currentTrackId();
    REQUIRE(cur2 == cur);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("gapless not triggered when remaining > gap", "[engine_gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless_notrig").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "gap_n" + std::to_string(i) + ".wav";
        t.duration = 5.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0xD0 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    // duration 5s, gap 0.3 -> remaining 4.8 > gap, should not trigger within 200ms
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(eng->currentTrackId() == first);
    // seek near end then gapless should trigger
    REQUIRE(eng->seek(4.8).has_value());
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(2000),
                  std::chrono::milliseconds(50));
    REQUIRE(eng->currentTrackId() != first);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}
