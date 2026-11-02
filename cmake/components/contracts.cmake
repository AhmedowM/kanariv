# contracts.cmake -- backend-neutral component contracts.
# Header-only in headers mode; compiled as a static module carrier when modules
# are enabled. The .cppm sources are installed
# directly; consumers doing find_package(kanariv) + import kanariv.contracts
# add the install prefix/modules to their C++ module search path.
set(KANARIV_CONTRACTS_MODULE_SOURCES
  modules/kanariv/contracts.cppm
  modules/kanariv/contracts/database.cppm
)
if(KANARIV_ENABLE_MODULES)
  add_library(contracts STATIC)
else()
  add_library(contracts INTERFACE)
endif()
add_library(kanariv::contracts ALIAS contracts)
set_property(DIRECTORY APPEND PROPERTY KANARIV_TARGETS contracts)
if(KANARIV_ENABLE_MODULES)
  target_include_directories(contracts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(contracts PUBLIC cxx_std_23)
  target_link_libraries(contracts PUBLIC kanariv::types kanariv::utils)
else()
  target_include_directories(contracts INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(contracts INTERFACE cxx_std_23)
  target_link_libraries(contracts INTERFACE kanariv::types kanariv::utils)
endif()
if(KANARIV_ENABLE_MODULES)
  target_sources(contracts PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
    FILES ${KANARIV_CONTRACTS_MODULE_SOURCES})
  ca_set_warnings(contracts)
  ca_set_module_warnings(contracts)
  # Install the .cppm sources so consumers can compile them into BMIs or
  # add the install/modules dir to their module search path.
  install(FILES ${KANARIV_CONTRACTS_MODULE_SOURCES}
    DESTINATION modules/kanariv/contracts COMPONENT dev)
endif()
