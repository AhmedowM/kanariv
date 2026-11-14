# Experimental Rust frontend (kanariv-rs).
#
# Phase 1 scope: an out-of-process CLI that speaks the existing IPC wire to
# the C++ daemon. It never replaces the C++ `kanariv` target, links no C++
# code, and contains no audio path. Enable with:
#
#   cmake -DKANARIV_ENABLE_RUST_CLI=ON ...
#   # or: python scripts/configure.py dev --toolchain clang \
#   #       --define KANARIV_ENABLE_RUST_CLI=ON
#
# The crate is a standalone cargo workspace; this file only orchestrates
# `cargo build` and stages the binary next to the C++ CLI so the sibling
# daemon lookup in `src/daemon.rs` works without extra configuration.

find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR
    "KANARIV_ENABLE_RUST_CLI=ON requires cargo on PATH "
    "(install the Rust toolchain or disable the option)")
endif()

file(GLOB_RECURSE KANARIV_RUST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-cli/src/*.rs")

set(KANARIV_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/target")
set(KANARIV_RUST_BIN
  "${KANARIV_RUST_TARGET_DIR}/release/kanariv-rs${CMAKE_EXECUTABLE_SUFFIX}")

add_custom_command(
  OUTPUT "${KANARIV_RUST_BIN}"
  COMMAND ${CMAKE_COMMAND} -E env
          "CARGO_TARGET_DIR=${KANARIV_RUST_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" build --release
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-cli/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/kanariv-cli/build.rs"
    ${KANARIV_RUST_SOURCES}
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

# Quality gate for the crate; not part of the default build.
add_custom_target(kanariv_rs_check
  COMMAND ${CMAKE_COMMAND} -E env
          "CARGO_TARGET_DIR=${KANARIV_RUST_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" fmt --check
  COMMAND ${CMAKE_COMMAND} -E env
          "CARGO_TARGET_DIR=${KANARIV_RUST_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" clippy --all-targets -- -D warnings
  COMMAND ${CMAKE_COMMAND} -E env
          "CARGO_TARGET_DIR=${KANARIV_RUST_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" test
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Running rustfmt, clippy, and tests for kanariv-rs"
  USES_TERMINAL
  VERBATIM
)
