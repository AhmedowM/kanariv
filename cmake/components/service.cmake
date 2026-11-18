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
  src/service/dispatch_tag.cpp
  src/service/dispatch_playlist.cpp
  src/service/dispatch_lyrics_bookmark.cpp
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
  # Android media-session and JNI bridge live under /android/ and are built
  # separately for Android targets; they are not part of the core service
  # component.
elseif(UNIX)
  list(APPEND KANARIV_SERVICE_SOURCES src/service/media_session_linux.cpp)
endif()

# INCLUDES vendor: src/service/service_status.cpp includes <blake3.h>.
# INCLUDES KANARIV_TAGLIB_BUILD_INCLUDES: tag_writer.cpp needs TagLib headers
# (a private implementation detail; they never enter the installed tree).
# PRIVATE_DEPS tag: the tag writer links TagLib, so the shared flavour derives
# the same dependency instead of repeating it below.
kanariv_add_component(service SOURCES ${KANARIV_SERVICE_SOURCES} MODULE_SOURCES ${KANARIV_SERVICE_MODULE_SOURCES} DEPS kanariv::config kanariv::contracts kanariv::ipc kanariv::types kanariv::utils Threads::Threads PRIVATE_DEPS kanariv::engine kanariv::db kanariv::media kanariv::player tag INCLUDES vendor ${KANARIV_TAGLIB_BUILD_INCLUDES})
kanariv_component_target(service KANARIV_SERVICE_TARGET)
# The media-session backend pulls in platform libraries (WinRT/COM, MediaPlayer,
# libdbus-1). combined links the same object files, so the list lives in one
# helper shared by both instead of being restated per target.
kanariv_service_platform_libs(${KANARIV_SERVICE_TARGET} OBJECTS service_objects)

# STATIC is a per-target definition, not an include path: the amalgamation is
# linked directly in static builds, so TagLib must not expand its dllimport
# branch. The include dirs themselves arrive through INCLUDES above.
if(NOT KANARIV_BUILD_SHARED)
  target_compile_definitions(service_objects PRIVATE TAGLIB_STATIC)
endif()

if(WIN32)
  # cppwinrt headers live in the Windows SDK the toolchain actually targets.
  # Ask CMake for that version first; only fall back to scanning the SDK dir
  # (natural order, so 10.0.26100.0 sorts above 10.0.9999.0) when the
  # generator does not report one (Ninja + clang outside a vcvars shell).
  set(_KANARIV_WINSDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
  if(DEFINED ENV{WindowsSdkDir} AND NOT "$ENV{WindowsSdkDir}" STREQUAL "")
    # vcvars exports the root with a trailing separator ("...\Windows Kits\10\").
    string(REGEX REPLACE "[/\\\\]+$" "" _KANARIV_WINSDK_ROOT "$ENV{WindowsSdkDir}")
  endif()
  set(_KANARIV_WINSDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  set(_KANARIV_WINSDK_SOURCE "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION")
  if(NOT _KANARIV_WINSDK_VERSION AND DEFINED ENV{WindowsSDKVersion})
    # vcvars exports e.g. "10.0.26100.0\" while IDE builds use a plain version.
    string(REGEX REPLACE "\\\\$" "" _KANARIV_WINSDK_VERSION "$ENV{WindowsSDKVersion}")
    set(_KANARIV_WINSDK_SOURCE "WindowsSDKVersion")
  endif()
  if(NOT _KANARIV_WINSDK_VERSION)
    set(_KANARIV_WINSDK_SOURCE "SDK directory scan")
    file(GLOB _KANARIV_WINSDK_CANDIDATES LIST_DIRECTORIES true
      "${_KANARIV_WINSDK_ROOT}/Include/*")
    set(_KANARIV_WINSDK_VERSIONS "")
    foreach(_sdk_dir IN LISTS _KANARIV_WINSDK_CANDIDATES)
      if(EXISTS "${_sdk_dir}/cppwinrt")
        get_filename_component(_sdk_ver "${_sdk_dir}" NAME)
        list(APPEND _KANARIV_WINSDK_VERSIONS "${_sdk_ver}")
      endif()
    endforeach()
    if(_KANARIV_WINSDK_VERSIONS)
      list(SORT _KANARIV_WINSDK_VERSIONS COMPARE NATURAL ORDER DESCENDING)
      list(GET _KANARIV_WINSDK_VERSIONS 0 _KANARIV_WINSDK_VERSION)
    endif()
    unset(_KANARIV_WINSDK_CANDIDATES)
    unset(_KANARIV_WINSDK_VERSIONS)
    unset(_sdk_dir)
    unset(_sdk_ver)
  endif()
  set(_KANARIV_CPPWINRT_INCLUDE_DIR
    "${_KANARIV_WINSDK_ROOT}/Include/${_KANARIV_WINSDK_VERSION}/cppwinrt")
  if(_KANARIV_WINSDK_VERSION AND EXISTS "${_KANARIV_CPPWINRT_INCLUDE_DIR}")
    message(STATUS "cppwinrt (${_KANARIV_WINSDK_SOURCE}): Windows SDK ${_KANARIV_WINSDK_VERSION} -> ${_KANARIV_CPPWINRT_INCLUDE_DIR}")
    target_include_directories(${KANARIV_SERVICE_TARGET} SYSTEM PRIVATE
      ${_KANARIV_CPPWINRT_INCLUDE_DIR})
    target_include_directories(service_objects SYSTEM PRIVATE
      ${_KANARIV_CPPWINRT_INCLUDE_DIR})
  else()
    message(STATUS "cppwinrt: no matching Windows SDK headers under ${_KANARIV_WINSDK_ROOT}/Include")
  endif()
  unset(_KANARIV_WINSDK_ROOT)
  unset(_KANARIV_WINSDK_VERSION)
  unset(_KANARIV_WINSDK_SOURCE)
  unset(_KANARIV_CPPWINRT_INCLUDE_DIR)
endif()

ca_set_module_warnings(service)
if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(service)
  if(KANARIV_BUILD_STATIC)
    # A real service_shared target only exists when static libraries are
    # built too. In a shared-only build kanariv_add_shared_variant() aliases
    # service_shared to service, and CMake rejects target_* commands on ALIAS
    # targets. service already carries these platform deps (applied above),
    # so the alias needs nothing extra.
    # service_shared carries $<TARGET_OBJECTS:service_objects>, so the dbus
    # include dirs are already on the objects; only the link line is needed.
    kanariv_service_platform_libs(service_shared)
  endif()
endif()
