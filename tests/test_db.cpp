#include <sqlite3.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <common.hpp>
#include <cstdint>
#include <db/database_access.hpp>
#include <db/migration.hpp>
#include <expected>
#include <fstream>
#include <kanariv/contracts/null_history_store.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/db/json_state_store.hpp>
#include <kanariv/db/smart.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

static kanariv::types::Track makeTrack(int idx, const std::string& pathOverride = "") {
    kanariv::types::Track t;
    t.path = pathOverride.empty() ? ("/tmp/track_" + std::to_string(idx) + ".mp3") : pathOverride;
    t.title = "Title" + std::to_string(idx);
    t.artist = "Artist" + std::to_string(idx);
    t.album = "Album" + std::to_string(idx);
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(idx * 37 + b * 7);
    t.duration = 1.0 + idx;
    t.library_id = 1;
    return t;
}

namespace {

bool tableExists(sqlite3* db, std::string_view name) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
                           &raw, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(raw, 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    const bool found = sqlite3_step(raw) == SQLITE_ROW;
    sqlite3_finalize(raw);
    return found;
}

bool indexExists(sqlite3* db, std::string_view name) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?", -1,
                           &raw, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(raw, 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    const bool found = sqlite3_step(raw) == SQLITE_ROW;
    sqlite3_finalize(raw);
    return found;
}

bool columnExists(sqlite3* db, std::string_view table, std::string_view column) {
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return false;
    bool found = false;
    while (sqlite3_step(raw) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(raw, 1));
        if (name && column == name) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(raw);
    return found;
}

struct SqliteHandleCloser {
    void operator()(sqlite3* h) const noexcept {
        if (h)
            sqlite3_close(h);
    }
};

} // namespace

TEST_CASE("insertTrack duplicate AlreadyExists", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t = makeTrack(1);
    auto r1 = db->insertTrack(t);
    REQUIRE(r1.has_value());
    // fingerprint is UNIQUE per schema; duplicate fingerprint should be AlreadyExists
    kanariv::types::Track t2 = makeTrack(1);
    // ensure same fingerprint but different path to prove fingerprint uniqueness
    t2.path = "/tmp/track_dup_path.mp3";
    auto r2 = db->insertTrack(t2);
    REQUIRE(!r2.has_value());
    REQUIRE(r2.error().code == StatusCode::AlreadyExists);
    // same path with different fingerprint should succeed (path not unique)
    kanariv::types::Track t3 = makeTrack(99, t.path);
    for (int b = 0; b < 32; ++b)
        t3.fingerprint[b] = (uint8_t)(0xFF - b);
    auto r3 = db->insertTrack(t3);
    // this should succeed because fingerprint differs and path allows duplicates
    REQUIRE(r3.has_value());
}

TEST_CASE("updateTrack missing NotFound", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t = makeTrack(2);
    t.id = 9999; // non-existent id
    auto r = db->updateTrack(t);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::NotFound);
    // updating id 0 should be InvalidArg
    t.id = 0;
    auto r2 = db->updateTrack(t);
    REQUIRE(!r2.has_value());
    REQUIRE(r2.error().code == StatusCode::InvalidArg);
}

TEST_CASE("getTrack NotFound", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    auto r = db->getTrack(9999);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::NotFound);
    auto r2 = db->getTrack(0);
    REQUIRE(!r2.has_value());
    REQUIRE(r2.error().code == StatusCode::InvalidArg);
}

TEST_CASE("deleteTrack and verify", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t = makeTrack(3);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    int64_t id = *ins;
    REQUIRE(db->deleteTrack(id).has_value());
    auto g = db->getTrack(id);
    REQUIRE(!g.has_value());
    REQUIRE(g.error().code == StatusCode::NotFound);
    // delete again -> NotFound
    auto del2 = db->deleteTrack(id);
    REQUIRE(!del2.has_value());
    REQUIRE(del2.error().code == StatusCode::NotFound);
    // delete 0 -> InvalidArg
    REQUIRE(!db->deleteTrack(0).has_value());
    REQUIRE(db->deleteTrack(0).error().code == StatusCode::InvalidArg);
}

TEST_CASE("listTracks pagination limit/offset", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 5; ++i) {
        kanariv::types::Track t = makeTrack(10 + i);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
    }
    kanariv::types::TrackQuery q;
    q.limit = 2;
    q.offset = 0;
    auto l1 = db->listTracks(q);
    REQUIRE(l1.has_value());
    REQUIRE(l1->size() == 2);
    q.offset = 2;
    auto l2 = db->listTracks(q);
    REQUIRE(l2.has_value());
    REQUIRE(l2->size() == 2);
    REQUIRE((*l2)[0].id != (*l1)[0].id);
    q.offset = 4;
    q.limit = 2;
    auto l3 = db->listTracks(q);
    REQUIRE(l3.has_value());
    REQUIRE(l3->size() == 1);
    q.offset = 10;
    auto l4 = db->listTracks(q);
    REQUIRE(l4.has_value());
    REQUIRE(l4->empty());
    // limit 0 means no limit (all)
    q.limit = 0;
    q.offset = 0;
    auto l5 = db->listTracks(q);
    REQUIRE(l5.has_value());
    REQUIRE(l5->size() == 5);
    // offset only (no limit)
    q.limit = 0;
    q.offset = 3;
    auto l6 = db->listTracks(q);
    REQUIRE(l6.has_value());
    REQUIRE(l6->size() == 2);
}

