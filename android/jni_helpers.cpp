#include <jni_helpers.hpp>

#include <jni.h>
#include <mutex>

namespace kanariv::android {

namespace {

JavaVM* g_jvm{nullptr};
std::mutex g_callbackMutex;
jobject g_callbackObj{nullptr};
// Per-thread flag: only detach a thread that this helper attached itself.
thread_local bool t_detachOnExit{false};

} // namespace

void jniInit(JavaVM* vm) noexcept {
    g_jvm = vm;
}

JNIEnv* jniAttach() noexcept {
    if (!g_jvm)
        return nullptr;
    JNIEnv* env = nullptr;
    jint rc = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        rc = g_jvm->AttachCurrentThread(&env, nullptr);
        if (rc != JNI_OK)
            return nullptr;
        t_detachOnExit = true;
    } else if (rc != JNI_OK) {
        return nullptr;
    }
    return env;
}

void jniDetach() noexcept {
    if (t_detachOnExit && g_jvm) {
        g_jvm->DetachCurrentThread();
        t_detachOnExit = false;
    }
}

void jniSetCallbackObject(jobject obj) noexcept {
    JNIEnv* env = jniAttach();
    if (!env)
        return;
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (g_callbackObj)
        env->DeleteGlobalRef(g_callbackObj);
    g_callbackObj = obj ? env->NewGlobalRef(obj) : nullptr;
    jniDetach();
}

void jniClearCallbackObject() noexcept {
    jniSetCallbackObject(nullptr);
}

jobject jniGetCallbackObject() noexcept {
    if (!g_jvm)
        return nullptr;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    if (!g_callbackObj)
        return nullptr;
    return env->NewLocalRef(g_callbackObj);
}

} // namespace kanariv::android
