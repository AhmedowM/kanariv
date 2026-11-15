# Compiler policy backstop: the intent presets guide selection, this enforces
# the hard rules for raw configures that bypass the resolver.
#
# macOS: AppleClang (default via `auto`, used for release artifacts) and
# upstream LLVM Clang (--toolchain clang) are both supported. GCC is the only
# rejected compiler: miniaudio loses its CoreAudio backend there, because
# Apple's ObjC-blocks SDK headers do not parse under GCC
# (see cmake/components/player.cmake), so audio output is unavailable.
option(KANARIV_ALLOW_UNSUPPORTED_COMPILER
  "allow known-bad compiler/host combos (no CoreAudio on macOS/GCC)" OFF)

if(APPLE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$"
     AND NOT KANARIV_ALLOW_UNSUPPORTED_COMPILER)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      message(FATAL_ERROR "GCC on macOS builds miniaudio without its "
        "CoreAudio backend (Apple's ObjC-blocks SDK headers do not parse "
        "under GCC, see cmake/components/player.cmake), so audio output is "
        "unavailable. Use --toolchain apple (AppleClang) or --toolchain clang "
        "(brew LLVM) through scripts/configure.py or pass "
        "-DKANARIV_ALLOW_UNSUPPORTED_COMPILER=ON.")
    else()
      message(FATAL_ERROR "Only AppleClang and LLVM Clang are supported on "
        "macOS (this is ${CMAKE_CXX_COMPILER_ID}). Pass "
        "-DKANARIV_ALLOW_UNSUPPORTED_COMPILER=ON to override.")
    endif()
  endif()
elseif(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(WARNING "MinGW GCC works but ships larger binaries (+winpthread "
    "DLL); the clang toolchain is preferred for releases.")
endif()
# Linux: GCC default, Clang first-class -- silence is the policy.
