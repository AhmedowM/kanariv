#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <common.hpp>
#include <cstdint>
#include <engine/fade.hpp>
#include <filesystem>
#include <fstream>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <utils/path_utf8.hpp>
#include <vector>
using namespace kanariv::db;
using namespace kanariv::engine;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

TEST_CASE("gapless CAS arms once via Engine", "[engine_gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless_cas").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "cas" + std::to_string(i) + ".wav";
        t.duration = 0.8;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xC8 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    // gapless should arm once when remaining <=0.3 (pos>=0.5) and trigger next via engineTick
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(2000),
                  std::chrono::milliseconds(50));
    REQUIRE(eng->currentTrackId() != first);
    // ensure CAS prevented double trigger: wait 300ms and verify still on second track only
    int64_t second = eng->currentTrackId();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(eng->currentTrackId() == second);
    // verify gapless armed flag implicitly via exactly one advancement (queue now has second track)
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("gapless 300ms lookahead triggers next once", "[engine_gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "gap" + std::to_string(i) + ".wav";
        t.duration = 0.8;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xC0 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    // duration 0.8, gap 0.3 -> when remaining <=0.3 (pos>=0.5) should arm and trigger next
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(2000),
                  std::chrono::milliseconds(50));
    int64_t cur = eng->currentTrackId();
    // should have advanced to second track via gapless
    REQUIRE(cur != first);
    // ensure only once: poll 300ms, should not advance again (queue now empty or last track)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int64_t cur2 = eng->currentTrackId();
    REQUIRE(cur2 == cur);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

TEST_CASE("gapless not triggered when remaining > gap", "[engine_gapless]") {
    KANARIV_SKIP_IF_NOAUDIO();
    std::string dbPath = tempDbPath("gapless_notrig").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto db = std::move(dbRes.value());
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = "gap_n" + std::to_string(i) + ".wav";
        t.duration = 5.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = (uint8_t)(0xD0 + i * 16 + b);
        auto r = db->insertTrack(t);
        REQUIRE(r.has_value());
        REQUIRE(db->queueEnqueue(1, *r, -1).has_value());
    }
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(std::shared_ptr<kanariv::db::Database>(std::move(db))).has_value());
    REQUIRE(eng->play(1).has_value());
    int64_t first = eng->currentTrackId();
    // duration 5s, gap 0.3 -> remaining 4.8 > gap, should not trigger within 200ms
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(eng->currentTrackId() == first);
    // seek near end then gapless should trigger
    REQUIRE(eng->seek(4.8).has_value());
    busyWaitUntil([&] { return eng->currentTrackId() != first; }, std::chrono::milliseconds(2000),
                  std::chrono::milliseconds(50));
    REQUIRE(eng->currentTrackId() != first);
    eng.reset();
    {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath + "-wal", ec);
        std::filesystem::remove(dbPath + "-shm", ec);
    }
}

namespace {

/// Chunks in these tests run at a fixed rate so frame counts are exact.
constexpr double kFadeRate = 48000.0;

/// @brief Interleaved buffer with a trailing canary region.
/// @details `applyFadeEnvelope()` only sees the first `used` floats as its
/// span, so a write past that (the E1 `frames*ch`-as-sample-count mix-up) lands
/// in the canary and is detected even when no sanitizer is available. The
/// canary is sized for the worst case: the pre-fix loop addressed up to
/// `frames*ch*ch` floats (ch <= 6 in these tests), i.e. six times the payload.
class CanaryBuffer {
  public:
    static constexpr float kSentinel = -12345.0f;

    explicit CanaryBuffer(std::size_t used, float value)
        : used_(used), data_(used * 6 + kGuard, value) {
        for (std::size_t i = used_; i < data_.size(); ++i)
            data_[i] = kSentinel;
    }

    std::span<float> span() {
        return std::span<float>(data_.data(), used_);
    }
    float& operator[](std::size_t i) {
        return data_[i];
    }
    const float& operator[](std::size_t i) const {
        return data_[i];
    }
    [[nodiscard]] bool canaryIntact() const {
        for (std::size_t i = used_; i < data_.size(); ++i)
            if (data_[i] != kSentinel)
                return false;
        return true;
    }

  private:
    static constexpr std::size_t kGuard = 64;
    std::size_t used_;
    std::vector<float> data_;
};

} // namespace

