cmake_minimum_required(VERSION 3.28)

include(cmake/Version.cmake)

project(kanariv VERSION ${PROJECT_VERSION} LANGUAGES CXX C)

# Install dirs first: KanarivHelpers forms `$<INSTALL_INTERFACE:...>` entries
# from these variables at target-declaration time (eager expansion), so they
# must exist before any component is declared -- previously masked by
# nlohmann's own include(GNUInstallDirs) running earlier via FetchContent.
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(KANARIV_ENABLE_MODULES "build/install C++23 modules (optional, headers are canonical)" OFF)
if(KANARIV_ENABLE_MODULES)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR
      "KANARIV_ENABLE_MODULES requires GCC 14 or newer; GCC ${CMAKE_CXX_COMPILER_VERSION} "
      "does not provide the C++ module import-graph scanner required by CMake.")
  endif()
  set(CMAKE_CXX_SCAN_FOR_MODULES ON)
else()
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()
# Single compilation per C++23 module: static libs own the module BMIs,
# shared variants link the static objects whole-archive (GCC -fmodules-ts
# BMI CRC covers defines/flags, so recompiling the same sources with
# different *_EXPORTS/PIC flags breaks dependents with CRC mismatch).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Compiler policy (default-per-host selection lives in the toolchain
# dispatcher via cmake/toolchains/auto.cmake; this is the backstop for raw
# configures).
include(cmake/CompilerPolicy.cmake)

# Windows/Clang-or-MSVC: silence "unsafe CRT" deprecation noise (C4996 on
# fopen/getenv/_wfopen, -Wdeprecated-declarations under Clang). The portable
# calls are correct; the _s variants would fork the code path for
# non-shipping compilers. CI/Release is GCC-only, so this affects local
# Windows builds only.
if(MSVC OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32))
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Options -- FFmpeg is required primary (dr_* deleted, vendor cleaned)
option(KANARIV_USE_TRIMMED_FFMPEG "build against a trimmed decode-only FFmpeg (cmake/FFmpegTrimmed.cmake whitelist)" OFF)
option(KANARIV_ENABLE_TESTS "build tests" OFF)
option(KANARIV_ENABLE_EXAMPLES "build examples" OFF)
option(KANARIV_ENABLE_SANITIZERS "Enable ASan+UBSan (like C: -fsanitize=address,undefined)" OFF)
option(KANARIV_BUILD_DOCS "Build Doxygen documentation" OFF)
option(KANARIV_TEST_NOAUDIO "skip audio device tests (no beep) for headless CI" OFF)
option(KANARIV_ENABLE_CLANG_TIDY "enable clang-tidy checks during build" OFF)
option(KANARIV_ENABLE_CCACHE "use ccache/sccache compiler launcher if found (opt-in)" ON)
# Shared mode builds per-component DLLs/import libraries by default.
# KANARIV_BUILD_STATIC can be enabled to build the static component libraries too.
option(KANARIV_BUILD_SHARED "build per-component shared libraries" ON)
if(KANARIV_BUILD_SHARED)
  set(_kanariv_build_static_default OFF)
else()
  set(_kanariv_build_static_default ON)
endif()
option(KANARIV_BUILD_STATIC "build static component libraries" ${_kanariv_build_static_default})
unset(_kanariv_build_static_default)
# White-box tests include src/ headers, so they need symbols that are not part
# of the DLL ABI. They link the static component flavour; the CLI and daemon
# link the shared one, so both paths are exercised. Forced rather than
# inherited: a shared-only build must not silently lose white-box coverage.
if(KANARIV_BUILD_SHARED AND KANARIV_ENABLE_TESTS AND NOT KANARIV_BUILD_STATIC)
  set(KANARIV_BUILD_STATIC ON CACHE BOOL "build static component libraries" FORCE)
  message(STATUS "Tests enabled: KANARIV_BUILD_STATIC forced ON (tests link static libs)")
