# ipc.cmake -- kanariv::ipc (IPC command/result/protocol)

set(KANARIV_IPC_MODULE_SOURCES
  modules/kanariv/ipc.cppm
  modules/kanariv/ipc/command.cppm
  modules/kanariv/ipc/result.cppm
  modules/kanariv/ipc/protocol.cppm
  modules/kanariv/ipc/shm_status.cppm
)
set(KANARIV_IPC_SOURCES
  src/ipc/protocol.cpp
  src/ipc/shm_status.cpp
)
kanariv_add_component(ipc SOURCES ${KANARIV_IPC_SOURCES} MODULE_SOURCES ${KANARIV_IPC_MODULE_SOURCES} DEPS kanariv::config kanariv::utils Threads::Threads)
ca_set_module_warnings(ipc)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(ipc EXTRA_DEPS kanariv::config_shared kanariv::utils_shared Threads::Threads)
endif()
