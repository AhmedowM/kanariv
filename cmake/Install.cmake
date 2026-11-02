# Install targets, exports, docs, man page, license, systemd service.
# Component targets are registered in the KANARIV_TARGETS directory property
# by kanariv_add_component / kanariv_add_shared_variant (cmake/KanarivHelpers.cmake).
# Vendor libs (sqlite3, blake3, tag) are appended manually.
get_directory_property(KANARIV_INSTALL_LIBS DIRECTORY ${CMAKE_SOURCE_DIR} KANARIV_TARGETS)
if(NOT KANARIV_INSTALL_LIBS)
  set(KANARIV_INSTALL_LIBS "")
endif()

# Split targets: shared DLLs go to runtime (alongside kanariv.exe),
# static libs and import libs go to dev.
set(_kanariv_static "")
set(_kanariv_shared "")
set(_kanariv_objects "")
if(KANARIV_BUILD_STATIC)
  foreach(_tgt IN LISTS KANARIV_INSTALL_LIBS)
    get_target_property(_type ${_tgt} TYPE)
    if(_type STREQUAL "OBJECT_LIBRARY")
      list(APPEND _kanariv_objects ${_tgt})
    elseif(_type STREQUAL "SHARED_LIBRARY")
      list(APPEND _kanariv_shared ${_tgt})
    else()
      list(APPEND _kanariv_static ${_tgt})
    endif()
  endforeach()
else()
  foreach(_tgt IN LISTS KANARIV_INSTALL_LIBS)
    get_target_property(_type ${_tgt} TYPE)
    if(_type STREQUAL "SHARED_LIBRARY")
      list(APPEND _kanariv_shared ${_tgt})
    else()
      list(APPEND _kanariv_static ${_tgt})
    endif()
  endforeach()
endif()
# combined owns no component sources of its own, so it is never registered in
# KANARIV_TARGETS; opt it into the shared set explicitly in both flavours.
if(TARGET combined)
  list(APPEND _kanariv_shared combined)
endif()
set(KANARIV_INSTALL_LIBS ${_kanariv_static})
unset(_kanariv_static)
unset(_kanariv_objects)

# Vendor libs ship as archives for static builds; in shared mode they are
# DLLs and join the shared install set (DLL -> runtime, import lib -> dev).
# Their headers still never ship: no public header references them.
foreach(_vendor IN ITEMS sqlite3 blake3 tag)
  if(NOT TARGET ${_vendor})
    continue()
  endif()
  get_target_property(_vendor_type ${_vendor} TYPE)
  if(_vendor_type STREQUAL "SHARED_LIBRARY")
    list(APPEND _kanariv_shared ${_vendor})
  else()
    list(APPEND KANARIV_INSTALL_LIBS ${_vendor})
  endif()
endforeach()
unset(_vendor_type)

# Relocatable install tree: every shared library lands in one directory, so
# each one resolves its siblings relative to itself while the CLI resolves
# them from bin/ one level up. A DT_RUNPATH is not inherited by dependencies,
# so per-library RPATHs are what make bin/kanariv run from an unpacked runtime
# archive without LD_LIBRARY_PATH (verified by .github/workflows/shared-abi.yml;
# before this the loader only found the CLI's direct NEEDED entries and died on
# libengine.so.1).
if(UNIX)
  if(APPLE)
    set(_kanariv_lib_rpath "@loader_path")
    set(_kanariv_bin_rpath "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  else()
    set(_kanariv_lib_rpath "$ORIGIN")
    set(_kanariv_bin_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(_kanariv_shared)
    set_target_properties(${_kanariv_shared} PROPERTIES INSTALL_RPATH "${_kanariv_lib_rpath}")
  endif()
  if(TARGET kanariv_cli)
    set_target_properties(kanariv_cli PROPERTIES INSTALL_RPATH "${_kanariv_bin_rpath}")
  endif()
endif()

# Install static libs + import libs as dev.
if(KANARIV_ENABLE_MODULES)
  set(_kanariv_module_fs "FILE_SET;CXX_MODULES;DESTINATION;modules;COMPONENT;dev")
else()
  set(_kanariv_module_fs "")
endif()
install(
  TARGETS ${KANARIV_INSTALL_LIBS}
  EXPORT kanarivTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dev
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  ${_kanariv_module_fs}
)

# Install shared libraries as runtime: DLLs beside kanariv.exe (the import
# libs stay dev), and on Unix the .so/.dylib files the binary loads. The Unix
# halves used to land in dev, which left the runtime archive with only
# bin/kanariv and could not run on its own.
# On Unix there is no separate import lib: the .so/.dylib is needed both at
# link time and at run time. Ship it in both archives so the dev archive is a
# standalone SDK (like the Windows .lib) and the runtime archive stays
# self-contained. The same file is tagged for both components; CPack splits
# them into separate archives. On Windows this is a no-op: DLLs are RUNTIME
# artifacts and the import lib is the ARCHIVE, which already goes to dev.
if(_kanariv_shared)
  install(
    TARGETS ${_kanariv_shared}
    EXPORT kanarivTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
    ${_kanariv_module_fs}
  )
  install(
    TARGETS ${_kanariv_shared}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
  )
endif()

# Modules-only umbrella: install its BMI files separately.
if(KANARIV_ENABLE_MODULES)
  install(
    TARGETS kanariv_umbrella
    EXPORT kanarivTargets
    FILE_SET CXX_MODULES DESTINATION modules COMPONENT dev
  )
endif()

# Version header
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/kanariv/version_config.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kanariv COMPONENT dev)

# Public headers
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev FILES_MATCHING PATTERN "*.hpp")

# NOTE: vendor headers (sqlite3.h, blake3.h) are deliberately NOT installed --
# no public header references them (fwd-decls only). Only the static archives
# below ship (transitive link deps). miniaudio.h was never installed either.

install(FILES cmake/FindFFmpeg.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kanariv COMPONENT dev)
install(EXPORT kanarivTargets NAMESPACE kanariv:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kanariv COMPONENT dev)

if(KANARIV_BUILD_CLI)
  install(TARGETS kanariv_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
endif()

# Man page -- roff source tracked at docs/man/kanariv.1
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/man/kanariv.1")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/man/kanariv.1"
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT runtime)
endif()

# License at the archive top level (CPACK_RESOURCE_FILE_LICENSE is not
# bundled into plain TGZ/ZIP archives).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT runtime)
endif()

# Systemd user service
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/kanariv.service")
  install(FILES packaging/kanariv.service DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/systemd/user COMPONENT runtime)
endif()
