# Compiler policy backstop: the intent presets guide selection, this enforces
# the hard rules for raw configures that bypass the resolver.
option(KANARIV_ALLOW_UNSUPPORTED_COMPILER
  "allow known-bad compiler/host combos (no audio on macOS/GCC)" OFF)

if(APPLE)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
     AND NOT KANARIV_ALLOW_UNSUPPORTED_COMPILER)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      message(FATAL_ERROR "GCC on macOS builds miniaudio with the dummy "
        "backend (no audio). Use --toolchain clang (brew LLVM) through "
        "scripts/configure.py or pass -DKANARIV_ALLOW_UNSUPPORTED_COMPILER=ON.")
    else()
      message(FATAL_ERROR "Only LLVM Clang is supported on macOS "
        "(this is ${CMAKE_CXX_COMPILER_ID}). AppleClang lags our C++23 "
        "baseline. Use --toolchain clang or pass "
        "-DKANARIV_ALLOW_UNSUPPORTED_COMPILER=ON.")
    endif()
  endif()
elseif(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(WARNING "MinGW GCC works but ships larger binaries (+winpthread "
    "DLL); the clang toolchain is preferred for releases.")
endif()
# Linux: GCC default, Clang first-class -- silence is the policy.