TEST_CASE("fade envelope applies one gain per frame at mono, stereo and 5.1", "[engine_fade]") {
    using kanariv::engine::detail::applyFadeEnvelope;
    using kanariv::engine::detail::FadeParams;
    const std::uint32_t ch = GENERATE(1u, 2u, 6u);
    constexpr std::size_t frames = 512;
    const std::size_t samples = frames * ch;

    SECTION("fade-in and fade-out attenuate the first and last frame") {
        CanaryBuffer out(samples, 1.0f);
        FadeParams p;
        p.startFrame = 0;
        p.fadeInFrames = 256;
        p.fadeOutFrames = 256;
        p.fadeOutStartSec = 256.0 / kFadeRate;
        p.sampleRate = kFadeRate;
        REQUIRE(applyFadeEnvelope(out.span(), frames, ch, p) == samples);
        REQUIRE(out.canaryIntact());
        for (std::uint32_t c = 0; c < ch; ++c) {
            // Frame 0 is fully attenuated by the fade-in; frame 1 is one step in.
            REQUIRE(out[c] == Catch::Approx(0.0).margin(1e-6));
            REQUIRE(out[ch + c] == Catch::Approx(1.0 / 256.0).margin(1e-6));
            // Fade-out starts at frame 256, where the gain is back to 1.0 ...
            REQUIRE(out[256 * ch + c] == Catch::Approx(1.0).margin(1e-6));
            // ... and the last frame is one step short of fully faded out.
            REQUIRE(out[(frames - 1) * ch + c] == Catch::Approx(1.0 / 256.0).margin(1e-6));
        }
        // One envelope value per frame: equal input yields bit-equal channels.
        for (std::size_t f = 0; f < frames; ++f)
            for (std::uint32_t c = 1; c < ch; ++c)
                REQUIRE(out[f * ch + c] == out[f * ch]);
        for (std::size_t i = 0; i < samples; ++i) {
            REQUIRE(std::isfinite(out[i]));
            REQUIRE(out[i] >= 0.0f);
            REQUIRE(out[i] <= 1.0f);
        }
    }

    SECTION("full-scale audio after the fade passes through untouched") {
        CanaryBuffer out(samples, 1.0f);
        FadeParams p;
        p.startFrame = 1000; // past the fade-in ramp; no fade-out configured
        p.fadeInFrames = 256;
        p.sampleRate = kFadeRate;
        REQUIRE(applyFadeEnvelope(out.span(), frames, ch, p) == samples);
        REQUIRE(out.canaryIntact());
        for (std::size_t i = 0; i < samples; ++i) {
            REQUIRE(out[i] == 1.0f);
            REQUIRE(std::isfinite(out[i]));
        }
    }

    // A gain of 1.0 is the identity, so an overrun into the canary would be
    // invisible; this section ramps the gain to 0 across the chunk so any write
    // past `frames*ch` alters the canary and fails `canaryIntact()`.
    SECTION("envelope stops at exactly frames*ch samples") {
        CanaryBuffer out(samples, 1.0f);
        FadeParams p;
        p.startFrame = 0;
        p.fadeOutFrames = 256;
        p.fadeOutStartSec = 0.0; // fade out over the first 256 frames, then silence
        p.sampleRate = kFadeRate;
        REQUIRE(applyFadeEnvelope(out.span(), frames, ch, p) == samples);
        REQUIRE(out.canaryIntact());
        for (std::uint32_t c = 0; c < ch; ++c) {
            REQUIRE(out[(frames - 1) * ch + c] == 0.0f);
        }
        for (std::size_t i = 0; i < samples; ++i) {
            REQUIRE(std::isfinite(out[i]));
            REQUIRE(out[i] >= 0.0f);
            REQUIRE(out[i] <= 1.0f);
        }
    }

    SECTION("crossfade blends the incoming track per channel") {
        CanaryBuffer out(samples, 1.0f);
        CanaryBuffer in(samples, 0.0f);
        for (std::size_t f = 0; f < frames; ++f)
            for (std::uint32_t c = 0; c < ch; ++c)
                in[f * ch + c] = 0.04f * static_cast<float>(c + 1);
        FadeParams p;
        p.startFrame = 1000; // no envelope gain, isolates the mix
        p.sampleRate = kFadeRate;
        constexpr double progress = 0.25;
        REQUIRE(applyFadeEnvelope(out.span(), frames, ch, p, in.span(), progress) == samples);
        REQUIRE(out.canaryIntact());
        REQUIRE(in.canaryIntact());
        const double outWeight = std::sqrt(1.0 - progress);
        const double inWeight = std::sqrt(progress);
        for (std::size_t f = 0; f < frames; ++f) {
            for (std::uint32_t c = 0; c < ch; ++c) {
                const double expected =
                    1.0 * outWeight + (0.04 * static_cast<double>(c + 1)) * inWeight;
                REQUIRE(out[f * ch + c] == Catch::Approx(expected).margin(1e-6));
                REQUIRE(std::isfinite(out[f * ch + c]));
                REQUIRE(out[f * ch + c] <= 1.0f);
            }
        }
    }
}

