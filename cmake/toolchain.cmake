# kanariv toolchain dispatcher (CMAKE_TOOLCHAIN_FILE).
# Read before project(): selects one of the per-toolchain detail files.
# Deliberately contains no project logic -- auto detection lives in
# toolchains/auto.cmake, which delegates to the same detail files.
set(KANARIV_TOOLCHAIN "auto" CACHE STRING
  "Toolchain to configure: auto|gcc|clang|msvc|apple")
set_property(CACHE KANARIV_TOOLCHAIN PROPERTY STRINGS auto gcc clang msvc apple)

set(_kanariv_toolchains auto gcc clang msvc apple)
if(NOT KANARIV_TOOLCHAIN IN_LIST _kanariv_toolchains)
  message(FATAL_ERROR
    "Unknown KANARIV_TOOLCHAIN='${KANARIV_TOOLCHAIN}'. "
    "Valid values: auto, gcc, clang, msvc, apple.")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/toolchains/${KANARIV_TOOLCHAIN}.cmake")
