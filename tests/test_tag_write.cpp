#include <catch2/catch_test_macros.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/utils/error.hpp>
#include <common.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "../src/service/tag_writer.hpp"

using namespace kanariv::service::detail;
using kanariv::types::Track;

namespace {

std::filesystem::path copyFixture(const std::string& name) {
    auto dir = kanariv::test_helpers::tempDirPath("tagwrite");
    std::filesystem::create_directories(dir);
    auto dst = dir / name;
    std::filesystem::copy_file(std::filesystem::path(std::string(TEST_DATA_DIR)) / name, dst,
                               std::filesystem::copy_options::overwrite_existing);
    return dst;
}

kanariv::types::Track taggedRow(const std::filesystem::path& p) {
    kanariv::types::Track t;
    t.path = p.generic_string();
    t.title = "TagTitle";
    t.artist = "TagArtist";
    t.album = "TagAlbum";
    t.album_artist = "TagAlbumArtist";
    t.genre = "TagGenre";
    t.year = 1999;
    t.track_num = 7;
    t.disc_num = 2;
    return t;
}

void requireRoundTrip(const std::string& fixture) {
    auto p = copyFixture(fixture);
    kanariv::types::Track t = taggedRow(p);
    auto w = writeFileTags(p, t);
    REQUIRE(w.has_value());
    auto meta = kanariv::media::extractMetadata(p.generic_string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "TagTitle");
    REQUIRE(meta->artist == "TagArtist");
    REQUIRE(meta->album == "TagAlbum");
    REQUIRE(meta->album_artist == "TagAlbumArtist");
    REQUIRE(meta->genre == "TagGenre");
    REQUIRE(meta->year == 1999);
    REQUIRE(meta->track_num == 7);
    REQUIRE(meta->disc_num == 2);
}

} // namespace

TEST_CASE("tag write gate allows writable formats", "[tag_write]") {
    for (const char* ext : {".mp3", ".MP3", ".flac", ".m4a", ".mp4", ".ogg", ".opus", ".wma",
                            ".asf", ".wav", ".aiff", ".aif", ".ape", ".mpc", ".wv"}) {
        std::filesystem::path p = std::string("song") + ext;
        REQUIRE(canWriteFileTags(p));
    }
    for (const char* name : {"song.txt", "song.tak", "song.tta", "song.xyz", "song"}) {
        REQUIRE(!canWriteFileTags(std::filesystem::path(name)));
    }
}

TEST_CASE("tag write round-trips mp3", "[tag_write]") {
    requireRoundTrip("sample.mp3");
}

TEST_CASE("tag write round-trips flac", "[tag_write]") {
    requireRoundTrip("sample.flac");
}

TEST_CASE("tag write round-trips m4a", "[tag_write]") {
    requireRoundTrip("sample.m4a");
}

TEST_CASE("tag write round-trips ogg", "[tag_write]") {
    requireRoundTrip("sample.ogg");
}

TEST_CASE("tag write round-trips opus", "[tag_write]") {
    requireRoundTrip("sample.opus");
}

TEST_CASE("tag write round-trips wma", "[tag_write]") {
    requireRoundTrip("sample.wma");
}

TEST_CASE("tag write round-trips wav", "[tag_write]") {
    requireRoundTrip("sample.wav");
}

TEST_CASE("tag write round-trips aiff", "[tag_write]") {
    requireRoundTrip("sample.aiff");
}

TEST_CASE("tag write round-trips wavpack", "[tag_write]") {
    requireRoundTrip("sample.wv");
}

TEST_CASE("tag write rejects unsupported format", "[tag_write]") {
    kanariv::types::Track t;
    t.path = "song.tak";
    auto w = writeFileTags(t.path, t);
    REQUIRE(!w.has_value());
    REQUIRE(w.error().code == kanariv::utils::StatusCode::InvalidArg);
}

TEST_CASE("reregister keeps id and refreshes row", "[tag_write]") {
    auto p = copyFixture("sample.mp3");
    auto dbRes = kanariv::db::Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto& db = *dbRes.value();
    kanariv::types::Track t = taggedRow(p);
    auto ins = db.insertTrack(t);
    REQUIRE(ins.has_value());
    t.id = *ins;
    auto fpBefore = t.fingerprint;
    REQUIRE(writeFileTags(p, t).has_value());
    auto reg = reregisterTrackRow(db, t);
    REQUIRE(reg.has_value());
    REQUIRE(reg->id == t.id);
    REQUIRE(reg->title == "TagTitle");
    REQUIRE(reg->album_artist == "TagAlbumArtist");
    REQUIRE(reg->fingerprint != fpBefore);
    auto back = db.getTrack(t.id);
    REQUIRE(back.has_value());
    REQUIRE(back->title == "TagTitle");
}
