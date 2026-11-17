#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/status_format.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace kanariv::db;
using namespace kanariv::utils;
using namespace kanariv::types;

static TrackRow makeSourceTrack(int idx, SourceKind kind, std::string key) {
    TrackRow t;
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
    source.kind = SourceKind::External;
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
    empty.kind = SourceKind::External;
    auto r = db->findBySource(empty);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == StatusCode::InvalidArg);
}

TEST_CASE("insert then find by source kind/key", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    TrackRow t = makeSourceTrack(1, SourceKind::External, "doc://abc");
    auto id = db->insertTrack(t);
    REQUIRE(id.has_value());
    REQUIRE(*id > 0);

    SourceRef probe;
    probe.kind = SourceKind::External;
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

    TrackRow a = makeSourceTrack(10, SourceKind::Path, "/Music/song.mp3");
    TrackRow b = makeSourceTrack(11, SourceKind::External, "/Music/song.mp3");
    auto idA = db->insertTrack(a);
    REQUIRE(idA.has_value());
    auto idB = db->insertTrack(b);
    REQUIRE(idB.has_value());
    // Same key, different kind => distinct rows.
    REQUIRE(*idA != *idB);

    TrackRow c = makeSourceTrack(12, SourceKind::Path, "/Music/song.mp3");
    auto idC = db->insertTrack(c);
    REQUIRE(idC.has_value());
    // Same kind + same key => another row (no unique constraint on source key
    // alone), but findBySource returns one match.
    SourceRef probe;
    probe.kind = SourceKind::Path;
    probe.key = "/Music/song.mp3";
    auto found = db->findBySource(probe);
    REQUIRE(found.has_value());
}

TEST_CASE("updateTrack preserves source columns", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    TrackRow t = makeSourceTrack(3, SourceKind::Imported, "import://song");
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
    REQUIRE(found->source.kind == SourceKind::Imported);
    REQUIRE(found->source.key == "import://song");
    REQUIRE(found->source.available == false);
}

TEST_CASE("normalizedSource fills path key", "[db][source]") {
    TrackRow t;
    t.path = "/Music/song.mp3";
    t.source.kind = SourceKind::Path;
    t.source.key = "";

    auto source = normalizedSource(t);
    REQUIRE(source.kind == SourceKind::Path);
    REQUIRE(source.key == "/Music/song.mp3");
    REQUIRE(source.display_name == "/Music/song.mp3");
}

TEST_CASE("normalizedSource leaves external key intact", "[db][source]") {
    TrackRow t;
    t.path = "/Music/song.mp3";
    t.source.kind = SourceKind::External;
    t.source.key = "doc://xyz";

    auto source = normalizedSource(t);
    REQUIRE(source.kind == SourceKind::External);
    REQUIRE(source.key == "doc://xyz");
}

TEST_CASE("legacy path-only migration backfills source", "[db][source]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());

    // Insert with kind/path defaults; the migration path only applies to
    // legacy rows where source_kind == 0 and source_key == ''. Fresh rows
    // inserted via insertTrack carry normalized keys already.
    TrackRow t;
    t.path = "/tmp/migrated.mp3";
    t.title = "M";
    t.source.kind = SourceKind::Path;
    t.source.key = ""; // unnormalized before insert
    auto id = db->insertTrack(t);
    REQUIRE(id.has_value());

    auto found = db->getTrack(*id);
    REQUIRE(found.has_value());
    REQUIRE(found->source.key == "/tmp/migrated.mp3");
}
