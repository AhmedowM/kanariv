# android/CMakeLists.txt -- Android-specific native sources.
#
# This is a standalone CMake project used by Gradle's externalNativeBuild.
# It builds the JNI bridge and Android media-session backend as a shared
# library that links against the prebuilt kanariv component libraries.
#
# Usage (from android/app/build.gradle externalNativeBuild):
#   cmake {
#       path "../CMakeLists.txt"
#       arguments "-DKANARIV_ROOT=<path to kanariv repo root>",
#                 "-DANDROID_PLATFORM=android-24",
#                 "-DANDROID_STL=c++_shared"
#   }

cmake_minimum_required(VERSION 3.28)
project(kanariv_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED KANARIV_ROOT)
    # Auto-detect: this file lives in <kanariv_root>/android/, so the parent
    # directory is the kanariv repository root.
    get_filename_component(_android_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
    get_filename_component(KANARIV_ROOT "${_android_dir}/.." ABSOLUTE)
    message(STATUS "KANARIV_ROOT auto-detected: ${KANARIV_ROOT}")
endif()

# Match the prebuilt component libraries to the ABI Gradle is building.
# The arm64 tree keeps its historical name; other ABIs use android-<abi>.
if(DEFINED ANDROID_ABI AND NOT ANDROID_ABI STREQUAL "")
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(KANARIV_BUILD_DIR "${KANARIV_ROOT}/build/android-arm64")
    else()
        set(KANARIV_BUILD_DIR "${KANARIV_ROOT}/build/android-${ANDROID_ABI}")
    endif()
else()
    set(KANARIV_BUILD_DIR "${KANARIV_ROOT}/build/android-arm64")
endif()

# Verify prebuilt kanariv libraries exist (built by the main kanariv CMake for Android)
set(KANARIV_LIBS service engine db player media utils config ipc client)
foreach(_lib IN LISTS KANARIV_LIBS)
    set(_lib_path "${KANARIV_BUILD_DIR}/lib${_lib}.so")
    if(NOT EXISTS "${_lib_path}")
        message(FATAL_ERROR "Prebuilt kanariv library not found: ${_lib_path}")
    endif()
endforeach()

# Build the Android-specific shared library
add_library(kanariv_android SHARED
    jni_bridge.cpp
    jni_helpers.cpp
    media_session_android.cpp
)

target_include_directories(kanariv_android PRIVATE
    "${KANARIV_ROOT}/include"
    "${KANARIV_ROOT}/src"
    "${CMAKE_CURRENT_LIST_DIR}"
)

# Link against prebuilt kanariv component libraries + Android platform libs
foreach(_lib IN LISTS KANARIV_LIBS)
    target_link_libraries(kanariv_android PRIVATE "${KANARIV_BUILD_DIR}/lib${_lib}.so")
endforeach()
target_link_libraries(kanariv_android PRIVATE
    android
    log
    OpenSLES
)

target_compile_definitions(kanariv_android PRIVATE ANDROID)
