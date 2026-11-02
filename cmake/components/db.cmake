# db.cmake -- kanariv::db (SQLite library/engine-state persistence).
# NOTE: statement/transaction/queue/schema/detail/fts/stmt_helpers and the
# background writer (src/db/write_thread.hpp) live in src/db/ (NOT installed)
# and have no module partitions. Fingerprinting moved to the media component
# (src/media/fingerprint.cpp) with the scan discovery half, so db no longer
# links blake3.
set(KANARIV_DB_MODULE_SOURCES
  modules/kanariv/db.cppm
  modules/kanariv/db/core.cppm
  modules/kanariv/db/scan.cppm
  modules/kanariv/db/maintenance.cppm
  modules/kanariv/db/search.cppm
  modules/kanariv/db/smart.cppm
  modules/kanariv/db/json.cppm
)

set(KANARIV_DB_SOURCES
  src/db/core.cpp
  src/db/engine_api.cpp
  src/db/queue.cpp
  src/db/scan.cpp
  src/db/search.cpp
  src/db/smart.cpp
  src/db/json.cpp
  src/db/maintenance.cpp
  src/db/write_thread.cpp
  src/db/statement.cpp
  src/db/transaction.cpp
  src/db/fts.cpp
  src/db/stmt_helpers.cpp
)
kanariv_add_component(db SOURCES ${KANARIV_DB_SOURCES} MODULE_SOURCES ${KANARIV_DB_MODULE_SOURCES} DEPS kanariv::contracts kanariv::utils kanariv::media Threads::Threads)
target_link_libraries(db PRIVATE kanariv::sqlite3)
ca_set_module_warnings(db)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(db EXTRA_DEPS kanariv::utils_shared kanariv::media_shared Threads::Threads)
  if(KANARIV_BUILD_STATIC)
    target_link_libraries(db_shared PRIVATE kanariv::sqlite3)
  endif()
endif()
