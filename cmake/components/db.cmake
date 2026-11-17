# db.cmake -- kanariv::db (SQLite library/engine-state persistence).
# NOTE: statement/transaction/queue/migration/detail/fts/stmt_helpers live in
# src/db/ (NOT installed) and have no module partitions. Scanning moved to the
# library component (src/library/scan.cpp): it is the only place that combines
# the media discovery stack with the batched upsert, so db links neither media
# nor blake3 (W2).
set(KANARIV_DB_MODULE_SOURCES
  modules/kanariv/db.cppm
  modules/kanariv/db/core.cppm
  modules/kanariv/db/maintenance.cppm
  modules/kanariv/db/search.cppm
  modules/kanariv/db/smart.cppm
  modules/kanariv/db/json.cppm
)

set(KANARIV_DB_SOURCES
  src/db/core.cpp
  src/db/track_repo.cpp
  src/db/playlist_repo.cpp
  src/db/queue_repo.cpp
  src/db/history_repo.cpp
  src/db/engine_api.cpp
  src/db/state_store.cpp
  src/db/queue.cpp
  src/db/search.cpp
  src/db/smart.cpp
  src/db/json.cpp
  src/db/maintenance.cpp
  src/db/migration.cpp
  src/db/statement.cpp
  src/db/transaction.cpp
  src/db/fts.cpp
  src/db/stmt_helpers.cpp
)
# INCLUDES vendor: src/db/*.cpp include <sqlite3.h> from the amalgamation.
kanariv_add_component(db SOURCES ${KANARIV_DB_SOURCES} MODULE_SOURCES ${KANARIV_DB_MODULE_SOURCES} DEPS kanariv::contracts kanariv::types kanariv::utils Threads::Threads PRIVATE_DEPS kanariv::sqlite3 INCLUDES vendor)
ca_set_module_warnings(db)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(db)
endif()
