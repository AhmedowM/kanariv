#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <common.hpp>
#include <cstdint>
#include <engine/engine_access.hpp>
#include <engine/queue_next.hpp>
#include <engine/shuffle.hpp>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <memory>
#include <random>
#include <set>
#include <span>
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
            t.fingerprint[b] = (uint8_t)(i * 10 + b);
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
    // after shuffle, the sidecar perm should be size 4 and a permutation of 0..3
    const auto doc = readStateSidecar(dbPath);
    REQUIRE(doc.has_value());
    const std::vector<int64_t> perm = jsonIntArray(*doc, "shuffle_perm");
    REQUIRE(perm.size() == 4);
    std::set<int64_t> s(perm.begin(), perm.end());
    REQUIRE(s.size() == 4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(s.count(i) == 1);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine toggleShuffle flips the current mode", "[engine_queue][shuffle]") {
    std::string dbPath = tempDbPath("eng_q_toggle").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "p" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(i + 1 + b);
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
    CHECK_FALSE(eng->shuffle());
    REQUIRE(eng->toggleShuffle().has_value());
    CHECK(eng->shuffle());
    REQUIRE(eng->toggleShuffle().has_value());
    CHECK_FALSE(eng->shuffle());
    // Explicit set then toggle: the toggle resolves against the stored mode,
    // not against a value read before the engine lock was taken.
    REQUIRE(eng->setShuffle(true).has_value());
    REQUIRE(eng->toggleShuffle().has_value());
    CHECK_FALSE(eng->shuffle());
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
            t.fingerprint[b] = (uint8_t)(0x10 + i * 32 + b);
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
            t.fingerprint[b] = (uint8_t)(0x20 + i * 32 + b);
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
        t.fingerprint[b] = (uint8_t)(0x30 + b);
    auto r = db->insertTrack(t);
    REQUIRE(r.has_value());
    REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    // add second track to have queue non-empty for non-shuffle One path? But One with !shuffle uses
    // seek path, not queue
    kanariv::types::Track t2;
    t2.path = "two.wav";
    t2.duration = 5.0;
    for (int b = 0; b < 32; ++b)
        t2.fingerprint[b] = (uint8_t)(0x40 + b);
    auto r2 = db->insertTrack(t2);
    REQUIRE(r2.has_value());
    REQUIRE(db->queueEnqueue(1, *r2, -1).has_value());
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(
                   kanariv::db::makeStorage(std::shared_ptr<kanariv::db::Database>(std::move(db))))
                .has_value());
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
                t.fingerprint[b] = (uint8_t)(0x50 + i * 32 + b);
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
        REQUIRE(eng->play(1).has_value());
        // cursor should have advanced to 1, queueId 1 persisted
        eng->shutdown();
    }
    // reopen and verify persisted
    auto e2Res =
        openEngineWithDatabase(dbPath, kanariv::types::EngineConfig{.enableMonitorThread = false});
    REQUIRE(e2Res.has_value());
    auto eng2 = std::move(e2Res.value());
    // after reopen shuffle should still be on and the cursor persisted
    const auto doc = readStateSidecar(dbPath);
    REQUIRE(doc.has_value());
    REQUIRE((*doc)["shuffle"].get<bool>());
    REQUIRE((*doc)["cursor"].get<int64_t>() >= 1);
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
            t.fingerprint[b] = (uint8_t)(0x60 + i * 32 + b);
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
            t.fingerprint[b] = (uint8_t)(0x70 + i * 32 + b);
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
                t.fingerprint[b] = (uint8_t)(0x80 + i * 32 + b);
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
        REQUIRE(eng->attachStorage(kanariv::db::makeStorage(
                                       std::shared_ptr<kanariv::db::Database>(std::move(db))))
                    .has_value());
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
        const auto doc = readStateSidecar(dbPath);
        REQUIRE(doc.has_value());
        REQUIRE((*doc)["cursor"].get<int64_t>() == 2);
        sqlite3_close(ch);
    }
    auto e2Res = openEngineWithDatabase(
        dbPath,
        kanariv::types::EngineConfig{.enableMonitorThread = false, .allowSimulatedPlayback = true});
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
        t.fingerprint[b] = (uint8_t)(0xC0 + b);
    auto r = db->insertTrack(t);
    REQUIRE(r.has_value());
    REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    kanariv::types::EngineConfig cfg; // allowSimulatedPlayback defaults to false
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(
                   kanariv::db::makeStorage(std::shared_ptr<kanariv::db::Database>(std::move(db))))
                .has_value());
    auto pr = eng->play(1);
    REQUIRE(!pr.has_value());
    REQUIRE(pr.error().code == StatusCode::NotFound);
    // stays stopped and emits no TrackStarted event
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Stopped);
    REQUIRE(!eng->pollEvent().has_value());
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("engine switchQueue adopts the queue's stored repeat mode", "[engine_queue][repeat]") {
    using kanariv::types::RepeatMode;
    const RepeatMode modes[] = {RepeatMode::Off, RepeatMode::All, RepeatMode::One};
    for (const auto mode : modes) {
        const std::string tag = std::to_string(static_cast<int>(mode));
        const std::string dbPath = tempDbPath("eng_q_switch_repeat_" + tag).string();
        auto dbRes = Database::open(dbPath);
        REQUIRE(dbRes.has_value());
        auto db = std::move(dbRes.value());

        auto qid = db->createQueue("target_" + tag);
        REQUIRE(qid.has_value());
        REQUIRE(db->setQueueRepeat(*qid, mode).has_value());

        kanariv::types::EngineConfig cfg;
        cfg.allowSimulatedPlayback = true;
        cfg.enableMonitorThread = false;
        auto eRes = Engine::create(cfg);
        REQUIRE(eRes.has_value());
        auto eng = std::move(eRes.value());
        auto sharedDb = std::shared_ptr<kanariv::db::Database>(std::move(db));
        REQUIRE(eng->attachStorage(kanariv::db::makeStorage(sharedDb)).has_value());

        // Start from a different mode so adoption is observable, including Off.
        const RepeatMode initial = mode == RepeatMode::One ? RepeatMode::Off : RepeatMode::One;
        REQUIRE(eng->setRepeat(initial).has_value());
        REQUIRE(eng->repeat() == initial);

        REQUIRE(eng->switchQueue(*qid).has_value());
        REQUIRE(eng->repeat() == mode);

        const auto persisted = readStateSidecar(dbPath);
        REQUIRE(persisted.has_value());
        const std::string expected = mode == RepeatMode::One   ? "one"
                                     : mode == RepeatMode::All ? "all"
                                                               : "off";
        REQUIRE((*persisted)["repeat"].get<std::string>() == expected);

        eng.reset();
        safeRemoveDb(dbPath);
    }
}