TEST_CASE("parseSmartQuery", "[db]") {
    auto check = [](std::string_view q, kanariv::types::TrackQuery expected) {
        auto r = parseSmartQuery(q);
        REQUIRE(r.has_value());
        REQUIRE(r->rating == expected.rating);
        REQUIRE(r->year == expected.year);
        REQUIRE(r->year_min == expected.year_min);
        REQUIRE(r->year_max == expected.year_max);
        REQUIRE(r->play_count_min == expected.play_count_min);
        REQUIRE(r->play_count_max == expected.play_count_max);
        REQUIRE(r->duration_min == expected.duration_min);
        REQUIRE(r->duration_max == expected.duration_max);
        REQUIRE(r->artist == expected.artist);
        REQUIRE(r->album == expected.album);
        REQUIRE(r->genre == expected.genre);
        REQUIRE(r->search == expected.search);
    };

    // Empty -> match all.
    auto empty = parseSmartQuery("");
    REQUIRE(empty.has_value());

    // Exact rating.
    check("rating=5", kanariv::types::TrackQuery{.rating = 5});

    // Year range ops.
    check("year>=2000", kanariv::types::TrackQuery{.year_min = 2000});
    check("year<=1999", kanariv::types::TrackQuery{.year_max = 1999});
    check("year>2000", kanariv::types::TrackQuery{.year_min = 2001});
    check("year<2000", kanariv::types::TrackQuery{.year_max = 1999});
    check("year=1985", kanariv::types::TrackQuery{.year = 1985});

    // Play count.
    check("play_count>=10", kanariv::types::TrackQuery{.play_count_min = 10});
    check("play_count<=5", kanariv::types::TrackQuery{.play_count_max = 5});
    check("play_count>5", kanariv::types::TrackQuery{.play_count_min = 6});
    check("play_count<5", kanariv::types::TrackQuery{.play_count_max = 4});

    // Duration.
    check("duration>=300", kanariv::types::TrackQuery{.duration_min = 300.0});
    check("duration<=600", kanariv::types::TrackQuery{.duration_max = 600.0});
    check("duration=240.5",
          kanariv::types::TrackQuery{.duration_min = 240.5, .duration_max = 240.5});

    // Exact text fields.
    check("artist=Miles Davis", kanariv::types::TrackQuery{.artist = "Miles Davis"});
    check("album=Kind of Blue", kanariv::types::TrackQuery{.album = "Kind of Blue"});
    check("genre=Jazz", kanariv::types::TrackQuery{.genre = "Jazz"});

    // Search.
    check("search=blue", kanariv::types::TrackQuery{.search = "blue"});

    // Combined AND.
    check("rating=5,year>=2000,genre=Jazz",
          kanariv::types::TrackQuery{.genre = "Jazz", .year_min = 2000, .rating = 5});

    // Rejection: unknown field.
    auto bad1 = parseSmartQuery("foo=bar");
    REQUIRE(!bad1.has_value());
    REQUIRE(bad1.error().code == kanariv::utils::StatusCode::InvalidArg);

    // Rejection: rating with range op.
    auto bad2 = parseSmartQuery("rating>=4");
    REQUIRE(!bad2.has_value());
    REQUIRE(bad2.error().code == kanariv::utils::StatusCode::InvalidArg);

    // Rejection: rating out of range.
    auto bad3 = parseSmartQuery("rating=9");
    REQUIRE(!bad3.has_value());
    REQUIRE(bad3.error().code == kanariv::utils::StatusCode::InvalidArg);

    // Rejection: malformed predicate.
    auto bad4 = parseSmartQuery("rating");
    REQUIRE(!bad4.has_value());
    REQUIRE(bad4.error().code == kanariv::utils::StatusCode::InvalidArg);
}

TEST_CASE("playlistGetTracks smart playlist evaluation", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    // Insert tracks with varied rating/year.
    kanariv::types::Track t1 = makeTrack(1);
    t1.rating = 5;
    t1.year = 1970;
    REQUIRE(db->insertTrack(t1).has_value());

    kanariv::types::Track t2 = makeTrack(2);
    t2.rating = 4;
    t2.year = 2005;
    REQUIRE(db->insertTrack(t2).has_value());

    kanariv::types::Track t3 = makeTrack(3);
    t3.rating = 5;
    t3.year = 2010;
    REQUIRE(db->insertTrack(t3).has_value());

    // Create a manual playlist (should return its items).
    auto pid = db->createPlaylist("manual");
    REQUIRE(pid.has_value());
    REQUIRE(db->playlistAddTrack(*pid, 1).has_value());
    REQUIRE(db->playlistAddTrack(*pid, 2).has_value());
    auto manual = db->playlistGetTracks(*pid);
    REQUIRE(manual.has_value());
    REQUIRE(manual->size() == 2);

    // Create a smart playlist: rating=5.
    auto spid = db->createPlaylist("top rated", 1, "rating=5");
    REQUIRE(spid.has_value());
    auto top = db->playlistGetTracks(*spid);
    REQUIRE(top.has_value());
    REQUIRE(top->size() == 2);
    for (auto& t : *top)
        REQUIRE(t.rating == 5);

    // Create a smart playlist: year>=2000.
    auto spid2 = db->createPlaylist("modern", 1, "year>=2000");
    REQUIRE(spid2.has_value());
    auto modern = db->playlistGetTracks(*spid2);
    REQUIRE(modern.has_value());
    REQUIRE(modern->size() == 2);
    for (auto& t : *modern)
        REQUIRE(t.year >= 2000);

    // Invalid smart_query should propagate an error.
    auto badpid = db->createPlaylist("bad", 1, "bogus=1");
    REQUIRE(badpid.has_value());
    auto bad = db->playlistGetTracks(*badpid);
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().code == kanariv::utils::StatusCode::InvalidArg);
}

