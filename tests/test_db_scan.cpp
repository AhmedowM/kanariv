#include <sqlite3.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <db/database_access.hpp>
#include <filesystem>
#include <fstream>
#include <ios>
#include <kanariv/db/database.hpp>
#include <kanariv/db/maintenance.hpp>
#include <kanariv/library/scan.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <media/fingerprint.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::utils;
using kanariv::library::Scanner;
using namespace kanariv::test_helpers;

namespace {

/// Minimal 16-bit PCM mono WAV with a sine tone, enough for FFmpeg to decode.
std::vector<std::byte> makeSineWav() {
    constexpr uint32_t sampleRate = 8000;
    constexpr uint32_t sampleCount = 4000; // 0.5 s
    constexpr uint16_t channels = 1;
    constexpr uint16_t bits = 16;
    const uint32_t dataBytes = sampleCount * channels * (bits / 8);
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
    for (uint32_t i = 0; i < sampleCount; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 * i / sampleRate;
        const auto sample = static_cast<int16_t>(std::sin(phase) * 12000.0);
        put16(44 + i * 2, static_cast<uint16_t>(sample));
    }
    return wav;
}

} // namespace

TEST_CASE("scan generator yields audio files recursively", "[db_scan]") {
    auto root = tempDirPath("scan_test");
    std::filesystem::create_directories(root / "sub");
    // create dummy audio files
    for (auto name : {root / "a.mp3", root / "sub" / "b.wav", root / "c.ogg"}) {
        std::ofstream f(name, std::ios::binary);
        f << "dummy content " << name.string();
    }
    std::ofstream f2(root / "ignore.txt", std::ios::binary);
    f2 << "not audio";
    auto tracks = kanariv::media::scanTracksToVector(root, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 3);
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
}

TEST_CASE("scan fingerprint head+tail+size sampled deterministic", "[db_scan]") {
    auto dir = tempDirPath("fp_test");
    auto p = dir / "file.wav";
    // create 100K file with known pattern
    std::vector<uint8_t> data(100 * 1024, 0xAB);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (uint8_t)(i & 0xFF);
    {
        std::ofstream f(p, std::ios::binary);
        f.write((char*)data.data(), data.size());
    }
    // Test via public API: scanTracksToVector returns tracks with fingerprints
    auto tracks1 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks1.has_value());
    REQUIRE(tracks1->size() == 1);
    auto fp1 = tracks1->front().fingerprint;

    // Re-scan same file -> same fingerprint
    auto tracks2 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks2.has_value());
    REQUIRE(tracks2->size() == 1);
    auto fp2 = tracks2->front().fingerprint;
    REQUIRE(fp1 == fp2);

    // same head different tail -> different fingerprint
    std::vector<uint8_t> data2 = data;
    // modify tail last 1K
    for (size_t i = 99 * 1024; i < 100 * 1024; ++i)
        data2[i] = 0xFF;
    auto p2 = dir / "file2.wav";
    {
        std::ofstream f(p2, std::ios::binary);
        f.write((char*)data2.data(), data2.size());
    }
    auto tracks3 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks3.has_value());
    REQUIRE(tracks3->size() == 2);
    bool foundDifferent = false;
    for (auto& t : *tracks3) {
        if (t.fingerprint != fp1) {
            foundDifferent = true;
            break;
        }
    }
    REQUIRE(foundDifferent);

    // size included: same head+tail but different size -> different
    auto p3 = dir / "file3.wav";
    std::vector<uint8_t> data3(data.begin(), data.begin() + 50 * 1024);
    {
        std::ofstream f(p3, std::ios::binary);
        f.write((char*)data3.data(), data3.size());
    }
    auto tracks4 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks4.has_value());
    REQUIRE(tracks4->size() == 3);
    foundDifferent = false;
    for (auto& t : *tracks4) {
        if (t.fingerprint != fp1) {
            foundDifferent = true;
            break;
        }
    }
    REQUIRE(foundDifferent);
    // fingerprint size is 32 bytes
    REQUIRE(fp1.size() == 32);
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

