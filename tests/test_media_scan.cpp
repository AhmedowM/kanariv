#include <array>
#include <catch2/catch_test_macros.hpp>
#include <common.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <kanariv/fwd.hpp>
#include <kanariv/media/fingerprint.hpp>
#include <kanariv/media/scan.hpp>
#include <kanariv/utils/error.hpp>
#include <media/convert_loop.hpp>
#include <media/scan_walker.hpp>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utils/path_utf8.hpp>
#include <vector>

using kanariv::media::ScanIssue;
using kanariv::media::ScanIssueKind;
using kanariv::media::ScanMode;
using kanariv::media::internal::ScanWalker;
using kanariv::test_helpers::tempDirPath;
using PathBytes = std::array<std::uint8_t, 32>;

namespace {

void writeDummyFile(const std::filesystem::path& p, std::string_view content = "dummy audio") {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

kanariv::media::internal::FingerprintProvider failingProvider() {
    return [](const std::filesystem::path&, ScanMode) -> kanariv::utils::Expected<PathBytes> {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "simulated unreadable file")};
    };
}

kanariv::media::internal::FingerprintProvider zeroProvider() {
    return [](const std::filesystem::path&, ScanMode) -> kanariv::utils::Expected<PathBytes> {
        return PathBytes{};
    };
}

} // namespace

TEST_CASE("ScanWalker skips a file it cannot fingerprint", "[media_scan]") {
    auto dir = tempDirPath("walker_fail");
    const auto file = dir / "unreadable.wav";
    writeDummyFile(file);

    std::vector<ScanIssue> issues;
    ScanWalker walker(
        dir, ScanMode::Sampled, [&](const ScanIssue& i) { issues.push_back(i); },
        failingProvider());
    REQUIRE(walker.open().has_value());
    CHECK_FALSE(walker.next().has_value());
    CHECK_FALSE(walker.walkError().has_value());

    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == ScanIssueKind::FileSkipped);
    CHECK(issues[0].path == kanariv::utils::detail::pathToUtf8(file));
    CHECK(issues[0].message.find("simulated unreadable file") != std::string::npos);
}

TEST_CASE("ScanWalker rejects the all-zero fingerprint sentinel", "[media_scan]") {
    auto dir = tempDirPath("walker_zero");
    writeDummyFile(dir / "silent.flac");

    std::vector<ScanIssue> issues;
    ScanWalker walker(
        dir, ScanMode::Sampled, [&](const ScanIssue& i) { issues.push_back(i); }, zeroProvider());
    REQUIRE(walker.open().has_value());
    // A zero digest would collide with every other zero digest in the UNIQUE
    // index, so it must never reach a TrackRow.
    CHECK_FALSE(walker.next().has_value());
    CHECK_FALSE(walker.walkError().has_value());
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == ScanIssueKind::FileSkipped);
    CHECK(issues[0].message.find("all-zero") != std::string::npos);
}

TEST_CASE("ScanWalker with the real provider yields every audio file", "[media_scan]") {
    auto dir = tempDirPath("walker_ok");
    writeDummyFile(dir / "one.mp3", "content one");
    writeDummyFile(dir / "two.ogg", "content two");
    writeDummyFile(dir / "notes.txt", "not audio");

    std::vector<ScanIssue> issues;
    ScanWalker walker(dir, ScanMode::Sampled, [&](const ScanIssue& i) { issues.push_back(i); });
    REQUIRE(walker.open().has_value());

    std::vector<kanariv::types::TrackRow> tracks;
    while (auto track = walker.next())
        tracks.push_back(std::move(*track));

    CHECK_FALSE(walker.walkError().has_value());
    CHECK(issues.empty());
    REQUIRE(tracks.size() == 2);
    for (const auto& t : tracks) {
        CHECK_FALSE(kanariv::media::internal::isZeroFingerprint(t.fingerprint));
        CHECK_FALSE(t.path.empty());
    }
}

TEST_CASE("ScanWalker open reports a missing root as NotFound", "[media_scan]") {
    auto dir = tempDirPath("walker_missing");
    const auto missing = dir / "does_not_exist";

    std::vector<ScanIssue> issues;
    ScanWalker walker(missing, ScanMode::Sampled, [&](const ScanIssue& i) { issues.push_back(i); });
    auto opened = walker.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == kanariv::utils::StatusCode::NotFound);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == ScanIssueKind::WalkError);
    CHECK_FALSE(walker.next().has_value());
}

