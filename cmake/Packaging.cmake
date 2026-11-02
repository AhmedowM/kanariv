# Packaging (CPack) -- TGZ/ZIP only until the first release; distro
# generators (DEB/RPM/NSIS/DragNDrop) return when versions stabilize.
# Always invoke packaging through the `package` target
# (`cmake --build <build-dir> --target package`): it runs CPack with CWD in
# the build tree. A bare `cpack --config` from the source root stages
# packages under that root instead.
# Components (runtime vs dev, set on each install() above) produce one
# archive per component; the release workflow publishes one format per OS.
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
# Only our components ship: third-party FetchContent install rules carry no
# COMPONENT and would otherwise land in "Unspecified" archives (TagLib
# headers, .pc/config scripts). Our own tag.lib still ships via the dev
# component (KANARIV_INSTALL_LIBS); only their loose files are excluded.
set(CPACK_COMPONENTS_ALL runtime dev)
set(CPACK_PACKAGE_NAME "kanariv")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "kanariv")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++23 music player library -- utils, player, db, engine, ipc, client, service")
set(CPACK_PACKAGE_CONTACT "kanariv-devel@github.com")
string(TOLOWER "${CMAKE_SYSTEM_NAME}" CPACK_SYSTEM_NAME)
# Architecture tag for filenames (normalize generator spellings).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
  set(KANARIV_ARCH_TAG "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
  set(KANARIV_ARCH_TAG "arm64")
else()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" KANARIV_ARCH_TAG)
endif()
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}${KANARIV_VERSION_SUFFIX}-${CPACK_SYSTEM_NAME}-${KANARIV_ARCH_TAG}")
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
endif()
include(CPack)
