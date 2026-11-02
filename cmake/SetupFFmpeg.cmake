# SetupFFmpeg.cmake -- FFmpeg provider (kept for CI fallback path only)
#
# The default provider is system FFmpeg found by FindFFmpeg.cmake.
# The trimmed provider is built out-of-tree by scripts/build-ffmpeg.py and
# consumed via FFmpeg_ROOT or KANARIV_TRIMMED_ROOT.
#
# This fallback is only reachable when KANARIV_WITH_FETCH_FFMPEG=ON and neither
# system FFmpeg nor a trimmed prefix is available. It does no online fetching;
# the user must install FFmpeg or point FFmpeg_ROOT at a prebuilt tree.

include(FindPackageHandleStandardArgs)

function(kanariv_setup_ffmpeg)
  find_package(FFmpeg QUIET)
  if(FFmpeg_FOUND)
    message(STATUS "FFmpeg found (system): ${FFmpeg_AVCODEC_LIBRARY}")
    set(FFmpeg_FOUND TRUE PARENT_SCOPE)
    return()
  endif()

  message(FATAL_ERROR "FFmpeg is required -- install FFmpeg (tools + dev "
    "packages) or set FFmpeg_ROOT=/path/to/ffmpeg. For trimmed builds, run "
    "scripts/build-ffmpeg.py to produce ./build/ffmpeg, then configure with "
    "-DKANARIV_TRIMMED_ROOT=./build/ffmpeg.")
endfunction()
