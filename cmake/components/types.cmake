# types.cmake -- kanariv::types (dependency-free wire + record value types).
# Header-only in headers mode; compiled as a static module carrier when modules
# are enabled so CMake can schedule and export its CXX_MODULES file set.
set(KANARIV_TYPES_MODULE_SOURCES
  modules/kanariv/types.cppm
  modules/kanariv/types/wire.cppm
  modules/kanariv/types/format.cppm
  modules/kanariv/types/parse.cppm
  modules/kanariv/types/record.cppm
  modules/kanariv/types/db.cppm
  modules/kanariv/types/engine.cppm
)
if(KANARIV_ENABLE_MODULES)
  add_library(types STATIC)
else()
  add_library(types INTERFACE)
endif()
add_library(kanariv::types ALIAS types)
set_property(DIRECTORY APPEND PROPERTY KANARIV_TARGETS types)
if(KANARIV_ENABLE_MODULES)
  target_include_directories(types PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(types PUBLIC cxx_std_23)
else()
  target_include_directories(types INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(types INTERFACE cxx_std_23)
endif()
if(KANARIV_ENABLE_MODULES)
  target_sources(types PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
    FILES ${KANARIV_TYPES_MODULE_SOURCES})
  ca_set_warnings(types)
  ca_set_module_warnings(types)
  # Install .cppm sources directly; consumers compile them to BMIs or add the
  # install/modules dir to their C++ module search path.
  install(FILES ${KANARIV_TYPES_MODULE_SOURCES}
    DESTINATION modules/kanariv/types COMPONENT dev)
endif()
