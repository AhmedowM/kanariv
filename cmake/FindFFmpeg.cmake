# FindFFmpeg.cmake -- locates FFmpeg shared build
# Provides imported targets FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil etc.
# Search order: CMAKE_PREFIX_PATH / FFmpeg_ROOT (var or env) / auto-detected from ffmpeg executable / system paths
# No hardcoded absolute paths -- set -DFFmpeg_ROOT=/path/to/ffmpeg or add ffmpeg/bin to PATH

# Auto-detect FFmpeg root from ffmpeg executable in PATH (portable, no hardcoding)
if(NOT FFmpeg_ROOT AND NOT DEFINED ENV{FFmpeg_ROOT})
  find_program(_FFmpeg_EXECUTABLE_HINT ffmpeg)
  if(_FFmpeg_EXECUTABLE_HINT)
    get_filename_component(_FFmpeg_BIN_HINT "${_FFmpeg_EXECUTABLE_HINT}" DIRECTORY)
    get_filename_component(_FFmpeg_ROOT_HINT "${_FFmpeg_BIN_HINT}/.." ABSOLUTE)
    list(APPEND _FFmpeg_SEARCH_HINTS "${_FFmpeg_ROOT_HINT}")
  endif()
  # Also check common user-local install via USERPROFILE (still env-based, not hardcoded user)
  if(DEFINED ENV{USERPROFILE})
    list(APPEND _FFmpeg_SEARCH_HINTS "$ENV{USERPROFILE}/ffmpeg")
  endif()
  if(DEFINED ENV{HOME})
    list(APPEND _FFmpeg_SEARCH_HINTS "$ENV{HOME}/ffmpeg")
  endif()
endif()

set(_ffmpeg_libs
  "AVCODEC|libavcodec/avcodec.h|avcodec libavcodec"
  "AVFORMAT|libavformat/avformat.h|avformat libavformat"
  "AVUTIL|libavutil/avutil.h|avutil libavutil"
  "SWRESAMPLE|libswresample/swresample.h|swresample libswresample"
)

foreach(_entry IN LISTS _ffmpeg_libs)
  string(REPLACE "|" ";" _parts "${_entry}")
  list(GET _parts 0 _suffix)
  list(GET _parts 1 _header)
  list(GET _parts 2 _libs)
  string(REPLACE " " ";" _libs "${_libs}")

  set(_inc_var "FFmpeg_${_suffix}_INCLUDE_DIR")
  set(_lib_var "FFmpeg_${_suffix}_LIBRARY")

  find_path(${_inc_var}
    NAMES ${_header}
    PATHS ${CMAKE_PREFIX_PATH} ${FFmpeg_ROOT} $ENV{FFmpeg_ROOT} ${_FFmpeg_SEARCH_HINTS}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
  if(NOT ${_inc_var})
    find_path(${_inc_var}
      NAMES ${_header}
      PATH_SUFFIXES include
    )
  endif()

  find_library(${_lib_var}
    NAMES ${_libs}
    PATHS ${CMAKE_PREFIX_PATH} ${FFmpeg_ROOT} $ENV{FFmpeg_ROOT} ${_FFmpeg_SEARCH_HINTS}
    PATH_SUFFIXES lib bin
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
  if(NOT ${_lib_var})
    find_library(${_lib_var}
      NAMES ${_libs}
      PATH_SUFFIXES lib bin
    )
  endif()
endforeach()

# avcodec's major is the ABI identity used by the installed kanarivConfig.cmake.
set(FFmpeg_VERSION "")
if(FFmpeg_AVCODEC_INCLUDE_DIR)
  set(_ffmpeg_version_header "${FFmpeg_AVCODEC_INCLUDE_DIR}/libavcodec/version_major.h")
  if(NOT EXISTS "${_ffmpeg_version_header}")
    set(_ffmpeg_version_header "${FFmpeg_AVCODEC_INCLUDE_DIR}/libavcodec/version.h")
  endif()
  if(EXISTS "${_ffmpeg_version_header}")
    file(STRINGS "${_ffmpeg_version_header}" _ffmpeg_major_line
      REGEX "^#define[ \t]+LIBAVCODEC_VERSION_MAJOR[ \t]+[0-9]+")
    string(REGEX MATCH "[0-9]+" FFmpeg_VERSION "${_ffmpeg_major_line}")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS
    FFmpeg_AVCODEC_LIBRARY
    FFmpeg_AVCODEC_INCLUDE_DIR
    FFmpeg_AVFORMAT_LIBRARY
    FFmpeg_AVUTIL_LIBRARY
    FFmpeg_SWRESAMPLE_LIBRARY
  VERSION_VAR FFmpeg_VERSION
)

if(FFmpeg_FOUND)
  foreach(_suffix AVCODEC AVFORMAT AVUTIL SWRESAMPLE)
    string(TOLOWER "${_suffix}" _lower)
    set(_target "FFmpeg::${_lower}")
    if(NOT TARGET ${_target})
      set(_lib_var "FFmpeg_${_suffix}_LIBRARY")
      set(_inc_var "FFmpeg_${_suffix}_INCLUDE_DIR")
      add_library(${_target} UNKNOWN IMPORTED)
      set_target_properties(${_target} PROPERTIES
        IMPORTED_LOCATION "${${_lib_var}}"
        INTERFACE_INCLUDE_DIRECTORIES "${${_inc_var}}"
      )
    endif()
  endforeach()
endif()

set(_ffmpeg_advanced "FFmpeg_AVCODEC_INCLUDE_DIR" "FFmpeg_AVCODEC_LIBRARY")
foreach(_suffix AVFORMAT AVUTIL SWRESAMPLE)
  list(APPEND _ffmpeg_advanced "FFmpeg_${_suffix}_INCLUDE_DIR" "FFmpeg_${_suffix}_LIBRARY")
endforeach()
mark_as_advanced(${_ffmpeg_advanced})
