# config.cmake -- kanariv::config (configuration management: canonical paths,
# load/save; raw key/value helpers live in kanariv::config::detail internally).

set(KANARIV_CONFIG_MODULE_SOURCES
  modules/kanariv/config.cppm
)
set(KANARIV_CONFIG_SOURCES
  src/config.cpp
  src/config/runtime.cpp
  src/config/raw.cpp
)
kanariv_add_component(config SOURCES ${KANARIV_CONFIG_SOURCES} MODULE_SOURCES ${KANARIV_CONFIG_MODULE_SOURCES} DEPS kanariv::utils Threads::Threads)
ca_set_module_warnings(config)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(config EXTRA_DEPS kanariv::utils_shared Threads::Threads)
endif()
