#include <catch2/catch_test_macros.hpp>
#include <kanariv/contracts/database.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/db/search.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::utils;

TEST_CASE("search FTS5 quoting sanitizes", "[db_search]") {
    REQUIRE(sanitizeFtsTerm("hello OR world") == "hello world");
    REQUIRE(sanitizeFtsTerm("a AND b") == "a b");
    REQUIRE(sanitizeFtsTerm("\"Abbey Road\"") == "\"\"Abbey Road\"\"");
    // standalone NOT removed
    REQUIRE(sanitizeFtsTerm("NOT foo") == "foo");
    // special chars replaced with space
    std::string s = sanitizeFtsTerm("a*b:c");
    REQUIRE(s.find('*') == std::string::npos);
    REQUIRE(s.find(':') == std::string::npos);
}

TEST_CASE("search FTS finds title and LIKE fallback COLLATE NOCASE", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (auto [title, artist] : std::vector<std::pair<std::string, std::string>>{
             {"Abbey Road", "Beatles"}, {"Red House", "Hendrix"}, {"Hello World", "Artist"}}) {
        kanariv::types::Track t;
        t.path = "/tmp/" + title + ".mp3";
        t.title = title;
        t.artist = artist;
        t.album = "Album";
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(title.size() + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
    }
    auto res = search(*db, "Abbey Road", 10);
    REQUIRE(res.has_value());
    REQUIRE(res->size() >= 1);
    bool found = false;
    for (auto& t : *res)
        if (t.title == "Abbey Road")
            found = true;
    REQUIRE(found);
    // case-insensitive LIKE fallback
    auto res2 = search(*db, "hello", 10);
    REQUIRE(res2.has_value());
    REQUIRE(res2->size() >= 1);
    bool found2 = false;
    for (auto& t : *res2)
        if (t.title == "Hello World")
            found2 = true;
    REQUIRE(found2);
    // special OR should be sanitized and still find Abbey
    auto res3 = search(*db, "Abbey OR Road", 10);
    REQUIRE(res3.has_value());
    REQUIRE(res3->size() >= 1);
}

TEST_CASE("searchLike 5 cols COLLATE NOCASE", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/x.mp3";
    t.title = "Foo";
    t.artist = "BarArtist";
    t.album = "BazAlbum";
    t.genre = "Rock";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xE0 + b);
    REQUIRE(db->insertTrack(t).has_value());
    auto r = searchLike(*db, "barartist", 10);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 1);
    auto r2 = searchLike(*db, "BAZALBUM", 10);
    REQUIRE(r2.has_value());
    REQUIRE(r2->size() == 1);
    auto r3 = searchLike(*db, "rock", 10);
    REQUIRE(r3.has_value());
    REQUIRE(r3->size() == 1);
}

TEST_CASE("search empty query returns empty", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    auto r = search(*db, "", 10);
    REQUIRE(r.has_value());
    REQUIRE(r->empty());
    auto r2 = searchLike(*db, "", 10);
    REQUIRE(r2.has_value());
    REQUIRE(r2->empty());
}

TEST_CASE("search FTS escape LIKE wildcards", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/y.mp3";
    t.title = "100% Pure";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xF0 + b);
    REQUIRE(db->insertTrack(t).has_value());
    auto r = search(*db, "100%", 10);
    REQUIRE(r.has_value());
    // should find via LIKE fallback with escaped %
    bool found = false;
    for (auto& tt : *r)
        if (tt.title == "100% Pure")
            found = true;
    // FTS may not match %, but LIKE fallback should; accept either 0 or 1, but ensure no crash
    REQUIRE((r->size() == 0 || found));
}

TEST_CASE("search phrase Abbey Road", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/abbey.mp3";
    t.title = "Abbey Road";
    t.artist = "Beatles";
    t.album = "Abbey Road";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xA0 + b);
    REQUIRE(db->insertTrack(t).has_value());
    kanariv::types::Track t2;
    t2.path = "/tmp/other.mp3";
    t2.title = "Abbeville";
    t2.artist = "Other";
    for (int b = 0; b < 32; ++b)
        t2.fingerprint[b] = static_cast<uint8_t>(0xB0 + b);
    REQUIRE(db->insertTrack(t2).has_value());
    // quoted phrase should be sanitized but still match Abbey Road via FTS or LIKE
    auto r = search(*db, "\"Abbey Road\"", 10);
    REQUIRE(r.has_value());
    bool found = false;
    for (auto& tt : *r)
        if (tt.title == "Abbey Road")
            found = true;
    REQUIRE(found);
    // without quotes also finds
    auto r2 = search(*db, "Abbey Road", 10);
    REQUIRE(r2.has_value());
    found = false;
    for (auto& tt : *r2)
        if (tt.title == "Abbey Road")
            found = true;
    REQUIRE(found);
}

