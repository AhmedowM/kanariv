#include <sqlite3.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <db/database_access.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/db/smart.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::utils;

static kanariv::types::Track makeTrack(int idx, const std::string& pathOverride = "") {
    kanariv::types::Track t;
    t.path = pathOverride.empty() ? ("/tmp/track_" + std::to_string(idx) + ".mp3") : pathOverride;
    t.title = "Title" + std::to_string(idx);
    t.artist = "Artist" + std::to_string(idx);
    t.album = "Album" + std::to_string(idx);
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(idx * 37 + b * 7);
    t.duration = 1.0 + idx;
    t.library_id = 1;
    return t;
}

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
        t3.fingerprint[b] = static_cast<uint8_t>(0xFF - b);
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
    auto l1 = db->listTracks(&q);
    REQUIRE(l1.has_value());
    REQUIRE(l1->size() == 2);
    q.offset = 2;
    auto l2 = db->listTracks(&q);
    REQUIRE(l2.has_value());
    REQUIRE(l2->size() == 2);
    REQUIRE((*l2)[0].id != (*l1)[0].id);
    q.offset = 4;
    q.limit = 2;
    auto l3 = db->listTracks(&q);
    REQUIRE(l3.has_value());
    REQUIRE(l3->size() == 1);
    q.offset = 10;
    auto l4 = db->listTracks(&q);
    REQUIRE(l4.has_value());
    REQUIRE(l4->empty());
    // limit 0 means no limit (all)
    q.limit = 0;
    q.offset = 0;
    auto l5 = db->listTracks(&q);
    REQUIRE(l5.has_value());
    REQUIRE(l5->size() == 5);
    // offset only (no limit)
    q.limit = 0;
    q.offset = 3;
    auto l6 = db->listTracks(&q);
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
    auto r5 = db->listTracks(&q);
    REQUIRE(r5.has_value());
    REQUIRE(r5->size() == 2);
    for (auto& t : *r5)
        REQUIRE(t.rating == 5);

    // Year range inclusive.
    q = kanariv::types::TrackQuery{};
    q.year_min = 1980;
    q.year_max = 2005;
    auto ry = db->listTracks(&q);
    REQUIRE(ry.has_value());
    REQUIRE(ry->size() == 2);
    for (auto& t : *ry) {
        REQUIRE(t.year >= 1980);
        REQUIRE(t.year <= 2005);
    }

    // One-sided year max only.
    q = kanariv::types::TrackQuery{};
    q.year_max = 1975;
    auto ry2 = db->listTracks(&q);
    REQUIRE(ry2.has_value());
    REQUIRE(ry2->size() == 1);
    REQUIRE((*ry2)[0].year == 1970);

    // Play count range.
    q = kanariv::types::TrackQuery{};
    q.play_count_min = 10;
    q.play_count_max = 200;
    auto rp = db->listTracks(&q);
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
    auto rd = db->listTracks(&q);
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
    auto rc = db->listTracks(&q);
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
    auto list = db->listHistory(&q);
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 3);
    REQUIRE((*list)[0].completion_pct == 100.0);
    REQUIRE((*list)[1].completion_pct == 50.0);
    REQUIRE((*list)[2].completion_pct == 0.0);
}
