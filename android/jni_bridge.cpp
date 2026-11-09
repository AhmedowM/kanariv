#include <jni_bridge.hpp>
#include <media_session_android.hpp>

#include <kanariv/config/runtime.hpp>
#include <kanariv/contracts/database.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/service/host.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>

#include <filesystem>
#include <jni.h>
#include <jni_helpers.hpp>
#include <memory>
#include <mutex>
#include <string>

#include <android/log.h>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "KanarivBridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KanarivBridge", __VA_ARGS__)

namespace kanariv::android {

namespace {

/**
 * @brief The in-process daemon host, owned by the bridge.
 * @details Only one daemon instance exists per app process. Created by
 * nativeStartDaemon and destroyed by nativeStopDaemon.
 */
std::unique_ptr<kanariv::service::Host> g_host;
std::mutex g_hostMutex;

const char* kBridgeClass = "com/kanariv/app/KanarivBridge";

/**
 * @brief Dispatch an IPC command through the service and return whether it
 * succeeded.
 * @callfrom Java thread (serialized via g_hostMutex).
 */
[[maybe_unused]] bool dispatchCmd(kanariv::ipc::Command cmd) noexcept {
    if (!g_host)
        return false;
    auto res = g_host->service().dispatchCommand(cmd);
    if (!res) {
        LOGE("dispatchCommand failed: %s", res.error().message.c_str());
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Native methods called from Java
// ---------------------------------------------------------------------------

/**
 * @brief Start the in-process daemon.
 * @param env JNI environment.
 * @param clazz Java class reference.
 * @param dbPath Path to the SQLite database file.
 * @param logsPath Path to the log directory.
 * @param configPath Optional path to the configuration file.
 * @return Pointer to the created Host (as jlong), or 0 on failure.
 * @callfrom Java thread.
 */
static jlong nativeStartDaemon(JNIEnv* env, jclass clazz,
                               jstring dbPath, jstring logsPath,
                               jstring configPath) noexcept {
    (void)clazz;
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        return reinterpret_cast<jlong>(g_host.get());

    auto getStr = [env](jstring s) -> std::string {
        if (!s)
            return {};
        const char* cstr = env->GetStringUTFChars(s, nullptr);
        std::string result(cstr ? cstr : "");
        if (cstr)
            env->ReleaseStringUTFChars(s, cstr);
        return result;
    };

    kanariv::service::ServiceConfig cfg;
    cfg.inProcess = true;
    cfg.logToStderr = false;

    kanariv::config::RuntimePaths runtime;
    runtime.database = getStr(dbPath);
    runtime.logs = getStr(logsPath);
    runtime.configuration = getStr(configPath);
    cfg.runtime = runtime;

    // Ensure runtime directories exist before creating the service.
    if (auto ec = runtime.ensureDirectories(); !ec) {
        if (env) {
            jclass ieCls = env->FindClass("java/io/IOException");
            if (ieCls)
                env->ThrowNew(ieCls, ec.error().message.c_str());
        }
        return 0;
    }

    // Open the database before creating the service.
    auto dbRes = kanariv::db::Database::open(runtime.database.string());
    if (!dbRes) {
        LOGE("nativeStartDaemon: database open failed: %s", dbRes.error().message.c_str());
        return 0;
    }

    auto hostRes = kanariv::service::Host::create(cfg, std::move(dbRes.value()));
    if (!hostRes) {
        if (env) {
            jclass reCls = env->FindClass("java/lang/RuntimeException");
            if (reCls)
                env->ThrowNew(reCls, hostRes.error().message.c_str());
        }
        return 0;
    }

    g_host = std::move(*hostRes);
    return reinterpret_cast<jlong>(g_host.get());
}

/**
 * @brief Stop the in-process daemon.
 * @callfrom Java thread.
 */
static void nativeStopDaemon(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host) {
        g_host->stop();
        g_host.reset();
    }
    kanariv::android::jniClearCallbackObject();
}

/**
 * @brief Query whether the daemon is running.
 * @return JNI true if the host exists and the service loop is running.
 * @callfrom Java thread.
 */
static jboolean nativeIsDaemonRunning(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    return g_host && g_host->running() ? JNI_TRUE : JNI_FALSE;
}

/**
 * @brief Issue a play command via IPC.
 * @callfrom Java thread.
 */
static void nativePlay(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativePlay: g_host is null");
        return;
    }
    dispatchCmd(kanariv::ipc::Play{});
}

/**
 * @brief Issue a pause command via IPC.
 * @callfrom Java thread.
 */
static void nativePause(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativePause: g_host is null");
        return;
    }
    dispatchCmd(kanariv::ipc::Pause{});
}

/**
 * @brief Issue a stop command via IPC.
 * @callfrom Java thread.
 */
static void nativeStop(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeStop: g_host is null");
        return;
    }
    dispatchCmd(kanariv::ipc::Stop{});
}

