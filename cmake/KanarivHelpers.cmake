# KanarivHelpers.cmake -- DRY helpers for kanariv components
# Provides ca_set_warnings / ca_set_module_warnings and shared component scaffolding.

if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL} AND NOT CMAKE_GENERATOR MATCHES "Ninja")
  set(KANARIV_MSVC_MP_FLAG "/MP$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
else()
  set(KANARIV_MSVC_MP_FLAG "")
endif()

# U-6b gate: our own targets treat use of the deprecated kanariv::db:: /
# kanariv::engine:: aliases as an error -- new internal code must spell
# kanariv::types:: directly. GCC/Clang only: MSVC's C4996 also fires for CRT /
# secure-CRT deprecations, so /we4996 would be too broad (MSVC is CI-only;
# shipping compilers are GCC/Clang, where alias-use deprecation is verified).
set(KANARIV_WARNING_FLAGS
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /wd4324 /WX ${KANARIV_MSVC_MP_FLAG}>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror=deprecated-declarations -Werror>)

function(ca_set_warnings tgt)
  target_compile_options(${tgt} PRIVATE ${KANARIV_WARNING_FLAGS})
  if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${tgt} PRIVATE -Wa,--mbig-obj)
  endif()
endfunction()

function(ca_set_module_warnings tgt)
  # Component files pass the plain component name; the real target carries the
  # flavour (W5) and target_* commands reject aliases, so resolve it first.
  get_property(_kanariv_component_target GLOBAL PROPERTY KANARIV_COMPONENT_TARGET_${tgt})
  if(_kanariv_component_target)
    set(tgt ${_kanariv_component_target})
  endif()
  if(KANARIV_ENABLE_MODULES)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        # GCC 14 re-applies visibility attributes while composing imported
        # module partitions and reports them after the definition. The
        # attributes are intentional on the canonical headers.
        target_compile_options(${tgt} PRIVATE -Wno-attributes)
      else()
        target_compile_options(${tgt} PRIVATE
          -Wno-template-names-tu-local
          -Wno-expose-global-module-tu-local
          -Wno-global-module)
      endif()
    endif()
  endif()
endfunction()

# Append the KANARIV_EXTRA_* cache strings to one of our own targets. Never
# call this on vendor targets, and never fold these into CMAKE_CXX_FLAGS /
# CMAKE_C_FLAGS / CMAKE_EXE_LINKER_FLAGS: the cache defaults stay untouched
# and an empty value is a no-op.
#
# clang-tidy is applied per target here (W22) instead of through the
# directory-wide CMAKE_CXX_CLANG_TIDY: the variable is inherited by every
# target created afterwards, which included the fetched TagLib tree.
function(kanariv_apply_extra_flags tgt)
  if(KANARIV_EXTRA_CXX_FLAGS)
    separate_arguments(_kanariv_extra_cxx NATIVE_COMMAND "${KANARIV_EXTRA_CXX_FLAGS}")
    foreach(_flag IN LISTS _kanariv_extra_cxx)
      target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${_flag}>)
    endforeach()
  endif()
  if(KANARIV_EXTRA_C_FLAGS)
    separate_arguments(_kanariv_extra_c NATIVE_COMMAND "${KANARIV_EXTRA_C_FLAGS}")
    foreach(_flag IN LISTS _kanariv_extra_c)
      target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:C>:${_flag}>)
    endforeach()
  endif()
  if(KANARIV_EXTRA_LINK_FLAGS)
    separate_arguments(_kanariv_extra_link NATIVE_COMMAND "${KANARIV_EXTRA_LINK_FLAGS}")
    target_link_options(${tgt} PRIVATE ${_kanariv_extra_link})
  endif()
  if(KANARIV_CLANG_TIDY_PROGRAM)
    set_target_properties(${tgt} PROPERTIES CXX_CLANG_TIDY "${KANARIV_CLANG_TIDY_PROGRAM}")
  endif()
endfunction()

