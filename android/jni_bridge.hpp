#pragma once

/**
 * @file jni_bridge.hpp
 * @brief JNI bridge: exposes daemon control to the Android app.
 * @ingroup kanariv_android
 *
 * Declares the native functions the Java/Kotlin side calls to start/stop
 * the in-process daemon (Host) and issue playback commands. These are
 * implemented in jni_bridge.cpp and registered via JNI_OnLoad.
 */

#include <jni.h>
#include <cstdint>

namespace kanariv::android {

/**
 * @brief Register all native methods with the JVM.
 * @param env JNI environment.
 * @return JNI version on success, or negative on failure.
 * @callfrom JNI_OnLoad (Java thread).
 */
int jniRegisterNatives(JNIEnv* env) noexcept;

} // namespace kanariv::android

/**
 * @brief JNI_OnLoad entry point.
 * @param vm JavaVM pointer.
 * @param reserved Reserved.
 * @return JNI version supported.
 * @callfrom System.loadLibrary() (Java thread).
 */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved);