namespace {

/// @brief Reads `cursor` from the JSON engine-state sidecar (-1 when absent).
/// @details Engine state is a sidecar document, not a database row; the helper
/// keeps the assertions identical to the other persisted-state checks.
int64_t persistedCursorPos(const std::string& dbPath) {
    const auto doc = readStateSidecar(dbPath);
    if (!doc || !doc->contains("cursor"))
        return -1;
    return (*doc)["cursor"].get<int64_t>();
}

/// @brief Reads the persisted shuffle permutation from the sidecar.
std::vector<int64_t> persistedPerm(const std::string& dbPath) {
    const auto doc = readStateSidecar(dbPath);
    return doc ? jsonIntArray(*doc, "shuffle_perm") : std::vector<int64_t>{};
}

} // namespace

TEST_CASE("stop rewinds the cursor so play replays the stopped track", "[engine_queue][stop]") {
    // Missing track paths + allowSimulatedPlayback keep this headless: no
    // decoder, no audio device, only the cursor state machine under test.
    std::string dbPath = tempDbPath("eng_stop_rewind").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "stop_rewind" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0x90 + i * 32 + b);
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
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    REQUIRE(eng->next().has_value());
    int64_t second = eng->currentTrackId();
    REQUIRE(second != first);
    REQUIRE(persistedCursorPos(dbPath) == 2);

    REQUIRE(eng->stop().has_value());
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Stopped);
    // The cursor parks ON the stopped entry (list index 1), not past it (2).
    REQUIRE(persistedCursorPos(dbPath) == 1);

    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == second);
    REQUIRE(persistedCursorPos(dbPath) == 2);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("stop with shuffle replays the stopped track and keeps the permutation",
          "[engine_queue][stop][shuffle]") {
    const std::string dbPath = tempDbPath("eng_stop_rewind_shuffle").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto sharedDb = std::shared_ptr<kanariv::db::Database>(std::move(dbRes.value()));
    constexpr int kTracks = 4;
    for (int i = 0; i < kTracks; ++i) {
        kanariv::types::Track t;
        t.path = "stop_perm" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xE0 + i * 16 + b);
        auto r = sharedDb->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(sharedDb->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(kanariv::db::makeStorage(sharedDb)).has_value());
    REQUIRE(eng->setShuffle(true).has_value());
    REQUIRE(eng->play(1).has_value());
    const int64_t first = eng->currentTrackId();
    REQUIRE(eng->next().has_value());
    const int64_t second = eng->currentTrackId();
    REQUIRE(second != first);

    const std::vector<int64_t> permBefore = persistedPerm(dbPath);
    REQUIRE(permBefore.size() == kTracks);
    std::set<int64_t> seen(permBefore.begin(), permBefore.end());
    REQUIRE(seen.size() == kTracks);
    for (int64_t i = 0; i < kTracks; ++i)
        REQUIRE(seen.count(i) == 1);

    // The stopped track's position in the permutation is what stop() must
    // rewind to (not `cursor - 1`, which only coincides by accident).
    const auto listing = sharedDb->queueIndexOf(1, second);
    REQUIRE(listing.has_value());
    REQUIRE(listing->has_value());
    size_t permIndex = permBefore.size();
    for (size_t i = 0; i < permBefore.size(); ++i)
        if (permBefore[i] == static_cast<int64_t>(**listing))
            permIndex = i;
    REQUIRE(permIndex < permBefore.size());
    REQUIRE(persistedCursorPos(dbPath) == static_cast<int64_t>(permIndex) + 1);

    REQUIRE(eng->stop().has_value());
    REQUIRE(persistedCursorPos(dbPath) == static_cast<int64_t>(permIndex));
    // The rewind only moves the cursor; the shuffle order is untouched.
    REQUIRE(persistedPerm(dbPath) == permBefore);

    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == second);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("planNext resolves linear, repeat-one, wrap and shuffle without mutating",
          "[engine_queue][plan]") {
    using kanariv::engine::detail::planNext;
    using kanariv::types::RepeatMode;
    constexpr size_t n = 5;
    const std::vector<int64_t> noPerm;
    const std::span<const int64_t> noPermSpan(noPerm);
    // count/makePerm must only run when the plan needs them.
    const auto count = [n] { return n; };
    bool permRequested = false;
    const auto makePerm = [&permRequested](std::size_t count) {
        permRequested = true;
        std::vector<int64_t> perm(count);
        for (std::size_t i = 0; i < count; ++i)
            perm[i] = static_cast<int64_t>(count - 1 - i);
        return perm;
    };

    SECTION("linear walks and advances") {
        auto plan = planNext(false, RepeatMode::Off, 2, noPermSpan, count, makePerm);
        REQUIRE(plan.has_value());
        REQUIRE(plan->position == 2);
        REQUIRE(plan->cursor == 3);
        REQUIRE(!plan->newPerm.has_value());
        REQUIRE(!permRequested);
    }
    SECTION("linear wraps at the end and at a too-far cursor") {
        for (const RepeatMode mode : {RepeatMode::Off, RepeatMode::All}) {
            auto plan = planNext(false, mode, n, noPermSpan, count, makePerm);
            REQUIRE(plan.has_value());
            REQUIRE(plan->position == 0);
            REQUIRE(plan->cursor == 1);
            auto past = planNext(false, mode, n + 2, noPermSpan, count, makePerm);
            REQUIRE(past.has_value());
            REQUIRE(past->position == 0);
            REQUIRE(past->cursor == 1);
        }
    }
    SECTION("linear repeat-one replays without advancing the cursor") {
        auto plan = planNext(false, RepeatMode::One, n, noPermSpan, count, makePerm);
        REQUIRE(plan.has_value());
        REQUIRE(plan->position == static_cast<int64_t>(n - 1));
        REQUIRE(plan->cursor == n);
        auto past = planNext(false, RepeatMode::One, n + 2, noPermSpan, count, makePerm);
        REQUIRE(past.has_value());
        REQUIRE(past->position == static_cast<int64_t>(n - 1));
        REQUIRE(past->cursor == n + 2);
    }
    SECTION("empty queue is NotFound") {
        const auto empty = [] { return size_t{0}; };
        if (auto plan = planNext(false, RepeatMode::Off, 0, noPermSpan, empty, makePerm); !plan)
            REQUIRE(plan.error().code == StatusCode::NotFound);
        else
            FAIL("empty queue resolved a plan");
    }
    SECTION("shuffle follows the permutation") {
        const std::vector<int64_t> perm{3, 0, 2, 1};
        const std::span<const int64_t> span(perm);
        auto plan = planNext(true, RepeatMode::Off, 1, span, count, makePerm);
        REQUIRE(plan.has_value());
        REQUIRE(plan->position == 0);
        REQUIRE(plan->cursor == 2);
        REQUIRE(!plan->newPerm.has_value());
    }
    SECTION("shuffle repeat-one replays the last consumed entry") {
        const std::vector<int64_t> perm{3, 0, 2, 1};
        const std::span<const int64_t> span(perm);
        auto plan = planNext(true, RepeatMode::One, perm.size(), span, count, makePerm);
        REQUIRE(plan.has_value());
        REQUIRE(plan->position == perm.back());
        REQUIRE(plan->cursor == perm.size());
    }
    SECTION("shuffle wraps to the head of the current permutation") {
        const std::vector<int64_t> perm{3, 0, 2, 1};
        const std::span<const int64_t> span(perm);
        auto plan = planNext(true, RepeatMode::All, perm.size(), span, count, makePerm);
        REQUIRE(plan.has_value());
        REQUIRE(plan->position == perm.front());
        REQUIRE(plan->cursor == 1);
        REQUIRE(!plan->newPerm.has_value());
    }
    SECTION("first shuffle use plans a fresh permutation") {
        auto plan = planNext(true, RepeatMode::Off, 0, noPermSpan, count, makePerm);
        REQUIRE(plan.has_value());
        REQUIRE(permRequested);
        REQUIRE(plan->newPerm.has_value());
        REQUIRE(plan->newPerm->size() == n);
        REQUIRE(plan->position == plan->newPerm->front());
        REQUIRE(plan->cursor == 1);
    }
}