TEST_CASE("ScanWalker open reports a non-directory root as Io", "[media_scan]") {
    auto dir = tempDirPath("walker_file_root");
    const auto file = dir / "not_a_dir.wav";
    writeDummyFile(file);

    ScanWalker walker(file, ScanMode::Sampled);
    auto opened = walker.open();
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == kanariv::utils::StatusCode::Io);
    // Recorded too, so the vector overload fails instead of returning {}.
    CHECK(walker.walkError().has_value());
}

TEST_CASE("scanTracksToVector reports issues through the sink", "[media_scan]") {
    auto dir = tempDirPath("walker_sink");
    const auto missing = dir / "absent";
    std::vector<ScanIssue> issues;
    auto tracks = kanariv::media::scanTracksToVector(
        missing, ScanMode::Sampled, [&](const ScanIssue& i) { issues.push_back(i); });
    REQUIRE_FALSE(tracks.has_value());
    CHECK(tracks.error().code == kanariv::utils::StatusCode::NotFound);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == ScanIssueKind::WalkError);
    CHECK(issues[0].path == kanariv::utils::detail::pathToUtf8(missing));
}

TEST_CASE("scanTracks yields nothing and does not throw for a missing root", "[media_scan]") {
    auto dir = tempDirPath("walker_generator");
    const auto missing = dir / "gone";
    std::size_t seen = 0;
    for (const auto& track : kanariv::media::scanTracks(missing)) {
        (void)track;
        ++seen;
    }
    CHECK(seen == 0);
}

TEST_CASE("pumpConvert drains an oversized source in window-sized blocks", "[media_scan]") {
    constexpr std::size_t windowFrames = 4;
    constexpr std::size_t channels = 2;
    constexpr std::size_t sourceFrames = 10;

    std::vector<float> window(windowFrames * channels, -1.0f);
    std::size_t producedFrames = 0;
    // Fake resampler: hands out `sourceFrames` frames in windows, encoding the
    // absolute sample index so order and loss are both observable.
    auto convert = [&](float* out, std::size_t frames) -> int {
        const std::size_t remaining = sourceFrames - producedFrames;
        const std::size_t n = frames < remaining ? frames : remaining;
        for (std::size_t f = 0; f < n; ++f) {
            for (std::size_t c = 0; c < channels; ++c)
                out[f * channels + c] = static_cast<float>((producedFrames + f) * channels + c);
        }
        producedFrames += n;
        return static_cast<int>(n);
    };
    std::vector<float> collected;
    auto onBlock = [&](std::span<const float> block) {
        collected.insert(collected.end(), block.begin(), block.end());
    };

    const int rc =
        kanariv::media::internal::pumpConvert(std::span<float>(window), channels, convert, onBlock);
    CHECK(rc == 0);
    // Nothing is truncated when the source exceeds the window, and the blocks
    // arrive in stream order.
    REQUIRE(collected.size() == sourceFrames * channels);
    for (std::size_t i = 0; i < collected.size(); ++i)
        CHECK(collected[i] == static_cast<float>(i));
}

TEST_CASE("pumpConvert returns the first negative error", "[media_scan]") {
    std::vector<float> window(8, 0.0f);
    std::size_t calls = 0;
    auto convert = [&](float*, std::size_t) -> int {
        ++calls;
        return calls == 1 ? 2 : -42;
    };
    std::size_t blocks = 0;
    auto onBlock = [&](std::span<const float>) { ++blocks; };
    const int rc =
        kanariv::media::internal::pumpConvert(std::span<float>(window), 2, convert, onBlock);
    CHECK(rc == -42);
    CHECK(blocks == 1); // the block produced before the failure is still delivered
}

TEST_CASE("pumpConvert clamps a converter that overstates its output", "[media_scan]") {
    std::vector<float> window(4 * 2, 0.0f);
    std::size_t calls = 0;
    auto convert = [&](float*, std::size_t) -> int {
        return calls++ == 0 ? 99 : 0; // impossible count, then drained
    };
    std::size_t samples = 0;
    auto onBlock = [&](std::span<const float> block) { samples += block.size(); };
    const int rc =
        kanariv::media::internal::pumpConvert(std::span<float>(window), 2, convert, onBlock);
    CHECK(rc == 0);
    CHECK(samples == window.size()); // never reads past the window
}

TEST_CASE("pumpConvert is a no-op without a window or channels", "[media_scan]") {
    auto convert = [](float*, std::size_t) -> int { return 1; };
    auto onBlock = [](std::span<const float>) {};
    CHECK(kanariv::media::internal::pumpConvert(std::span<float>{}, 2, convert, onBlock) == 0);
    std::vector<float> window(4, 0.0f);
    CHECK(kanariv::media::internal::pumpConvert(std::span<float>(window), 0, convert, onBlock) ==
          0);
}
