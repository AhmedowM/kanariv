#include <catch2/catch_test_macros.hpp>
#include <common.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <kanariv/contracts/database.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/db/json.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <string>
#include <system_error>
#include <utility>
using namespace kanariv::db;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

TEST_CASE("trackToJson and trackFromJson roundtrip ordered", "[db_json]") {
    kanariv::types::Track t;
    t.id = 1;
    t.path = "/music/a.mp3";
    t.title = "Title";
    t.artist = "Artist";
    t.album = "Album";
    t.album_artist = "AA";
    t.genre = "Rock";
    t.year = 2020;
    t.track_num = 3;
    t.sample_rate = 44100;
    t.channels = 2;
    t.duration = 123.4;
    t.size = 9999;
    t.mtime = 1700000000;
    t.play_count = 5;
    t.rating = 4;
    t.library_id = 1;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(b);
    const auto j = kanariv::utils::trackToJson(t);
    REQUIRE(j.contains("title"));
    REQUIRE(j["title"].get<std::string>() == "Title");
    REQUIRE(j["fingerprint"].get<std::string>().size() == 64);
    // Json preserves insertion order; just check all fields present
    REQUIRE(j.contains("path"));
    REQUIRE(j.contains("artist"));
    auto t2Res = kanariv::utils::trackFromJson(j);
    REQUIRE(t2Res.has_value());
    auto t2 = *t2Res;
    REQUIRE(t2.title == t.title);
    REQUIRE(t2.artist == t.artist);
    REQUIRE(t2.album == t.album);
    REQUIRE(t2.fingerprint == t.fingerprint);
    REQUIRE(t2.path == t.path);
    REQUIRE(t2.duration == t.duration);
}

TEST_CASE("exportJson and importJson roundtrip", "[db_json]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/j.mp3";
    t.title = "JsonTrack";
    t.artist = "JsonArtist";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0x11 + b);
    auto ins = db->insertTrack(t);
    REQUIRE(ins.has_value());
    auto outPath = tempDbPath("json_export");
    outPath.replace_extension(".json");
    registerTempPath(outPath);
    auto er = exportJson(*db, outPath);
    REQUIRE(er.has_value());
    REQUIRE(std::filesystem::exists(outPath));
    // read file and check the tracks array made it to disk
    std::ifstream f(outPath);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("JsonTrack") != std::string::npos);
    f.close();
    // import into new db
    auto db2Res = Database::open(":memory:");
    REQUIRE(db2Res.has_value());
    auto db2 = std::move(db2Res.value());
    auto ir = importJson(*db2, outPath);
    REQUIRE(ir.has_value());
    auto tracks = db2->listTracks(nullptr);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    REQUIRE((*tracks)[0].title == "JsonTrack");
    {
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
    }
}

TEST_CASE("importJson handles duplicate fingerprint upsert", "[db_json]") {
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    kanariv::types::Track t;
    t.path = "/tmp/dup.mp3";
    t.title = "Dup";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = static_cast<uint8_t>(0x22 + b);
    REQUIRE(db->insertTrack(t).has_value());
    auto outPath = tempDbPath("json_dup");
    outPath.replace_extension(".json");
    registerTempPath(outPath);
    REQUIRE(exportJson(*db, outPath).has_value());
    // import same file again should not duplicate
    REQUIRE(importJson(*db, outPath).has_value());
    auto tracks = db->listTracks(nullptr);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    {
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
    }
}

TEST_CASE("trackFromJson invalid fingerprint falls back", "[db_json]") {
    Json j;
    j["path"] = "/tmp/f.mp3";
    j["title"] = "T";
    j["fingerprint"] = "zzzz"; // invalid hex length
    j["library_id"] = 1;
    auto r = kanariv::utils::trackFromJson(j);
    REQUIRE(r.has_value());
    // should have generated fallback fingerprint not empty
    bool allZero = true;
    for (auto b : r->fingerprint)
        if (b != 0)
            allZero = false;
    REQUIRE(!allZero);
}
