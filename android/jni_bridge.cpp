#include <jni_bridge.hpp>

#include <kanariv/config/runtime.hpp>
#include <kanariv/contracts/database.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/engine/engine.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/audio_ext.hpp>
#include <kanariv/service/host.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>

#include <filesystem>
#include <fstream>
#include <jni.h>
#include <jni_helpers.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

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

/** @brief Converts a StatusCode to a Java-friendly string. */
[[maybe_unused]] std::string statusCodeToString(kanariv::utils::StatusCode code) {
    return std::string(kanariv::utils::toString(code));
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
 * @brief Issue a play command to the engine.
 * @callfrom Java thread.
 */
static void nativePlay(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host) {
        auto& eng = g_host->service().engine();
        LOGD("nativePlay: state=%d curTrackId=%ld activeQueueId=%ld",
             static_cast<int>(eng.state()), eng.currentTrackId(), eng.activeQueueId());
        (void)eng.play(eng.activeQueueId());
    } else {
        LOGE("nativePlay: g_host is null");
    }
}

/**
 * @brief Issue a pause command to the engine.
 * @callfrom Java thread.
 */
static void nativePause(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        (void)g_host->service().engine().pause();
}

/**
 * @brief Issue a stop command to the engine.
 * @callfrom Java thread.
 */
static void nativeStop(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        (void)g_host->service().engine().stop();
}

/**
 * @brief Skip to the next track.
 * @callfrom Java thread.
 */
static void nativeNext(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        (void)g_host->service().engine().next();
}

/**
 * @brief Skip to the previous track.
 * @callfrom Java thread.
 */
static void nativePrev(JNIEnv*, jclass) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        (void)g_host->service().engine().prev();
}

/**
 * @brief Seek by a relative delta (seconds).
 * @param delta Seconds to seek (may be negative).
 * @callfrom Java thread.
 */
static void nativeSeek(JNIEnv*, jclass, jdouble delta) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host) {
        auto cur = g_host->service().engine().position();
        (void)g_host->service().engine().seek(cur + delta);
    }
}

/**
 * @brief Seek to an absolute position (seconds).
 * @param position Seconds from the start of the track.
 * @callfrom Java thread.
 */
static void nativeSeekTo(JNIEnv*, jclass, jdouble position) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        (void)g_host->service().engine().seek(position);
}

/**
 * @brief Set playback volume.
 * @param volume Linear volume 0.0-1.0.
 * @callfrom Java thread.
 */
static void nativeSetVolume(JNIEnv*, jclass, jfloat volume) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        g_host->service().engine().setVolume(volume);
}

/**
 * @brief Set repeat mode.
 * @param repeat Ordinal of RepeatMode (off=0, one=1, all=2).
 * @callfrom Java thread.
 */
static void nativeSetRepeat(JNIEnv*, jclass, jint repeat) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (!g_host)
        return;
    switch (repeat) {
        case 0:
            (void)g_host->service().engine().setRepeat(kanariv::types::RepeatMode::Off);
            break;
        case 1:
            (void)g_host->service().engine().setRepeat(kanariv::types::RepeatMode::All);
            break;
        case 2:
            (void)g_host->service().engine().setRepeat(kanariv::types::RepeatMode::One);
            break;
        default:
            break;
    }
}

/**
 * @brief Set shuffle mode.
 * @param shuffle JNI boolean.
 * @callfrom Java thread.
 */
static void nativeSetShuffle(JNIEnv*, jclass, jboolean shuffle) noexcept {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    if (g_host)
        (void)g_host->service().engine().setShuffle(shuffle == JNI_TRUE);
}

/**
 * @brief Add a single audio file to the library.
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

    LOGD("nativeLibraryAdd: file valid, extracting metadata");

    kanariv::types::Track t;
    t.path = pathStr;

    if (auto meta = kanariv::media::extractMetadata(pathStr); meta) {
        t.title = std::move(meta->title);
        t.artist = std::move(meta->artist);
        t.album = std::move(meta->album);
        t.album_artist = std::move(meta->album_artist);
        t.genre = std::move(meta->genre);
        t.year = meta->year;
        t.track_num = meta->track_num;
        t.disc_num = meta->disc_num;
        if (meta->duration > 0)
            t.duration = meta->duration;
        t.sample_rate = static_cast<uint32_t>(meta->sample_rate);
        t.channels = static_cast<uint32_t>(meta->channels);
        t.bitrate = meta->bitrate;
        LOGD("nativeLibraryAdd: metadata extracted title=%s artist=%s duration=%.1f",
             t.title.c_str(), t.artist.c_str(), t.duration);
    } else {
        LOGD("nativeLibraryAdd: metadata extraction failed: %s",
             meta.error().message.c_str());
    }

    auto sz = std::filesystem::file_size(p, ec);
    if (!ec)
        t.size = static_cast<int64_t>(sz);
    auto ftime = std::filesystem::last_write_time(p, ec);
    if (!ec)
        t.mtime = static_cast<int64_t>(ftime.time_since_epoch().count());

    LOGD("nativeLibraryAdd: calling insertTrack path=%s size=%ld",
         t.path.c_str(), t.size);

    auto ins = g_host->service().db().insertTrack(t);
    if (!ins) {
        LOGE("nativeLibraryAdd: insertTrack failed: %s", ins.error().message.c_str());
        if (ins.error().code == kanariv::utils::StatusCode::AlreadyExists) {
            auto existing = g_host->service().db().findByPath(t.path);
            if (existing) {
                int64_t trackId = existing->id;
                LOGD("nativeLibraryAdd: track already exists, trackId=%ld", trackId);
                int64_t qid = g_host->service().engine().activeQueueId();
                auto enq = g_host->service().db().queueEnqueue(qid, trackId, true);
                if (!enq) {
                    LOGD("nativeLibraryAdd: queueEnqueue failed: %s", enq.error().message.c_str());
                } else {
                    LOGD("nativeLibraryAdd: enqueued existing trackId=%ld into queue %ld", trackId, qid);
                }
                return JNI_TRUE;
            }
        }
        return JNI_FALSE;
    }
    int64_t trackId = *ins;
    LOGD("nativeLibraryAdd: insertTrack success trackId=%ld", trackId);

    // Enqueue the track into the active playback queue so it can be played.
    int64_t qid = g_host->service().engine().activeQueueId();
    auto enq = g_host->service().db().queueEnqueue(qid, trackId, true);
    if (!enq) {
        LOGD("nativeLibraryAdd: queueEnqueue failed: %s", enq.error().message.c_str());
    } else {
        LOGD("nativeLibraryAdd: enqueued trackId=%ld into queue %ld", trackId, qid);
    }

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
    {"nativeSeek", "(D)V", reinterpret_cast<void*>(nativeSeek)},
    {"nativeSeekTo", "(D)V", reinterpret_cast<void*>(nativeSeekTo)},
    {"nativeSetVolume", "(F)V", reinterpret_cast<void*>(nativeSetVolume)},
    {"nativeSetRepeat", "(I)V", reinterpret_cast<void*>(nativeSetRepeat)},
    {"nativeSetShuffle", "(Z)V", reinterpret_cast<void*>(nativeSetShuffle)},
    {"nativeLibraryAdd", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(nativeLibraryAdd)},
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
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    if (kanariv::android::jniRegisterNatives(env) < 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
