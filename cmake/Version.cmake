# Version precedence (top wins). Release candidates ride along as an optional
# semver suffix (e.g. v1.0.0-rc1): the numeric core feeds project()/package
# machinery (numbers only), the full string feeds display (--version, CPack
# names). Suffix is derived from the tag/override/file -- never typed twice.
#   1. -DKANARIV_PROJECT_VERSION=X.Y.Z[-suffix] (explicit override for packagers stripping .git)
#   2. git describe --tags (normal clones, CI with fetch-depth: 0)
#   3. VERSION file (tarballs, shallow exports -- bumped in the release-prep commit)
#   4. 0.1.0 + loud warning (never silent; NOT a release build)
# Release-toolchain contract (single source): minsize intent with clang
# on Linux/Windows, apple toolchain on macOS. Stated here; CI and docs
# reference this, do not duplicate elsewhere.
option(KANARIV_REQUIRE_GIT_VERSION "fail configure when no git tag is found (default OFF)" OFF)
set(KANARIV_VERSION_SUFFIX "")
macro(_kanariv_split_version INPUT OUT_OK)
  if("${INPUT}" MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(PROJECT_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set(KANARIV_VERSION_SUFFIX "${CMAKE_MATCH_4}")
    set(${OUT_OK} TRUE)
  else()
    set(${OUT_OK} FALSE)
  endif()
endmacro()
if(DEFINED KANARIV_PROJECT_VERSION AND NOT KANARIV_PROJECT_VERSION STREQUAL "")
  _kanariv_split_version("${KANARIV_PROJECT_VERSION}" _kanariv_ver_ok)
  if(NOT _kanariv_ver_ok)
    message(FATAL_ERROR "KANARIV_PROJECT_VERSION='${KANARIV_PROJECT_VERSION}' is not X.Y.Z[-suffix] (optional leading v)")
  endif()
  set(GIT_TAG "v${PROJECT_VERSION}${KANARIV_VERSION_SUFFIX}")
  message(STATUS "Using explicit version override: ${PROJECT_VERSION}${KANARIV_VERSION_SUFFIX} (-DKANARIV_PROJECT_VERSION)")
else()
  execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  _kanariv_split_version("${GIT_TAG}" _kanariv_ver_ok)
  if(NOT _kanariv_ver_ok)
    set(GIT_TAG "")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VERSION_FILE_CONTENT)
    string(STRIP "${VERSION_FILE_CONTENT}" VERSION_FILE_CONTENT)
    _kanariv_split_version("${VERSION_FILE_CONTENT}" _kanariv_ver_ok)
    if(_kanariv_ver_ok)
      set(GIT_TAG "v${PROJECT_VERSION}${KANARIV_VERSION_SUFFIX}")
      message(STATUS "No git tag -- using VERSION file: ${PROJECT_VERSION}${KANARIV_VERSION_SUFFIX}")
    else()
      if(KANARIV_REQUIRE_GIT_VERSION)
        message(FATAL_ERROR "KANARIV_REQUIRE_GIT_VERSION=ON but no git tag found -- clone with tags (git fetch --tags) or configure with -DKANARIV_REQUIRE_GIT_VERSION=OFF")
      endif()
      message(WARNING "No git tag and no usable VERSION file -- using fallback version 0.1.0. Tag-less builds are NOT release builds; docs/CHANGELOG claim the latest tagged release.")
      set(PROJECT_VERSION_MAJOR "0")
      set(PROJECT_VERSION_MINOR "1")
      set(PROJECT_VERSION_PATCH "0")
      set(PROJECT_VERSION "0.1.0")
      set(KANARIV_VERSION_SUFFIX "")
    endif()
  endif()
endif()
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE KANARIV_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT KANARIV_GIT_HASH)
  set(KANARIV_GIT_HASH "unknown")
endif()
set(KANARIV_VERSION_FULL "${PROJECT_VERSION}")
if(GIT_TAG)
  set(KANARIV_VERSION_FULL "${GIT_TAG}")
endif()