endif()
option(KANARIV_BUNDLE_FFMPEG_MACOS "bundle brew FFmpeg dylibs with @rpath fixup (standalone mac archives)" ON)
# Section GC for the minsize-lto presets. Apple ld rejects --gc-sections at
# compiler validation, so presets carry only the intent and the per-linker
# flags live here. GNU/Clang also need function/data sections at compile time;
# MSVC links /OPT:REF by default (/Gy only splits functions).
option(KANARIV_LINK_SECTION_GC "drop unreferenced code/data sections at link (Apple -dead_strip, otherwise --gc-sections)" OFF)
option(KANARIV_ENABLE_HEADER_SELF_CONTAINMENT "compile every public header as standalone TU" OFF)
# Extra flags appended to our own targets only (see kanariv_apply_extra_flags).
# These never replace CMAKE_CXX_FLAGS / CMAKE_C_FLAGS / CMAKE_EXE_LINKER_FLAGS
# and the empty default is a no-op. Vendor/third-party targets are unaffected.
set(KANARIV_EXTRA_CXX_FLAGS "" CACHE STRING "extra C++ compile flags for kanariv targets")
set(KANARIV_EXTRA_C_FLAGS "" CACHE STRING "extra C compile flags for kanariv targets")
set(KANARIV_EXTRA_LINK_FLAGS "" CACHE STRING "extra link flags for kanariv targets")
if(KANARIV_LINK_SECTION_GC)
  if(APPLE)
    add_link_options(-Wl,-dead_strip)
  elseif(MSVC)
    add_compile_options(/Gy)
  else()
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
  endif()
endif()
# NOTE: ccache/sccache is opt-in via KANARIV_ENABLE_CCACHE; never hard-required
# so user builds don't crash when the launcher is missing.
if(KANARIV_ENABLE_CCACHE)
  foreach(_launcher ccache sccache)
    find_program(_KANARIV_LAUNCHER ${_launcher})
    if(_KANARIV_LAUNCHER)
      set(CMAKE_CXX_COMPILER_LAUNCHER ${_KANARIV_LAUNCHER})
      set(CMAKE_C_COMPILER_LAUNCHER ${_KANARIV_LAUNCHER})
      message(STATUS "Compiler launcher: ${_KANARIV_LAUNCHER}")
      break()
    endif()
  endforeach()
  if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    message(WARNING "KANARIV_ENABLE_CCACHE=ON but neither ccache nor sccache found -- compiling without launcher")
  endif()
endif()
unset(_launcher)

# clang-tidy integration (requires compile_commands.json)
if(KANARIV_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_PROGRAM clang-tidy)
  if(CLANG_TIDY_PROGRAM)
    set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_PROGRAM})
    message(STATUS "clang-tidy enabled: ${CLANG_TIDY_PROGRAM}")
  else()
    message(WARNING "KANARIV_ENABLE_CLANG_TIDY=ON but clang-tidy not found")
  endif()
endif()

# Module path for FindFFmpeg
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Threads REQUIRED)

# Trimmed FFmpeg selection: KANARIV_USE_TRIMMED_FFMPEG=ON selects the trimmed
# provider. FFmpeg_ROOT (cache or env) takes precedence; otherwise
# KANARIV_TRIMMED_ROOT; otherwise the default ./build/ffmpeg. CMake never builds
# FFmpeg itself -- scripts/build-ffmpeg.py produces the trimmed tree out of tree.
if(KANARIV_USE_TRIMMED_FFMPEG)
  set(_trim_candidate "")
  if(FFmpeg_ROOT)
    set(_trim_candidate "${FFmpeg_ROOT}")
  elseif(DEFINED ENV{FFmpeg_ROOT})
    set(_trim_candidate "$ENV{FFmpeg_ROOT}")
  elseif(DEFINED KANARIV_TRIMMED_ROOT AND EXISTS "${KANARIV_TRIMMED_ROOT}/lib")
    set(_trim_candidate "${KANARIV_TRIMMED_ROOT}")
  elseif(DEFINED ENV{KANARIV_TRIMMED_ROOT} AND EXISTS "$ENV{KANARIV_TRIMMED_ROOT}/lib")
    set(_trim_candidate "$ENV{KANARIV_TRIMMED_ROOT}")
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg/lib")
    set(_trim_candidate "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg")
  endif()
  if(_trim_candidate)
    set(FFmpeg_ROOT "${_trim_candidate}" CACHE PATH "FFmpeg root (trimmed)" FORCE)
    message(STATUS "Trimmed FFmpeg root: ${_trim_candidate}")
  endif()
  unset(_trim_candidate)
endif()

function(kanariv_ffmpeg_fatal)
  message(FATAL_ERROR "FFmpeg is required -- install FFmpeg (tools + dev packages) or set FFmpeg_ROOT")
endfunction()

if(KANARIV_USE_TRIMMED_FFMPEG)
  include(cmake/FFmpegTrimmed.cmake)
  kanariv_setup_trimmed_ffmpeg()
  if(NOT FFmpeg_FOUND)
    kanariv_ffmpeg_fatal()
  endif()
  message(STATUS "FFmpeg found (trimmed): ${FFmpeg_AVCODEC_LIBRARY}")
else()
  find_package(FFmpeg QUIET)
  if(NOT FFmpeg_FOUND)
    kanariv_ffmpeg_fatal()
  endif()
  message(STATUS "FFmpeg found (system): ${FFmpeg_AVCODEC_LIBRARY}")