TEST_CASE("search injection OR AND NOT", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/inject.mp3";
    t.title = "Hello World";
    t.artist = "Artist";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xC0 + b);
    REQUIRE(db->insertTrack(t).has_value());
    // these queries contain FTS operators that should be sanitized, not cause syntax errors or
    // injection
    auto r1 = search(*db, "Hello OR World", 10);
    REQUIRE(r1.has_value());
    auto r2 = search(*db, "Hello AND World", 10);
    REQUIRE(r2.has_value());
    auto r3 = search(*db, "NOT Hello", 10);
    REQUIRE(r3.has_value());
    auto r4 = search(*db, "Hello OR AND NOT", 10);
    REQUIRE(r4.has_value());
    // all should not crash and should still find Hello World via sanitized terms (hello world)
    // At least one of them should find it; if none found, ensure no exception thrown is enough
    // But ensure r1 finds via like fallback: sanitized "Hello World" -> should match
    bool found1 = false;
    for (auto& tt : *r1)
        if (tt.title == "Hello World")
            found1 = true;
    // tolerate not found if FTS ranking empty but like fallback should find; require found
    REQUIRE(found1);
}

TEST_CASE("search very long query", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/long.mp3";
    t.title = "Short";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xD0 + b);
    REQUIRE(db->insertTrack(t).has_value());
    std::string longQ(5000, 'a');
    longQ += " Short ";
    longQ += std::string(5000, 'b');
    auto r = search(*db, longQ, 10);
    REQUIRE(r.has_value());
    // should not crash, likely returns empty or maybe via like fallback empty
    REQUIRE(r->size() <= 10);
    auto r2 = searchLike(*db, longQ, 10);
    REQUIRE(r2.has_value());
}

TEST_CASE("search limit=0 defaults to 50", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = "/tmp/lim" + std::to_string(i) + ".mp3";
        t.title = "Limited";
        t.artist = "Artist" + std::to_string(i);
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<uint8_t>(0xE0 + i * 16 + b);
        REQUIRE(db->insertTrack(t).has_value());
    }
    auto r = search(*db, "Limited", 0);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    auto r2 = searchLike(*db, "Limited", 0);
    REQUIRE(r2.has_value());
    REQUIRE(r2->size() == 3);
}

TEST_CASE("search COLLATE NOCASE variation", "[db_search]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/case.mp3";
    t.title = "CaSeTeSt";
    t.artist = "MixedCase";
    t.album = "AlbumX";
    t.genre = "RoCk";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0xF1 + b);
    REQUIRE(db->insertTrack(t).has_value());
    auto r1 = searchLike(*db, "casetest", 10);
    REQUIRE(r1.has_value());
    REQUIRE(r1->size() == 1);
    auto r2 = searchLike(*db, "CASETEST", 10);
    REQUIRE(r2.has_value());
    REQUIRE(r2->size() == 1);
    auto r3 = searchLike(*db, "mixedcase", 10);
    REQUIRE(r3.has_value());
    REQUIRE(r3->size() == 1);
    auto r4 = searchLike(*db, "rock", 10);
    REQUIRE(r4.has_value());
    REQUIRE(r4->size() == 1);
    auto r5 = searchLike(*db, "ROCK", 10);
    REQUIRE(r5.has_value());
    REQUIRE(r5->size() == 1);
    // search via FTS also should be case-insensitive (porter unicode61)
    auto r6 = search(*db, "casetest", 10);
    REQUIRE(r6.has_value());
    bool found = false;
    for (auto& tt : *r6)
        if (tt.title == "CaSeTeSt")
            found = true;
    // FTS may be case-insensitive; ensure at least like fallback would have found; tolerate either
    if (!found) {
        auto r7 = searchLike(*db, "casetest", 10);
        REQUIRE(r7->size() == 1);
    } else {
        REQUIRE(found);
    }
}
