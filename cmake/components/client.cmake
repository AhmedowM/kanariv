# client.cmake -- kanariv::client (IPC client)

set(KANARIV_CLIENT_MODULE_SOURCES
  modules/kanariv/client.cppm
  modules/kanariv/client/core.cppm
)
set(KANARIV_CLIENT_SOURCES
  src/client/ipc_client.cpp
  src/client/client_impl.cpp
)
kanariv_add_component(client SOURCES ${KANARIV_CLIENT_SOURCES} MODULE_SOURCES ${KANARIV_CLIENT_MODULE_SOURCES} DEPS kanariv::config kanariv::ipc kanariv::utils Threads::Threads INCLUDES vendor)
ca_set_module_warnings(client)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(client EXTRA_DEPS kanariv::ipc_shared kanariv::utils_shared Threads::Threads)
endif()
