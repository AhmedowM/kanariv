# app.cmake -- kanariv::app (application orchestration shared by all frontends:
# daemon lifecycle today; transport policy and command dispatch over time).
# The CLI shell (cli/src/shell/*) stays a thin renderer on top of this lib.
set(KANARIV_APP_SOURCES
  src/app/lifecycle.cpp
  src/app/playback.cpp
  src/app/format.cpp
  src/app/paths.cpp
  src/app/detail.cpp
  src/app/queue.cpp
  src/app/playlist.cpp
  src/app/library.cpp
  src/app/tags.cpp
  src/app/system.cpp
  src/app/config.cpp
)
set(KANARIV_APP_MODULE_SOURCES
  modules/kanariv/app.cppm
  modules/kanariv/app/core.cppm
  modules/kanariv/app/paths.cppm
)
# kanariv::ipc is PUBLIC: include/kanariv/app/app.hpp includes
# <kanariv/ipc/command.hpp> and <kanariv/ipc/result.hpp>. client and media are
# used only from app's .cpp files, so they stay PRIVATE.
kanariv_add_component(app SOURCES ${KANARIV_APP_SOURCES} MODULE_SOURCES ${KANARIV_APP_MODULE_SOURCES} DEPS kanariv::config kanariv::ipc kanariv::utils Threads::Threads PRIVATE_DEPS kanariv::client kanariv::media)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(app)
endif()