TEST_CASE("crossfade mixes incoming tracks with their own channel count", "[engine_fade]") {
    using kanariv::engine::detail::applyFadeEnvelope;
    using kanariv::engine::detail::FadeParams;
    constexpr std::size_t frames = 64;
    constexpr double progress = 0.5;
    const double outWeight = std::sqrt(1.0 - progress);
    const double inWeight = std::sqrt(progress);
    FadeParams p;
    p.startFrame = 1000; // past any envelope ramp, isolates the mix
    p.sampleRate = kFadeRate;

    SECTION("mono incoming feeds every outgoing channel") {
        constexpr std::uint32_t outCh = 2;
        CanaryBuffer out(frames * outCh, 1.0f);
        CanaryBuffer in(frames, 0.0f);
        for (std::size_t f = 0; f < frames; ++f)
            in[f] = 0.5f;
        REQUIRE(applyFadeEnvelope(out.span(), frames, outCh, p, in.span(), progress, 1) ==
                frames * outCh);
        REQUIRE(out.canaryIntact());
        REQUIRE(in.canaryIntact());
        for (std::size_t f = 0; f < frames; ++f)
            for (std::uint32_t c = 0; c < outCh; ++c)
                REQUIRE(out[f * outCh + c] ==
                        Catch::Approx(1.0 * outWeight + 0.5 * inWeight).margin(1e-6));
    }

    SECTION("stereo incoming into a mono chunk uses its left channel") {
        CanaryBuffer out(frames, 1.0f);
        CanaryBuffer in(frames * 2, 0.0f);
        for (std::size_t f = 0; f < frames; ++f) {
            in[f * 2] = 0.25f;
            in[f * 2 + 1] = 0.75f;
        }
        REQUIRE(applyFadeEnvelope(out.span(), frames, 1, p, in.span(), progress, 2) == frames);
        REQUIRE(out.canaryIntact());
        REQUIRE(in.canaryIntact());
        for (std::size_t f = 0; f < frames; ++f)
            REQUIRE(out[f] == Catch::Approx(1.0 * outWeight + 0.25 * inWeight).margin(1e-6));
    }

    SECTION("a short incoming chunk only covers the frames it decoded") {
        constexpr std::uint32_t outCh = 2;
        constexpr std::size_t shortFrames = 16;
        CanaryBuffer out(frames * outCh, 1.0f);
        CanaryBuffer in(shortFrames * outCh, 0.0f);
        for (std::size_t i = 0; i < shortFrames * outCh; ++i)
            in[i] = 0.5f;
        REQUIRE(applyFadeEnvelope(out.span(), frames, outCh, p, in.span(), progress, outCh) ==
                frames * outCh);
        REQUIRE(out.canaryIntact());
        REQUIRE(in.canaryIntact());
        for (std::size_t f = 0; f < frames; ++f) {
            const double expected = f < shortFrames ? 1.0 * outWeight + 0.5 * inWeight
                                                    : 1.0 * outWeight;
            for (std::uint32_t c = 0; c < outCh; ++c)
                REQUIRE(out[f * outCh + c] == Catch::Approx(expected).margin(1e-6));
        }
    }
}