TEST_CASE("listTracks new filters rating/year/play_count/duration", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    // Insert tracks with varied rating, year, play_count, duration.
    kanariv::types::Track t1 = makeTrack(1);
    t1.rating = 5;
    t1.year = 1970;
    t1.play_count = 100;
    t1.duration = 420.0;
    REQUIRE(db->insertTrack(t1).has_value());

    kanariv::types::Track t2 = makeTrack(2);
    t2.rating = 4;
    t2.year = 1985;
    t2.play_count = 42;
    t2.duration = 240.0;
    REQUIRE(db->insertTrack(t2).has_value());

    kanariv::types::Track t3 = makeTrack(3);
    t3.rating = 3;
    t3.year = 2000;
    t3.play_count = 7;
    t3.duration = 180.0;
    REQUIRE(db->insertTrack(t3).has_value());

    kanariv::types::Track t4 = makeTrack(4);
    t4.rating = 5;
    t4.year = 2010;
    t4.play_count = 0;
    t4.duration = 600.0;
    REQUIRE(db->insertTrack(t4).has_value());

    // Exact rating filter.
    kanariv::types::TrackQuery q;
    q.rating = 5;
    auto r5 = db->listTracks(q);
    REQUIRE(r5.has_value());
    REQUIRE(r5->size() == 2);
    for (auto& t : *r5)
        REQUIRE(t.rating == 5);

    // Year range inclusive.
    q = kanariv::types::TrackQuery{};
    q.year_min = 1980;
    q.year_max = 2005;
    auto ry = db->listTracks(q);
    REQUIRE(ry.has_value());
    REQUIRE(ry->size() == 2);
    for (auto& t : *ry) {
        REQUIRE(t.year >= 1980);
        REQUIRE(t.year <= 2005);
    }

    // One-sided year max only.
    q = kanariv::types::TrackQuery{};
    q.year_max = 1975;
    auto ry2 = db->listTracks(q);
    REQUIRE(ry2.has_value());
    REQUIRE(ry2->size() == 1);
    REQUIRE((*ry2)[0].year == 1970);

    // Play count range.
    q = kanariv::types::TrackQuery{};
    q.play_count_min = 10;
    q.play_count_max = 200;
    auto rp = db->listTracks(q);
    REQUIRE(rp.has_value());
    REQUIRE(rp->size() == 2);
    for (auto& t : *rp) {
        REQUIRE(t.play_count >= 10);
        REQUIRE(t.play_count <= 200);
    }

    // Duration range.
    q = kanariv::types::TrackQuery{};
    q.duration_min = 200.0;
    q.duration_max = 500.0;
    auto rd = db->listTracks(q);
    REQUIRE(rd.has_value());
    REQUIRE(rd->size() == 2);
    for (auto& t : *rd) {
        REQUIRE(t.duration >= 200.0);
        REQUIRE(t.duration <= 500.0);
    }

    // Combined filters: rating=5 AND year>=2000.
    q = kanariv::types::TrackQuery{};
    q.rating = 5;
    q.year_min = 2000;
    auto rc = db->listTracks(q);
    REQUIRE(rc.has_value());
    REQUIRE(rc->size() == 1);
    REQUIRE((*rc)[0].rating == 5);
    REQUIRE((*rc)[0].year >= 2000);
}

TEST_CASE("findByFingerprint and findByPath", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t = makeTrack(42, "/tmp/find_test.mp3");
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    // findByPath success
    auto byPath = db->findByPath(t.path);
    REQUIRE(byPath.has_value());
    REQUIRE(byPath->id == *ins);
    // findByPath missing
    auto byPathMiss = db->findByPath("/nonexistent/path.mp3");
    REQUIRE(!byPathMiss.has_value());
    REQUIRE(byPathMiss.error().code == StatusCode::NotFound);
    // findByFingerprint success
    auto byFp = db->findByFingerprint(t.fingerprint);
    REQUIRE(byFp.has_value());
    REQUIRE(byFp->id == *ins);
    // findByFingerprint missing
    std::array<uint8_t, 32> wrong{};
    wrong.fill(0xFF);
    auto byFpMiss = db->findByFingerprint(wrong);
    REQUIRE(!byFpMiss.has_value());
    REQUIRE(byFpMiss.error().code == StatusCode::NotFound);
}

// Keep original sqlite sanity tests but adapted to use Database::open path check
TEST_CASE("SQLite open in-memory via Database", "[db_sqlite]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    REQUIRE(DatabaseAccess::handle(*dbRes.value()) != nullptr);
}

// From test_db_impl.cpp - SQLite direct tests
TEST_CASE("Database open/create SQLite in-memory", "[db]") {
    sqlite3* db{nullptr};
    int rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    REQUIRE(rc == SQLITE_OK);
    if (db)
        sqlite3_close(db);
}

