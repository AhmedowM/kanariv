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
    if(KANARIV_NLOHMANN_PRIVATE_INCLUDE)
      target_include_directories(${NAME}_objects SYSTEM PRIVATE ${KANARIV_NLOHMANN_PRIVATE_INCLUDE})
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

  # --- Component library ---
  if(KANARIV_BUILD_STATIC)
    add_library(${NAME} STATIC ${ARG_SOURCES})
    set_target_properties(${NAME} PROPERTIES OUTPUT_NAME ${NAME}_static)
  else()
    add_library(${NAME} SHARED)
    set_target_properties(${NAME} PROPERTIES
      OUTPUT_NAME ${NAME}
      ARCHIVE_OUTPUT_NAME ${NAME})
    if(ARG_SOURCES)
      target_sources(${NAME} PRIVATE $<TARGET_OBJECTS:${NAME}_objects>)
    endif()
  endif()
  add_library(kanariv::${NAME} ALIAS ${NAME})
  set_property(DIRECTORY APPEND PROPERTY KANARIV_TARGETS ${NAME})
  if(ARG_MODULE_SOURCES AND KANARIV_ENABLE_MODULES)
    # BASE_DIRS strips the leading modules/ so the install tree gets
    # <prefix>/modules/<component>/*.cppm (not modules/modules/...).
    target_sources(${NAME} PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules FILES ${ARG_MODULE_SOURCES})
  endif()
  target_include_directories(${NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_include_directories(${NAME} PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
  if(ARG_INCLUDES)
    target_include_directories(${NAME} SYSTEM PRIVATE ${ARG_INCLUDES})
  endif()
  if(KANARIV_NLOHMANN_PRIVATE_INCLUDE)
    target_include_directories(${NAME} SYSTEM PRIVATE ${KANARIV_NLOHMANN_PRIVATE_INCLUDE})
  endif()
  if(ARG_DEPS)
    target_link_libraries(${NAME} PUBLIC ${ARG_DEPS})
  endif()
  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${NAME} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
  # Public C++23 requirement: headers use C++23 throughout, and components
  # ship CXX_MODULES interface units -- consumers (incl. module synth targets)
  # must compile at C++23 or newer.
  target_compile_features(${NAME} PUBLIC cxx_std_23)
  # PUBLIC_HEADER: one source of truth for installed headers per target.
  set(_pub_hdrs "")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/kanariv/${NAME}.hpp")
    list(APPEND _pub_hdrs "${CMAKE_CURRENT_SOURCE_DIR}/include/kanariv/${NAME}.hpp")
  endif()
  file(GLOB _sub_hdrs "${CMAKE_CURRENT_SOURCE_DIR}/include/kanariv/${NAME}/*.hpp")
  list(APPEND _pub_hdrs ${_sub_hdrs})
  if(_pub_hdrs)
    set_target_properties(${NAME} PROPERTIES PUBLIC_HEADER "${_pub_hdrs}")
  endif()
  # FFmpeg (the decoder backend) is linked PRIVATE by the components that
  # actually use it (currently media; see media.cmake) -- not blanket-linked to
  # every component, which forced every target to resolve FFmpeg at link time
  # and pulled its include dirs into unrelated TUs.
  ca_set_warnings(${NAME})
  kanariv_apply_extra_flags(${NAME})
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(${NAME})
  endif()
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

function(kanariv_add_shared_variant NAME)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "EXTRA_DEPS")
  if(NOT KANARIV_BUILD_STATIC)
    add_library(${NAME}_shared ALIAS ${NAME})
    add_library(kanariv::${NAME}_shared ALIAS ${NAME})
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
  # MSVC C4251/C4275: STL types in dllexport classes. Suppress for shared builds;
  # these are well-known and unavoidable when exporting classes with STL members.
  if(MSVC)
    target_compile_options(${NAME}_shared PRIVATE /wd4251 /wd4275)
  endif()
  # Add the PIC-compiled object files without inheriting their link interface:
  # the object library records its own (static) component deps as
  # $<LINK_ONLY:...>, so linking the target would pull a second, static copy of
  # every dependency into this DLL. The shared deps are listed in EXTRA_DEPS.
  target_sources(${NAME}_shared PRIVATE $<TARGET_OBJECTS:${NAME}_objects>)
  # Link the shared component flavour (EXTRA_DEPS) so the linker resolves
  # cross-component symbols once, through the import libraries.
  if(ARG_EXTRA_DEPS)
    target_link_libraries(${NAME}_shared PUBLIC ${ARG_EXTRA_DEPS})
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
#   add_executable + link libs + vendor SYSTEM include + cxx_std_23 +
#   extra flags + sanitizers (if enabled).
function(kanariv_add_example name srcs)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "LIBS")
  add_executable(${name} ${srcs})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  target_include_directories(${name} SYSTEM PRIVATE vendor)
  target_compile_features(${name} PRIVATE cxx_std_23)
  kanariv_apply_extra_flags(${name})
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(${name})
  endif()
endfunction()
