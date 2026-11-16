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
#include <kanariv/types/record.hpp>
#include <kanariv/types/status.hpp>
#include <kanariv/types/wire.hpp>
#include <optional>
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
    int64_t queue_id{defaultQueueId};            ///< Active queue id at emission.
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
    std::function<void(kanariv::types::StatusCode err, std::string_view msg)>
        on_error{}; ///< Fired on Error.
};

/**
 * @brief ReplayGain normalization mode.
 * @ingroup kanariv_types
 */
enum class ReplayGainMode : std::uint8_t {
    Off = 0,   ///< No replay gain (raw volume).
    Track = 1, ///< Per-track gain normalization (replay_gain_track).
    Album = 2, ///< Per-album gain normalization (replay_gain_album).
};
// Both Track and Album consume TrackRow::replayGainTrack / replayGainAlbum. Nothing
// populates those columns yet (the scanner does not analyse loudness and there is
// no tag importer), so both modes are currently no-ops.

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
 * reporting Playing with no sound. When a device is unavailable but the
 * track decodes, the engine first retries the output on miniaudio's silent
 * null backend while this flag is set, so the real decode/ring path (and
 * therefore gapless/crossfade) still runs headlessly.
 *
 * Playback polish (all configurable, default off = gapless/raw):
 * - `replayGain`: TrackRow/Album/Off normalization applied as a volume
 *   multiplier at track load time from `TrackRow::replayGainTrack` /
 *   `TrackRow::replayGainAlbum` (dB). Nothing populates those columns yet, so this
 *   setting currently has no audible effect.
 * - `fadeInMs`/`fadeOutMs`: fade envelope applied to decoded samples in the
 *   decode loop (real-time safe, no blocking).
 * - `crossfadeMs`: equal-power overlap with the next track over the last
 *   `crossfadeMs` of the outgoing one (0 = off). Arming preloads the next
 *   entry early, so the transition never advances the cursor on its own.
 * - `interTrackGapMs`: real silence between tracks (0 = gapless). The
 *   outgoing tail drains to the device, the gap is held, then the next track
 *   starts. Ignored while `crossfadeMs` is set: an overlap is not a gap.
 */
struct EngineConfig {
    bool enableMonitorThread{true};     ///< Whether to start monitorLoop.
    int pollMs{10};                     ///< Monitor poll interval in ms (playing; idle backs off to
                                        ///< >=100 ms, see Engine::monitorLoop).
    int gaplessMs{300};                 ///< Gapless preroll window in ms (300 ms).
    int historyThresholdPct{60};        ///< History pct threshold (60%).
    int historyThresholdSecs{90};       ///< History absolute seconds threshold (90 s).
    bool allowSimulatedPlayback{false}; ///< Timer-only fallback for unplayable tracks (no audio
                                        ///< device; wall-clock position) and silent null-backend
                                        ///< retry when the device is missing. Tests/headless only:
                                        ///< with it off, play/next/prev fail instead of
                                        ///< reporting Playing with zero sound.
    /// Engine-level override populated by the service from
    /// `config::Config::autoResumeOnPlay`; not a duplicate public type.
    /// If true, play() in Stopped state resumes the persisted track/position
    /// (from DB) instead of advancing to the next queue track.
    bool autoResumeOnPlay{false};
    /// ReplayGain normalization mode (default Off).
    ReplayGainMode replayGain{ReplayGainMode::Off};
    /// Fade-in duration in ms (0 = off). Applied to first samples of each track.
    int fadeInMs{0};
    /// Fade-out duration in ms (0 = off). Applied to last samples of each track.
    int fadeOutMs{0};
    /// Equal-power crossfade overlap in ms (0 = off). The incoming track is
    /// preloaded and mixed in over the last crossfadeMs of the outgoing one.
    int crossfadeMs{0};
    /// Inter-track silence in ms (0 = gapless). Delay after the outgoing tail
    /// drains and before the next track starts; ignored when crossfadeMs > 0.
    int interTrackGapMs{0};
    /// Configured playback device (opaque stable id, display name, legacy
    /// index, or "auto"). The audio output falls back to the default device,
    /// with a warning, when the reference no longer resolves.
    std::string device{"auto"};
    /// Display name paired with `device`; tried when the id is not enumerated.
    std::string deviceName{};
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
 * @par Stable identities
 * The record survives restarts and database rebuilds, so it names its targets
 * instead of storing row ids: `queue_name` and `current_path` are the
 * identities, and `current_track_id` is an optional hint that merely saves the
 * lookup when the id still resolves. Consumers resolve the name/path against
 * their own store and fall back to the default queue or no current track.
 */
struct EngineState {
    std::string queue_name{"default"};       ///< Active queue name (identity, not id).
    int64_t cursor_pos{};                    ///< Next queue position index.
    std::string current_path;                ///< Last current track path (identity).
    std::optional<int64_t> current_track_id; ///< Optional id hint for `current_path`.
    std::vector<int64_t> shuffle_perm;       ///< Shuffle permutation (queue positions).
    kanariv::types::RepeatMode repeat_mode{kanariv::types::RepeatMode::Off}; ///< Repeat mode.
    bool shuffle_enabled{};      ///< Whether shuffle is active.
    float volume{1.0f};          ///< Volume in [0,1].
    float pre_mute_volume{0.5f}; ///< Restore level for unmute in (0,1].
    int64_t position_ms{};       ///< Position in the current track in ms.
};

} // namespace kanariv::types