/**
 * @brief Skip to the next track via IPC.
 * @callfrom Java thread.
 */
static void nativeNext(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeNext: g_host is null");
        return;
    }
    dispatchCmd(kanariv::ipc::Next{});
}

/**
 * @brief Skip to the previous track via IPC.
 * @callfrom Java thread.
 */
static void nativePrev(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativePrev: g_host is null");
        return;
    }
    dispatchCmd(kanariv::ipc::Prev{});
}

/**
 * @brief Toggle play/pause based on the engine's current state.
 * @details Used by headset media buttons and the notification's play/pause
 * action, where a single button has to decide between Play and Pause.
 * @callfrom Java thread.
 */
static void nativeTogglePlayPause(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeTogglePlayPause: g_host is null");
        return;
    }
    if (g_host->service().engine().state() == kanariv::types::PlaybackState::Playing)
        dispatchCmd(kanariv::ipc::Pause{});
    else
        dispatchCmd(kanariv::ipc::Play{});
}

/**
 * @brief Register the Java callback object that receives media-session events.
 * @param instance KanarivBridge instance, or null to clear it.
 * @callfrom Java thread.
 */
static void nativeSetCallbackInstance(JNIEnv*, jclass, jobject instance) noexcept {
    if (instance == nullptr)
        kanariv::android::jniClearCallbackObject();
    else
        kanariv::android::jniSetCallbackObject(instance);
}

/**
 * @brief Seek by a relative delta (seconds) via IPC.
 * @param delta Seconds to seek (may be negative).
 * @callfrom Java thread.
 */
static void nativeSeek(JNIEnv*, jclass, jdouble delta) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeSeek: g_host is null");
        return;
    }
    kanariv::ipc::Seek cmd;
    cmd.seconds = delta;
    dispatchCmd(std::move(cmd));
}

/**
 * @brief Seek to an absolute position (seconds) via IPC.
 * @param position Seconds from the start of the track.
 * @callfrom Java thread.
 */
static void nativeSeekTo(JNIEnv*, jclass, jdouble position) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeSeekTo: g_host is null");
        return;
    }
    kanariv::ipc::Seek cmd;
    cmd.seconds = position;
    dispatchCmd(std::move(cmd));
}

/**
 * @brief Set playback volume via IPC.
 * @param volume Linear volume 0.0-1.0.
 * @callfrom Java thread.
 */
static void nativeSetVolume(JNIEnv*, jclass, jfloat volume) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeSetVolume: g_host is null");
        return;
    }
    kanariv::ipc::VolumeSet cmd;
    cmd.level = static_cast<float>(volume) * 100.0f;
    dispatchCmd(std::move(cmd));
}

/**
 * @brief Set repeat mode via IPC.
 * @param repeat Ordinal of RepeatMode (off=0, one=1, all=2).
 * @callfrom Java thread.
 */
static void nativeSetRepeat(JNIEnv*, jclass, jint repeat) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeSetRepeat: g_host is null");
        return;
    }
    kanariv::ipc::QueueRepeat cmd;
    switch (repeat) {
        case 0:
            cmd.mode = kanariv::types::RepeatMode::Off;
            break;
        case 1:
            cmd.mode = kanariv::types::RepeatMode::All;
            break;
        case 2:
            cmd.mode = kanariv::types::RepeatMode::One;
            break;
        default:
            return;
    }
    dispatchCmd(std::move(cmd));
}

/**
 * @brief Set shuffle mode via IPC.
 * @param shuffle JNI boolean.
 * @callfrom Java thread.
 */
static void nativeSetShuffle(JNIEnv*, jclass, jboolean shuffle) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeSetShuffle: g_host is null");
        return;
    }
    kanariv::ipc::QueueShuffle cmd;
    cmd.on = shuffle == JNI_TRUE;
    dispatchCmd(std::move(cmd));
}

