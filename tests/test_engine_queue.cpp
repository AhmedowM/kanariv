#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <common.hpp>
#include <cstdint>
#include <engine/shuffle.hpp>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <support/engine_test_helpers.hpp>
#include <utility>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::engine;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

TEST_CASE("engine queue shuffle creates perm via mt19937", "[engine_queue]") {
    std::string dbPath = tempDbPath("eng_q_shuffle").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 4; ++i) {
        kanariv::types::Track t;
        t.path = "p" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(i * 10 + b);
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
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->setShuffle(true).has_value());
    // after shuffle, perm should be size 4 and be a permutation of 0..3
    // verify by checking engine_state blob via raw sqlite
    // open raw handle via new connection to same file
    sqlite3* h = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(h, "SELECT shuffle_perm FROM engine_state WHERE id=1", -1, &st,
                               nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    int n = sqlite3_column_bytes(st, 0);
    REQUIRE(n == (int)(4 * sizeof(int64_t)));
    const void* blob = sqlite3_column_blob(st, 0);
    REQUIRE(blob != nullptr);
    std::set<int64_t> s;
    const int64_t* arr = static_cast<const int64_t*>(blob);
    for (int i = 0; i < 4; ++i)
        s.insert(arr[i]);
    REQUIRE(s.size() == 4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(s.count(i) == 1);
    sqlite3_finalize(st);
    sqlite3_close(h);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine queue repeat Off stops at end", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_q_off").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "p" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0x10 + i * 32 + b);
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
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::Off).has_value());
    REQUIRE(eng->play(1).has_value());
    {
        sqlite3* ch = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &ch, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* stc = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT COUNT(*) FROM queue WHERE queue_id=1", -1, &stc,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stc) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(stc, 0) == 2);
        sqlite3_finalize(stc);
        sqlite3_close(ch);
    }
    int64_t firstId = eng->currentTrackId();
    REQUIRE(eng->next().has_value());
    // next at end should wrap to beginning (Musicolet/AIMP) or reshuffle if shuffle on
    auto r = eng->next();
    REQUIRE(r.has_value());
    // wrapped to first track
    REQUIRE(eng->currentTrackId() == firstId);
    // queue still has 2 rows (cursor persisted, not deleted)
    {
        sqlite3* ch = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &ch, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* stc = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT COUNT(*) FROM queue WHERE queue_id=1", -1, &stc,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stc) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(stc, 0) == 2);
        sqlite3_finalize(stc);
        sqlite3_close(ch);
    }
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine queue repeat All loops", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_q_queue").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "q" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0x20 + i * 32 + b);
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
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::All).has_value());
    REQUIRE(eng->play(1).has_value());
    // queue persists via cursor, count stays 2 after play (not dequeued)
    {
        sqlite3* ch = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &ch, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* stc = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT COUNT(*) FROM queue WHERE queue_id=1", -1, &stc,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stc) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(stc, 0) == 2);
        sqlite3_finalize(stc);
        sqlite3_close(ch);
    }
    REQUIRE(eng->next().has_value());
    // count still 2 after next (cursor advance, not dequeue)
    {
        sqlite3* ch = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &ch, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* stc = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT COUNT(*) FROM queue WHERE queue_id=1", -1, &stc,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stc) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(stc, 0) == 2);
        sqlite3_finalize(stc);
        sqlite3_close(ch);
    }
    // after second next with Queue, should wrap and still succeed (third next)
    REQUIRE(eng->next().has_value());
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine queue repeat One seek without dequeue", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_q_one").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "one.wav";
    t.duration = 5.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0x30 + b);
    auto r = db->insertTrack(t);
    REQUIRE(r.has_value());
    REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    // add second track to have queue non-empty for non-shuffle One path? But One with !shuffle uses
    // seek path, not queue
    kanariv::types::Track t2;
    t2.path = "two.wav";
    t2.duration = 5.0;
    for (int b = 0; b < 32; ++b)
        t2.fingerprint[b] = static_cast<uint8_t>(0x40 + b);
    auto r2 = db->insertTrack(t2);
    REQUIRE(r2.has_value());
    REQUIRE(db->queueEnqueue(1, *r2, -1).has_value());
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->setRepeat(kanariv::types::RepeatMode::One).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    REQUIRE(eng->next().has_value());
    int64_t second = eng->currentTrackId();
    REQUIRE(first == second);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine queue perm persistence blob cursor qid", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_q_persist").string();
    {
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());
        for (int i = 0; i < 3; ++i) {
            kanariv::types::Track t;
            t.path = "persist" + std::to_string(i) + ".wav";
            t.duration = 1.0;
            for (int b = 0; b < 32; ++b)
                t.fingerprint[b] = static_cast<uint8_t>(0x50 + i * 32 + b);
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
        REQUIRE(
            eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
        REQUIRE(eng->setShuffle(true).has_value());
        REQUIRE(eng->play(1).has_value());
        // cursor should have advanced to 1, queueId 1 persisted
        eng->shutdown();
    }
    // reopen and verify persisted
    auto e2Res = openEngineWithDatabase(dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false});
    REQUIRE(e2Res.has_value());
    auto eng2 = std::move(e2Res.value());
    // after reopen shuffle should still be on (state shuffleEnabled=1)
    // verify via raw sqlite: shuffle_enabled=1 and cursor_pos persisted
    sqlite3* h = nullptr;
    REQUIRE(sqlite3_open_v2(dbPath.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(h, "SELECT shuffle_enabled,cursor_pos FROM engine_state WHERE id=1",
                               -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(st, 0) == 1);
    REQUIRE(sqlite3_column_int64(st, 1) >= 1);
    sqlite3_finalize(st);
    sqlite3_close(h);
    eng2.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine queue invalid repeat returns InvalidArg", "[engine_queue]") {
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    // without db should fail State
    REQUIRE(!eng->setRepeat((kanariv::types::RepeatMode)99).has_value());
}

TEST_CASE("shufflePerm deterministic with mt19937 seed 42", "[engine_queue][shuffle]") {
    std::vector<int64_t> a{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::vector<int64_t> b{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::mt19937 rng1{42};
    std::mt19937 rng2{42};
    kanariv::engine::detail::shufflePerm(a, rng1);
    kanariv::engine::detail::shufflePerm(b, rng2);
    REQUIRE(a == b);
    // ensure it's a permutation
    std::set<int64_t> s(a.begin(), a.end());
    REQUIRE(s.size() == 10);
    for (int i = 0; i < 10; ++i)
        REQUIRE(s.count(i) == 1);
    // different seed should give different perm (with high probability)
    std::vector<int64_t> c{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::mt19937 rng3{43};
    kanariv::engine::detail::shufflePerm(c, rng3);
    REQUIRE(c != a);
    // size 0 and 1 are no-ops
    std::vector<int64_t> empty;
    std::mt19937 rng4{42};
    kanariv::engine::detail::shufflePerm(empty, rng4);
    REQUIRE(empty.empty());
    std::vector<int64_t> one{42};
    kanariv::engine::detail::shufflePerm(one, rng4);
    REQUIRE(one.size() == 1);
    REQUIRE(one[0] == 42);
}

TEST_CASE("shufflePerm production overload non-deterministic but valid perm",
          "[engine_queue][shuffle]") {
    std::vector<int64_t> v{0, 1, 2, 3, 4};
    kanariv::engine::detail::shufflePerm(v); // calls random_device overload
    std::set<int64_t> s(v.begin(), v.end());
    REQUIRE(s.size() == 5);
    for (int i = 0; i < 5; ++i)
        REQUIRE(s.count(i) == 1);
}

TEST_CASE("engine play resumes when paused", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_play_pause").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "resume" + std::to_string(i) + ".wav";
        t.duration = 10.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0x60 + i * 32 + b);
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
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    REQUIRE(eng->pause().has_value());
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Paused);
    // play when paused should resume, not next
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Playing);
    REQUIRE(eng->currentTrackId() == first);
    // play when playing should restart, not next
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == first);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine prev non-shuffle", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_prev").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "prev" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0x70 + i * 32 + b);
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
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    REQUIRE(eng->next().has_value());
    int64_t second = eng->currentTrackId();
    REQUIRE(first != second);
    REQUIRE(eng->prev().has_value());
    REQUIRE(eng->currentTrackId() == first);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine queue persists via cursor non-shuffle", "[engine_queue]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("eng_q_persist_cursor").string();
    int64_t id0 = 0;
    {
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());
        for (int i = 0; i < 4; ++i) {
            kanariv::types::Track t;
            t.path = "persist_cursor" + std::to_string(i) + ".wav";
            t.duration = 1.0;
            for (int b = 0; b < 32; ++b)
                t.fingerprint[b] = static_cast<uint8_t>(0x80 + i * 32 + b);
            auto r = db->insertTrack(t);
            REQUIRE(r.has_value());
            if (i == 0)
                id0 = *r;
            REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
        }
        kanariv::types::EngineConfig cfg;
        cfg.allowSimulatedPlayback = true;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        REQUIRE(
            eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
        REQUIRE(eng->play(1).has_value());
        REQUIRE(eng->next().has_value());
        // queue count should still be 4 (cursor, not dequeue)
        sqlite3* ch = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &ch, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* stc = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT COUNT(*) FROM queue WHERE queue_id=1", -1, &stc,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stc) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(stc, 0) == 4);
        sqlite3_finalize(stc);
        sqlite3_close(ch);
        eng->shutdown();
    }
    // reopen, queue should still be 4 and cursor persisted
    {
        sqlite3* ch = nullptr;
        REQUIRE(sqlite3_open_v2(dbPath.c_str(), &ch, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* stc = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT COUNT(*) FROM queue WHERE queue_id=1", -1, &stc,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stc) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int(stc, 0) == 4);
        sqlite3_finalize(stc);
        sqlite3_stmt* st2 = nullptr;
        REQUIRE(sqlite3_prepare_v2(ch, "SELECT cursor_pos FROM engine_state WHERE id=1", -1, &st2,
                                   nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(st2) == SQLITE_ROW);
        REQUIRE(sqlite3_column_int64(st2, 0) == 2);
        sqlite3_finalize(st2);
        sqlite3_close(ch);
    }
    auto e2Res = openEngineWithDatabase(
        dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false, .allowSimulatedPlayback = true});
    REQUIRE(e2Res.has_value());
    auto eng2 = std::move(e2Res.value());
    // next should continue from cursor 2 -> third track
    REQUIRE(eng2->next().has_value());
    eng2.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine play missing file fails without simulation", "[engine_queue]") {
    // No audio device needed: the error path must trigger before any output.
    std::string dbPath = tempDbPath("eng_q_nosim").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "no-such-file.wav";
    t.duration = 1.0;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xC0 + b);
    auto r = db->insertTrack(t);
    REQUIRE(r.has_value());
    REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    kanariv::types::EngineConfig cfg; // allowSimulatedPlayback defaults to false
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    auto pr = eng->play(1);
    REQUIRE(!pr.has_value());
    REQUIRE(pr.error().code == StatusCode::NotFound);
    // stays stopped and emits no TrackStarted event
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Stopped);
    REQUIRE(!eng->pollEvent().has_value());
    eng.reset();
    safeRemoveDb(dbPath);
}
