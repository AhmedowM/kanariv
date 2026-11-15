# FFmpegTrimmed.cmake -- decode-only FFmpeg whitelist (post-1.0 packaging track)
#
# Provides the FFmpeg trim flag list consumed by scripts/build-ffmpeg.py and
# validates that a prebuilt trimmed tree exists under FFmpeg_ROOT.
#
# The trimmed tree is produced by scripts/build-ffmpeg.py into ./build/ffmpeg
# (default prefix). CMake only consumes it; it never builds FFmpeg itself.
#
# Consumption:
#   1. CI restores the cached install prefix into KANARIV_TRIMMED_ROOT
#      (key: hash of cmake/ffmpeg_trimmed_flags.txt), then configures with
#      -DKANARIV_USE_TRIMMED_FFMPEG=ON. find_package consumes the prefix.
#   2. Local iteration: -DKANARIV_USE_TRIMMED_FFMPEG=ON with
#      -DKANARIV_TRIMMED_ROOT=/path/to/trimmed/ffmpeg (default: ./build/ffmpeg).

# Pinned source matching the audited inventory. Override with
# -DKANARIV_FFMPEG_TRIM_VERSION=x.y.z if the audit moves.
set(KANARIV_FFMPEG_TRIM_VERSION "8.1.2" CACHE STRING "pinned FFmpeg version for trimmed builds")
set(KANARIV_FFMPEG_TRIM_URL "https://ffmpeg.org/releases/ffmpeg-${KANARIV_FFMPEG_TRIM_VERSION}.tar.xz"
    CACHE STRING "source tarball for trimmed FFmpeg builds")

set(KANARIV_FFMPEG_TRIM_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg"
    CACHE PATH "install prefix for the trimmed FFmpeg build")

# Decode-only whitelist flags. One source of truth used by
# scripts/build-ffmpeg.py and cmake/FFmpegTrimmed.cmake. Flags live in
# cmake/ffmpeg_trimmed_flags.txt (one per line) to avoid regex parsing.
file(READ "${CMAKE_CURRENT_LIST_DIR}/ffmpeg_trimmed_flags.txt" _trim_flags_raw)
separate_arguments(FFMPEG_TRIM_FLAGS NATIVE_COMMAND "${_trim_flags_raw}")
unset(_trim_flags_raw)
set(KANARIV_FFMPEG_TRIM_FLAGS "${FFMPEG_TRIM_FLAGS}")

# Locates the prebuilt trimmed tree at ${trim_root}. The root is passed in as a
# normal variable (resolved by the caller from FFmpeg_ROOT / KANARIV_TRIMMED_ROOT
# / the default prefix) so no user-facing cache entry is rewritten (W18).
function(kanariv_setup_trimmed_ffmpeg trim_root)
  # Drop stale hits: find_library/include results persist in cache across
  # reconfigures, so a previous system/Gyan hit would shadow the trimmed tree
  # even after the root moves.
  foreach(_ff_var IN ITEMS FFmpeg_AVCODEC_INCLUDE_DIR FFmpeg_AVCODEC_LIBRARY
      FFmpeg_AVFORMAT_LIBRARY FFmpeg_AVUTIL_LIBRARY FFmpeg_SWRESAMPLE_LIBRARY)
    unset(${_ff_var} CACHE)
  endforeach()
  unset(FFmpeg_FOUND)
  unset(FFmpeg_FOUND CACHE)

  if(MSVC)
    message(FATAL_ERROR "KANARIV_USE_TRIMMED_FFMPEG needs a prebuilt trimmed tree "
      "under FFmpeg_ROOT with MSVC (upstream FFmpeg has no native MSVC build) -- "
      "use MinGW or the default provider")
  endif()

  # Function-local variable: FindFFmpeg prefers it over its default search.
  set(FFmpeg_ROOT "${trim_root}")
  find_package(FFmpeg QUIET)
  if(FFmpeg_FOUND)
    message(STATUS "Trimmed FFmpeg found: ${FFmpeg_AVCODEC_LIBRARY}")
    set(FFmpeg_FOUND TRUE PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR "Trimmed FFmpeg not found under '${trim_root}'. Build it first with "
    "scripts/build-ffmpeg.py (default prefix ./build/ffmpeg), then configure "
    "with -DKANARIV_TRIMMED_ROOT=./build/ffmpeg, or disable with "
    "-DKANARIV_USE_TRIMMED_FFMPEG=OFF to use system FFmpeg.")
endfunction()
