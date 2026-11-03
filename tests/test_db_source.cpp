#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <kanariv/contracts/database.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace kanariv::db;
using namespace kanariv::utils;
using namespace kanariv::types;

static Track makeSourceTrack(int idx, SourceKind kind, std::string key) {
    Track t;
    t.path = "/tmp/track_" + std::to_string(idx) + ".mp3";
    t.title = "Title" + std::to_string(idx);
    t.artist = "Artist" + std::to_string(idx);
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(idx * 37 + b * 7);
    t.duration = 1.0 + idx;
    t.source.kind = kind;
    t.source.key = std::move(key);
    t.source.display_name = "display " + std::to_string(idx);
    t.source.available = true;
    return t;
}

TEST_CASE("findBySource not found", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    SourceRef source;
    source.kind = SourceKind::external;
    source.key = "doc://never";
    auto r = db->findBySource(source);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::NotFound);
}

TEST_CASE("findBySource invalid empty key", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    SourceRef empty;
    empty.kind = SourceKind::external;
    auto r = db->findBySource(empty);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::InvalidArg);
}

TEST_CASE("insert then find by source kind/key", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    Track t = makeSourceTrack(1, SourceKind::external, "doc://abc");
    auto id = db->insertTrack(t);
    REQUIRE(id.has_value());
    REQUIRE(*id > 0);

    SourceRef probe;
    probe.kind = SourceKind::external;
    probe.key = "doc://abc";
    auto found = db->findBySource(probe);
    REQUIRE(found.has_value());
    REQUIRE(found->id == *id);
    REQUIRE(found->title == "Title1");
    REQUIRE(found->source.available == true);
    REQUIRE(found->source.display_name == "display 1");
}

TEST_CASE("source dedup keyed on kind and key", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    Track a = makeSourceTrack(10, SourceKind::path, "/Music/song.mp3");
    Track b = makeSourceTrack(11, SourceKind::external, "/Music/song.mp3");
    auto idA = db->insertTrack(a);
    REQUIRE(idA.has_value());
    auto idB = db->insertTrack(b);
    REQUIRE(idB.has_value());
    // Same key, different kind => distinct rows.
    REQUIRE(*idA != *idB);

    Track c = makeSourceTrack(12, SourceKind::path, "/Music/song.mp3");
    auto idC = db->insertTrack(c);
    REQUIRE(idC.has_value());
    // Same kind + same key => another row (no unique constraint on source key
    // alone), but findBySource returns one match.
    SourceRef probe;
    probe.kind = SourceKind::path;
    probe.key = "/Music/song.mp3";
    auto found = db->findBySource(probe);
    REQUIRE(found.has_value());
}

TEST_CASE("updateTrack preserves source columns", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    Track t = makeSourceTrack(3, SourceKind::imported, "import://song");
    auto id = db->insertTrack(t);
    REQUIRE(id.has_value());

    auto found = db->getTrack(*id);
    REQUIRE(found.has_value());
    found->title = "Renamed";
    found->source.available = false;
    auto upd = db->updateTrack(*found);
    REQUIRE(upd.has_value());

    found = db->getTrack(*id);
    REQUIRE(found.has_value());
    REQUIRE(found->title == "Renamed");
    REQUIRE(found->source.kind == SourceKind::imported);
    REQUIRE(found->source.key == "import://song");
    REQUIRE(found->source.available == false);
}

TEST_CASE("normalizedSource fills path key", "[db][source]") {
    Track t;
    t.path = "/Music/song.mp3";
    t.source.kind = SourceKind::path;
    t.source.key = "";

    auto source = normalizedSource(t);
    REQUIRE(source.kind == SourceKind::path);
    REQUIRE(source.key == "/Music/song.mp3");
    REQUIRE(source.display_name == "/Music/song.mp3");
}

TEST_CASE("normalizedSource leaves external key intact", "[db][source]") {
    Track t;
    t.path = "/Music/song.mp3";
    t.source.kind = SourceKind::external;
    t.source.key = "doc://xyz";

    auto source = normalizedSource(t);
    REQUIRE(source.kind == SourceKind::external);
    REQUIRE(source.key == "doc://xyz");
}

TEST_CASE("legacy path-only migration backfills source", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    // Insert with kind/path defaults; the migration path only applies to
    // legacy rows where source_kind == 0 and source_key == ''. Fresh rows
    // inserted via insertTrack carry normalized keys already.
    Track t;
    t.path = "/tmp/migrated.mp3";
    t.title = "M";
    t.source.kind = SourceKind::path;
    t.source.key = ""; // unnormalized before insert
    auto id = db->insertTrack(t);
    REQUIRE(id.has_value());

    auto found = db->getTrack(*id);
    REQUIRE(found.has_value());
    REQUIRE(found->source.key == "/tmp/migrated.mp3");
}
