#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <engine/replay_gain.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/record.hpp>

using kanariv::engine::detail::replayGainDbFor;
using kanariv::engine::detail::replayGainLinear;
using kanariv::types::ReplayGainMode;
using kanariv::types::TrackRow;

TEST_CASE("ReplayGain mode selects the stored column", "[engine_replay_gain]") {
    TrackRow t;
    t.replayGainTrack = 3.5;
    t.replayGainAlbum = -1.5;

    CHECK(replayGainDbFor(ReplayGainMode::Off, t) == 0.0);
    CHECK(replayGainDbFor(ReplayGainMode::Track, t) == 3.5);
    CHECK(replayGainDbFor(ReplayGainMode::Album, t) == -1.5);
}

TEST_CASE("Album mode falls back to the track gain when the album is unmeasured",
          "[engine_replay_gain]") {
    TrackRow t;
    t.replayGainTrack = 3.5;
    t.replayGainAlbum = 0.0;
    CHECK(replayGainDbFor(ReplayGainMode::Album, t) == 3.5);
    CHECK(replayGainDbFor(ReplayGainMode::Track, t) == 3.5);

    t.replayGainTrack = 0.0;
    CHECK(replayGainDbFor(ReplayGainMode::Album, t) == 0.0);
    CHECK(replayGainDbFor(ReplayGainMode::Track, t) == 0.0);
}

TEST_CASE("ReplayGain linear factor is 1 when no gain applies", "[engine_replay_gain]") {
    TrackRow t;
    t.replayGainTrack = 3.5;
    CHECK(replayGainLinear(ReplayGainMode::Off, t) == 1.0f);

    TrackRow unanalyzed;
    CHECK(replayGainLinear(ReplayGainMode::Track, unanalyzed) == 1.0f);
    CHECK(replayGainLinear(ReplayGainMode::Album, unanalyzed) == 1.0f);
}

TEST_CASE("ReplayGain linear factor converts dB with 10^(dB/20)", "[engine_replay_gain]") {
    TrackRow t;
    t.replayGainTrack = 6.0;
    t.replayGainAlbum = -6.0;

    CHECK(std::abs(replayGainLinear(ReplayGainMode::Track, t) - 1.99526f) < 1e-4f);
    CHECK(std::abs(replayGainLinear(ReplayGainMode::Album, t) - 0.50119f) < 1e-4f);

    t.replayGainTrack = 0.0;
    t.replayGainAlbum = 3.5;
    CHECK(std::abs(replayGainLinear(ReplayGainMode::Album, t) -
                   static_cast<float>(std::pow(10.0, 3.5 / 20.0))) < 1e-6f);
}
