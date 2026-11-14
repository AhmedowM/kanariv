#include <catch2/catch_test_macros.hpp>
#include <common.hpp>
#include <cstddef>
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
        t.fingerprint[b] = (uint8_t)b;
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
    t.dirty = true;
    for (int b = 0; b < 32; ++b)
        t.fingerprint[b] = (uint8_t)(0x11 + b);
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
    // dirty is exported as a JSON boolean and must survive the import.
    REQUIRE((*tracks)[0].dirty);
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
        t.fingerprint[b] = (uint8_t)(0x22 + b);
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

TEST_CASE("track JSON roundtrip preserves every field", "[db_json]") {
    kanariv::types::Track t;
    t.id = 7;
    t.size = 1001;
    t.mtime = 1002;
    t.duration = 123.25;
    t.replayGainTrack = -6.5;
    t.replayGainAlbum = -5.25;
    t.sample_rate = 48000;
    t.channels = 2;
    t.bitrate = 320000;
    t.year = 1999;
    t.track_num = 5;
    t.disc_num = 2;
    t.rating = 4;
    t.play_count = 13;
    t.last_played = 14;
    t.date_added = 15;
    t.last_scanned = 16;
    t.dirty = true;
    t.library_id = 3;
    t.deleted_at = 17;
    t.path = "/music/roundtrip.mp3";
    t.title = "Title X";
    t.artist = "Artist X";
    t.album = "Album X";
    t.album_artist = "Album Artist X";
    t.genre = "Genre X";
    t.cover_art_path = "/covers/x.jpg";
    for (int b = 0; b < 32; ++b)
        t.fingerprint[static_cast<std::size_t>(b)] = static_cast<uint8_t>(0x40 + b);
    t.source.kind = kanariv::types::SourceKind::external;
    t.source.key = "content://track/7";
    t.source.display_name = "Display X";
    t.source.available = false;

    const Json j = kanariv::utils::trackToJson(t);
    REQUIRE(j["dirty"].isBoolean());
    REQUIRE(j["replay_gain_track"].get<double>() == t.replayGainTrack);
    REQUIRE(j["replay_gain_album"].get<double>() == t.replayGainAlbum);

    auto parsed = kanariv::utils::trackFromJson(j);
    REQUIRE(parsed.has_value());
    const auto& r = *parsed;
    REQUIRE(r.id == t.id);
    REQUIRE(r.size == t.size);
    REQUIRE(r.mtime == t.mtime);
    REQUIRE(r.duration == t.duration);
    REQUIRE(r.replayGainTrack == t.replayGainTrack);
    REQUIRE(r.replayGainAlbum == t.replayGainAlbum);
    REQUIRE(r.sample_rate == t.sample_rate);
    REQUIRE(r.channels == t.channels);
    REQUIRE(r.bitrate == t.bitrate);
    REQUIRE(r.year == t.year);
    REQUIRE(r.track_num == t.track_num);
    REQUIRE(r.disc_num == t.disc_num);
    REQUIRE(r.rating == t.rating);
    REQUIRE(r.play_count == t.play_count);
    REQUIRE(r.last_played == t.last_played);
    REQUIRE(r.date_added == t.date_added);
    REQUIRE(r.last_scanned == t.last_scanned);
    REQUIRE(r.dirty == t.dirty);
    REQUIRE(r.library_id == t.library_id);
    REQUIRE(r.deleted_at == t.deleted_at);
    REQUIRE(r.path == t.path);
    REQUIRE(r.title == t.title);
    REQUIRE(r.artist == t.artist);
    REQUIRE(r.album == t.album);
    REQUIRE(r.album_artist == t.album_artist);
    REQUIRE(r.genre == t.genre);
    REQUIRE(r.cover_art_path == t.cover_art_path);
    REQUIRE(r.fingerprint == t.fingerprint);
    REQUIRE(r.source.kind == t.source.kind);
    REQUIRE(r.source.key == t.source.key);
    REQUIRE(r.source.display_name == t.source.display_name);
    REQUIRE(r.source.available == t.source.available);
}

TEST_CASE("trackFromJson accepts legacy integer dirty and missing gains", "[db_json]") {
    Json j;
    j["path"] = "/tmp/legacy.mp3";
    j["library_id"] = 1;
    j["dirty"] = 1;
    auto dirtyOne = kanariv::utils::trackFromJson(j);
    REQUIRE(dirtyOne.has_value());
    REQUIRE(dirtyOne->dirty);
    j["dirty"] = 0;
    auto dirtyZero = kanariv::utils::trackFromJson(j);
    REQUIRE(dirtyZero.has_value());
    REQUIRE(!dirtyZero->dirty);
    j["dirty"] = true;
    auto dirtyBool = kanariv::utils::trackFromJson(j);
    REQUIRE(dirtyBool.has_value());
    REQUIRE(dirtyBool->dirty);
    // Older exports have no ReplayGain keys: both default to 0 dB.
    REQUIRE(dirtyBool->replayGainTrack == 0.0);
    REQUIRE(dirtyBool->replayGainAlbum == 0.0);
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
