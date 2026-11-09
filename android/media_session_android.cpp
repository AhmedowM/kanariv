#include <media_session_android.hpp>

#include <service/media_session.hpp>
#include <jni.h>
#include <jni_helpers.hpp>

#include <cstdio>
#include <cstdarg>
#include <string>
#include <memory>

namespace kanariv::media_session {

namespace {

// Stable android.media.session.PlaybackState state values. The C++ engine
// enum has different ordinals, so translate here rather than exposing the
// wire enum to Java.
constexpr jint kAndroidStateNone = 0;
constexpr jint kAndroidStateStopped = 1;
constexpr jint kAndroidStatePaused = 2;
constexpr jint kAndroidStatePlaying = 3;

jint androidPlaybackState(kanariv::types::PlaybackState state) noexcept {
    switch (state) {
    case kanariv::types::PlaybackState::Playing:
        return kAndroidStatePlaying;
    case kanariv::types::PlaybackState::Paused:
        return kAndroidStatePaused;
    case kanariv::types::PlaybackState::Stopped:
        return kAndroidStateStopped;
    default:
        return kAndroidStateNone;
    }
}

} // namespace

MediaSessionAndroid::MediaSessionAndroid() = default;

MediaSessionAndroid::~MediaSessionAndroid() {
    (void)deactivate();
}

JNIEnv* MediaSessionAndroid::jniEnv() noexcept {
    return kanariv::android::jniAttach();
}

bool MediaSessionAndroid::callCallbackVoid(const char* methodName,
                                           const char* sig, ...) {
    JNIEnv* env = jniEnv();
    if (!env)
        return false;

    jobject callback = kanariv::android::jniGetCallbackObject();
    if (!callback) {
        kanariv::android::jniDetach();
        return false;
    }

    jclass cls = env->GetObjectClass(callback);
    if (!cls) {
        env->DeleteLocalRef(callback);
        kanariv::android::jniDetach();
        return false;
    }

    jmethodID mid = env->GetMethodID(cls, methodName, sig);
    if (!mid) {
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(callback);
        kanariv::android::jniDetach();
        return false;
    }

    va_list args;
    va_start(args, sig);
    env->CallVoidMethodV(callback, mid, args);
    va_end(args);

    bool success = (env->ExceptionCheck() == JNI_FALSE);
    if (!success)
        env->ExceptionClear();

    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(callback);
    kanariv::android::jniDetach();
    return success;
}

kanariv::utils::Expected<void>
MediaSessionAndroid::activate() {
    if (active_.load(std::memory_order_acquire))
        return {};

    if (logger_)
        logger_->debug("media session android: activate");

    if (!callCallbackVoid("onMediaSessionActivate", "()V"))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Device,
            "android media session activation failed")};

    active_.store(true, std::memory_order_release);
    return {};
}

kanariv::utils::Expected<void>
MediaSessionAndroid::deactivate() {
    if (!active_.load(std::memory_order_acquire))
        return {};

    if (logger_)
        logger_->debug("media session android: deactivate");

    callCallbackVoid("onMediaSessionDeactivate", "()V");
    active_.store(false, std::memory_order_release);
    return {};
}

kanariv::utils::Expected<void>
MediaSessionAndroid::setMetadata(const Metadata& meta) {
    if (!active_.load(std::memory_order_acquire))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::State,
            "media session not active")};

    if (logger_)
        logger_->debug("media session android: setMetadata title={} artist={}",
                       meta.title, meta.artist);

    JNIEnv* env = jniEnv();
    if (!env)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "jni attach failed")};

    jobject callback = kanariv::android::jniGetCallbackObject();
    if (!callback) {
        kanariv::android::jniDetach();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "callback object not set")};
    }

    jclass cls = env->GetObjectClass(callback);
    if (!cls) {
        env->DeleteLocalRef(callback);
        kanariv::android::jniDetach();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "callback class lookup failed")};
    }

    jmethodID mid = env->GetMethodID(
        cls, "onMediaMetadata",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)V");
    if (!mid) {
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(callback);
        kanariv::android::jniDetach();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "callback method lookup failed")};
    }

    jstring jTitle = env->NewStringUTF(meta.title.c_str());
    jstring jArtist = env->NewStringUTF(meta.artist.c_str());
    jstring jAlbum = meta.album ? env->NewStringUTF(meta.album->c_str()) : nullptr;
    jstring jArtPath =
        meta.artworkPath ? env->NewStringUTF(meta.artworkPath->c_str()) : nullptr;
    jstring jArtMime =
        env->NewStringUTF(meta.artworkMime.empty() ? "image/jpeg" : meta.artworkMime.c_str());
    jstring jArtBase64 =
        meta.artworkBase64 ? env->NewStringUTF(meta.artworkBase64->c_str()) : nullptr;
    jlong jTrackId = meta.trackId;

    env->CallVoidMethod(callback, mid, jTitle, jArtist, jAlbum, jArtPath, jArtMime,
                        jArtBase64, jTrackId);

    bool success = (env->ExceptionCheck() == JNI_FALSE);
    if (!success)
        env->ExceptionClear();

    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jArtist);
    if (jAlbum)
        env->DeleteLocalRef(jAlbum);
    if (jArtPath)
        env->DeleteLocalRef(jArtPath);
    env->DeleteLocalRef(jArtMime);
    if (jArtBase64)
        env->DeleteLocalRef(jArtBase64);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(callback);
    kanariv::android::jniDetach();

    return success ? kanariv::utils::Expected<void>{}
                   : std::unexpected{kanariv::utils::makeError(
                       kanariv::utils::StatusCode::Device,
                       "android media session metadata update failed")};
}

