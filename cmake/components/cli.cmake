# cli.cmake -- the kanariv executable (CLI11 shell).
# The shell sources compile into a static library so the declaration-vocabulary
# tripwire test can link the same declarations the binary ships (L6).

if(KANARIV_BUILD_CLI)
# Shared CLI shell: parsing policy, per-domain declarations and dispatch.
# Static (not a component): it is an internal library with no install surface.
add_library(kanariv_cli_shell STATIC
  cli/src/shell/shell.cpp
  cli/src/shell/render.cpp
  cli/src/shell/commands_global.cpp
  cli/src/shell/commands_playback.cpp
  cli/src/shell/commands_queue.cpp
  cli/src/shell/commands_playlist.cpp
  cli/src/shell/commands_library.cpp
  cli/src/shell/commands_tags.cpp
  cli/src/shell/commands_history.cpp
  cli/src/shell/commands_lyrics.cpp
  cli/src/shell/commands_preview.cpp
  cli/src/shell/commands_config.cpp
  cli/src/shell/output_formatter.cpp
)
# cli/ (its own headers) + the installed header tree + generated headers. No
# src/: the CLI is an ordinary consumer of the public API, not a white-box
# test (src/ is build-tree-only, see cmake/Tests.cmake).
target_include_directories(kanariv_cli_shell PUBLIC
  cli/src/shell
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Minimum honest set: exactly the components whose public API the CLI names.
# app (daemon lifecycle, dispatch, foreground hosting and preview) is the
# orchestration layer; client is used directly for status probes and
# persistent watch sessions. ipc/config/utils arrive through app's PUBLIC
# interface, and service/db/player are app's PRIVATE composition details (a
# DLL's PRIVATE deps are not transitive to consumers, but the CLI no longer
# names those components, so it does not link them by name).
# PUBLIC: consumers (the executable and the tripwire test) compile the same
# public headers, so they need the DLL import define (KANARIV_USING_DLL) that
# the shared components carry in their INTERFACE.
function(_kanariv_cli_link target dep)
  # The shipped CLI runs against the shared flavour; a build without shared
  # libraries links the canonical (static) flavour instead. No target-existence
  # guessing: the flavour is an explicit choice (W5).
  if(KANARIV_BUILD_SHARED)
    target_link_libraries(${target} PUBLIC kanariv::${dep}_shared)
  else()
    target_link_libraries(${target} PUBLIC kanariv::${dep})
  endif()
endfunction()
foreach(_cli_dep IN ITEMS app client)
  _kanariv_cli_link(kanariv_cli_shell ${_cli_dep})
endforeach()
unset(_cli_dep)
target_link_libraries(kanariv_cli_shell PUBLIC CLI11::CLI11 Threads::Threads)
ca_set_warnings(kanariv_cli_shell)
ca_set_module_warnings(kanariv_cli_shell)
kanariv_apply_extra_flags(kanariv_cli_shell)

# kanariv executable -- main entry only; everything else comes from the shell
# library. Target is kanariv_cli (OUTPUT_NAME kanariv); the shipped binary is
# still `kanariv`/`kanariv.exe`. (No static lib owns the `kanariv` target name:
# the sqlite archive is target sqlite3, like blake3.)
add_executable(kanariv_cli
  cli/src/main.cpp
)
set_target_properties(kanariv_cli PROPERTIES OUTPUT_NAME kanariv)
target_link_libraries(kanariv_cli PRIVATE kanariv_cli_shell)
# FFmpeg is not linked here: the decoding components (media) link it PRIVATE,
# so it reaches the CLI as $<LINK_ONLY:...> through app's transitive deps
# (app -> service -> media/player/engine/db).
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