namespace {

/// @brief Writes a 16-bit mono PCM WAV of silence.
/// @details Silence keeps the crossfade tests quiet while still giving the
/// engine a real file to decode (so `decodeLoop` runs and the preload/completion
/// paths are exercised) without needing an audio device: `allowSimulatedPlayback`
/// covers a missing `AudioOutput` (silent null backend). Fractional lengths let
/// a test make the incoming track shorter than the crossfade window.
bool writeSilentWav(const std::filesystem::path& path, double seconds) {
    constexpr std::uint32_t rate = 8000;
    const std::uint32_t frames = static_cast<std::uint32_t>(rate * seconds);
    const std::uint32_t bytes = frames * 2;
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    const auto put16 = [&f](std::uint16_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
    };
    const auto put32 = [&f](std::uint32_t v) {
        f.put(static_cast<char>(v & 0xFF));
        f.put(static_cast<char>((v >> 8) & 0xFF));
        f.put(static_cast<char>((v >> 16) & 0xFF));
        f.put(static_cast<char>((v >> 24) & 0xFF));
    };
    f.write("RIFF", 4);
    put32(36 + bytes);
    f.write("WAVEfmt ", 8);
    put32(16);
    put16(1); // PCM
    put16(1); // mono
    put32(rate);
    put32(rate * 2);
    put16(2);
    put16(16);
    f.write("data", 4);
    put32(bytes);
    for (std::uint32_t i = 0; i < frames; ++i)
        put16(0); // silence
    f.close();
    return static_cast<bool>(f);
}

} // namespace

TEST_CASE("failed crossfade preload keeps the cursor on the unreadable track",
          "[engine_gapless][crossfade]") {
    namespace fs = std::filesystem;
    const fs::path dir = tempDirPath("xf_preload");
    const fs::path wav = dir / "silence0.wav";
    REQUIRE(writeSilentWav(wav, 2));

    const std::string dbPath = tempDbPath("xf_preload").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto sharedDb = std::shared_ptr<Database>(std::move(dbRes.value()));
    int64_t ids[3] = {0, 0, 0};
    const std::string paths[3] = {kanariv::utils::detail::pathToUtf8(wav),
                                  kanariv::utils::detail::pathToUtf8(dir / "missing1.wav"),
                                  kanariv::utils::detail::pathToUtf8(dir / "missing2.wav")};
    for (int i = 0; i < 3; ++i) {
        kanariv::types::Track t;
        t.path = paths[i];
        t.duration = i == 0 ? 2.0 : 1.0;
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<std::uint8_t>(0xE8 + i * 16 + b);
        auto r = sharedDb->insertTrack(t);
        REQUIRE(r.has_value());
        ids[i] = *r;
        REQUIRE(sharedDb->queueEnqueue(1, *r, -1).has_value());
    }

    std::mutex logMutex;
    std::vector<std::string> logs;
    Logger logger(
        [&](LogLevel, std::string_view msg) {
            std::lock_guard<std::mutex> lk(logMutex);
            logs.emplace_back(msg);
        },
        LogLevel::Trace);
    const auto sawLog = [&](std::string_view needle) {
        std::lock_guard<std::mutex> lk(logMutex);
        for (const auto& line : logs)
            if (line.find(needle) != std::string::npos)
                return true;
        return false;
    };

    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.interTrackGapMs = 300; // crossfade window; its preload is what we test
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    cfg.logger = &logger;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(sharedDb).has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == ids[0]);
    const double dur = eng->duration();
    REQUIRE(dur > 1.0);
    // Enter the crossfade window so engineTick arms the preload of the
    // unreadable second entry.
    REQUIRE(eng->seek(dur - 0.1).has_value());
    busyWaitUntil([&] { return sawLog("crossfade preload failed"); },
                  std::chrono::milliseconds(3000), std::chrono::milliseconds(20));
    REQUIRE(sawLog("crossfade preload failed"));
    REQUIRE(!sawLog("crossfade armed"));

    // The preload failed, so nothing was committed: still on the outgoing
    // track and the persisted cursor still points at the unreadable entry.
    REQUIRE(eng->currentTrackId() == ids[0]);
    auto persisted = sharedDb->loadEngineState();
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->cursor_pos == 1);
    REQUIRE(persisted->current_track_id == ids[0]);

    // The next explicit advance targets the unreadable entry (which the
    // simulated backend can "play") instead of skipping to the third one.
    REQUIRE(eng->next().has_value());
    REQUIRE(eng->currentTrackId() == ids[1]);
    eng.reset();
    safeRemoveDb(dbPath);
}