kanariv::utils::Expected<void>
MediaSessionAndroid::setPlaybackState(
    kanariv::types::PlaybackState state, double position, double duration) {
    if (!active_.load(std::memory_order_acquire))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::State,
            "media session not active")};

    if (logger_)
        logger_->debug("media session android: setPlaybackState state={} pos={} dur={}",
                       static_cast<int>(state), position, duration);

    JNIEnv* env = jniEnv();
    if (!env)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "jni attach failed")};

    jobject callback = kanariv::android::jniGetCallbackObject();
    if (!callback) {
        kanariv::android::jniDetach();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "callback object not set")};
    }

    jclass cls = env->GetObjectClass(callback);
    if (!cls) {
        env->DeleteLocalRef(callback);
        kanariv::android::jniDetach();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "callback class lookup failed")};
    }

    jmethodID mid = env->GetMethodID(
        cls, "onMediaPlaybackState",
        "(IDD)V");
    if (!mid) {
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(callback);
        kanariv::android::jniDetach();
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Internal,
            "callback method lookup failed")};
    }

    jint jState = androidPlaybackState(state);
    jdouble jPosition = position;
    jdouble jDuration = duration;

    env->CallVoidMethod(callback, mid, jState, jPosition, jDuration);

    bool success = (env->ExceptionCheck() == JNI_FALSE);
    if (!success)
        env->ExceptionClear();

    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(callback);
    kanariv::android::jniDetach();

    return success ? kanariv::utils::Expected<void>{}
                   : std::unexpected{kanariv::utils::makeError(
                       kanariv::utils::StatusCode::Device,
                       "android media session playback state update failed")};
}

kanariv::utils::Expected<void>
MediaSessionAndroid::setPlaybackMode(bool shuffle,
                                     kanariv::types::RepeatMode repeat) {
    if (!active_.load(std::memory_order_acquire))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::State,
            "media session not active")};

    if (logger_)
        logger_->debug("media session android: setPlaybackMode shuffle={} repeat={}",
                       shuffle, static_cast<int>(repeat));

    if (!callCallbackVoid("onMediaPlaybackMode", "(ZI)V", shuffle,
                          static_cast<jint>(repeat)))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Device,
            "android media session playback mode update failed")};

    return {};
}

void MediaSessionAndroid::setLogger(kanariv::utils::Logger* logger) noexcept {
    logger_ = logger;
}

kanariv::utils::Expected<void>
MediaSessionAndroid::setVolume(float volume, bool muted) {
    if (!active_.load(std::memory_order_acquire))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::State,
            "media session not active")};

    if (logger_)
        logger_->debug("media session android: setVolume vol={} muted={}", volume, muted);

    if (!callCallbackVoid("onMediaVolume", "(FZ)V", volume, muted))
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Device,
            "android media session volume update failed")};

    return {};
}

bool MediaSessionAndroid::active() const noexcept {
    return active_.load(std::memory_order_acquire);
}

kanariv::utils::Expected<std::unique_ptr<kanariv::media_session::MediaSession>>
createAndroid(std::string appName, std::string appIcon, bool enableNotifications,
              bool enableMediaKeys) {
    (void)appName;
    (void)appIcon;
    (void)enableNotifications;
    (void)enableMediaKeys;
    // Android-specific parameters are configured on the Java/Kotlin side via
    // MediaSession.Builder and the foreground service. The C++ backend is
    // created here and wired to Java callbacks through the JNI bridge.
    return kanariv::media_session::MediaSession::wrap(
        std::make_unique<MediaSessionAndroid>());
}

} // namespace kanariv::media_session