TEST_CASE("Database open file path", "[db]") {
    sqlite3* db{nullptr};
    int rc = sqlite3_open_v2("test_db_temp.db", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    (void)rc;
    if (db)
        sqlite3_close(db);
}

TEST_CASE("insertTrack via Transaction", "[db]") {
    sqlite3* db{nullptr};
    int rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    REQUIRE(rc == SQLITE_OK);

    const char* sql = "CREATE TABLE tracks (id INTEGER PRIMARY KEY, name TEXT, path TEXT)";
    rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    REQUIRE(rc == SQLITE_OK);

    sql = "INSERT INTO tracks (name, path) VALUES (?, ?)";
    sqlite3_stmt* stmt{nullptr};
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    REQUIRE(rc == SQLITE_OK);

    sqlite3_bind_text(stmt, 1, "Test Track", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, "/path/to/track.mp3", -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    REQUIRE(rc == SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_CASE("listTracks after schema init", "[db]") {
    sqlite3* db{nullptr};
    int rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    REQUIRE(rc == SQLITE_OK);

    rc = sqlite3_exec(db, "CREATE TABLE tracks (id INTEGER PRIMARY KEY, name TEXT)", nullptr,
                      nullptr, nullptr);

    sqlite3_stmt* stmt{nullptr};
    const char* sql = "INSERT INTO tracks (name) VALUES ('Track 1')";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    REQUIRE(rc == SQLITE_OK);

    rc = sqlite3_step(stmt);
    REQUIRE(rc == SQLITE_DONE);
    sqlite3_finalize(stmt);

    sql = "SELECT name FROM tracks";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    REQUIRE(rc == SQLITE_OK);

    rc = sqlite3_step(stmt);
    REQUIRE(rc == SQLITE_ROW);

    const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    REQUIRE(name != nullptr);
    REQUIRE(std::string(name) == "Track 1");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_CASE("Database treats file: paths as URIs only when opted in", "[db]") {
    const auto path = kanariv::test_helpers::tempDbPath("uri");
    {
        auto created = Database::open(path.string());
        REQUIRE(created.has_value());
        REQUIRE((*created)->insertTrack(makeTrack(1)).has_value());
    }
    kanariv::db::DatabaseOptions opts;
    opts.allowUri = true;
    auto ro = Database::open("file:" + path.string() + "?mode=ro", opts);
    REQUIRE(ro.has_value());
    // The URI reached SQLite (not a literal filename): mode=ro is honoured and
    // a write is rejected.
    REQUIRE_FALSE((*ro)->insertTrack(makeTrack(2)).has_value());
}

TEST_CASE("Database open emits lifecycle log", "[db]") {
    using namespace kanariv::utils;
    std::vector<std::string> out;
    Logger logger([&](LogLevel, std::string_view msg) { out.emplace_back(msg); }, LogLevel::Trace);
    kanariv::db::DatabaseOptions opts;
    opts.logger = &logger;
    auto dbRes = kanariv::db::Database::open(":memory:", opts);
    REQUIRE(dbRes.has_value());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].find("database opened path=") != std::string::npos);
}

TEST_CASE("bookmark add/list/remove", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t = makeTrack(7);
    auto tr = db->insertTrack(t);
    REQUIRE(tr.has_value());

    REQUIRE(db->bookmarkAdd(*tr, 1234, "note one").has_value());
    REQUIRE(db->bookmarkAdd(*tr, 5678, "").has_value());
    auto list = db->listBookmarks(*tr);
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 2);
    REQUIRE(list->at(0).track_id == *tr);
    REQUIRE(list->at(0).position_ms == 1234);
    REQUIRE(list->at(0).note == "note one");
    REQUIRE(list->at(0).created > 0);
    REQUIRE(list->at(1).position_ms == 5678);

    REQUIRE(db->bookmarkRemove(list->at(0).id).has_value());
    auto after = db->listBookmarks(*tr);
    REQUIRE(after.has_value());
    REQUIRE(after->size() == 1);
    REQUIRE(after->at(0).position_ms == 5678);

    auto missing = db->bookmarkRemove(999999);
    REQUIRE(!missing.has_value());
    REQUIRE(missing.error().code == StatusCode::NotFound);
    REQUIRE(!db->bookmarkAdd(0, 0).has_value());
    REQUIRE(!db->bookmarkAdd(*tr, -1).has_value());
}

TEST_CASE("history completion_pct round-trip 0/50/100", "[db]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t = makeTrack(8);
    auto tr = db->insertTrack(t);
    REQUIRE(tr.has_value());

    for (int i = 0; i < 3; ++i) {
        kanariv::types::HistoryEntry e;
        e.track_id = *tr;
        e.started_at = 100 + i;
        e.completed_at = 200 + i;
        e.position_ms = i * 1000;
        e.completion_pct = i * 50.0;
        REQUIRE(db->historyAdd(e).has_value());
    }

    kanariv::types::HistoryQuery q;
    q.track_id = *tr;
    auto list = db->queryHistory(&q);
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 3);
    REQUIRE((*list)[0].completion_pct == 100.0);
    REQUIRE((*list)[1].completion_pct == 50.0);
    REQUIRE((*list)[2].completion_pct == 0.0);
}

TEST_CASE("historyList joins track metadata and historyClear empties it", "[db][history]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    auto tr = db->insertTrack(makeTrack(9));
    REQUIRE(tr.has_value());

    kanariv::types::HistoryEntry e;
    e.track_id = *tr;
    e.started_at = 1000;
    e.completed_at = 1100;
    e.position_ms = 750;
    e.completion_pct = 75.0;
    REQUIRE(db->historyAdd(e).has_value());

    auto listed = db->listHistory(10);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK((*listed)[0].track_id == *tr);
    CHECK((*listed)[0].title == "Title9");
    CHECK((*listed)[0].artist == "Artist9");
    CHECK((*listed)[0].duration == 10.0);

    REQUIRE(db->clearHistory().has_value());
    auto empty = db->listHistory(10);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("queue repeat mode round-trips through the DB", "[db][queue][repeat]") {
    // Pin the wire order the SQL column stores (Off=0, All=1, One=2).
    static_assert(static_cast<int>(kanariv::types::RepeatMode::Off) == 0);
    static_assert(static_cast<int>(kanariv::types::RepeatMode::All) == 1);
    static_assert(static_cast<int>(kanariv::types::RepeatMode::One) == 2);

    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    auto id = db->createQueue("repeat");
    REQUIRE(id.has_value());

    // A fresh queue defaults to Off (column default 0).
    auto fresh = db->getQueue(*id);
    REQUIRE(fresh.has_value());
    REQUIRE(fresh->repeat_mode == kanariv::types::RepeatMode::Off);

    for (auto mode : {kanariv::types::RepeatMode::Off, kanariv::types::RepeatMode::All,
                      kanariv::types::RepeatMode::One}) {
        REQUIRE(db->setQueueRepeat(*id, mode).has_value());
        auto stored = db->getQueue(*id);
        REQUIRE(stored.has_value());
        REQUIRE(stored->repeat_mode == mode);
    }

    // An out-of-range legacy value must read back as Off, never as a bogus mode.
    REQUIRE(db->setQueueRepeat(*id, static_cast<kanariv::types::RepeatMode>(99)).has_value());
    auto fallback = db->getQueue(*id);
    REQUIRE(fallback.has_value());
    REQUIRE(fallback->repeat_mode == kanariv::types::RepeatMode::Off);
}

TEST_CASE("schema user_version reaches current after open and reopen is a no-op",
          "[db][migration]") {
    auto path = kanariv::test_helpers::tempDbPath("schema_version");
    {
        auto db = Database::open(path.string());
        REQUIRE(db.has_value());
        auto version = internal::readUserVersion(DatabaseAccess::handle(**db));
        REQUIRE(version.has_value());
        CHECK(*version == internal::kSchemaVersion);

        auto steps = internal::schemaMigrations();
        REQUIRE(!steps.empty());
        CHECK(steps.front().version == 1);
        CHECK(steps.back().version == internal::kSchemaVersion);
        for (std::size_t i = 1; i < steps.size(); ++i)
            CHECK(steps[i - 1].version < steps[i].version);
    }
    // Reopening a current database skips every step and keeps the version.
    {
        auto db = Database::open(path.string());
        REQUIRE(db.has_value());
        auto version = internal::readUserVersion(DatabaseAccess::handle(**db));
        REQUIRE(version.has_value());
        CHECK(*version == internal::kSchemaVersion);
    }
}

TEST_CASE("failing schema migration rolls back and reports the step", "[db][migration]") {
    auto raw = std::unique_ptr<sqlite3, SqliteHandleCloser>{};
    sqlite3* handle = nullptr;
    REQUIRE(sqlite3_open(":memory:", &handle) == SQLITE_OK);
    raw.reset(handle);
    const std::array<internal::MigrationStep, 3> steps{{
        {1, "base", internal::schemaMigrations().front().sql, nullptr},
        {2, "committed probe", "CREATE TABLE probe_committed(x INTEGER)", nullptr},
        {3, "broken probe",
         "CREATE TABLE probe_rolled_back(y INTEGER);"
         "INSERT INTO table_that_does_not_exist VALUES (1)",
         nullptr},
    }};
    auto result = internal::applyMigrations(handle, steps);
    REQUIRE(!result.has_value());
    CHECK(result.error().message.find("schema migration 3") != std::string::npos);

    // The failing step rolled back; the previous step stayed committed.
    auto version = internal::readUserVersion(handle);
    REQUIRE(version.has_value());
    CHECK(*version == 2);
    CHECK(tableExists(handle, "probe_committed"));
    CHECK(!tableExists(handle, "probe_rolled_back"));
}

TEST_CASE("legacy user_version 0 database migrates without data loss", "[db][migration]") {
    auto path = kanariv::test_helpers::tempDbPath("legacy_schema");
    sqlite3* handle = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &handle) == SQLITE_OK);
    {
        auto raw = std::unique_ptr<sqlite3, SqliteHandleCloser>{handle};
        // Pre-source-identity tracks table: no source_* or replay_gain_* columns.
        const char* legacyDdl =
            "CREATE TABLE tracks ("
            "id INTEGER PRIMARY KEY, fingerprint BLOB(32) NOT NULL UNIQUE, path TEXT NOT NULL, "
            "deleted_at DATETIME, size INTEGER, mtime INTEGER, duration REAL, "
            "sample_rate INTEGER, channels INTEGER, bitrate INTEGER, title TEXT, artist TEXT, "
            "album TEXT, album_artist TEXT, genre TEXT, year INTEGER, track_num INTEGER, "
            "disc_num INTEGER, cover_art_path TEXT, rating INTEGER DEFAULT 0, "
            "play_count INTEGER DEFAULT 0, last_played DATETIME, "
            "date_added DATETIME DEFAULT CURRENT_TIMESTAMP, last_scanned DATETIME, "
            "dirty BOOLEAN DEFAULT 0, library_id INTEGER DEFAULT 1);"
            "INSERT INTO tracks (fingerprint, path, title, artist, album, album_artist, genre) "
            "VALUES (zeroblob(32), 'legacy/path.mp3', 'Legacy', 'Artist', 'Album', "
            "'Artist', 'Rock');";
        char* err = nullptr;
        REQUIRE(sqlite3_exec(handle, legacyDdl, nullptr, nullptr, &err) == SQLITE_OK);
        if (err)
            sqlite3_free(err);
        auto version = internal::readUserVersion(handle);
        REQUIRE(version.has_value());
        REQUIRE(*version == 0);
    }

    auto opened = Database::open(path.string());
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    auto tracks = db->listTracks();
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    CHECK((*tracks)[0].path == "legacy/path.mp3");
    CHECK((*tracks)[0].title == "Legacy");
    CHECK((*tracks)[0].source.key == "legacy/path.mp3");
    auto version = internal::readUserVersion(DatabaseAccess::handle(*db));
    REQUIRE(version.has_value());
    CHECK(*version == internal::kSchemaVersion);
    // Version 7 removes the soft-delete column the legacy table carried.
    CHECK(!columnExists(DatabaseAccess::handle(*db), "tracks", "deleted_at"));
}