namespace {

/// @brief Builds a DB with two silent WAV tracks and returns their ids.
/// @details Also returns the shared DB so a test can read the persisted engine
/// state while the engine is still running (WAL allows concurrent readers).
struct TwoSilentTracks {
    std::shared_ptr<Database> db;
    int64_t ids[2]{0, 0};
};

TwoSilentTracks makeTwoSilentTracks(const std::filesystem::path& dir, const std::string& dbPath,
                                    double firstSec, double secondSec,
                                    std::uint8_t fingerprintBase) {
    TwoSilentTracks out;
    auto dbRes = Database::open(dbPath);
    INFO("open database");
    REQUIRE(dbRes.has_value());
    out.db = std::shared_ptr<Database>(std::move(dbRes.value()));
    REQUIRE(writeSilentWav(dir / "first.wav", firstSec));
    REQUIRE(writeSilentWav(dir / "second.wav", secondSec));
    const std::string paths[2] = {kanariv::utils::detail::pathToUtf8(dir / "first.wav"),
                                  kanariv::utils::detail::pathToUtf8(dir / "second.wav")};
    const double durations[2] = {firstSec, secondSec};
    for (int i = 0; i < 2; ++i) {
        kanariv::types::Track t;
        t.path = paths[i];
        t.duration = durations[i];
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<std::uint8_t>(fingerprintBase + i * 16 + b);
        auto r = out.db->insertTrack(t);
        REQUIRE(r.has_value());
        out.ids[i] = *r;
        REQUIRE(out.db->queueEnqueue(1, *r, -1).has_value());
    }
    return out;
}

} // namespace

TEST_CASE("crossfade completion publishes the incoming track", "[engine_gapless][crossfade]") {
    namespace fs = std::filesystem;
    const fs::path dir = tempDirPath("xf_publish");
    const std::string dbPath = tempDbPath("xf_publish").string();
    TwoSilentTracks tracks = makeTwoSilentTracks(dir, dbPath, 1.0, 2.0, 0x10);
    const int64_t* ids = tracks.ids;

    std::mutex startedMutex;
    std::vector<int64_t> started;
    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.interTrackGapMs = 100; // crossfade window
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    cfg.callbacks.on_track_started = [&](int64_t id) {
        std::lock_guard<std::mutex> lk(startedMutex);
        started.push_back(id);
    };
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(tracks.db).has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == ids[0]);
    const double dur = eng->duration();
    REQUIRE(dur == Catch::Approx(1.0).margin(0.01));
    // Enter the crossfade window (remaining <= interTrackGapMs).
    REQUIRE(eng->seek(dur - 0.05).has_value());
    busyWaitUntil([&] { return eng->currentTrackId() == ids[1]; }, std::chrono::milliseconds(5000),
                  std::chrono::milliseconds(20));
    // E3: completion publishes the incoming track instead of leaving
    // currentTrack_/duration_ describing the outgoing one.
    REQUIRE(eng->currentTrackId() == ids[1]);
    REQUIRE(eng->duration() == Catch::Approx(2.0).margin(0.01));
    {
        std::lock_guard<std::mutex> lk(startedMutex);
        REQUIRE(std::find(started.begin(), started.end(), ids[1]) != started.end());
    }
    // The same publication persists the new current track and cursor.
    const auto persistedIncoming = [&]() {
        auto st = tracks.db->loadEngineState();
        return st && st->current_track_id == ids[1];
    };
    busyWaitUntil(persistedIncoming, std::chrono::milliseconds(3000),
                  std::chrono::milliseconds(20));
    auto persisted = tracks.db->loadEngineState();
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->current_track_id == ids[1]);
    // The preload transaction already committed the cursor past the incoming
    // entry (cursor addresses the NEXT entry), and publication persists it.
    REQUIRE(persisted->cursor_pos == 2);
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("crossfade publishes when the incoming track ends before the overlap",
          "[engine_gapless][crossfade]") {
    namespace fs = std::filesystem;
    const fs::path dir = tempDirPath("xf_earlyeof");
    const std::string dbPath = tempDbPath("xf_earlyeof").string();
    // 0.2 s incoming is shorter than the 500 ms crossfade window, so the
    // incoming decoder hits EOF while the outgoing track still has frames:
    // the early-EOF completion path must publish it all the same.
    TwoSilentTracks tracks = makeTwoSilentTracks(dir, dbPath, 2.0, 0.2, 0x30);
    const int64_t* ids = tracks.ids;

    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    cfg.interTrackGapMs = 500;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(tracks.db).has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == ids[0]);
    const double dur = eng->duration();
    REQUIRE(dur == Catch::Approx(2.0).margin(0.01));
    REQUIRE(eng->seek(dur - 0.4).has_value()); // remaining 0.4 s < window
    busyWaitUntil([&] { return eng->currentTrackId() == ids[1]; }, std::chrono::milliseconds(5000),
                  std::chrono::milliseconds(20));
    REQUIRE(eng->currentTrackId() == ids[1]);
    REQUIRE(eng->duration() == Catch::Approx(0.2).margin(0.01));
    eng.reset();
    safeRemoveDb(dbPath);
}

