# engine.cmake -- kanariv::engine (playback state machine + persistence).
# NOTE: history/shuffle live in src/engine/ (NOT installed), no partitions.
set(KANARIV_ENGINE_MODULE_SOURCES
  modules/kanariv/engine.cppm
  modules/kanariv/engine/core.cppm
)
set(KANARIV_ENGINE_SOURCES
  src/engine/engine.cpp
  src/engine/history.cpp
  src/engine/shuffle.cpp
)
kanariv_add_component(engine SOURCES ${KANARIV_ENGINE_SOURCES} MODULE_SOURCES ${KANARIV_ENGINE_MODULE_SOURCES} DEPS kanariv::contracts kanariv::utils Threads::Threads PRIVATE_DEPS  kanariv::player kanariv::media)
ca_set_module_warnings(engine)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(engine EXTRA_DEPS kanariv::player_shared kanariv::utils_shared Threads::Threads)
endif()
