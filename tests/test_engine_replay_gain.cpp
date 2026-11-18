#include <catch2/catch_test_macros.hpp>
#include <engine/replay_gain.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/record.hpp>

using kanariv::engine::detail::replayGainDbFor;
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
