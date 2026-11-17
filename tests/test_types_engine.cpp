// T1 regression: the value layer must not depend on `utils`. This translation
// unit includes the value-layer engine header FIRST, before any utils header,
// so it stays self-contained. `utils/status_format.hpp` owns the `std::formatter`
// specialization for `StatusCode`; the static assertion below flips if
// `types/engine.hpp` ever re-gains that include (the historical types <-> utils
// cycle), turning the regression into a compile error.
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <functional>
#include <kanariv/types/engine.hpp>
#include <string_view>
#include <type_traits>

namespace {

static_assert(!std::is_default_constructible_v<std::formatter<kanariv::types::StatusCode>>,
              "types/engine.hpp must not pull in utils/status_format.hpp");

} // namespace

TEST_CASE("types/engine.hpp stands alone without utils", "[types][engine]") {
    kanariv::types::EngineConfig cfg;
    cfg.pollMs = 5;
    cfg.replayGain = kanariv::types::ReplayGainMode::Track;

    kanariv::types::EngineCallbacks callbacks;
    // The callback error code is owned by the value layer, not `utils`.
    static_assert(
        std::is_same_v<decltype(callbacks.on_error),
                       std::function<void(kanariv::types::StatusCode, std::string_view)>>);

    callbacks.on_error = [](kanariv::types::StatusCode code, std::string_view msg) {
        REQUIRE(code == kanariv::types::StatusCode::State);
        REQUIRE(msg == "boom");
    };
    cfg.callbacks = std::move(callbacks);
    cfg.callbacks.on_error(kanariv::types::StatusCode::State, "boom");

    kanariv::types::EngineEvent event;
    event.type = kanariv::types::EngineEventType::Progress;
    event.position = 1.5;
    REQUIRE(event.type == kanariv::types::EngineEventType::Progress);

    kanariv::types::EngineState state;
    state.repeat_mode = kanariv::types::RepeatMode::One;
    REQUIRE(state.repeat_mode == kanariv::types::RepeatMode::One);
    REQUIRE(cfg.pollMs == 5);
    REQUIRE(cfg.replayGain == kanariv::types::ReplayGainMode::Track);
}
