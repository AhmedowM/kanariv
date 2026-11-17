# client.cmake -- kanariv::client (IPC client)

set(KANARIV_CLIENT_MODULE_SOURCES
  modules/kanariv/client.cppm
  modules/kanariv/client/core.cppm
)
set(KANARIV_CLIENT_SOURCES
  src/client/ipc_client.cpp
  src/client/client_impl.cpp
  src/client/output_formatter.cpp
)
kanariv_add_component(client SOURCES ${KANARIV_CLIENT_SOURCES} MODULE_SOURCES ${KANARIV_CLIENT_MODULE_SOURCES} DEPS kanariv::config kanariv::ipc kanariv::types kanariv::utils Threads::Threads)
ca_set_module_warnings(client)
if(KANARIV_BUILD_SHARED)
  # config is a declared DEPS entry: the shared flavour derives it.
  kanariv_add_shared_variant(client)
endif()
