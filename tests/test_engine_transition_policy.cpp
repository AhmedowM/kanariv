#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <engine/transition_policy.hpp>
#include <limits>

using namespace kanariv::engine::detail;

namespace {

NearEndInput nearEndWithCrossfade() {
    NearEndInput in;
    in.hasCurrent = true;
    in.crossfadeMs = 3000;
    in.gaplessMs = 300;
    in.remainingSec = 10.0;
    return in;
}

NearEndInput nearEndWithGap() {
    NearEndInput in;
    in.hasCurrent = true;
    in.interTrackGapMs = 1000;
    in.gaplessMs = 300;
    in.remainingSec = 10.0;
    return in;
}

NearEndInput nearEndGapless() {
    NearEndInput in;
    in.hasCurrent = true;
    in.gaplessMs = 300;
    in.remainingSec = 10.0;
    return in;
}

EofInput eofWithGap() {
    EofInput in;
    in.interTrackGapMs = 1000;
    return in;
}

} // namespace

TEST_CASE("near-end: outside every pre-roll window nothing is armed", "[engine][transition]") {
    CHECK(nearEndAction(nearEndWithCrossfade()) == NearEndAction::None);
    CHECK(nearEndAction(nearEndWithGap()) == NearEndAction::None);
    CHECK(nearEndAction(nearEndGapless()) == NearEndAction::None);
}

TEST_CASE("near-end: crossfade arms inside its window only", "[engine][transition]") {
    auto in = nearEndWithCrossfade();
    in.remainingSec = 3.5;
    CHECK(nearEndAction(in) == NearEndAction::None);
    in.remainingSec = 3.0;
    CHECK(nearEndAction(in) == NearEndAction::ArmCrossfade);
    in.remainingSec = 0.0;
    CHECK(nearEndAction(in) == NearEndAction::ArmCrossfade);
}

TEST_CASE("near-end: crossfade flags and track presence veto arming", "[engine][transition]") {
    auto in = nearEndWithCrossfade();
    in.remainingSec = 1.0;
    in.crossfadeActive = true;
    CHECK(nearEndAction(in) == NearEndAction::None);
    in.crossfadeActive = false;
    in.crossfadePending = true;
    CHECK(nearEndAction(in) == NearEndAction::None);
    in.crossfadePending = false;
    in.hasCurrent = false;
    CHECK(nearEndAction(in) == NearEndAction::None);
}

TEST_CASE("near-end: crossfade wins over a configured gap", "[engine][transition]") {
    auto in = nearEndWithCrossfade();
    in.interTrackGapMs = 1000;
    in.remainingSec = 1.0;
    CHECK(nearEndAction(in) == NearEndAction::ArmCrossfade);
    in.remainingSec = 5.0;
    CHECK(nearEndAction(in) == NearEndAction::None);
}

TEST_CASE("near-end: a configured gap never pre-empts itself", "[engine][transition]") {
    auto in = nearEndWithGap();
    in.remainingSec = 0.0;
    CHECK(nearEndAction(in) == NearEndAction::None);
    in.remainingSec = -0.5;
    CHECK(nearEndAction(in) == NearEndAction::None);
}

TEST_CASE("near-end: negative config never falls through to gapless", "[engine][transition]") {
    auto in = nearEndGapless();
    in.remainingSec = 0.1;
    in.crossfadeMs = -1;
    CHECK(nearEndAction(in) == NearEndAction::None);
    in.crossfadeMs = 0;
    in.interTrackGapMs = -1;
    CHECK(nearEndAction(in) == NearEndAction::None);
}

TEST_CASE("near-end: gapless advances inside the preroll window", "[engine][transition]") {
    auto in = nearEndGapless();
    in.remainingSec = 0.35;
    CHECK(nearEndAction(in) == NearEndAction::None);
    in.remainingSec = 0.30;
    CHECK(nearEndAction(in) == NearEndAction::AdvanceGapless);
    in.remainingSec = 0.0;
    CHECK(nearEndAction(in) == NearEndAction::AdvanceGapless);
    in.hasCurrent = false;
    CHECK(nearEndAction(in) == NearEndAction::None);
}

TEST_CASE("near-end: past the end or NaN remaining arms nothing", "[engine][transition]") {
    CHECK(nearEndAction([] {
              auto in = nearEndWithCrossfade();
              in.remainingSec = -0.1;
              return in;
          }()) == NearEndAction::None);
    CHECK(nearEndAction([] {
              auto in = nearEndGapless();
              in.remainingSec = std::numeric_limits<double>::quiet_NaN();
              return in;
          }()) == NearEndAction::None);
}

TEST_CASE("EOF: a crossfade owns the transition", "[engine][transition]") {
    EofInput in;
    in.crossfadeActive = true;
    in.crossfadeMs = 3000;
    CHECK(eofAction(in) == EofAction::PublishCrossfade);
    // Active wins even if the configuration changed mid-transition.
    in.crossfadeMs = 0;
    CHECK(eofAction(in) == EofAction::PublishCrossfade);
}

TEST_CASE("EOF: an armed-but-unfinished crossfade holds the cursor", "[engine][transition]") {
    EofInput in;
    in.crossfadeMs = 3000;
    in.interTrackGapMs = 1000;
    CHECK(eofAction(in) == EofAction::HoldForCrossfade);
}

TEST_CASE("EOF: the inter-track gap waits for the drain, then the clock", "[engine][transition]") {
    auto in = eofWithGap();
    CHECK(eofAction(in) == EofAction::WaitDrain);
    in.gapTailDrained = true;
    CHECK(eofAction(in) == EofAction::ArmGap);
    in.gapArmed = true;
    in.gapTailDrained = false;
    CHECK(eofAction(in) == EofAction::WaitGap);
    in.gapElapsed = true;
    CHECK(eofAction(in) == EofAction::Advance);
}

TEST_CASE("EOF: without a crossfade or gap the queue advances", "[engine][transition]") {
    EofInput in;
    in.crossfadeMs = 0;
    in.interTrackGapMs = 0;
    CHECK(eofAction(in) == EofAction::Advance);
}
