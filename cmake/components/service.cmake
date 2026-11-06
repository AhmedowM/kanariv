# service.cmake -- kanariv::service (daemon-side command dispatch + IPC server)

set(KANARIV_SERVICE_MODULE_SOURCES
  modules/kanariv/service.cppm
  modules/kanariv/service/core.cppm
  modules/kanariv/service/host.cppm
)
set(KANARIV_SERVICE_SOURCES
  src/service/ipc_server.cpp
  src/service/service_paths.cpp
  src/service/service_status.cpp
  src/service/service_impl.cpp
  src/service/host.cpp
  src/service/dispatch_playback.cpp
  src/service/dispatch_queue.cpp
  src/service/dispatch_library.cpp
  src/service/dispatch_config.cpp
  src/service/tag_writer.cpp
  src/service/media_session.cpp
)

# OS media-session backends: one platform file compiles per target.
if(WIN32)
  list(APPEND KANARIV_SERVICE_SOURCES src/service/media_session_win.cpp)
elseif(APPLE)
  list(APPEND KANARIV_SERVICE_SOURCES src/service/media_session_mac.mm)
elseif(ANDROID)
  # Android media_session lives under a separate /android/ path (TBD).
elseif(UNIX)
  list(APPEND KANARIV_SERVICE_SOURCES src/service/media_session_linux.cpp)
endif()

kanariv_add_component(service SOURCES ${KANARIV_SERVICE_SOURCES} MODULE_SOURCES ${KANARIV_SERVICE_MODULE_SOURCES} DEPS kanariv::config kanariv::ipc kanariv::utils Threads::Threads INCLUDES vendor PRIVATE_DEPS kanariv::engine kanariv::db kanariv::media kanariv::player)
target_link_libraries(service PRIVATE tag)
if(WIN32)
  # runtimeobject.lib provides RoGetActivationFactory / WindowsCreateString etc.
  find_library(KANARIV_RUNTIMEOBJECT_LIB runtimeobject
    PATHS "C:/Program Files (x86)/Windows Kits/10/Lib"
    PATH_SUFFIXES "10.0.26100.0/um/x64"
    NO_DEFAULT_PATH)
  if(KANARIV_RUNTIMEOBJECT_LIB)
    set(_K_WIN_LIBS user32 ole32 shcore ${KANARIV_RUNTIMEOBJECT_LIB})
  else()
    set(_K_WIN_LIBS user32 ole32 shcore runtimeobject)
  endif()
  target_link_libraries(service PRIVATE ${_K_WIN_LIBS})
elseif(APPLE)
  target_link_libraries(service PRIVATE "-framework MediaPlayer")
elseif(UNIX)
  # Linux MPRIS backend needs libdbus-1 (library + include dirs).
  find_path(KANARIV_DBUS_INCLUDE_DIR NAMES dbus/dbus.h)
  find_library(KANARIV_DBUS_LIB dbus-1)
  if(KANARIV_DBUS_INCLUDE_DIR AND KANARIV_DBUS_LIB)
    target_link_libraries(service PRIVATE ${KANARIV_DBUS_LIB})
    target_include_directories(service SYSTEM PRIVATE ${KANARIV_DBUS_INCLUDE_DIR})
  else()
    message(WARNING
      "libdbus-1 not found: Linux MPRIS media-session backend will fail "
      "to activate at runtime (install libdbus-1-dev)")
  endif()
endif()
target_include_directories(service SYSTEM PRIVATE ${KANARIV_TAGLIB_BUILD_INCLUDES})

if(WIN32)
  file(GLOB _KANARIV_CPPWINRT_INCLUDE_DIRS
    "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
  if(_KANARIV_CPPWINRT_INCLUDE_DIRS)
    list(SORT _KANARIV_CPPWINRT_INCLUDE_DIRS)
    list(GET _KANARIV_CPPWINRT_INCLUDE_DIRS -1 _KANARIV_CPPWINRT_INCLUDE_DIR)
    target_include_directories(service SYSTEM PRIVATE
      ${_KANARIV_CPPWINRT_INCLUDE_DIR})
    target_include_directories(service_objects SYSTEM PRIVATE
      ${_KANARIV_CPPWINRT_INCLUDE_DIR})
  endif()
endif()

# Propagate TagLib include dirs to the object library so it can compile
# service's .cpp files against TagLib headers. The object library is not a
# link target, so target_link_libraries would not give it compile-time
# includes. Needed in both static and shared builds because service_objects
# compiles the same sources as the static service library.
target_include_directories(service_objects SYSTEM PRIVATE ${KANARIV_TAGLIB_BUILD_INCLUDES})
if(NOT KANARIV_BUILD_SHARED)
  target_compile_definitions(service_objects PRIVATE TAGLIB_STATIC)
endif()

# Link rt by name PRIVATE: a find_library() absolute path would be copied into
# the installed kanarivTargets.cmake and break relocation. glibc >= 2.34 has the
# shm/clock symbols in libc; older Linux still needs -lrt, which stays a
# link-only (relocatable) name in the export.
function(_kanariv_service_link_rt tgt)
  if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(${tgt} PRIVATE rt)
  endif()
endfunction()
_kanariv_service_link_rt(service)
ca_set_module_warnings(service)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(service EXTRA_DEPS kanariv::ipc_shared kanariv::engine_shared kanariv::db_shared kanariv::media_shared kanariv::player_shared kanariv::utils_shared Threads::Threads)
  if(KANARIV_BUILD_STATIC)
    _kanariv_service_link_rt(service_shared)
    target_link_libraries(service_shared PRIVATE tag)
  endif()
  if(WIN32)
    target_link_libraries(service_shared PRIVATE ${_K_WIN_LIBS})
  elseif(APPLE)
    target_link_libraries(service_shared PRIVATE "-framework MediaPlayer")
  elseif(UNIX)
    if(KANARIV_DBUS_LIB)
      target_link_libraries(service_shared PRIVATE ${KANARIV_DBUS_LIB})
      if(KANARIV_DBUS_INCLUDE_DIR)
        target_include_directories(service_shared SYSTEM PRIVATE ${KANARIV_DBUS_INCLUDE_DIR})
      endif()
    endif()
  endif()
endif()
