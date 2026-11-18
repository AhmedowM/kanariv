#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <kanariv/db/database.hpp>
#include <kanariv/library/analyze.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/record.hpp>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using kanariv::library::analyzeLibrary;
using kanariv::library::AnalyzeOptions;
using kanariv::test_helpers::tempDirPath;

namespace {

/// Minimal 16-bit PCM stereo WAV holding a 1 kHz sine, decodable by FFmpeg.
void writeSineWav(const std::filesystem::path& path, double amplitude, double seconds = 2.0) {
    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t channels = 2;
    constexpr uint16_t bits = 16;
    const auto frames = static_cast<uint32_t>(sampleRate * seconds);
    const uint32_t dataBytes = frames * channels * (bits / 8);
    std::vector<std::byte> wav(44 + dataBytes);
    const auto put32 = [&](std::size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            wav[off + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    };
    const auto put16 = [&](std::size_t off, uint16_t v) {
        wav[off] = static_cast<std::byte>(v & 0xFF);
        wav[off + 1] = static_cast<std::byte>((v >> 8) & 0xFF);
    };
    const auto tag = [&](std::size_t off, const char* s) {
        for (int i = 0; i < 4; ++i)
            wav[off + i] = static_cast<std::byte>(s[i]);
    };
    tag(0, "RIFF");
    put32(4, 36 + dataBytes);
    tag(8, "WAVE");
    tag(12, "fmt ");
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, channels);
    put32(24, sampleRate);
    put32(28, sampleRate * channels * (bits / 8));
    put16(32, static_cast<uint16_t>(channels * (bits / 8)));
    put16(34, bits);
    tag(36, "data");
    put32(40, dataBytes);
    for (uint32_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * std::numbers::pi * 1000.0 * i / sampleRate;
        const auto sample = static_cast<int16_t>(std::sin(phase) * amplitude * 32767.0);
        put16(44 + (i * channels + 0) * 2, static_cast<uint16_t>(sample));
        put16(44 + (i * channels + 1) * 2, static_cast<uint16_t>(sample));
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
}

std::shared_ptr<kanariv::db::Database> openDb() {
    auto db = kanariv::db::Database::open(":memory:");
    REQUIRE(db.has_value());
    return std::move(*db);
}

int64_t insertTrack(kanariv::db::Database& db, const std::filesystem::path& path, uint8_t fpByte,
                    std::string album, std::string artist = "Band", std::string albumArtist = {}) {
    kanariv::types::TrackRow t;
    t.path = path.string();
    t.album = std::move(album);
    t.artist = std::move(artist);
    t.album_artist = std::move(albumArtist);
    t.fingerprint.fill(fpByte);
    auto id = db.insertTrack(t);
    REQUIRE(id.has_value());
    return *id;
}

} // namespace

TEST_CASE("analyze measures a stereo sine and persists track and album gain", "[analyze]") {
    auto dir = tempDirPath("analyze_single");
    const auto wav = dir / "tone.wav";
    writeSineWav(wav, 0.1);

    auto db = openDb();
    const auto id = insertTrack(*db, wav, 0x11, "Album A");

    auto report = analyzeLibrary(*db);
    REQUIRE(report.has_value());
    CHECK(report->tracks == 1);
    CHECK(report->analyzed == 1);
    CHECK(report->failed == 0);
    CHECK(report->skipped == 0);

    auto row = db->getTrack(id);
    REQUIRE(row.has_value());
    // Stereo 1 kHz sine at amplitude 0.1 reads -20 LUFS (K-weighting cancels
    // the BS.1770 offset), so the RG2 gain against -18 LUFS is +2 dB.
    CHECK_THAT(row->replayGainTrack, WithinAbs(2.0, 0.05));
    CHECK_THAT(row->replayGainAlbum, WithinAbs(2.0, 0.05));

    // A second pass leaves the measured track alone...
    auto again = analyzeLibrary(*db);
    REQUIRE(again.has_value());
    CHECK(again->analyzed == 0);
    CHECK(again->skipped == 1);

    // ...unless the caller asks for a re-measurement.
    auto forced = analyzeLibrary(*db, 0, AnalyzeOptions{.reanalyze = true});
    REQUIRE(forced.has_value());
    CHECK(forced->analyzed == 1);
}

TEST_CASE("album gain integrates the whole album, not the per-track average", "[analyze]") {
    auto dir = tempDirPath("analyze_album");
    const auto loud = dir / "loud.wav";
    const auto quiet = dir / "quiet.wav";
    const auto other = dir / "other.wav";
    writeSineWav(loud, 0.1);
    writeSineWav(quiet, 0.001);
    writeSineWav(other, 0.1);

    auto db = openDb();
    const auto loudId = insertTrack(*db, loud, 0x21, "Same Album");
    const auto quietId = insertTrack(*db, quiet, 0x22, "Same Album");
    const auto otherId = insertTrack(*db, other, 0x23, "Other Album");

    auto report = analyzeLibrary(*db);
    REQUIRE(report.has_value());
    CHECK(report->analyzed == 3);

    auto loudRow = db->getTrack(loudId);
    auto quietRow = db->getTrack(quietId);
    auto otherRow = db->getTrack(otherId);
    REQUIRE(loudRow.has_value());
    REQUIRE(quietRow.has_value());
    REQUIRE(otherRow.has_value());

    CHECK_THAT(loudRow->replayGainTrack, WithinAbs(2.0, 0.05));
    CHECK_THAT(quietRow->replayGainTrack, WithinAbs(42.0, 0.5));
    // The quiet track's blocks fall below the album's relative gate, so the
    // album integrates to the loud track's loudness -- both members must carry
    // that same album gain, not their own track gain.
    CHECK_THAT(loudRow->replayGainAlbum, WithinAbs(2.0, 0.05));
    CHECK_THAT(quietRow->replayGainAlbum, WithinAbs(2.0, 0.05));
    CHECK_THAT(otherRow->replayGainAlbum, WithinAbs(2.0, 0.05));
    CHECK(quietRow->replayGainAlbum != quietRow->replayGainTrack);
}

TEST_CASE("a track without an album tag gains its album value from itself", "[analyze]") {
    auto dir = tempDirPath("analyze_noalbum");
    const auto wav = dir / "loose.wav";
    writeSineWav(wav, 0.1);

    auto db = openDb();
    const auto id = insertTrack(*db, wav, 0x31, "");

    auto report = analyzeLibrary(*db);
    REQUIRE(report.has_value());
    CHECK(report->analyzed == 1);

    auto row = db->getTrack(id);
    REQUIRE(row.has_value());
    CHECK_THAT(row->replayGainTrack, WithinAbs(2.0, 0.05));
    CHECK(row->replayGainAlbum == row->replayGainTrack);
}

TEST_CASE("an undecodable track fails alone and keeps its album unmeasured", "[analyze]") {
    auto dir = tempDirPath("analyze_fail");
    const auto good = dir / "good.wav";
    writeSineWav(good, 0.1);

    auto db = openDb();
    const auto goodId = insertTrack(*db, good, 0x41, "Mixed Album");
    const auto badId = insertTrack(*db, dir / "missing.wav", 0x42, "Mixed Album");

    auto report = analyzeLibrary(*db);
    REQUIRE(report.has_value());
    CHECK(report->analyzed == 1);
    CHECK(report->failed == 1);

    auto goodRow = db->getTrack(goodId);
    auto badRow = db->getTrack(badId);
    REQUIRE(goodRow.has_value());
    REQUIRE(badRow.has_value());
    CHECK_THAT(goodRow->replayGainTrack, WithinAbs(2.0, 0.05));
    // The group is incomplete: persisting an album gain now would describe only
    // half the album, so it stays unmeasured (0) and the next pass redoes it.
    CHECK(goodRow->replayGainAlbum == 0.0);
    CHECK(badRow->replayGainTrack == 0.0);
    CHECK(badRow->replayGainAlbum == 0.0);
}