TEST_CASE("a track change cancels an armed crossfade", "[engine_gapless][crossfade]") {
    namespace fs = std::filesystem;
    const fs::path dir = tempDirPath("xf_cancel");
    const std::string dbPath = tempDbPath("xf_cancel").string();
    auto dbRes = Database::open(dbPath);
    REQUIRE(dbRes.has_value());
    auto sharedDb = std::shared_ptr<Database>(std::move(dbRes.value()));
    int64_t ids[3] = {0, 0, 0};
    // Lengths: outgoing 3 s, preloaded 8 s, skipped-to 8 s. The preloaded and
    // skipped-to tracks must not enter their own crossfade window (remaining
    // <= 2 s) during the 2.5 s observation, so a stale publication is stable
    // and cannot self-correct.
    const double secs[3] = {3.0, 8.0, 8.0};
    for (int i = 0; i < 3; ++i) {
        const fs::path wav = dir / ("track" + std::to_string(i) + ".wav");
        REQUIRE(writeSilentWav(wav, secs[i]));
        kanariv::types::Track t;
        t.path = kanariv::utils::detail::pathToUtf8(wav);
        t.duration = secs[i];
        for (int b = 0; b < 32; ++b)
            t.fingerprint[b] = static_cast<std::uint8_t>(0x58 + i * 16 + b);
        auto r = sharedDb->insertTrack(t);
        REQUIRE(r.has_value());
        ids[i] = *r;
        REQUIRE(sharedDb->queueEnqueue(1, *r, -1).has_value());
    }

    std::mutex logMutex;
    std::vector<std::string> logs;
    Logger logger(
        [&](LogLevel, std::string_view msg) {
            std::lock_guard<std::mutex> lk(logMutex);
            logs.emplace_back(msg);
        },
        LogLevel::Trace);
    const auto sawLog = [&](std::string_view needle) {
        std::lock_guard<std::mutex> lk(logMutex);
        for (const auto& line : logs)
            if (line.find(needle) != std::string::npos)
                return true;
        return false;
    };

    kanariv::types::EngineConfig cfg;
    cfg.allowSimulatedPlayback = true;
    cfg.enableMonitorThread = true;
    cfg.pollMs = 10;
    cfg.gaplessMs = 300;
    // A 2 s overlap cannot finish quickly: the decode thread may only run
    // ~1 s (8192 frames at 8 kHz) ahead of the consuming output, so the
    // transition stays in flight for roughly a second after it is armed.
    cfg.interTrackGapMs = 2000;
    cfg.historyThresholdPct = 100;
    cfg.historyThresholdSecs = 100;
    cfg.logger = &logger;
    auto eRes = Engine::create(cfg);
    REQUIRE(eRes.has_value());
    auto eng = std::move(eRes.value());
    REQUIRE(eng->attachDatabase(sharedDb).has_value());
    REQUIRE(eng->play(1).has_value());
    REQUIRE(eng->currentTrackId() == ids[0]);
    // Remaining 2.5 s: an engine tick arms the overlap ~0.5 s from now.
    REQUIRE(eng->seek(eng->duration() - 2.5).has_value());
    busyWaitUntil([&] { return sawLog("crossfade armed"); }, std::chrono::milliseconds(3000),
                  std::chrono::milliseconds(10));
    REQUIRE(sawLog("crossfade armed"));
    REQUIRE(eng->currentTrackId() == ids[0]);

    // Skip forward while the overlap is in flight: the preloaded track must not
    // survive into the new track or be published over it later. A stale
    // preload would complete its remaining ~2 s of mixing and publish ids[1].
    REQUIRE(eng->next().has_value());
    REQUIRE(eng->currentTrackId() == ids[2]);
    // A stale preload would finish its remaining ~2 s of mixing and publish
    // ids[1]; the real track keeps playing for the whole observation.
    busyWaitUntil([&] { return eng->position() >= 2.0 || eng->currentTrackId() != ids[2]; },
                  std::chrono::milliseconds(5000), std::chrono::milliseconds(10));
    REQUIRE(eng->currentTrackId() == ids[2]);
    REQUIRE(eng->position() >= 2.0);
    REQUIRE(eng->state() == kanariv::types::PlaybackState::Playing);
    eng.reset();
    safeRemoveDb(dbPath);
}