endif()

# Record the ABI floor for the installed config so downstream consumers link
# against the same FFmpeg major instead of silently re-finding another one.
function(kanariv_detect_ffmpeg_avcodec_major out)
  set(_header "${FFmpeg_AVCODEC_INCLUDE_DIR}/libavcodec/version_major.h")
  if(NOT EXISTS "${_header}")
    set(_header "${FFmpeg_AVCODEC_INCLUDE_DIR}/libavcodec/version.h")
  endif()
  set(_major "")
  if(EXISTS "${_header}")
    file(STRINGS "${_header}" _line REGEX "^#define[ \t]+LIBAVCODEC_VERSION_MAJOR[ \t]+[0-9]+")
    string(REGEX MATCH "[0-9]+" _major "${_line}")
  endif()
  set(${out} "${_major}" PARENT_SCOPE)
endfunction()
kanariv_detect_ffmpeg_avcodec_major(KANARIV_FFMPEG_AVCODEC_MAJOR)

function(kanariv_apply_sanitizers tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /fsanitize=address)
    target_link_options(${tgt} PRIVATE /fsanitize=address)
  else()
    target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g -O0)
    target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

if(KANARIV_ENABLE_SANITIZERS)
  if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "KANARIV_ENABLE_SANITIZERS is not supported with ${CMAKE_CXX_COMPILER_ID} on Windows (MinGW has no working ASan/UBSan runtime). Use Linux, or MSVC for ASan only.")
  elseif(MSVC)
    message(STATUS "Sanitizers enabled: MSVC ASan (/fsanitize=address)")
  else()
    message(STATUS "Sanitizers enabled: ASan+UBSan")
  endif()
endif()

# Configure version header (template lives in cmake/ -- canonical for CMake @ONLY files)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/kanariv/version_config.hpp
  @ONLY
)

# Vendor: sqlite3 (sqlite3 amalgamation, SQLITE_ENABLE_FTS5=1). Built STATIC
# for static builds; in shared mode it is a DLL so the process has exactly one
# SQLite copy (two static copies -- one per component DLL -- corrupt handles:
# each copy has its own sqlite3GlobalConfig and mutex table).
# Content-named like blake3 below; the only artifact called "kanariv" is the
# opt-in shared umbrella (combined -> libkanariv).
if(KANARIV_BUILD_SHARED)
  add_library(sqlite3 SHARED vendor/sqlite3.c)
  # WINDOWS_EXPORT_ALL_SYMBOLS makes CMake scan the object file with
  # __create_def to build the export table. __create_def can't parse /GL
  # bitcode objects, so exempt sqlite3 from LTCG while keeping interprocedural
  # optimization on for all kanariv code (the release-lto/minsize-lto presets
  # still exercise LTO where it matters).
  set_target_properties(sqlite3 PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    INTERPROCEDURAL_OPTIMIZATION OFF
  )
else()
  add_library(sqlite3 STATIC vendor/sqlite3.c)
