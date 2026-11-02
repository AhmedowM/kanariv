# Default toolchain selector: picks the canonical toolchain for this host
# so the intent presets (dev, ci, release, ...) mean "the right compiler
# here". Delegates to the sibling files -- single source of truth per
# toolchain, no compiler assignments duplicated here. An explicit
# --toolchain gcc|clang|msvc|apple includes the matching detail file instead.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  # AppleClang (via xcrun) is the default and fully supported compiler on macOS.
  include("${CMAKE_CURRENT_LIST_DIR}/apple.cmake")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  # Windows releases prefer LLVM Clang when it is on PATH (MSVC ABI target,
  # verified by clang.cmake); MSVC stays for --toolchain msvc and CI.
  find_program(_kanariv_win_clangxx NAMES clang++)
  if(_kanariv_win_clangxx)
    if(NOT _kanariv_auto_msg_shown)
      message(STATUS "auto toolchain: clang++ from PATH "
        "(Windows releases prefer Clang; use --toolchain msvc or a vcvars shell for MSVC)")
      set(_kanariv_auto_msg_shown TRUE)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/clang.cmake")
  elseif(CMAKE_GENERATOR MATCHES "Visual Studio" OR DEFINED ENV{VCINSTALLDIR})
    include("${CMAKE_CURRENT_LIST_DIR}/msvc.cmake")
  else()
    if(NOT _kanariv_auto_msg_shown)
      message(STATUS "auto toolchain: no VS environment; MinGW GCC from PATH "
        "(use --toolchain msvc or a vcvars shell for MSVC)")
      set(_kanariv_auto_msg_shown TRUE)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/gcc.cmake")
  endif()
else()
  include("${CMAKE_CURRENT_LIST_DIR}/gcc.cmake")
endif()