TEST_CASE("migration 7 drops the unused soft-delete column", "[db][migration]") {
    auto path = kanariv::test_helpers::tempDbPath("drop_soft_delete");
    sqlite3* handle = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &handle) == SQLITE_OK);
    {
        auto raw = std::unique_ptr<sqlite3, SqliteHandleCloser>{handle};
        // Stop at v6, then recreate a pre-v7 file: the column existed and a row
        // carried a value that nothing reads.
        const auto steps = internal::schemaMigrations();
        REQUIRE(steps.size() >= 7);
        REQUIRE(internal::applyMigrations(handle, steps.first(6)).has_value());
        char* err = nullptr;
        REQUIRE(sqlite3_exec(handle,
                             "ALTER TABLE tracks ADD COLUMN deleted_at DATETIME;"
                             "INSERT INTO tracks (fingerprint, path, title, deleted_at) "
                             "VALUES (randomblob(32), 'old/soft.mp3', 'Kept', 1234)",
                             nullptr, nullptr, &err) == SQLITE_OK);
        if (err)
            sqlite3_free(err);
        REQUIRE(columnExists(handle, "tracks", "deleted_at"));
    }

    // Opening applies step 7: the column is gone, the row and its other
    // columns survive the table rewrite.
    auto opened = Database::open(path.string());
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    sqlite3* h = DatabaseAccess::handle(*db);
    CHECK(!columnExists(h, "tracks", "deleted_at"));
    auto tracks = db->listTracks();
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    CHECK((*tracks)[0].path == "old/soft.mp3");
    CHECK((*tracks)[0].title == "Kept");
    auto version = internal::readUserVersion(h);
    REQUIRE(version.has_value());
    CHECK(*version == internal::kSchemaVersion);
}

