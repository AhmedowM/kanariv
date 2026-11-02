# utils.cmake -- kanariv::utils (no dependencies; everything links this).
set(KANARIV_UTILS_MODULE_SOURCES
  modules/kanariv/utils.cppm
  modules/kanariv/utils/result.cppm
  modules/kanariv/utils/error.cppm
  modules/kanariv/utils/log.cppm
  modules/kanariv/utils/json.cppm
  modules/kanariv/utils/math.cppm
  modules/kanariv/utils/ring.cppm
  modules/kanariv/utils/print.cppm
  modules/kanariv/utils/thread.cppm
  modules/kanariv/version.cppm
  modules/kanariv/utils/generator.cppm
  modules/kanariv/utils/record_json.cppm
)

set(KANARIV_UTILS_SOURCES
  src/utils/error.cpp
  src/utils/json.cpp
  src/utils/log.cpp
  src/utils/log_sink.cpp
  # Shared Track <-> Json codec (db export/import + ipc wire). It includes the
  # header-only kanariv::types records, so no extra link dependency is added.
  src/utils/record_json.cpp
  src/utils/thread.cpp
)
kanariv_add_component(utils SOURCES ${KANARIV_UTILS_SOURCES} MODULE_SOURCES ${KANARIV_UTILS_MODULE_SOURCES} DEPS Threads::Threads)
ca_set_module_warnings(utils)
target_include_directories(utils PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(utils EXTRA_DEPS Threads::Threads)
  if(KANARIV_BUILD_STATIC)
    target_include_directories(utils_shared PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
  else()
    target_include_directories(utils PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
  endif()
endif()
