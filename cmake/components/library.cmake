# library.cmake -- kanariv::library (DB-coupled library scan orchestration).
# The only component that depends on both media (walk, fingerprints, tags) and
# db (batched upsert of the discovered rows), so db itself no longer links the
# decoder stack (W2). Consumers that only need the scan entry point depend on
# kanariv::contracts (ILibraryScanner) instead.
set(KANARIV_LIBRARY_MODULE_SOURCES
  modules/kanariv/library.cppm
  modules/kanariv/library/scan.cppm
)
kanariv_add_component(library SOURCES src/library/scan.cpp MODULE_SOURCES ${KANARIV_LIBRARY_MODULE_SOURCES} DEPS kanariv::contracts kanariv::db kanariv::media kanariv::types kanariv::utils Threads::Threads)
ca_set_module_warnings(library)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(library)
endif()