TEST_CASE("migration 8 indexes the global history timeline", "[db][migration]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    CHECK(indexExists(DatabaseAccess::handle(*dbRes.value()), "idx_history_started"));
}

TEST_CASE("migration 5 clears gains written by the old loudness heuristic", "[db][migration]") {
    auto path = kanariv::test_helpers::tempDbPath("replay_gain_migration");
    sqlite3* handle = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &handle) == SQLITE_OK);
    {
        auto raw = std::unique_ptr<sqlite3, SqliteHandleCloser>{handle};
        // Stop at v4: the schema the old scanner wrote its mis-scaled gains into.
        const auto steps = internal::schemaMigrations();
        REQUIRE(steps.size() >= 5);
        REQUIRE(internal::applyMigrations(handle, steps.first(4)).has_value());
        char* err = nullptr;
        const char* insertSql =
            "INSERT INTO tracks (fingerprint, path, replay_gain_track, replay_gain_album) "
            "VALUES (randomblob(32), 'old/gain.mp3', 7.5, 3.25)";
        REQUIRE(sqlite3_exec(handle, insertSql, nullptr, nullptr, &err) == SQLITE_OK);
        if (err)
            sqlite3_free(err);
        auto version = internal::readUserVersion(handle);
        REQUIRE(version.has_value());
        REQUIRE(*version == 4);
    }

    // Opening the database applies step 5, which must clear both columns rather
    // than reinterpret them as ReplayGain (M10).
    auto opened = Database::open(path.string());
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    auto tracks = db->listTracks();
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    CHECK((*tracks)[0].replayGainTrack == 0.0);
    CHECK((*tracks)[0].replayGainAlbum == 0.0);
    auto version = internal::readUserVersion(DatabaseAccess::handle(*db));
    REQUIRE(version.has_value());
    CHECK(*version == internal::kSchemaVersion);
}

TEST_CASE("migration 6 drops indexes that duplicate table constraints", "[db][migration]") {
    auto path = kanariv::test_helpers::tempDbPath("redundant_indexes");
    auto opened = Database::open(path.string());
    REQUIRE(opened.has_value());
    auto db = std::move(*opened);
    sqlite3* handle = DatabaseAccess::handle(*db);
    CHECK(!indexExists(handle, "idx_tracks_fingerprint"));
    CHECK(!indexExists(handle, "idx_queue_queue_pos"));

    // The table constraint still rejects a duplicate queue position.
    char* err = nullptr;
    int rc = sqlite3_exec(handle,
                          "INSERT INTO queue (queue_id, position) VALUES (1, 0);"
                          "INSERT INTO queue (queue_id, position) VALUES (1, 0);",
                          nullptr, nullptr, &err);
    if (err)
        sqlite3_free(err);
    CHECK(rc != SQLITE_OK);
}

TEST_CASE("queue positions are stored ordering values and APIs take indices",
          "[db][queue][position]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    const auto a = db->insertTrack(makeTrack(1));
    const auto b = db->insertTrack(makeTrack(2));
    const auto c = db->insertTrack(makeTrack(3));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());

    // qid 0 selects the default queue; -1 appends.
    REQUIRE(db->queueEnqueue(0, *a, -1).has_value());
    REQUIRE(db->queueEnqueue(0, *b, -1).has_value());
    // Insert at index 0.
    REQUIRE(db->queueEnqueue(0, *c, 0).has_value());

    auto items = db->listQueueItems(0);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 3);
    CHECK((*items)[0].track_id == *c);
    CHECK((*items)[1].track_id == *a);
    CHECK((*items)[2].track_id == *b);
    // position is the gap-based ordering value, not the index.
    CHECK((*items)[0].position < (*items)[1].position);
    CHECK((*items)[1].position < (*items)[2].position);
    CHECK((*items)[1].position - (*items)[0].position > 1);

    auto atZero = db->queueTrackAt(0, 0);
    REQUIRE(atZero.has_value());
    CHECK(atZero->id == *c);
    auto pastEnd = db->queueTrackAt(0, 3);
    REQUIRE(!pastEnd.has_value());
    CHECK(pastEnd.error().code == StatusCode::NotFound);

    auto tooFar = db->queueEnqueue(0, *a, 4);
    REQUIRE(!tooFar.has_value());
    CHECK(tooFar.error().code == StatusCode::InvalidArg);
    auto belowAppend = db->queueEnqueue(0, *a, -2);
    REQUIRE(!belowAppend.has_value());
    CHECK(belowAppend.error().code == StatusCode::InvalidArg);
}

TEST_CASE("queue remove takes a zero-based index", "[db][queue][position]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    const auto a = db->insertTrack(makeTrack(1));
    const auto b = db->insertTrack(makeTrack(2));
    const auto c = db->insertTrack(makeTrack(3));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(db->queueEnqueueBatch(0, std::vector<int64_t>{*a, *b, *c}).has_value());

    // Removing stored positions directly would be wrong: index 1 is b.
    REQUIRE(db->queueRemove(0, 1).has_value());
    auto items = db->listQueueItems(0);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 2);
    CHECK((*items)[0].track_id == *a);
    CHECK((*items)[1].track_id == *c);

    auto negative = db->queueRemove(0, -1);
    REQUIRE(!negative.has_value());
    CHECK(negative.error().code == StatusCode::InvalidArg);
    auto pastEnd = db->queueRemove(0, 2);
    REQUIRE(!pastEnd.has_value());
    CHECK(pastEnd.error().code == StatusCode::NotFound);
}

