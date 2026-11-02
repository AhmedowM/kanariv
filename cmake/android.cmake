# Android NDK toolchain wrapper for kanariv.
#
# The NDK toolchain does the compiler/sysroot/API setup. This wrapper keeps
# Android-specific project defaults in one place and remains usable from both
# CMakePresets.json and scripts/configure.py.
if(NOT DEFINED CMAKE_ANDROID_NDK OR CMAKE_ANDROID_NDK STREQUAL "")
  if(DEFINED ENV{ANDROID_NDK_ROOT})
    set(CMAKE_ANDROID_NDK "$ENV{ANDROID_NDK_ROOT}" CACHE PATH
      "Android NDK root" FORCE)
  elseif(DEFINED ENV{ANDROID_NDK_HOME})
    set(CMAKE_ANDROID_NDK "$ENV{ANDROID_NDK_HOME}" CACHE PATH
      "Android NDK root" FORCE)
  else()
    message(FATAL_ERROR
      "Android NDK not found. Set CMAKE_ANDROID_NDK or ANDROID_NDK_ROOT.")
  endif()
endif()

# CMake re-runs the toolchain for compiler ABI probes. Preserve the NDK
# selection for those nested projects before loading the NDK toolchain.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_ANDROID_NDK)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  "${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES}" CACHE STRING
  "Android platform variables propagated to try_compile" FORCE)

set(KANARIV_BUILD_CLI OFF CACHE BOOL
  "Build the kanariv executable" FORCE)
set(KANARIV_BUILD_STATIC OFF CACHE BOOL
  "Build static component libraries" FORCE)
set(KANARIV_BUILD_SHARED ON CACHE BOOL
  "Build per-component shared libraries" FORCE)
set(KANARIV_ENABLE_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(KANARIV_ENABLE_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(KANARIV_ENABLE_MODULES OFF CACHE BOOL
  "Build/install C++23 module interfaces" FORCE)
set(KANARIV_BUILD_DOCS OFF CACHE BOOL
  "Build Doxygen documentation" FORCE)
set(KANARIV_ENABLE_SANITIZERS OFF CACHE BOOL
  "Enable sanitizers" FORCE)

include("${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