TEST_CASE("scanLibrary inserts and dedup by fingerprint", "[db_scan]") {
    auto dir = tempDirPath("scanlib");
    // create two audio files with distinct content
    {
        std::ofstream f1(dir / "x.mp3", std::ios::binary);
        std::vector<uint8_t> d1(10 * 1024, 0x42);
        for (size_t i = 0; i < d1.size(); ++i)
            d1[i] = (uint8_t)(i & 0xFF);
        f1.write((char*)d1.data(), d1.size());
    }
    {
        std::ofstream f2(dir / "y.wav", std::ios::binary);
        std::vector<uint8_t> d2(10 * 1024, 0x43);
        for (size_t i = 0; i < d2.size(); ++i)
            d2[i] = (uint8_t)((i + 1) & 0xFF);
        f2.write((char*)d2.data(), d2.size());
    }
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    std::shared_ptr<Database> db = std::move(dbRes.value());
    auto libIdRes = db->libraryAdd(dir.string(), "testlib");
    REQUIRE(libIdRes.has_value());
    int64_t libId = *libIdRes;
    Scanner scanner{db};
    auto sr = scanner.scanLibrary(libId);
    REQUIRE(sr.has_value());
    auto stats = db->getStats();
    REQUIRE(stats.has_value());
    REQUIRE(stats->num_tracks == 2);
    // second scan should dedup (same path+size+mtime -> skip) and not increase count
    auto sr2 = scanner.scanLibrary(libId);
    REQUIRE(sr2.has_value());
    auto stats2 = db->getStats();
    REQUIRE(stats2.has_value());
    REQUIRE(stats2->num_tracks == 2);
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

TEST_CASE("scanLibrary never persists an all-zero fingerprint or synthesised gain", "[db_scan]") {
    auto dir = tempDirPath("scanlib_fp");
    {
        std::ofstream f(dir / "tone.wav", std::ios::binary);
        const auto wav = makeSineWav();
        f.write(reinterpret_cast<const char*>(wav.data()),
                static_cast<std::streamsize>(wav.size()));
    }
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    std::shared_ptr<Database> db = std::move(dbRes.value());
    auto libIdRes = db->libraryAdd(dir.string(), "testlib");
    REQUIRE(libIdRes.has_value());
    Scanner scanner{db};
    auto sr = scanner.scanLibrary(*libIdRes);
    REQUIRE(sr.has_value());

    auto tracks = db->listTracks();
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    // Media layer already skips unhashable files; this asserts the stored
    // invariant that the DB backstop protects (M8).
    CHECK_FALSE(kanariv::media::internal::isZeroFingerprint(tracks->front().fingerprint));
    // The scanner no longer writes loudness values into the ReplayGain columns
    // (M10); only embedded tags may supply them, and this WAV has none.
    CHECK(tracks->front().replayGainTrack == 0.0);
    CHECK(tracks->front().replayGainAlbum == 0.0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("isZeroFingerprint identifies only the all-zero sentinel", "[db_scan]") {
    std::array<uint8_t, 32> zero{};
    CHECK(kanariv::media::internal::isZeroFingerprint(zero));
    auto oneBit = zero;
    oneBit[31] = 1;
    CHECK_FALSE(kanariv::media::internal::isZeroFingerprint(oneBit));
    auto allOnes = zero;
    allOnes.fill(0xFF);
    CHECK_FALSE(kanariv::media::internal::isZeroFingerprint(allOnes));
}

TEST_CASE("scan non-existent dir reports NotFound instead of an empty library", "[db_scan]") {
    // An empty vector must mean "no audio files", never "the root is missing":
    // a typo'd library path used to look like a successfully scanned empty dir.
    auto tracks = kanariv::media::scanTracksToVector("/nonexistent_path_xyz_12345",
                                                     kanariv::media::ScanMode::Sampled);
    REQUIRE_FALSE(tracks.has_value());
    REQUIRE(tracks.error().code == StatusCode::NotFound);
}

TEST_CASE("computeLoudnessGain accepts a non-NUL-terminated path view", "[db_scan]") {
    auto dir = tempDirPath("replaygain");
    const auto p = dir / "tone.wav";
    {
        const auto wav = makeSineWav();
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()),
                static_cast<std::streamsize>(wav.size()));
    }
    const std::string raw = p.string();
    // The view ends before the string's terminator; M5 required an owned copy
    // so FFmpeg never reads the trailing bytes as part of the path.
    const std::string padded = raw + "garbage";
    const std::string_view view(padded.data(), raw.size());
    auto gain = kanariv::media::computeLoudnessGain(view);
    REQUIRE(gain.has_value());
    REQUIRE(std::isfinite(*gain));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("library maintenance reports missing and duplicate rows", "[db_scan]") {
    auto root = tempDirPath("maintenance");
    std::filesystem::create_directories(root);
    auto present = root / "present.mp3";
    std::ofstream(present, std::ios::binary) << "present";

    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    auto library = db->libraryAdd(root.string(), "maintenance");
    REQUIRE(library.has_value());

    kanariv::types::Track first;
    first.path = present.string();
    first.library_id = *library;
    first.fingerprint.fill(0x11);
    auto firstId = db->insertTrack(first);
    REQUIRE(firstId.has_value());

    kanariv::types::Track duplicate = first;
    duplicate.path = present.string();
    duplicate.fingerprint.fill(0x33);
    auto duplicateId = db->insertTrack(duplicate);
    REQUIRE(duplicateId.has_value());

    kanariv::types::Track missing = first;
    missing.path = (root / "missing.mp3").string();
    missing.fingerprint.fill(0x22);
    auto missingId = db->insertTrack(missing);
    REQUIRE(missingId.has_value());

    auto dryRun = libraryReport(*db, *library);
    REQUIRE(dryRun.has_value());
    REQUIRE(dryRun->tracks_checked == 3);
    REQUIRE(dryRun->missingFiles() == 1);
    REQUIRE(dryRun->duplicateTracks() == 1);

    auto preview = libraryApplyMaintenance(
        *db, *library, LibraryMaintenanceOptions{.dry_run = true, .prune_missing = true});
    REQUIRE(preview.has_value());
    REQUIRE(preview->missingFiles() == 1);
    REQUIRE(db->getTrack(*missingId).has_value());

    auto prune = libraryApplyMaintenance(
        *db, *library, LibraryMaintenanceOptions{.dry_run = false, .prune_missing = true});
    REQUIRE(prune.has_value());
    REQUIRE(prune->missingFiles() == 1);
    REQUIRE(!db->getTrack(*missingId).has_value());
    REQUIRE(db->getTrack(*firstId).has_value());
    REQUIRE(db->getTrack(*duplicateId).has_value());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("scan empty dir returns empty", "[db_scan]") {
    auto dir = tempDirPath("empty_scan");
    auto tracks = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->empty());
    // also .txt noise only should be empty
    {
        std::ofstream f(dir / "notes.txt", std::ios::binary);
        f << "hello";
    }
    auto tracks2 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks2.has_value());
    REQUIRE(tracks2->empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("scanDirectory filters by audio extensions case-insensitively", "[db_scan]") {
    auto dir = tempDirPath("ext_test");
    // Clean up any existing files
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    // Create files with various extensions - use unique base names to avoid case-insensitive FS
    // collisions
    std::ofstream(dir / "a.MP3", std::ios::binary) << "audio";
    std::ofstream(dir / "b.mp3", std::ios::binary) << "audio";
    std::ofstream(dir / "c.Mp3", std::ios::binary) << "audio";
    std::ofstream(dir / "d.m4a", std::ios::binary) << "audio";
    std::ofstream(dir / "e.M4A", std::ios::binary) << "audio";
    std::ofstream(dir / "f.FLAC", std::ios::binary) << "audio";
    std::ofstream(dir / "g.WAV", std::ios::binary) << "audio";
    std::ofstream(dir / "h.OGG", std::ios::binary) << "audio";
    std::ofstream(dir / "j.OPUS", std::ios::binary) << "audio";
    std::ofstream(dir / "k.WMA", std::ios::binary) << "audio";
    std::ofstream(dir / "m.MP4", std::ios::binary) << "audio";
    std::ofstream(dir / "n.ASF", std::ios::binary) << "audio";
    std::ofstream(dir / "o.AIFF", std::ios::binary) << "audio";
    std::ofstream(dir / "p.APE", std::ios::binary) << "audio";
    std::ofstream(dir / "q.MPC", std::ios::binary) << "audio";
    std::ofstream(dir / "r.WV", std::ios::binary) << "audio";
    std::ofstream(dir / "s.OGA", std::ios::binary) << "audio";
    std::ofstream(dir / "i.txt", std::ios::binary) << "not audio";
    std::ofstream(dir / "l", std::ios::binary) << "no ext";

    auto tracks = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(tracks.has_value());
    // Only 17 audio files should be found
    REQUIRE(tracks->size() == 17);

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("ScanMode Full vs Sampled for file >128 KiB", "[db_scan]") {
    auto dir = tempDirPath("scan_mode");
    auto p = dir / "large.wav";
    // 200 KiB file ( >128 KiB = 2*64K )
    std::vector<uint8_t> data(200 * 1024);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (uint8_t)(i * 13 & 0xFF);
    {
        std::ofstream f(p, std::ios::binary);
        f.write((char*)data.data(), data.size());
    }
    auto sampled = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(sampled.has_value());
    REQUIRE(sampled->size() == 1);
    auto full = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Full);
    REQUIRE(full.has_value());
    REQUIRE(full->size() == 1);
    // Sampled uses head+tail+size, Full uses BLAKE3 of entire file -> should differ for large file
    REQUIRE(sampled->front().fingerprint != full->front().fingerprint);
    // But re-scanning same mode is deterministic
    auto sampled2 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(sampled2->front().fingerprint == sampled->front().fingerprint);
    auto full2 = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Full);
    REQUIRE(full2->front().fingerprint == full->front().fingerprint);
    // Modify middle byte only (not in head/tail 64K) -> Sampled should stay same, Full should
    // change For 200K file, head 64K = [0,64K), tail 64K = [136K,200K), middle [64K,136K)
    std::vector<uint8_t> dataMod = data;
    dataMod[100 * 1024] ^= 0xFF;
    auto p2 = dir / "large2.wav";
    // overwrite original with modified middle
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write((char*)dataMod.data(), dataMod.size());
    }
    auto sampledMod = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Sampled);
    REQUIRE(sampledMod.has_value());
    REQUIRE(sampledMod->size() == 1);
    // sampled should be same as before because middle not covered (unless size changed? size same)
    // For this file, change is in middle so sampled stays same, but full changes
    // Note: if sampled differs, still valid test; we check full definitely differs from original
    // full
    auto fullMod = kanariv::media::scanTracksToVector(dir, kanariv::media::ScanMode::Full);
    REQUIRE(fullMod.has_value());
    REQUIRE(fullMod->front().fingerprint != full->front().fingerprint);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("scanLibrary preserves play_count", "[db_scan]") {
    auto dir = tempDirPath("scan_preserve");
    auto p = dir / "song.mp3";
    std::vector<uint8_t> d1(10 * 1024, 0x11);
    for (size_t i = 0; i < d1.size(); ++i)
        d1[i] = (uint8_t)(i & 0xFF);
    {
        std::ofstream f(p, std::ios::binary);
        f.write((char*)d1.data(), d1.size());
    }
    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    std::shared_ptr<Database> db = std::move(dbRes.value());
    auto libIdRes = db->libraryAdd(dir.string(), "lib");
    REQUIRE(libIdRes.has_value());
    int64_t libId = *libIdRes;
    Scanner scanner{db};
    REQUIRE(scanner.scanLibrary(libId).has_value());
    auto tracks = db->listTracks();
    REQUIRE(tracks.has_value());
    REQUIRE(tracks->size() == 1);
    int64_t tid = tracks->front().id;
    // set play_count and rating, then modify file content (same path, different fingerprint) and
    // rescan
    {
        auto t = db->getTrack(tid);
        REQUIRE(t.has_value());
        t->play_count = 42;
        t->rating = 5;
        t->title = "Custom Title";
        REQUIRE(db->updateTrack(*t).has_value());
    }
    // modify file to trigger content-changed path: change size as well to guarantee early-exit
    // fails (size != trk.size)
    std::vector<uint8_t> d2(12 * 1024, 0x22);
    for (size_t i = 0; i < d2.size(); ++i)
        d2[i] = (uint8_t)((i * 3) & 0xFF);
    // ensure different fingerprint from d1
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write((char*)d2.data(), d2.size());
    }
    REQUIRE(scanner.scanLibrary(libId).has_value());
    auto t2 = db->getTrack(tid);
    REQUIRE(t2.has_value());
    REQUIRE(t2->play_count == 42);
    REQUIRE(t2->rating == 5);
    // title should have been cleared (metadata staleness) but play_count preserved
    REQUIRE(t2->title.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("scanLibrary reports start/finish summary", "[db_scan]") {
    auto dir = tempDirPath("scanlib_log");
    // empty file: fingerprint works, metadata open fails, insert still succeeds
    {
        std::ofstream f(dir / "z.mp3", std::ios::binary);
    }
    std::vector<std::string> out;
    Logger logger([&](LogLevel, std::string_view msg) { out.emplace_back(msg); }, LogLevel::Trace);
    DatabaseOptions opts;
    opts.logger = &logger;
    auto dbRes = Database::open(":memory:", opts);
    REQUIRE(dbRes.has_value());
    std::shared_ptr<Database> db = std::move(dbRes.value());
    auto libIdRes = db->libraryAdd(dir.string(), "testlib");
    REQUIRE(libIdRes.has_value());
    Scanner scanner{db};
    auto sr = scanner.scanLibrary(*libIdRes);
    REQUIRE(sr.has_value());
    bool started = false;
    bool finished = false;
    // NOTE: no metadata-failure assertion — FFmpeg opens even garbage
    // successfully (empty tags), so the "scan metadata failed" debug line
    // only fires on hard open_input errors; covered by review, not fixtures.
    for (const auto& m : out) {
        if (m.find("scan started") != std::string::npos)
            started = true;
        if (m.find("scan finished") != std::string::npos) {
            finished = true;
            REQUIRE(m.find("scanned=1") != std::string::npos);
            REQUIRE(m.find("added=1") != std::string::npos);
        }
    }
    REQUIRE(started);
    REQUIRE(finished);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("maintenance reports non-ASCII paths without pruning representable rows",
          "[db_scan][utf8]") {
    const auto root = tempDirPath("utf8_maintenance");
    const auto present = root / kanariv::utils::detail::pathFromUtf8("İ present.mp3");
    std::ofstream(present, std::ios::binary) << "present";
    REQUIRE(std::filesystem::is_regular_file(present));

    auto dbRes = Database::open(":memory:");
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    auto library = db->libraryAdd(kanariv::utils::detail::pathToUtf8(root), "utf8 maintenance");
    REQUIRE(library.has_value());

    kanariv::types::Track presentTrack;
    presentTrack.path = kanariv::utils::detail::pathToUtf8(present);
    presentTrack.library_id = *library;
    presentTrack.fingerprint.fill(0x44);
    auto presentId = db->insertTrack(presentTrack);
    REQUIRE(presentId.has_value());

    kanariv::types::Track missingTrack = presentTrack;
    missingTrack.path = kanariv::utils::detail::pathToUtf8(
        root / kanariv::utils::detail::pathFromUtf8("İ missing.mp3"));
    missingTrack.fingerprint.fill(0x55);
    auto missingId = db->insertTrack(missingTrack);
    REQUIRE(missingId.has_value());

    auto report = libraryReport(*db, *library);
    REQUIRE(report.has_value());
    CHECK(report->tracks_checked == 2);
    REQUIRE(report->missing_tracks.size() == 1);
    CHECK(report->missing_tracks[0].id == *missingId);

    auto pruned = libraryPruneMissing(*db, *report);
    REQUIRE(pruned.has_value());
    CHECK(*pruned == 1);
    CHECK(db->getTrack(*presentId).has_value());
    auto gone = db->getTrack(*missingId);
    REQUIRE(!gone.has_value());
    CHECK(gone.error().code == StatusCode::NotFound);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("scanLibrary BEGIN failure returns Busy promptly instead of deadlocking",
          "[db_scan][scan]") {
    auto root = tempDirPath("scan_busy");
    std::ofstream(root / "track.mp3", std::ios::binary) << "fingerprint input";
    auto dbPath = tempDbPath("scan_busy");

    auto dbRes = Database::open(dbPath.string());
    REQUIRE(dbRes.has_value());
    std::shared_ptr<Database> db = std::move(dbRes.value());
    auto library = db->libraryAdd(kanariv::utils::detail::pathToUtf8(root), "busy scan");
    REQUIRE(library.has_value());

    sqlite3* handle = DatabaseAccess::handle(*db);
    REQUIRE(handle != nullptr);
    // Bound the wait so a regression that blocks on the lock fails fast.
    REQUIRE(sqlite3_busy_timeout(handle, 100) == SQLITE_OK);

    sqlite3* blocker = nullptr;
    REQUIRE(sqlite3_open(dbPath.string().c_str(), &blocker) == SQLITE_OK);
    REQUIRE(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);

    const auto started = std::chrono::steady_clock::now();
    Scanner scanner{db};
    auto result = scanner.scanLibrary(*library);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(!result.has_value());
    CHECK(result.error().code == StatusCode::Busy);
    CHECK(elapsed < std::chrono::seconds(2));

    sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(blocker);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
