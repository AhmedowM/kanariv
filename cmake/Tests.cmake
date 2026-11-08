# Tests -- Catch2 via system package or FetchContent (144 tests:
# utils/player/db/engine/ipc/cli paths).
if(KANARIV_ENABLE_TESTS)
  find_package(Catch2 QUIET)
  if(NOT Catch2_FOUND)
    # Keep Catch2 out of our install/packages (cf. JSON_Install=OFF above).
    set(CATCH_INSTALL_DOCS OFF)
    set(CATCH_INSTALL_EXTRAS OFF)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.16.0
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
  else()
    message(STATUS "Catch2 found (system): tests link Catch2::Catch2WithMain")
  endif()
  include(CTest)
  include(Catch)
  enable_testing()

  if(KANARIV_TEST_NOAUDIO)
    add_compile_definitions(KANARIV_TEST_NOAUDIO_BUILD=1)
  endif()

  function(kanariv_add_catch_test name src)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "WITH_TEST_DATA;WITH_ALLOW_MULTIPLE_DEFINITION" "TEST_CLI_BIN" "LIBS")
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Catch2::Catch2WithMain)
    # + src/: white-box tests cover src-internal helpers (engine detail fns)
    # directly. Build-tree only -- src/ is never installed.
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(${name} SYSTEM PRIVATE vendor)
    target_compile_features(${name} PRIVATE cxx_std_23)
    kanariv_apply_extra_flags(${name})
    if(ARG_WITH_TEST_DATA)
      target_compile_definitions(${name} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    endif()
    if(ARG_TEST_CLI_BIN)
      target_compile_definitions(${name} PRIVATE TEST_CLI_BIN="${ARG_TEST_CLI_BIN}")
    endif()
    if(ARG_WITH_ALLOW_MULTIPLE_DEFINITION)
      target_link_options(${name} PRIVATE $<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<PLATFORM_ID:Darwin>>>:-Wl,--allow-multiple-definition>)
    endif()
    if(KANARIV_ENABLE_SANITIZERS)
      kanariv_apply_sanitizers(${name})
    endif()
    catch_discover_tests(${name} PROPERTIES SKIP_RETURN_CODE 4)
    # Pin test discovery to the shared libraries it links so Ninja cannot
    # run catch_discover_tests (which executes the binary) while the DLLs
    # it depends on are still being linked in parallel. On Windows a
    # partially-linked DLL produces STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139).
    foreach(lib ${ARG_LIBS})
      if(TARGET ${lib})
        add_dependencies(${name} ${lib})
      endif()
    endforeach()
  endfunction()

  kanariv_add_catch_test(test_utils_result tests/test_utils_result.cpp LIBS kanariv::utils)
  kanariv_add_catch_test(test_utils_ring tests/test_utils_ring.cpp LIBS kanariv::utils)
  kanariv_add_catch_test(test_utils_queue tests/test_utils_queue.cpp LIBS kanariv::utils)
  kanariv_add_catch_test(test_utils_thread tests/test_utils_thread.cpp LIBS kanariv::utils)
  kanariv_add_catch_test(test_utils_log tests/test_utils_log.cpp LIBS kanariv::utils)

  kanariv_add_catch_test(test_reader tests/test_reader.cpp LIBS kanariv::player kanariv::media kanariv::utils)
  kanariv_add_catch_test(test_decoder tests/test_decoder.cpp LIBS kanariv::player kanariv::media kanariv::utils WITH_TEST_DATA)
  kanariv_add_catch_test(test_ffmpeg tests/test_ffmpeg.cpp LIBS kanariv::player kanariv::media kanariv::utils WITH_TEST_DATA)

  kanariv_add_catch_test(test_db tests/test_db.cpp LIBS kanariv::db kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_write_thread tests/test_write_thread.cpp LIBS kanariv::db kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_output tests/test_output.cpp LIBS kanariv::player kanariv::utils)

  kanariv_add_catch_test(test_engine_queue tests/test_engine_queue.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_engine_history tests/test_engine_history.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_engine_events tests/test_engine_events.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_engine_state tests/test_engine_state.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_engine_gapless tests/test_engine_gapless.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3)

  kanariv_add_catch_test(test_db_scan tests/test_db_scan.cpp LIBS kanariv::db kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_tag_write tests/test_tag_write.cpp LIBS kanariv::service kanariv::db kanariv::player kanariv::utils kanariv::sqlite3 WITH_TEST_DATA)
  kanariv_add_catch_test(test_db_search tests/test_db_search.cpp LIBS kanariv::db kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_db_json tests/test_db_json.cpp LIBS kanariv::db kanariv::utils kanariv::sqlite3)

  kanariv_add_catch_test(test_player_integration tests/test_player_integration.cpp LIBS kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3 WITH_TEST_DATA)

  kanariv_add_catch_test(test_ipc tests/test_ipc.cpp LIBS kanariv::service kanariv::client kanariv::ipc kanariv::engine kanariv::db kanariv::player kanariv::utils kanariv::sqlite3 WITH_ALLOW_MULTIPLE_DEFINITION)
  kanariv_add_catch_test(test_config tests/test_config.cpp LIBS kanariv::config kanariv::utils)
  kanariv_add_catch_test(test_app_render tests/test_app_render.cpp LIBS kanariv::app kanariv::ipc kanariv::utils)

  kanariv_add_catch_test(test_media_source tests/test_media_source.cpp LIBS kanariv::media kanariv::player kanariv::utils)
  kanariv_add_catch_test(test_db_source tests/test_db_source.cpp LIBS kanariv::db kanariv::utils kanariv::sqlite3)
  kanariv_add_catch_test(test_service_host tests/test_service_host.cpp LIBS kanariv::service kanariv::config kanariv::db kanariv::utils kanariv::sqlite3)

  # CLI golden tests: spawn the real executable with hermetic temp configs
  # and assert on stdout/stderr/exit codes (help texts, parse errors,
  # no-daemon errors, full daemon lifecycle). Needs the CLI binary, so it
  # only exists when KANARIV_BUILD_CLI is on.
  if(TARGET kanariv_cli)
    kanariv_add_catch_test(test_cli_golden tests/test_cli_golden.cpp LIBS Catch2::Catch2WithMain WITH_TEST_DATA TEST_CLI_BIN "$<TARGET_FILE:kanariv_cli>")
  endif()

  # D-4: compile every public header as a standalone TU (build gate, no ctest
  # entry -- the objects are never linked, only compiled). Opt-in via
  # KANARIV_ENABLE_HEADER_SELF_CONTAINMENT because it adds one compile per
  # public header with no link artifact.
  if(KANARIV_ENABLE_HEADER_SELF_CONTAINMENT)
    kanariv_add_header_self_containment()
  endif()
endif()
