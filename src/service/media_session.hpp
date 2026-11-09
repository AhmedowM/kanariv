#pragma once

/**
 * @file media_session.hpp
 * @brief Internal OS media-session / now-playing integration for the daemon.
 * @ingroup kanariv_service
 * @details Not installed; used only by Service::Impl. Pushes playback state
 * and metadata to the OS (media keys, lock screen, MPRIS, Windows media bar)
 * and forwards OS media-key events back to the engine. Android is a stub
 * pending a Java/JNI implementation.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace kanariv::media_session {

/**
 * @brief Metadata published to the OS now-playing UI.
 * @ingroup kanariv_service
 */
struct Metadata {
    std::string title;                ///< Track title.
    std::string artist;               ///< Track artist.
    std::optional<std::string> album;        ///< Album (optional).
    std::optional<std::string> artworkPath;  ///< Local artwork file path (optional).
    std::optional<std::string> artworkBase64; ///< RFC 4648 artwork bytes.
    std::string artworkMime;                 ///< Artwork MIME type.
    std::int64_t trackId{0};          ///< kanariv track id for correlation.
};

/**
 * @brief Callbacks invoked by the OS backend on media-key events.
 * @ingroup kanariv_service
 *
 * The daemon wires these to engine commands. Callbacks may be invoked from
 * backend threads; the daemon must serialize them if it touches shared state.
 */
struct Callbacks {
    std::function<void()> onPlay{};      ///< Media key: play.
    std::function<void()> onPause{};     ///< Media key: pause.
    std::function<void()> onStop{};      ///< Media key: stop.
    std::function<void()> onNext{};      ///< Media key: next track.
    std::function<void()> onPrev{};      ///< Media key: previous track.
    std::function<void(double)> onSeek{}; ///< Media key: seek (seconds, may be negative).
    std::function<void(double)> onSeekTo{}; ///< SMTC absolute seek position.
    std::function<void(kanariv::types::RepeatMode)> onRepeat{};
    std::function<void(bool)> onShuffle{};
    /// Media key / OS mixer volume change. First parameter is linear volume
    /// 0.0-1.0; second is true when the OS requested mute.
    std::function<void(float, bool)> onVolume{};
};

/**
 * @brief Abstract backend implementation for MediaSession.
 * @ingroup kanariv_service
 *
 * Platform backends (Windows, macOS, Linux, Android) inherit from this and
 * are instantiated by the platform-specific factory in media_session.cpp.
 * Android remains a stub pending Java/JNI work.
 */
struct Impl {
    virtual ~Impl() = default;

    virtual kanariv::utils::Expected<void> activate() = 0;
    virtual kanariv::utils::Expected<void> deactivate() = 0;
    virtual kanariv::utils::Expected<void> setMetadata(const Metadata&) = 0;
    virtual kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState, double, double) = 0;
    virtual kanariv::utils::Expected<void> setPlaybackMode(
      bool, kanariv::types::RepeatMode) { return {}; }
    virtual void setLogger(kanariv::utils::Logger*) noexcept {}
    virtual kanariv::utils::Expected<void> setVolume(float, bool) = 0;
    virtual bool active() const noexcept = 0;

    void setCallbacks(Callbacks cb) noexcept { callbacks_ = std::move(cb); }
    const Callbacks& callbacks() const noexcept { return callbacks_; }

  protected:
    Callbacks callbacks_;
    bool active_{false};
};

/**
 * @class MediaSession
 * @brief Cross-platform OS media-session integration (daemon-internal).
 * @ingroup kanariv_service
 *
 * Owned by Service::Impl. One instance per daemon. Platform backends compile
 * only on their target OS; unsupported platforms get a stub that reports
 * @c StatusCode::Unsupported.
 *
 * @par Thread safety
 * Methods are safe to call from any thread; the backend serializes OS calls
 * internally. Callbacks may be invoked from backend threads.
 */
class MediaSession {
  public:
    using ExpectedSession = std::expected<std::unique_ptr<MediaSession>, kanariv::utils::Error>;

    /**
     * @brief Factory signature for an externally-provided platform backend.
     * @details Android keeps its backend in the JNI shared library
     * (`android/media_session_android.cpp`), which is linked after the core
     * service. `setAndroidFactory()` lets that library register the real
     * backend before the daemon creates its `MediaSession`.
     */
    using Factory = ExpectedSession (*)(std::string appName, std::string appIcon,
                                        bool enableNotifications, bool enableMediaKeys);

    /**
     * @brief Register the Android media-session backend factory.
     * @param factory Factory function, or nullptr to clear it.
     * @note Called from `JNI_OnLoad` in the Android JNI library. The factory
     * is only consulted by `create()` on Android; registering one on another
     * platform has no effect.
     */
    KANARIV_API static void setAndroidFactory(Factory factory) noexcept;

    /**
     * @brief Create the platform-specific backend.
     * @param appName Application name shown in OS UI.
     * @param appIcon Path to application icon (optional).
     * @param enableNotifications Show OS media notification (lock screen, etc.).
     * @param enableMediaKeys Handle media-key hardware events.
     * @return MediaSession on success, or Error if no backend is available.
     */
    static ExpectedSession create(std::string appName,
                                  std::string appIcon,
                                  bool enableNotifications,
                                  bool enableMediaKeys);

    /**
     * @brief Wrap an already-created backend implementation.
     * Used by platform factory functions.
     */
    KANARIV_API static std::unique_ptr<MediaSession> wrap(std::unique_ptr<Impl> impl) noexcept;

    KANARIV_API ~MediaSession();
    MediaSession(const MediaSession&) = delete;
    MediaSession& operator=(const MediaSession&) = delete;
    MediaSession(MediaSession&&) = delete;
    MediaSession& operator=(MediaSession&&) = delete;

    /**
     * @brief Register with the OS media subsystem.
     */
    kanariv::utils::Expected<void> activate();

    /**
     * @brief Unregister from the OS media subsystem.
     */
    kanariv::utils::Expected<void> deactivate();

    /**
     * @brief Update now-playing metadata (called on track change).
     */
    kanariv::utils::Expected<void> setMetadata(const Metadata& meta);

    /**
     * @brief Update playback state (called on play/pause/stop/seek).
     * @param state Current playback state.
     * @param position Current position in seconds.
     * @param duration Total duration in seconds.
     */
    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState state, double position, double duration);

    kanariv::utils::Expected<void> setPlaybackMode(
      bool shuffle, kanariv::types::RepeatMode repeat);

    void setLogger(kanariv::utils::Logger* logger) noexcept;

    /**
     * @brief Update volume (called on volume/mute change).
     * @param volume Linear volume 0.0-1.0.
     * @param muted Whether muted.
     */
    kanariv::utils::Expected<void> setVolume(float volume, bool muted);

    /**
     * @brief Set callbacks for OS media-key events.
     */
    void setCallbacks(Callbacks cb) noexcept;

    /**
     * @brief Whether the backend is registered and active with the OS.
     */
    [[nodiscard]] bool active() const noexcept;

  private:
    std::unique_ptr<Impl> impl_;

    explicit MediaSession(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace kanariv::media_session
