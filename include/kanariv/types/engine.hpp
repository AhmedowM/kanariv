#pragma once

/**
 * @file engine.hpp
 * @brief Playback value types shared across kanariv components.
 * @ingroup kanariv_types
 *
 * @details Contains the public event, callback, configuration and persisted
 * state records used by the playback engine. The owning `Engine` class
 * remains in `kanariv::engine`.
 */

#include <cstdint>
#include <functional>
#include <kanariv/fwd.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::types {

/**
 * @brief Engine event types emitted via the internal event queue.
 * @ingroup kanariv_types
 * @see EngineEvent
 * @see kanariv::engine::Engine::pollEvent
 */
enum class EngineEventType : std::uint8_t {
    None = 0,         ///< No event.
    TrackStarted = 1, ///< New track started (pushEvent from doPlayTrack / next).
    TrackEnded = 2,   ///< Reserved (emitted via callbacks with pct).
    QueueChanged = 3, ///< Queue/shuffle switched (setShuffle/switchQueue).
    Progress = 4,     ///< Periodic progress tick (~500 ms, see engineTick).
    Error = 5         ///< Internal error forwarded to on_error.
};

/**
 * @brief Event payload queued to Engine::eventQueue_.
 * @ingroup kanariv_types
 * @details Produced by Engine::pushEvent and consumed via
 * Engine::pollEvent / drainEvents / drainAll. Progress events carry
 * position/duration; TrackStarted carries track_id/queue_id/duration.
 */
struct EngineEvent {
    EngineEventType type{EngineEventType::None}; ///< Event kind.
    int64_t track_id{0};                         ///< Affected track id (if any).
    int64_t queue_id{1};                         ///< Active queue id at emission.
    double position{0.0};                        ///< Position in seconds (Progress/TrackEnded).
    double duration{0.0};                        ///< Duration in seconds.
    std::string msg{};                           ///< Error text for Error events.
};

/**
 * @brief User callbacks invoked synchronously from pushEvent.
 * @ingroup kanariv_types
 * @details Callbacks are copied under `cbMutex_` then invoked outside
 * the event queue lock. Must be thread-safe if they touch shared state.
 * `on_track_ended` receives `(track_id, pct)` where pct is
 * `position/duration*100` clamped to [0,100].
 */
struct EngineCallbacks {
    std::function<void(int64_t track_id)> on_track_started{}; ///< Fired on TrackStarted.
    std::function<void(int64_t track_id, double pct)>
        on_track_ended{}; ///< Fired on TrackEnded with completion pct.
    std::function<void(int64_t queue_id)> on_queue_changed{}; ///< Fired on QueueChanged.
    std::function<void(kanariv::utils::StatusCode err, std::string_view msg)>
        on_error{}; ///< Fired on Error.
};

/**
 * @brief Configuration for Engine construction.
 * @ingroup kanariv_types
 * @details `pollMs` controls monitorLoop sleep (default 10 ms);
 * `gaplessMs` is the preroll window for gapless transition (default 300 ms);
 * `historyThresholdPct`/`historyThresholdSecs` are thresholds for
 * shouldMarkPlayed (defaults 60 / 90). `enableMonitorThread` gates
 * monitorLoop creation. `allowSimulatedPlayback` (default off) gates the
 * timer-only fallback for tracks whose file/decoder/device cannot be
 * opened; with it off, play/next/prev return an error instead of
 * reporting Playing with no sound.
 */
struct EngineConfig {
    bool enableMonitorThread{true};     ///< Whether to start monitorLoop.
    int pollMs{10};                     ///< Monitor poll interval in ms (playing; idle backs off to
                                        ///< >=100 ms, see Engine::monitorLoop).
    int gaplessMs{300};                 ///< Gapless preroll window in ms (300 ms).
    int historyThresholdPct{60};        ///< History pct threshold (60%).
    int historyThresholdSecs{90};       ///< History absolute seconds threshold (90 s).
    bool allowSimulatedPlayback{false}; ///< Timer-only fallback for unplayable tracks (no audio
                                        ///< device; wall-clock position). Tests/headless only:
                                        ///< with it off, play/next/prev fail instead of
                                        ///< reporting Playing with zero sound.
    /// Engine-level override populated by the service from
    /// `config::Config::autoResumeOnPlay`; not a duplicate public type.
    /// If true, play() in Stopped state resumes the persisted track/position
    /// (from DB) instead of advancing to the next queue track.
    bool autoResumeOnPlay{false};
    EngineCallbacks callbacks{}; ///< Initial callbacks (also via setCallbacks).
    /// Non-owning logger (may be null = silent). Must outlive the Engine;
    /// never emitted from destructors. Threaded through to decode paths.
    kanariv::utils::Logger* logger{nullptr};
};

/**
 * @brief Persisted playback-engine state.
 * @ingroup kanariv_types
 * @details Stores the queue cursor, repeat/shuffle settings, volume and
 * position needed to restore playback after restarting the service.
 */
struct EngineState {
    int shuffle_enabled{};
    kanariv::types::RepeatMode repeat_mode{kanariv::types::RepeatMode::Off};
    int64_t cursor_pos{};
    int64_t current_track_id{};
    float volume{1.0f};
    int64_t active_queue_id{1};
    float pre_mute_volume{0.5f};
    int64_t position_ms{};
    std::vector<int64_t> shuffle_perm;
};

} // namespace kanariv::types
