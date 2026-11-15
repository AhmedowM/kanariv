# cli.cmake -- the kanariv executable (CLI11 shell).
# The CLI shell itself (cli/src/shell/*) compiles into the kanariv executable only; no cli library.

# kanariv executable -- App dispatch (CLI11) + main entry (header-source only).
# Target is kanariv_cli (OUTPUT_NAME kanariv); the shipped binary is still
# `kanariv`/`kanariv.exe`. (No static lib owns the `kanariv` target name: the
# sqlite archive is target sqlite3, like blake3.)
if(KANARIV_BUILD_CLI)
add_executable(kanariv_cli
  cli/src/main.cpp
)
set_target_properties(kanariv_cli PROPERTIES OUTPUT_NAME kanariv)
target_sources(kanariv_cli PRIVATE cli/src/shell/shell.cpp cli/src/shell/output_formatter.cpp cli/src/shell/preview.cpp)
# cli/ (its own headers) + the installed header tree + generated headers. No
# src/: the CLI is an ordinary consumer of the public API, not a white-box
# test (src/ is build-tree-only, see cmake/Tests.cmake).
target_include_directories(kanariv_cli PRIVATE
  cli/src/shell
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Minimum honest set: exactly the components whose public API the CLI names.
# app (daemon lifecycle/dispatch) and service (daemon host path) are the
# orchestration layer; client (IpcClient/Client), db (Database::open for the
# local preview) and player (Player::create for `preview`) are called directly
# from cli/src/shell. ipc/config/utils arrive through app's PUBLIC interface.
# A DLL's PRIVATE deps are not transitive to consumers, so the directly used
# components must be linked by name (that is also why this cannot be reduced to
# app + service alone). One helper replaces the per-component flavour loop.
function(_kanariv_cli_link dep)
  if(TARGET kanariv::${dep}_shared)
    target_link_libraries(kanariv_cli PRIVATE kanariv::${dep}_shared)
  else()
    target_link_libraries(kanariv_cli PRIVATE kanariv::${dep})
  endif()
endfunction()
foreach(_cli_dep IN ITEMS app service client db player)
  _kanariv_cli_link(${_cli_dep})
endforeach()
unset(_cli_dep)
target_link_libraries(kanariv_cli PRIVATE CLI11::CLI11 Threads::Threads)
# FFmpeg is not linked here: the decoding components (media) link it PRIVATE,
# so it reaches the CLI as $<LINK_ONLY:...> through app/service and their
# transitive deps (service -> media/player/engine/db).
ca_set_warnings(kanariv_cli)
ca_set_module_warnings(kanariv_cli)
kanariv_apply_extra_flags(kanariv_cli)
# Needed on MinGW for template instantiation deduplication across executable + static libs.
# Scoped to Windows-GNU only: the Linux-GNU links (release intents across
# the bake-off matrix) succeed without it.
target_link_options(kanariv_cli PRIVATE $<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-Wl,--allow-multiple-definition>)
# Static GNU runtime for the shipped Windows executable: avoid missing
# libstdc++/libgcc DLLs on bare machines. FreeBSD keeps the shared runtime
# because its packaged GCC runtime has unresolved quad-precision symbols when
# statically linked on ARM64. libwinpthread stays shared -- a single instance
# shared with FFmpeg's own DLLs; swresample/av* stay DLL (no static FFmpeg
# build here). Skipped under sanitizers (ASan runtime interplays badly with
# static libstdc++). Replaces the old libstdc++exp workaround: no TU uses
# std::print directly anymore (see utils/print.hpp).
if(MSVC)
  target_link_options(kanariv_cli PRIVATE /INCREMENTAL:NO)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND WIN32 AND NOT KANARIV_ENABLE_SANITIZERS)
  target_link_options(kanariv_cli PRIVATE -static-libgcc -static-libstdc++)
endif()
if(KANARIV_ENABLE_SANITIZERS)
  kanariv_apply_sanitizers(kanariv_cli)
endif()
endif()