endif()
add_library(kanariv::sqlite3 ALIAS sqlite3)
target_include_directories(sqlite3 SYSTEM PRIVATE vendor)
# BUILD_INTERFACE only: vendor headers are NOT installed (nothing installed
# references them), so no INSTALL_INTERFACE -- a dangling install path breaks
# downstream configure with "non-existent path" errors.
# SYSTEM: vendored amalgamation -- dependents (db/*.cpp include sqlite3.h)
# must not inherit its extension-token noise under Clang. Propagates.
target_compile_definitions(sqlite3 PUBLIC SQLITE_ENABLE_FTS5=1)
target_compile_options(sqlite3 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
# Trimmed amalgamation: only the API surface
# in use (prepare/bind/step/column/finalize, exec, open_v2, WAL + foreign_keys
# + FTS5). OMIT_LOAD_EXTENSION is also hardening. None of the omitted areas
# (JSON1 SQL, authorizer, progress callbacks, get_table, loadable extensions,
# deprecated spellings) is referenced anywhere in src/tests/examples/cli.
target_compile_definitions(sqlite3 PUBLIC
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_OMIT_DEPRECATED=1
  SQLITE_OMIT_AUTHORIZER=1
  SQLITE_OMIT_PROGRESS_CALLBACK=1
  SQLITE_OMIT_GET_TABLE=1
  SQLITE_OMIT_JSON=1
)

# Vendor: BLAKE3 C core (portable only - no AVX/SSE/NEON). Shared alongside
# the components in shared mode; vendor/blake3.h already carries the
# BLAKE3_DLL/BLAKE3_DLL_EXPORTS switch for the DLL boundary.
if(KANARIV_BUILD_SHARED)
  add_library(blake3 SHARED vendor/blake3.c vendor/blake3_portable.c)
  if(WIN32)
    target_compile_definitions(blake3 PRIVATE BLAKE3_DLL BLAKE3_DLL_EXPORTS)
    target_compile_definitions(blake3 INTERFACE BLAKE3_DLL)
  endif()
else()
  add_library(blake3 STATIC vendor/blake3.c vendor/blake3_portable.c)
endif()
target_include_directories(blake3 SYSTEM PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>)
target_compile_definitions(blake3 PRIVATE BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2 BLAKE3_USE_NEON=0)
target_compile_options(blake3 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

# FetchContent deps needed before components. CLI11/Catch2 are lazy: prefer
# a system/vcpkg/Conan package first so plain configures don't pay for a git
# clone; fetch only as fallback. nlohmann_json is vendored instead
# (vendor/nlohmann/, see vendor/README.md): it is PRIVATE (backend for
# src/utils/json.cpp only) -- its headers never enter installed headers, so
# no JSON_Install and no find_dependency downstream. Components get it as
# PRIVATE SYSTEM -I (KANARIV_NLOHMANN_PRIVATE_INCLUDE): even a PRIVATE target
# link would force it into install(EXPORT).
include(FetchContent)
# Vendored single-header backend, version locked in vendor/README.md.
set(KANARIV_NLOHMANN_PRIVATE_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# CLI11 is private to the kanariv executable: skip entirely with KANARIV_BUILD_CLI=OFF.
option(KANARIV_BUILD_CLI "build the kanariv executable (requires CLI11)" ON)
if(KANARIV_BUILD_CLI)
  find_package(CLI11 QUIET)
  if(NOT CLI11_FOUND)
    FetchContent_Declare(
      CLI11
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
      GIT_TAG v2.7.2
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(CLI11)
  else()
    message(STATUS "CLI11 found (system): includes come from CLI11::CLI11")
  endif()
endif()

# DRY helpers + component split (include preserves single-BMI build)
# TagLib is vendored like sqlite3/blake3 (always fetched, never system):
# private file-tag implementation detail, installed as kanariv::tag next to
# them so install(EXPORT) resolves. Headers never enter installed headers.
FetchContent_Declare(
  TagLib
  GIT_REPOSITORY https://github.com/taglib/taglib.git
  GIT_TAG v2.3.2
  GIT_SHALLOW TRUE
)
  set(BUILD_TESTS OFF)
  set(BUILD_EXAMPLES OFF)
  set(BUILD_BINDINGS OFF)
set(BUILD_TESTING OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_CppUnit TRUE)
# No zlib: TagLib would otherwise pick up platform SDK headers (macOS),
# poisoning brew-libc++ resolution under our -nostdinc++ toolchain, and
# silently vary by host. Compressed ID3v2 frames are rare; builds without
# them read such frames as absent. Deterministic everywhere.
set(WITH_ZLIB OFF)
# TagLib is a DLL in shared mode (same one-copy argument as sqlite3, plus LGPL
# distribution friendliness). BUILD_SHARED_LIBS is scoped to this fetch: it is
# a global option that would also turn Catch2/CLI11 into DLLs.
set(_kanariv_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
if(KANARIV_BUILD_SHARED)
  set(BUILD_SHARED_LIBS ON)
endif()
FetchContent_MakeAvailable(TagLib)
set(BUILD_SHARED_LIBS "${_kanariv_saved_build_shared_libs}")
unset(_kanariv_saved_build_shared_libs)
message(STATUS "TagLib fetched (v2.3.2): file tag writing enabled")
unset(BUILD_TESTS)
unset(BUILD_EXAMPLES)
unset(BUILD_BINDINGS)
unset(BUILD_TESTING)
unset(CMAKE_DISABLE_FIND_PACKAGE_CppUnit)
unset(WITH_ZLIB)
# Headers stay private (like sqlite3.h/blake3.h): clear the PUBLIC_HEADER
# property so install() ships the archive only, without warnings.
set_target_properties(tag PROPERTIES PUBLIC_HEADER "")
if(APPLE AND KANARIV_BUILD_SHARED)
  # TagLib bakes the configure-time prefix into its install name
  # (/usr/local/lib/libtag.2.dylib), which a relocated archive cannot resolve.
  # @rpath puts it beside our own dylibs, which find siblings via @loader_path
  # (cmake/Install.cmake).
  set_target_properties(tag PROPERTIES MACOSX_RPATH ON INSTALL_NAME_DIR "@rpath")
endif()
if(KANARIV_BUILD_SHARED)
  # TagLib builds in _deps/taglib-build; keep its DLL beside our binaries so
  # build-tree runs (ctest, soak_daemon.py, kanariv.exe) resolve it without
  # PATH changes.
  set_target_properties(tag PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
add_library(kanariv::tag ALIAS tag)
# TAGLIB_STATIC suppresses TAGLIB_EXPORT's dllimport branch; only valid when
# the archive is linked directly (static builds).
if(NOT KANARIV_BUILD_SHARED)
  target_compile_definitions(tag PRIVATE TAGLIB_STATIC)
endif()
target_compile_options(tag PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W0> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
# TagLib's `tag` target exports install includes only; consumers use flat
# per-subdir includes (`#include <apetag.h>`). Derive every header dir
# from the pinned source so all subdirectory includes resolve.
file(GLOB_RECURSE _taglib_hdrs "${taglib_SOURCE_DIR}/taglib/*.h")
set(KANARIV_TAGLIB_BUILD_INCLUDES "${taglib_SOURCE_DIR}" "${taglib_BINARY_DIR}")
foreach(_h IN LISTS _taglib_hdrs)
  get_filename_component(_d "${_h}" DIRECTORY)
  list(APPEND KANARIV_TAGLIB_BUILD_INCLUDES "${_d}")
endforeach()
list(REMOVE_DUPLICATES KANARIV_TAGLIB_BUILD_INCLUDES)
unset(_taglib_hdrs)
unset(_d)
include(cmake/KanarivHelpers.cmake)
# Component includes in dependency order. Each component's .cmake registers
# its target via kanariv_add_component; including them here builds the surface.
set(KANARIV_COMPONENTS
  types
  utils
  contracts
  config
  media
  player
  db
  engine
  ipc
  service
  client
  app
  combined
  cli
)
foreach(_comp IN LISTS KANARIV_COMPONENTS)
  include(cmake/components/${_comp}.cmake)
endforeach()
unset(_comp)

# Umbrella module (optional, headers are canonical)
if(KANARIV_ENABLE_MODULES)
  add_library(kanariv_umbrella STATIC)
  target_sources(kanariv_umbrella PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules FILES modules/kanariv.cppm)
  target_include_directories(kanariv_umbrella PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
  target_link_libraries(kanariv_umbrella PUBLIC kanariv::types kanariv::utils kanariv::contracts kanariv::config kanariv::media kanariv::player kanariv::db kanariv::engine kanariv::ipc kanariv::service kanariv::client kanariv::app)
  ca_set_warnings(kanariv_umbrella)
  ca_set_module_warnings(kanariv_umbrella)
  kanariv_apply_extra_flags(kanariv_umbrella)
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(kanariv_umbrella)
  endif()
endif()

# Documentation (Doxygen) -- see cmake/Docs.cmake
if(KANARIV_BUILD_DOCS)
  include(cmake/Docs.cmake)
  kanariv_add_docs()
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kanarivConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/kanarivConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kanariv
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/kanarivConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/kanarivConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/kanarivConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kanariv
  COMPONENT dev
)

# Install targets, exports, docs, man page, license, systemd service.
include(cmake/Install.cmake)

# Bundle FFmpeg runtime DLLs/dylibs and run Apple fixups after the CLI install
# rule so the executable exists in the staged install tree.
include(cmake/BundleFFmpeg.cmake)

# CPack packaging configuration.
include(cmake/Packaging.cmake)

# Examples (C parity: mini + player_db_demo + engine_demo)
if(KANARIV_ENABLE_EXAMPLES)
  kanariv_add_example(kanariv_mini examples/mini_cpp.cpp LIBS kanariv::player kanariv::utils)
  kanariv_add_example(kanariv_player_db_demo examples/player_db_demo.cpp LIBS kanariv::player kanariv::db kanariv::utils)
  kanariv_add_example(kanariv_engine_demo examples/engine_demo.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils)
endif()

# Modules-only consumer check: same program as mini_cpp, but importing
# modules instead of including headers. Modules build only.
if(KANARIV_ENABLE_EXAMPLES AND KANARIV_ENABLE_MODULES)
  add_executable(kanariv_mini_mod examples/mini_mod.cpp)
  target_link_libraries(kanariv_mini_mod PRIVATE kanariv::player kanariv::utils)
  target_compile_features(kanariv_mini_mod PRIVATE cxx_std_23)
  kanariv_apply_extra_flags(kanariv_mini_mod)
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(kanariv_mini_mod)
  endif()
endif()

include(cmake/Tests.cmake)