function(kanariv_add_component NAME)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;MODULE_SOURCES;DEPS;PRIVATE_DEPS;INCLUDES")
  if(NOT ARG_SOURCES AND NOT ARG_MODULE_SOURCES)
    message(FATAL_ERROR "kanariv_add_component(${NAME}): SOURCES or MODULE_SOURCES required")
  endif()

  # --- Object library: sources shared by static and shared targets ---
  if(ARG_SOURCES)
    add_library(${NAME}_objects OBJECT ${ARG_SOURCES})
    add_library(kanariv::${NAME}_objects ALIAS ${NAME}_objects)
    set_property(DIRECTORY APPEND PROPERTY KANARIV_TARGETS ${NAME}_objects)
    target_include_directories(${NAME}_objects PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
    target_include_directories(${NAME}_objects PRIVATE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
    if(ARG_INCLUDES)
      target_include_directories(${NAME}_objects SYSTEM PRIVATE ${ARG_INCLUDES})
    endif()
    if(KANARIV_BUILD_SHARED)
      target_compile_definitions(${NAME}_objects PRIVATE KANARIV_BUILDING_DLL)
      # Same explicit ABI boundary the Windows build gets from
      # __declspec(dllexport): only KANARIV_API entities leave the shared object.
      # Windows needs no visibility flags, so this is Unix-only.
      if(NOT WIN32)
        set_target_properties(${NAME}_objects PROPERTIES
          CXX_VISIBILITY_PRESET hidden
          VISIBILITY_INLINES_HIDDEN ON)
      endif()
      if(MSVC)
        target_compile_options(${NAME}_objects PRIVATE /wd4251 /wd4275)
      endif()
    endif()
    # Propagate include dirs from all deps (PUBLIC and PRIVATE) to the object
    # library so it can compile the same sources as the static library. INTERFACE
    # links only propagate headers/defines, no binary output, so no duplicate
    # symbols can arise.
    if(ARG_DEPS)
      target_link_libraries(${NAME}_objects PRIVATE ${ARG_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
      target_link_libraries(${NAME}_objects PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
    target_compile_features(${NAME}_objects PUBLIC cxx_std_23)
    ca_set_warnings(${NAME}_objects)
    kanariv_apply_extra_flags(${NAME}_objects)
    if(KANARIV_ENABLE_SANITIZERS)
      kanariv_apply_sanitizers(${NAME}_objects)
    endif()
  endif()

  # Record both dependency lists so the shared flavour can be derived from the
  # same declaration (W6): a dependency added here can never drift from the one
  # the shared variant links.
  set_property(GLOBAL PROPERTY KANARIV_COMPONENT_DEPS_${NAME} "${ARG_DEPS}")
  set_property(GLOBAL PROPERTY KANARIV_COMPONENT_PRIVATE_DEPS_${NAME} "${ARG_PRIVATE_DEPS}")
  # Files the include/DEPS gate reads (kanariv_check_component_includes): the
  # component's sources plus its installed headers. Globbing here keeps the
  # check independent of the layout of include/kanariv/.
  set(_kanariv_checked_files ${ARG_SOURCES})
  file(GLOB_RECURSE _kanariv_component_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/include/kanariv/${NAME}.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/kanariv/${NAME}/*.hpp")
  list(APPEND _kanariv_checked_files ${_kanariv_component_headers})
  set_property(GLOBAL PROPERTY KANARIV_COMPONENT_FILES_${NAME} "${_kanariv_checked_files}")
  unset(_kanariv_checked_files)
  unset(_kanariv_component_headers)

  # --- Component target ------------------------------------------------------
  # The real target carries its flavour in the name (W5): <name>_static when
  # this build produces static libraries, <name>_shared otherwise. The
  # canonical kanariv::<name> is an alias to that primary flavour, so a
  # consumer never has to guess which target exists; kanariv::<name>_static /
  # kanariv::<name>_shared name one flavour explicitly. The un-namespaced
  # <name> alias keeps target-name linkers working for one release.
  if(KANARIV_BUILD_STATIC)
    # Archive the object files the shared and combined flavours already
    # compile: one compilation per configuration (W4). KANARIV_BUILDING_DLL and
    # hidden visibility only shape the dynamic symbol table, so a static
    # consumer still resolves internal symbols from the archive.
    set(_kanariv_primary ${NAME}_static)
    add_library(${_kanariv_primary} STATIC)
    set_target_properties(${_kanariv_primary} PROPERTIES OUTPUT_NAME ${NAME}_static)
  else()
    # Shared-only build: this function owns the only flavour, so it carries
    # the defines kanariv_add_shared_variant() would add -- the DLL is compiled
    # by ${NAME}_objects (KANARIV_BUILDING_DLL), and consumers must take the
    # __declspec(dllimport) branch of KANARIV_API (include/kanariv/fwd.hpp).
    set(_kanariv_primary ${NAME}_shared)
    add_library(${_kanariv_primary} SHARED)
    set_target_properties(${_kanariv_primary} PROPERTIES
      OUTPUT_NAME ${NAME}
      ARCHIVE_OUTPUT_NAME ${NAME})
    target_compile_definitions(${_kanariv_primary} PRIVATE KANARIV_BUILDING_DLL)
    target_compile_definitions(${_kanariv_primary} INTERFACE KANARIV_USING_DLL)
  endif()
  set_property(GLOBAL PROPERTY KANARIV_COMPONENT_TARGET_${NAME} ${_kanariv_primary})
  if(ARG_SOURCES)
    target_sources(${_kanariv_primary} PRIVATE $<TARGET_OBJECTS:${NAME}_objects>)
  endif()
  add_library(kanariv::${_kanariv_primary} ALIAS ${_kanariv_primary})
  add_library(kanariv::${NAME} ALIAS ${_kanariv_primary})
  add_library(${NAME} ALIAS ${_kanariv_primary})
  set_property(DIRECTORY APPEND PROPERTY KANARIV_TARGETS ${_kanariv_primary})
  if(ARG_MODULE_SOURCES AND KANARIV_ENABLE_MODULES)
    # BASE_DIRS strips the leading modules/ so the install tree gets
    # <prefix>/modules/<component>/*.cppm (not modules/modules/...).
    target_sources(${_kanariv_primary} PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules FILES ${ARG_MODULE_SOURCES})
  endif()
  target_include_directories(${_kanariv_primary} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_include_directories(${_kanariv_primary} PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
  if(ARG_INCLUDES)
    target_include_directories(${_kanariv_primary} SYSTEM PRIVATE ${ARG_INCLUDES})
  endif()
  # The static archive links the static flavours so its exported interface
  # names exported targets only; the shared variant maps to _shared itself.
  if(ARG_DEPS)
    if(KANARIV_BUILD_STATIC)
      _kanariv_map_static_deps(_kanariv_deps ${ARG_DEPS})
    else()
      set(_kanariv_deps ${ARG_DEPS})
    endif()
    target_link_libraries(${_kanariv_primary} PUBLIC ${_kanariv_deps})
    unset(_kanariv_deps)
  endif()
  if(ARG_PRIVATE_DEPS)
    if(KANARIV_BUILD_STATIC)
      _kanariv_map_static_deps(_kanariv_private_deps ${ARG_PRIVATE_DEPS})
    else()
      set(_kanariv_private_deps ${ARG_PRIVATE_DEPS})
    endif()
    target_link_libraries(${_kanariv_primary} PRIVATE ${_kanariv_private_deps})
    unset(_kanariv_private_deps)
  endif()
  # Public C++23 requirement: headers use C++23 throughout, and components
  # ship CXX_MODULES interface units -- consumers (incl. module synth targets)
  # must compile at C++23 or newer.
  target_compile_features(${_kanariv_primary} PUBLIC cxx_std_23)
  # Installed headers have exactly one mechanism: the wholesale
  # install(DIRECTORY include/ ...) in cmake/Install.cmake, which preserves the
  # kanariv/<component>/ prefix and nested directories. PUBLIC_HEADER is
  # deliberately not set here -- it flattened every component into include/,
  # where library/scan.hpp + media/scan.hpp (and json.hpp, result.hpp, the
  # <component>.hpp facades) collided, and its non-recursive glob skipped
  # nested headers such as ipc/command/*.hpp.
  #
  # FFmpeg (the decoder backend) is linked PRIVATE by the components that
  # actually use it (currently media; see media.cmake) -- not blanket-linked to
  # every component, which forced every target to resolve FFmpeg at link time
  # and pulled its include dirs into unrelated TUs.
  ca_set_warnings(${_kanariv_primary})
  kanariv_apply_extra_flags(${_kanariv_primary})
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(${_kanariv_primary})
  endif()
  unset(_kanariv_primary)
endfunction()

# Configure-time dependency gate (AR-01.3): every component may only name
# headers of components it declares. The declared DEPS lists are otherwise
# convention-only -- static builds hide omissions because all components share
# the same include root and link-only propagation, and modules mode breaks.
#
# Rule: for each registered component, every `#include <kanariv/<c>/...>` in
# its sources and installed headers must map to itself or to one of its
# DEPS/PRIVATE_DEPS entries. Top-level shared headers (fwd.hpp,
# windows_fwd.hpp, version.hpp) belong to no component and are exempt.
function(kanariv_check_component_includes)
  foreach(_name IN LISTS KANARIV_COMPONENTS)
    get_property(_files GLOBAL PROPERTY KANARIV_COMPONENT_FILES_${_name})
    if(NOT _files)
      continue()
    endif()
    get_property(_deps GLOBAL PROPERTY KANARIV_COMPONENT_DEPS_${_name})
    get_property(_priv GLOBAL PROPERTY KANARIV_COMPONENT_PRIVATE_DEPS_${_name})
    set(_declared "")
    foreach(_dep IN LISTS _deps _priv)
      string(REGEX REPLACE "^kanariv::" "" _dep "${_dep}")
      if(_dep IN_LIST KANARIV_COMPONENTS)
        list(APPEND _declared "${_dep}")
      endif()
    endforeach()
    foreach(_file IN LISTS _files)
      file(STRINGS "${_file}" _include_lines
        REGEX "^[ \t]*#[ \t]*include[ \t]*<kanariv/")
      foreach(_line IN LISTS _include_lines)
        string(REGEX MATCH "<kanariv/([^>]+)>" _match "${_line}")
        if(NOT CMAKE_MATCH_1)
          continue()
        endif()
        set(_include "${CMAKE_MATCH_1}")
        if(_include MATCHES "^(fwd|windows_fwd|version)\\.hpp$")
          continue()
        endif()
        if(_include MATCHES "/")
          string(REGEX REPLACE "/.*$" "" _target_component "${_include}")
        else()
          string(REGEX REPLACE "\\.hpp$" "" _target_component "${_include}")
        endif()
        if(NOT _target_component IN_LIST KANARIV_COMPONENTS
           OR _target_component STREQUAL _name
           OR _target_component IN_LIST _declared)
          continue()
        endif()
        string(REPLACE "${CMAKE_SOURCE_DIR}/" "" _rel "${_file}")
        message(FATAL_ERROR
          "kanariv dependency gate: '${_name}' includes <kanariv/${_include}> "
          "(${_rel}) but declares no kanariv::${_target_component}. "
          "Add it to DEPS (public header surface) or PRIVATE_DEPS "
          "(implementation only) in cmake/components/${_name}.cmake, or drop "
          "the include.")
      endforeach()
    endforeach()
  endforeach()
endfunction()

# The real target backing a component name (see kanariv_add_component). Files
# that configure a component directly -- extra link libraries, compile
# definitions, platform include dirs -- must use it: target_* commands reject
# aliases, and the canonical kanariv::<name> is one.
function(kanariv_component_target NAME out)
  get_property(_tgt GLOBAL PROPERTY KANARIV_COMPONENT_TARGET_${NAME})
  if(NOT _tgt)
    message(FATAL_ERROR "kanariv_component_target(${NAME}): no such component")
  endif()
  set(${out} "${_tgt}" PARENT_SCOPE)
endfunction()

# Map a component dependency list onto its static flavour: every kanariv::<x>
# that has a <x>_static target becomes kanariv::<x>_static. Everything else
# (Threads, FFmpeg, vendor libs, header-only carriers) keeps its name.
function(_kanariv_map_static_deps out)
  set(_mapped "")
  foreach(_dep IN LISTS ARGN)
    if(_dep MATCHES "^kanariv::([^:]+)$")
      set(_plain "${CMAKE_MATCH_1}")
      if(TARGET kanariv::${_plain}_static)
        list(APPEND _mapped "kanariv::${_plain}_static")
      else()
        list(APPEND _mapped "${_dep}")
      endif()
    else()
      list(APPEND _mapped "${_dep}")
    endif()
  endforeach()
  set(${out} "${_mapped}" PARENT_SCOPE)
endfunction()

# D-4: header self-containment. Every public header is compiled as its own
# translation unit with only include/ on the search path -- no PCH and no
# other kanariv headers pre-included -- so a header that silently leans on
# another header's includes (or on the component PCH) fails the build instead
# of leaking the requirement downstream. Compile-only: the objects are never
# linked. Policy: every include/kanariv/**.hpp (and the kanariv.hpp umbrella)
# must stand alone.
function(_kanariv_write_if_different path content)
  set(_existing "")
  if(EXISTS "${path}")
    file(READ "${path}" _existing)
  endif()
  if(NOT "${_existing}" STREQUAL "${content}")
    file(WRITE "${path}" "${content}")
  endif()
endfunction()

function(kanariv_add_header_self_containment)
  set(_hsc_inc "${CMAKE_CURRENT_SOURCE_DIR}/include")
  file(GLOB_RECURSE _hsc_headers CONFIGURE_DEPENDS "${_hsc_inc}/kanariv/*.hpp")
  list(APPEND _hsc_headers "${_hsc_inc}/kanariv.hpp")

  add_library(kanariv_header_self_containment OBJECT)
  target_include_directories(kanariv_header_self_containment PRIVATE
    "${_hsc_inc}"
    "${CMAKE_CURRENT_BINARY_DIR}/include")
  target_compile_features(kanariv_header_self_containment PRIVATE cxx_std_23)
  ca_set_warnings(kanariv_header_self_containment)

  set(_hsc_gen "${CMAKE_CURRENT_BINARY_DIR}/header_self_containment")
  file(MAKE_DIRECTORY "${_hsc_gen}")
  foreach(_hdr IN LISTS _hsc_headers)
    file(RELATIVE_PATH _rel "${_hsc_inc}" "${_hdr}")
    string(REPLACE "\\" "/" _rel "${_rel}")
    string(REPLACE "/" "_" _stem "${_rel}")
    string(REPLACE ".hpp" ".cpp" _stem "${_stem}")
    set(_tu "${_hsc_gen}/${_stem}")
    _kanariv_write_if_different("${_tu}"
      "// D-4: standalone TU for <${_rel}> (generated).\n#include <${_rel}>\n")
    target_sources(kanariv_header_self_containment PRIVATE "${_tu}")
  endforeach()
endfunction()

# Map a component dependency list onto its shared flavour: every
# kanariv::<x> whose <x>_shared target exists becomes kanariv::<x>_shared.
# Header-only carriers (types/contracts in headers mode) are expanded into
# their own dependencies: linking the INTERFACE target would drag the static
# flavour of the components it names into this DLL (two copies in one process).
# Everything else (Threads, FFmpeg, vendor libs, module carriers) keeps its
# declared name.
function(_kanariv_map_shared_deps out)
  set(_mapped "")
  foreach(_dep IN LISTS ARGN)
    if(_dep MATCHES "^kanariv::([^:]+)$")
      set(_plain "${CMAKE_MATCH_1}")
      if(TARGET kanariv::${_plain}_shared)
        set(_dep kanariv::${_plain}_shared)
      elseif(TARGET kanariv::${_plain})
        get_target_property(_type kanariv::${_plain} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
          get_target_property(_iface kanariv::${_plain} INTERFACE_LINK_LIBRARIES)
          if(_iface)
            _kanariv_map_shared_deps(_iface_mapped ${_iface})
            list(APPEND _mapped ${_iface_mapped})
          endif()
          continue()
        endif()
      endif()
    endif()
    list(APPEND _mapped "${_dep}")
  endforeach()
  set(${out} "${_mapped}" PARENT_SCOPE)
endfunction()

function(kanariv_add_shared_variant NAME)
  if(NOT KANARIV_BUILD_STATIC)
    # kanariv_add_component already created <name>_shared as the primary
    # flavour of a shared-only build; there is nothing to add here.
    return()
  endif()
  add_library(${NAME}_shared SHARED)
  # On Windows, SHARED targets also emit an import library. Keep the DLL's
  # runtime name stable while avoiding a collision with the static ${NAME}.lib.
  set_target_properties(${NAME}_shared PROPERTIES
    OUTPUT_NAME ${NAME}
    ARCHIVE_OUTPUT_NAME ${NAME})
  add_library(kanariv::${NAME}_shared ALIAS ${NAME}_shared)
  set_property(DIRECTORY APPEND PROPERTY KANARIV_TARGETS ${NAME}_shared)
  target_include_directories(${NAME}_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(${NAME}_shared PUBLIC cxx_std_23)
  # Define KANARIV_BUILDING_DLL so KANARIV_API expands to dllexport on Windows.
  target_compile_definitions(${NAME}_shared PRIVATE KANARIV_BUILDING_DLL)
  # Consumers of the DLL take the __declspec(dllimport) branch of KANARIV_API.
  # INTERFACE so the definition is exported with the target and never applied
  # to the DLL's own objects (those are compiled by ${NAME}_objects with
  # KANARIV_BUILDING_DLL).
  target_compile_definitions(${NAME}_shared INTERFACE KANARIV_USING_DLL)
  # MSVC C4251/C4275: STL types in dllexport classes. Suppress for shared builds;
  # these are well-known and unavoidable when exporting classes with STL members.
  if(MSVC)
    target_compile_options(${NAME}_shared PRIVATE /wd4251 /wd4275)
  endif()
  # Add the PIC-compiled object files without inheriting their link interface:
  # the object library records its own (static) component deps as
  # $<LINK_ONLY:...>, so linking the target would pull a second, static copy of
  # every dependency into this DLL. Dependencies are linked explicitly below.
  target_sources(${NAME}_shared PRIVATE $<TARGET_OBJECTS:${NAME}_objects>)
  # Link the shared component flavour so the linker resolves cross-component
  # symbols once, through the import libraries. The lists come from
  # kanariv_add_component's DEPS/PRIVATE_DEPS (recorded above), so the shared
  # variant cannot drift from the static one.
  get_property(_deps GLOBAL PROPERTY KANARIV_COMPONENT_DEPS_${NAME})
  get_property(_private_deps GLOBAL PROPERTY KANARIV_COMPONENT_PRIVATE_DEPS_${NAME})
  _kanariv_map_shared_deps(_shared_deps ${_deps})
  _kanariv_map_shared_deps(_shared_private_deps ${_private_deps})
  if(_shared_deps)
    target_link_libraries(${NAME}_shared PUBLIC ${_shared_deps})
  endif()
  if(_shared_private_deps)
    target_link_libraries(${NAME}_shared PRIVATE ${_shared_private_deps})
  endif()
  # Versioned soname for every shared target, not just combined.
  set_target_properties(${NAME}_shared PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
  # No --allow-multiple-definition needed: we link object files directly,
  # not whole-archived static archives, so there are no duplicate symbols.
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(${NAME}_shared)
  endif()
endfunction()

# Build an example executable with the standard boilerplate:
#   add_executable + link libs + cxx_std_23 + extra flags + sanitizers (if
#   enabled). No vendor include: examples only use installed kanariv headers,
#   and vendor/ is scoped per component (see ARG_INCLUDES in
#   kanariv_add_component).
function(kanariv_add_example name srcs)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "LIBS")
  add_executable(${name} ${srcs})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  target_compile_features(${name} PRIVATE cxx_std_23)
  kanariv_apply_extra_flags(${name})
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(${name})
  endif()
endfunction()
