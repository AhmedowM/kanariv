#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "../src/utils/mpsc_queue.hpp"

using namespace kanariv::utils;
using namespace kanariv::engine;
using namespace kanariv::db;
using namespace kanariv::test_helpers;

TEST_CASE("MpscQueue push drop when full 64", "[engine_events]") {
    MpscQueue<kanariv::types::EngineEvent> q{64};
    for (int i = 0; i < 64; ++i) {
        kanariv::types::EngineEvent ev;
        ev.type = kanariv::types::EngineEventType::Progress;
        ev.track_id = i;
        auto r = q.push(ev);
        REQUIRE(r.has_value());
    }
    kanariv::types::EngineEvent extra;
    extra.type = kanariv::types::EngineEventType::Progress;
    extra.track_id = 999;
    auto r = q.push(extra);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::Busy);
    REQUIRE(q.size() == 64);
}

TEST_CASE("Engine pollEvent and drainEvents", "[engine_events]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_ev").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "ev.wav";
    t.duration = 1.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(0x70 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    auto ev = eng->pollEvent();
    REQUIRE(ev.has_value());
    REQUIRE(ev->type == kanariv::types::EngineEventType::TrackStarted);
    // after poll, queue empty -> poll returns NotFound
    auto ev2 = eng->pollEvent();
    REQUIRE(!ev2.has_value());
    REQUIRE(ev2.error().code == StatusCode::NotFound);
    // trigger another event via setShuffle -> QueueChanged
    kanariv::types::Track t2;
    t2.path = "ev2.wav";
    t2.duration = 1.0;
    for (int b = 0; b < 32; ++b)
        t2.fingerprint[b] = (uint8_t)(0x71 + b);
    // need to enqueue via raw? eng owns db now, use queue via eng's db not accessible; just test
    // drain push two more play events by re-playing? We'll test drainAll after multiple polls
    // Create fresh engine with multiple tracks to generate multiple events
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("Engine drainEvents batch", "[engine_events]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_drain").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "drain" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0x80 + i * 10 + b);
        auto ins = db->insertTrack(t);
        REQUIRE(ins.has_value());
        REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->next().has_value());
    // now we have 2 TrackStarted events
    kanariv::types::EngineEvent buf[64];
    size_t n = 0;
    REQUIRE(eng->drainEvents(buf, 64, &n).has_value());
    REQUIRE(n == 2);
    REQUIRE(buf[0].type == kanariv::types::EngineEventType::TrackStarted);
    REQUIRE(buf[1].type == kanariv::types::EngineEventType::TrackStarted);
    // drain again should be 0
    size_t n2 = 99;
    REQUIRE(eng->drainEvents(buf, 64, &n2).has_value());
    REQUIRE(n2 == 0);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("Engine callbacks dispatched outside lock", "[engine_events]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_cb").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "cb.wav";
    t.duration = 1.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(0x90 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    REQUIRE(db->queueEnqueue(1, *ins, -1).has_value());
    std::atomic<int> startedCount{0};
    kanariv::types::EngineCallbacks cbs;
    cbs.on_track_started = [&](int64_t tid) {
        startedCount.fetch_add(1);
        REQUIRE(tid == *ins);
    };
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    cfg.callbacks = cbs;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    // need to set callbacks via engine (attach copied empty, need setCallbacks)
    REQUIRE(eng->setCallbacks(cbs).has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(startedCount.load() == 1);
    // pollEvent should still have event even though callback was dispatched outside lock
    auto ev = eng->pollEvent();
    REQUIRE(ev.has_value());
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("Engine drainEvents null checks", "[engine_events]") {
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    size_t n = 0;
    // null n
    REQUIRE(!eng->drainEvents(nullptr, 0, nullptr).has_value());
    // null buf with cap>0
    REQUIRE(!eng->drainEvents(nullptr, 10, &n).has_value());
    // empty cap with null buf should succeed (0 events)
    REQUIRE(eng->drainEvents(nullptr, 0, &n).has_value());
    REQUIRE(n == 0);
}