/**
 * @brief Add a single audio file to the library via IPC.
 * @param path Absolute filesystem path to an audio file (already copied into
 *             app-private storage by the Java side).
 * @return JNI true on success, false on failure.
 * @callfrom Java thread.
 */
static jboolean nativeLibraryAdd(JNIEnv* env, jclass, jstring path) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host) {
        LOGE("nativeLibraryAdd: g_host is null");
        return JNI_FALSE;
    }
    LOGD("nativeLibraryAdd: g_host is set");

    const char* cstr = env->GetStringUTFChars(path, nullptr);
    std::string pathStr(cstr ? cstr : "");
    if (cstr)
        env->ReleaseStringUTFChars(path, cstr);

    LOGD("nativeLibraryAdd: path=%s", pathStr.c_str());

    std::filesystem::path p = pathStr;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
        LOGE("nativeLibraryAdd: file does not exist or is not regular: %s ec=%s",
             pathStr.c_str(), ec.message().c_str());
        return JNI_FALSE;
    }
    if (!kanariv::media::hasAudioExt(p)) {
        LOGE("nativeLibraryAdd: not an audio file: %s", pathStr.c_str());
        return JNI_FALSE;
    }

    LOGD("nativeLibraryAdd: sending QueueAdd command via IPC");

    kanariv::ipc::QueueAdd cmd;
    cmd.query = pathStr;
    cmd.search = false;

    if (!dispatchCmd(std::move(cmd))) {
        LOGE("nativeLibraryAdd: QueueAdd command failed");
        return JNI_FALSE;
    }

    LOGD("nativeLibraryAdd: QueueAdd command succeeded");
    return JNI_TRUE;
}

// ---------------------------------------------------------------------------
// JNI native method table
// ---------------------------------------------------------------------------

static JNINativeMethod kMethods[] = {
    {"nativeStartDaemon", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J",
     reinterpret_cast<void*>(nativeStartDaemon)},
    {"nativeStopDaemon", "()V", reinterpret_cast<void*>(nativeStopDaemon)},
    {"nativeIsDaemonRunning", "()Z", reinterpret_cast<void*>(nativeIsDaemonRunning)},
    {"nativePlay", "()V", reinterpret_cast<void*>(nativePlay)},
    {"nativePause", "()V", reinterpret_cast<void*>(nativePause)},
    {"nativeStop", "()V", reinterpret_cast<void*>(nativeStop)},
    {"nativeNext", "()V", reinterpret_cast<void*>(nativeNext)},
    {"nativePrev", "()V", reinterpret_cast<void*>(nativePrev)},
    {"nativeTogglePlayPause", "()V", reinterpret_cast<void*>(nativeTogglePlayPause)},
    {"nativeSeek", "(D)V", reinterpret_cast<void*>(nativeSeek)},
    {"nativeSeekTo", "(D)V", reinterpret_cast<void*>(nativeSeekTo)},
    {"nativeSetVolume", "(F)V", reinterpret_cast<void*>(nativeSetVolume)},
    {"nativeSetRepeat", "(I)V", reinterpret_cast<void*>(nativeSetRepeat)},
    {"nativeSetShuffle", "(Z)V", reinterpret_cast<void*>(nativeSetShuffle)},
    {"nativeLibraryAdd", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(nativeLibraryAdd)},
    {"nativeSetCallbackInstance", "(Lcom/kanariv/app/KanarivBridge;)V",
     reinterpret_cast<void*>(nativeSetCallbackInstance)},
};

} // namespace kanariv::android

int kanariv::android::jniRegisterNatives(JNIEnv* env) noexcept {
    jclass cls = env->FindClass(kBridgeClass);
    if (!cls)
        return -1;
    if (env->RegisterNatives(cls, kMethods,
                             sizeof(kMethods) / sizeof(kMethods[0])) != 0)
        return -1;
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    kanariv::android::jniInit(vm);
    // Register the Android media-session backend before the service creates
    // its MediaSession. The backend lives in this JNI library, not libservice.
    kanariv::media_session::MediaSession::setAndroidFactory(
        &kanariv::media_session::createAndroid);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    if (kanariv::android::jniRegisterNatives(env) < 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
