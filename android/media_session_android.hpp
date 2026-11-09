#pragma once

/**
 * @file media_session_android.hpp
 * @brief Android media-session backend for kanariv.
 * @ingroup kanariv_media_session_android
 *
 * Implements `kanariv::media_session::Impl` using Android's MediaSession
 * API via JNI. Publishes playback state and metadata to the lock screen
 * and media notification, and forwards media-key events back to the daemon.
 *
 * All OS calls are made through JNI to the Java callback object registered
 * via jniSetCallbackObject(). C++ methods are thread-safe; JNI calls from
 * non-Java threads attach to the JVM as needed.
 */

#include <service/media_session.hpp>

#include <jni.h>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace kanariv::media_session {

/**
 * @class MediaSessionAndroid
 * @brief Android implementation of the media-session backend.
 * @ingroup kanariv_media_session_android
 *
 * Uses JNI to interact with `android.media.session.MediaSession`,
 * `MediaController`, `NotificationManager`, and `AudioManager`.
 * The Java side (KanarivBridge + foreground service) creates the actual
 * MediaSession and receives media-key events; this backend forwards
 * state changes to it via JNI.
 */
class MediaSessionAndroid : public Impl {
  public:
    MediaSessionAndroid();
    ~MediaSessionAndroid() override;

    MediaSessionAndroid(const MediaSessionAndroid&) = delete;
    MediaSessionAndroid& operator=(const MediaSessionAndroid&) = delete;
    MediaSessionAndroid(MediaSessionAndroid&&) = delete;
    MediaSessionAndroid& operator=(MediaSessionAndroid&&) = delete;

    // --- Impl interface ----------------------------------------------------

    kanariv::utils::Expected<void> activate() override;
    kanariv::utils::Expected<void> deactivate() override;
    kanariv::utils::Expected<void> setMetadata(const Metadata& meta) override;
    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState state, double position,
        double duration) override;
    kanariv::utils::Expected<void> setPlaybackMode(
        bool shuffle, kanariv::types::RepeatMode repeat) override;
    void setLogger(kanariv::utils::Logger* logger) noexcept override;
    kanariv::utils::Expected<void> setVolume(float volume, bool muted) override;
    [[nodiscard]] bool active() const noexcept override;

  private:
    /** @brief JNI environment accessor for the current thread. */
    JNIEnv* jniEnv() noexcept;

    /** @brief Call a void method on the registered callback object. */
    bool callCallbackVoid(const char* methodName, const char* sig, ...);

    /** @brief Java-side class that owns the MediaSession instance. */
    static constexpr const char* kCallbackClass =
        "com/kanariv/app/KanarivBridge";

    /** @brief Whether activate() has succeeded. */
    std::atomic_bool active_{false};

    /** @brief Non-owning logger. */
    kanariv::utils::Logger* logger_{nullptr};
};

/**
 * @brief Factory function creating the Android media-session backend.
 * @ingroup kanariv_media_session_android
 * @param appName Application name (handled on Java side).
 * @param appIcon Application icon path (handled on Java side).
 * @param enableNotifications Whether to show notifications (Java side).
 * @param enableMediaKeys Whether to handle media keys (Java side).
 * @return MediaSession wrapping MediaSessionAndroid, or Error.
 * @details Parameters are accepted for API compatibility with the platform
 * factory in media_session.cpp but are not used by the Android backend;
 * Android configures its MediaSession via the Java/Kotlin side.
 */
kanariv::utils::Expected<std::unique_ptr<kanariv::media_session::MediaSession>>
createAndroid(std::string appName, std::string appIcon, bool enableNotifications,
              bool enableMediaKeys);

} // namespace kanariv::media_session
