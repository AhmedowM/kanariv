#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <chrono>
#include <cmath>
#include <common.hpp>
#include <cstdint>
#include <engine/fade.hpp>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
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