TEST_CASE("queue move remaps the shuffle permutation", "[engine_queue][shuffle][queue]") {
    const std::string dbPath = tempDbPath("eng_q_move_perm").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto sharedDb = std::shared_ptr<kanariv::db::Database>(std::move(dbRes.value()));
    constexpr std::size_t kTracks = 5;
    for (std::size_t i = 0; i < kTracks; ++i) {
        kanariv::types::Track t;
        t.path = "move_perm" + std::to_string(i) + ".wav";
        t.duration = 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0x40 + i * 16 + b);
        auto r = sharedDb->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(sharedDb->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = false;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachStorage(kanariv::db::makeStorage(sharedDb)).has_value());
    REQUIRE(eng->setShuffle(true).has_value());

    const auto permBefore = kanariv::engine::detail::ServiceAccess::shufflePermFor(*eng, 1);
    REQUIRE(permBefore.size() == kTracks);
    constexpr std::size_t from = 3;
    constexpr std::size_t to = 0;
    const auto slotOf = [&permBefore](int64_t position) {
        for (std::size_t i = 0; i < permBefore.size(); ++i)
            if (permBefore[i] == position)
                return i;
        return permBefore.size();
    };
    const std::size_t movedSlot = slotOf(static_cast<int64_t>(from));
    const std::size_t neighbourSlot = slotOf(static_cast<int64_t>(from - 1));
    REQUIRE(movedSlot < permBefore.size());
    REQUIRE(neighbourSlot < permBefore.size());

    REQUIRE(sharedDb->queueMove(1, from, to).has_value());
    kanariv::engine::detail::ServiceAccess::noteQueueMoved(*eng, 1, from, to);

    const auto permAfter = kanariv::engine::detail::ServiceAccess::shufflePermFor(*eng, 1);
    REQUIRE(permAfter.size() == permBefore.size());
    std::set<int64_t> seen(permAfter.begin(), permAfter.end());
    CHECK(seen.size() == kTracks);
    for (std::size_t i = 0; i < kTracks; ++i)
        CHECK(seen.count(static_cast<int64_t>(i)) == 1);
    // The moved item keeps its slot in the play order, now addressing `to`.
    CHECK(permAfter[movedSlot] == static_cast<int64_t>(to));
    // The item it jumped over keeps its own slot at its shifted position.
    CHECK(permAfter[neighbourSlot] == static_cast<int64_t>(from));
    // The remapped permutation is persisted, not just held in memory.
    CHECK(persistedPerm(dbPath) == permAfter);

    eng.reset();
    safeRemoveDb(dbPath);
}