TEST_CASE("queue tracks join lists tracks in queue order", "[db][queue][position]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    const auto a = db->insertTrack(makeTrack(1));
    const auto b = db->insertTrack(makeTrack(2));
    const auto c = db->insertTrack(makeTrack(3));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(db->queueEnqueueBatch(0, std::vector<int64_t>{*c, *a, *b, *c}).has_value());

    auto tracks = db->tracksForQueue(0);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 4);
    CHECK((*tracks)[0].id == *c);
    CHECK((*tracks)[1].id == *a);
    CHECK((*tracks)[2].id == *b);
    CHECK((*tracks)[3].id == *c);
}

TEST_CASE("queue move reorders without rewriting rows", "[db][queue][position]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    const auto a = db->insertTrack(makeTrack(1));
    const auto b = db->insertTrack(makeTrack(2));
    const auto c = db->insertTrack(makeTrack(3));
    const auto d = db->insertTrack(makeTrack(4));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(d.has_value());
    REQUIRE(db->queueEnqueueBatch(0, std::vector<int64_t>{*a, *b, *c, *d}).has_value());
    auto before = db->listQueueItems(0);
    REQUIRE(before.has_value());

    // Move to the front and to the back.
    REQUIRE(db->queueMove(0, 2, 0).has_value());
    auto items = db->listQueueItems(0);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 4);
    CHECK((*items)[0].track_id == *c);
    CHECK((*items)[1].track_id == *a);
    CHECK((*items)[2].track_id == *b);
    CHECK((*items)[3].track_id == *d);

    REQUIRE(db->queueMove(0, 0, 3).has_value());
    items = db->listQueueItems(0);
    REQUIRE(items.has_value());
    CHECK((*items)[0].track_id == *a);
    CHECK((*items)[1].track_id == *b);
    CHECK((*items)[2].track_id == *d);
    CHECK((*items)[3].track_id == *c);

    // Same rows, same insertion timestamps: only positions changed.
    std::set<int64_t> idsBefore;
    std::set<int64_t> idsAfter;
    for (auto& it : *before)
        idsBefore.insert(it.id);
    for (auto& it : *items)
        idsAfter.insert(it.id);
    CHECK(idsBefore == idsAfter);
    for (std::size_t i = 1; i < items->size(); ++i)
        CHECK((*items)[i - 1].position < (*items)[i].position);

    // A move to the same index is a no-op, and out-of-range is rejected.
    REQUIRE(db->queueMove(0, 2, 2).has_value());
    auto same = db->listQueueItems(0);
    REQUIRE(same.has_value());
    CHECK((*same)[2].track_id == *d);
    auto tooFar = db->queueMove(0, 0, 4);
    REQUIRE(!tooFar.has_value());
    CHECK(tooFar.error().code == StatusCode::InvalidArg);
    auto fromFar = db->queueMove(0, 4, 0);
    REQUIRE(!fromFar.has_value());
    CHECK(fromFar.error().code == StatusCode::InvalidArg);
    auto tracks = db->tracksForQueue(0);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 4);
    CHECK((*tracks)[0].id == *a);
    CHECK((*tracks)[3].id == *c);
}

TEST_CASE("queue insertion renormalizes exhausted gaps without changing order",
          "[db][queue][position]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    const auto first = db->insertTrack(makeTrack(1));
    const auto last = db->insertTrack(makeTrack(2));
    REQUIRE(first.has_value());
    REQUIRE(last.has_value());
    REQUIRE(db->queueEnqueue(0, *first).has_value());
    REQUIRE(db->queueEnqueue(0, *last).has_value());

    // >30 midpoint insertions between the same pair exhausts the gap and
    // forces renormalization.
    std::vector<int64_t> inserted;
    for (int i = 0; i < 40; ++i) {
        auto id = db->insertTrack(makeTrack(100 + i));
        REQUIRE(id.has_value());
        REQUIRE(db->queueEnqueue(0, *id, 1).has_value());
        inserted.push_back(*id);
    }

    auto items = db->listQueueItems(0);
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 42);
    CHECK((*items)[0].track_id == *first);
    CHECK((*items)[1].track_id == inserted.back());
    CHECK((*items)[2].track_id == inserted[inserted.size() - 2]);
    CHECK((*items)[41].track_id == *last);
    for (std::size_t i = 1; i < items->size(); ++i)
        CHECK((*items)[i - 1].position < (*items)[i].position);

    auto middle = db->queueTrackAt(0, 1);
    REQUIRE(middle.has_value());
    CHECK(middle->id == inserted.back());
}

TEST_CASE("NullHistoryStore accepts writes and lists nothing", "[db][contracts]") {
    kanariv::contracts::NullHistoryStore store;
    CHECK(store.isOpen());
    CHECK(store.markPlayed(1, 10, 20, 1000, 50.0, 1).has_value());
    auto listed = store.listHistory(10);
    REQUIRE(listed.has_value());
    CHECK(listed->empty());
    CHECK(store.clearHistory().has_value());
}

namespace {

kanariv::types::EngineState sampleEngineState() {
    kanariv::types::EngineState state;
    state.queue_name = "chill";
    state.cursor_pos = 3;
    state.current_path = "music/current.flac";
    state.current_track_id = 42;
    state.shuffle_perm = {2, 0, 1};
    state.shuffle_enabled = true;
    state.repeat_mode = kanariv::types::RepeatMode::All;
    state.volume = 0.25f;
    state.pre_mute_volume = 0.75f;
    state.position_ms = 12345;
    return state;
}

} // namespace

