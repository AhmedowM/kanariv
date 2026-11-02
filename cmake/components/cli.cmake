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
target_include_directories(kanariv_cli PRIVATE cli/src/shell)
target_include_directories(kanariv_cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Link the DLL flavour when it exists (dual builds keep the static libs for
# the white-box tests); a pure static build falls back to the static targets.
set(_kanariv_cli_libs "")
foreach(_cli_dep IN ITEMS app service client ipc db player)
  if(TARGET kanariv::${_cli_dep}_shared)
    list(APPEND _kanariv_cli_libs kanariv::${_cli_dep}_shared)
  else()
    list(APPEND _kanariv_cli_libs kanariv::${_cli_dep})
  endif()
endforeach()
unset(_cli_dep)
target_link_libraries(kanariv_cli PRIVATE ${_kanariv_cli_libs} CLI11::CLI11 Threads::Threads)
# FFmpeg is not linked here directly: kanariv::media links it PRIVATE (see
# media.cmake) and that propagates as $<LINK_ONLY:...> through cli -> player -> media.
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
