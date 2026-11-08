#pragma once

/**
 * @file jni_helpers.hpp
 * @brief JNI utility helpers for the Android bridge.
 * @ingroup kanariv_android
 *
 * Manages the JavaVM pointer, thread attachment/detachment, and global
 * references so C++ code running on non-Java threads (the Host worker,
 * the media-session backend) can safely call back into Java.
 */

#include <jni.h>
#include <memory>
#include <mutex>

namespace kanariv::android {

/**
 * @brief Initialize the JNI bridge with the JavaVM from JNI_OnLoad.
 * @param vm JavaVM pointer from JNI_OnLoad.
 * @callfrom Java thread (JNI_OnLoad).
 */
void jniInit(JavaVM* vm) noexcept;

/**
 * @brief Attach the current thread to the JVM if needed.
 * @return JNIEnv* for the current thread, or nullptr on failure.
 * @callfrom Any C++ thread.
 * @details The caller must match with jniDetach() if this returns non-null
 * and the thread was not previously attached.
 */
JNIEnv* jniAttach() noexcept;

/**
 * @brief Detach the current thread if it was attached by jniAttach().
 * @callfrom Any C++ thread that called jniAttach().
 */
void jniDetach() noexcept;

/**
 * @brief Set the global Java callback object (receives media-session events).
 * @param obj jobject (local ref) to promote to a global ref.
 * @callfrom Java thread.
 * @details Replaces any previously set callback object.
 */
void jniSetCallbackObject(jobject obj) noexcept;

/**
 * @brief Clear the global callback object.
 * @callfrom Any thread.
 */
void jniClearCallbackObject() noexcept;

/**
 * @brief Get the cached global callback object (local ref returned).
 * @return jobject local ref, or nullptr if not set.
 * @callfrom Any thread (thread-safe).
 * @note The returned reference is valid only for the current JNI frame.
 */
jobject jniGetCallbackObject() noexcept;

} // namespace kanariv::android