TEST_CASE("JsonPlaybackStateStore round-trips the engine snapshot", "[db][state_store]") {
    const std::string dbPath = tempDbPath("state_store_rt").string();
    const std::string sidecar = dbPath + ".state.json";
    JsonPlaybackStateStore store(sidecar);
    CHECK(store.isOpen());
    CHECK(store.path() == sidecar);

    // A missing sidecar starts from a default snapshot, never an error.
    auto missing = store.load();
    REQUIRE(missing.has_value());
    CHECK(missing->queue_name == "default");
    CHECK(missing->volume == 1.0f);

    REQUIRE(store.save(sampleEngineState()).has_value());
    JsonPlaybackStateStore reopened(sidecar);
    auto loaded = reopened.load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->queue_name == "chill");
    CHECK(loaded->cursor_pos == 3);
    CHECK(loaded->current_path == "music/current.flac");
    REQUIRE(loaded->current_track_id.has_value());
    CHECK(*loaded->current_track_id == 42);
    CHECK(loaded->shuffle_perm == std::vector<int64_t>{2, 0, 1});
    CHECK(loaded->shuffle_enabled);
    CHECK(loaded->repeat_mode == kanariv::types::RepeatMode::All);
    CHECK(loaded->volume > 0.24f);
    CHECK(loaded->volume < 0.26f);
    CHECK(loaded->pre_mute_volume > 0.74f);
    CHECK(loaded->pre_mute_volume < 0.76f);
    CHECK(loaded->position_ms == 12345);

    // The document on disk is the versioned JSON format, not a default blob.
    const auto doc = readStateSidecar(dbPath);
    REQUIRE(doc.has_value());
    CHECK((*doc)["version"].get<int>() == 1);
    CHECK((*doc)["repeat"].get<std::string>() == "all");
}

TEST_CASE("JsonPlaybackStateStore keeps state in memory without a path", "[db][state_store]") {
    JsonPlaybackStateStore store;
    CHECK(store.path().empty());
    auto initial = store.load();
    REQUIRE(initial.has_value());
    CHECK(initial->volume == 1.0f);
    REQUIRE(store.save(sampleEngineState()).has_value());
    auto loaded = store.load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->queue_name == "chill");
    CHECK(loaded->position_ms == 12345);
}

TEST_CASE("JsonPlaybackStateStore ignores unknown document versions", "[db][state_store]") {
    const std::string dbPath = tempDbPath("state_store_version").string();
    {
        std::ofstream out(dbPath + ".state.json", std::ios::binary);
        out << R"({"version":99,"volume":0.1,"queue_name":"future"})";
    }
    JsonPlaybackStateStore store(dbPath + ".state.json");
    auto loaded = store.load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->queue_name == "default");
    CHECK(loaded->volume == 1.0f);
}

TEST_CASE("JsonPlaybackStateStore recovers from the previous generation", "[db][state_store]") {
    const std::string dbPath = tempDbPath("state_store_prev").string();
    const std::string sidecar = dbPath + ".state.json";
    JsonPlaybackStateStore store(sidecar);
    kanariv::types::EngineState first = sampleEngineState();
    first.volume = 0.2f;
    REQUIRE(store.save(first).has_value());
    kanariv::types::EngineState second = sampleEngineState();
    second.volume = 0.9f;
    REQUIRE(store.save(second).has_value()); // rotates `first` into `.prev`
    {
        std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
        out << "{ broken";
    }
    auto recovered = store.load();
    REQUIRE(recovered.has_value());
    CHECK(recovered->volume > 0.19f);
    CHECK(recovered->volume < 0.21f);
}

TEST_CASE("makeStorage imports the legacy engine_state row once", "[db][state_store][migration]") {
    const std::string dbPath = tempDbPath("state_import").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::shared_ptr<Database>(std::move(dbRes.value()));

    auto trackId = db->insertTrack(makeTrack(7, "legacy/current.flac"));
    REQUIRE(trackId.has_value());
    auto qid = db->createQueue("chill");
    REQUIRE(qid.has_value());

    // Simulate a pre-sidecar database: migrations no longer create the table.
    const int64_t perm[2] = {1, 2};
    {
        sqlite3* h = nullptr;
        REQUIRE(sqlite3_open(dbPath.c_str(), &h) == SQLITE_OK);
        REQUIRE(sqlite3_exec(h,
                             "CREATE TABLE engine_state(id INTEGER PRIMARY KEY, "
                             "active_queue_id INTEGER, cursor_pos INTEGER, shuffle_enabled INTEGER, "
                             "repeat_mode INTEGER, volume REAL, pre_mute_volume REAL, "
                             "position_ms INTEGER, shuffle_perm BLOB, current_track_id INTEGER)",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_stmt* st = nullptr;
        REQUIRE(sqlite3_prepare_v2(
                    h,
                    "INSERT INTO engine_state VALUES(1,?,5,1,1,0.33,0.5,12000,?,?)", -1, &st,
                    nullptr) == SQLITE_OK);
        sqlite3_bind_int64(st, 1, *qid);
        sqlite3_bind_blob(st, 2, perm, static_cast<int>(sizeof perm), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, *trackId);
        REQUIRE(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
        sqlite3_close(h);
    }

    auto storage = makeStorage(db);
    REQUIRE(storage.state != nullptr);
    auto imported = storage.state->load();
    REQUIRE(imported.has_value());
    CHECK(imported->queue_name == "chill");
    CHECK(imported->cursor_pos == 5);
    CHECK(imported->shuffle_enabled);
    CHECK(imported->repeat_mode == kanariv::types::RepeatMode::All);
    CHECK(imported->volume > 0.32f);
    CHECK(imported->volume < 0.34f);
    CHECK(imported->position_ms == 12000);
    CHECK(imported->shuffle_perm == std::vector<int64_t>{1, 2});
    REQUIRE(imported->current_track_id.has_value());
    CHECK(*imported->current_track_id == *trackId);
    CHECK(imported->current_path == "legacy/current.flac");
    CHECK(readStateSidecar(dbPath).has_value());

    // The sidecar now exists: a later composition must not re-import the row.
    {
        sqlite3* h = nullptr;
        REQUIRE(sqlite3_open(dbPath.c_str(), &h) == SQLITE_OK);
        REQUIRE(sqlite3_exec(h, "UPDATE engine_state SET cursor_pos=9", nullptr, nullptr,
                             nullptr) == SQLITE_OK);
        sqlite3_close(h);
    }
    auto again = makeStorage(db);
    REQUIRE(again.state != nullptr);
    auto reloaded = again.state->load();
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->cursor_pos == 5);
}
