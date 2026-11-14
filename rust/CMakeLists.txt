# Experimental Rust components.
#
# This file is added only when KANARIV_ENABLE_RUST_CLI or KANARIV_ENABLE_RUST_DB
# is ON (see the top-level CMakeLists.txt). It only orchestrates cargo; nothing
# here links C++ code or participates in the daemon/audio path.
#
#   KANARIV_ENABLE_RUST_CLI=ON  builds kanariv-rs and stages it beside the C++
#                               CLI so the sibling daemon lookup works.
#   KANARIV_ENABLE_RUST_DB=ON   adds the `kanariv_db_check` quality gate for the
#                               experimental kanariv-db crate (not linked).
#
# Both are also drivable via scripts/configure.py dev --toolchain clang
# --define KANARIV_ENABLE_RUST_CLI=ON (likewise ..._RUST_DB).

find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR
    "KANARIV_ENABLE_RUST_CLI/KANARIV_ENABLE_RUST_DB require cargo on PATH "
    "(install the Rust toolchain or disable the options)")
endif()

set(KANARIV_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/target")
set(KANARIV_RUST_CARGO_ENV
  ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${KANARIV_RUST_TARGET_DIR}")

set(KANARIV_RUST_WORKSPACE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml")

if(KANARIV_ENABLE_RUST_CLI)
  file(GLOB_RECURSE KANARIV_RUST_CLI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-cli/src/*.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-ipc/src/*.rs")

  set(KANARIV_RUST_BIN
    "${KANARIV_RUST_TARGET_DIR}/release/kanariv-rs${CMAKE_EXECUTABLE_SUFFIX}")

  add_custom_command(
    OUTPUT "${KANARIV_RUST_BIN}"
    COMMAND ${KANARIV_RUST_CARGO_ENV}
            "${CARGO_EXECUTABLE}" build --release -p kanariv-cli
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS
      ${KANARIV_RUST_WORKSPACE_FILES}
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-cli/Cargo.toml"
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-cli/build.rs"
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-ipc/Cargo.toml"
      ${KANARIV_RUST_CLI_SOURCES}
    COMMENT "Building experimental Rust CLI (kanariv-rs)"
    VERBATIM
  )

  add_custom_target(kanariv_rs ALL DEPENDS "${KANARIV_RUST_BIN}")

  # Stage beside the C++ executable so `kanariv-rs start` can find its sibling
  # daemon. The C++ target keeps its own name and output; nothing is replaced.
  if(TARGET kanariv_cli)
    add_dependencies(kanariv_rs kanariv_cli)
    add_custom_command(
      TARGET kanariv_rs POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${KANARIV_RUST_BIN}" "$<TARGET_FILE_DIR:kanariv_cli>"
      VERBATIM
    )
  endif()

  # Quality gate for the CLI crates; not part of the default build.
  add_custom_target(kanariv_rs_check
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" fmt --all -- --check
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" clippy
            -p kanariv-cli -p kanariv-ipc --all-targets -- -D warnings
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" test
            -p kanariv-cli -p kanariv-ipc
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running rustfmt, clippy, and tests for kanariv-cli/kanariv-ipc"
    USES_TERMINAL
    VERBATIM
  )
endif()

if(KANARIV_ENABLE_RUST_DB)
  # Quality gate for the experimental DB crate; not part of the default build
  # and never linked into the daemon. Tests include a byte-for-byte comparison
  # of the Rust schema constant against src/db/core.cpp.
  add_custom_target(kanariv_db_check
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" fmt --all -- --check
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" clippy
            -p kanariv-db --all-targets -- -D warnings
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" test -p kanariv-db
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running rustfmt, clippy, and tests for kanariv-db"
    USES_TERMINAL
    VERBATIM
  )
endif()

if(KANARIV_ENABLE_RUST_DB_FFI)
  # Read-only C ABI pilot: build the cdylib, then a standalone C++ executable
  # that compares Rust search/lookup with the C++ database on the same file.
  # Nothing here is linked into the daemon or the audio path.
  file(GLOB_RECURSE KANARIV_RUST_FFI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-db-ffi/src/*.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-db/src/*.rs")

  set(KANARIV_RUST_FFI_RELEASE_DIR "${KANARIV_RUST_TARGET_DIR}/release")
  if(WIN32)
    set(KANARIV_RUST_FFI_LIBRARY "${KANARIV_RUST_FFI_RELEASE_DIR}/kanariv_db_ffi.dll")
    set(KANARIV_RUST_FFI_IMPLIB "${KANARIV_RUST_FFI_RELEASE_DIR}/kanariv_db_ffi.dll.lib")
    set(KANARIV_RUST_FFI_OUTPUTS
      "${KANARIV_RUST_FFI_LIBRARY}" "${KANARIV_RUST_FFI_IMPLIB}")
  elseif(APPLE)
    set(KANARIV_RUST_FFI_LIBRARY "${KANARIV_RUST_FFI_RELEASE_DIR}/libkanariv_db_ffi.dylib")
    set(KANARIV_RUST_FFI_OUTPUTS "${KANARIV_RUST_FFI_LIBRARY}")
  else()
    set(KANARIV_RUST_FFI_LIBRARY "${KANARIV_RUST_FFI_RELEASE_DIR}/libkanariv_db_ffi.so")
    set(KANARIV_RUST_FFI_OUTPUTS "${KANARIV_RUST_FFI_LIBRARY}")
  endif()

  add_custom_command(
    OUTPUT ${KANARIV_RUST_FFI_OUTPUTS}
    COMMAND ${KANARIV_RUST_CARGO_ENV}
            "${CARGO_EXECUTABLE}" build --release -p kanariv-db-ffi
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS
      ${KANARIV_RUST_WORKSPACE_FILES}
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-db/Cargo.toml"
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-db-ffi/Cargo.toml"
      ${KANARIV_RUST_FFI_SOURCES}
    COMMENT "Building experimental Rust DB C ABI library"
    VERBATIM
  )
  add_custom_target(kanariv_db_ffi ALL DEPENDS "${KANARIV_RUST_FFI_LIBRARY}")

  # Quality gate for the FFI crate (the parity executable is run by ctest).
  add_custom_target(kanariv_db_ffi_check
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" fmt --all -- --check
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" clippy
            -p kanariv-db-ffi --all-targets -- -D warnings
    COMMAND ${KANARIV_RUST_CARGO_ENV} "${CARGO_EXECUTABLE}" test -p kanariv-db-ffi
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running rustfmt, clippy, and tests for kanariv-db-ffi"
    USES_TERMINAL
    VERBATIM
  )

  if(KANARIV_ENABLE_TESTS)
    add_executable(test_rust_db_ffi
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-db-ffi/parity.cpp")
    target_include_directories(test_rust_db_ffi PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-db-ffi/include"
      "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(test_rust_db_ffi PRIVATE
      kanariv::db kanariv::utils kanariv::sqlite3)
    target_compile_features(test_rust_db_ffi PRIVATE cxx_std_23)
    kanariv_apply_extra_flags(test_rust_db_ffi)
    add_dependencies(test_rust_db_ffi kanariv_db_ffi)

    add_library(kanariv_db_ffi_imported SHARED IMPORTED)
    set_target_properties(kanariv_db_ffi_imported PROPERTIES
      IMPORTED_LOCATION "${KANARIV_RUST_FFI_LIBRARY}")
    if(WIN32)
      set_target_properties(kanariv_db_ffi_imported PROPERTIES
        IMPORTED_IMPLIB "${KANARIV_RUST_FFI_IMPLIB}")
      add_custom_command(TARGET test_rust_db_ffi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${KANARIV_RUST_FFI_LIBRARY}" "$<TARGET_FILE_DIR:test_rust_db_ffi>"
        VERBATIM)
    else()
      set_target_properties(test_rust_db_ffi PROPERTIES
        BUILD_RPATH "${KANARIV_RUST_FFI_RELEASE_DIR}")
    endif()
    target_link_libraries(test_rust_db_ffi PRIVATE kanariv_db_ffi_imported)

    # Testing is enabled by cmake/Tests.cmake, which is included after this
    # subdirectory; defer registration to the end of the top-level configure.
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL add_test
      NAME test_rust_db_ffi COMMAND test_rust_db_ffi)
  endif()
endif()
