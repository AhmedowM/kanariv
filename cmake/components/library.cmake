# library.cmake -- kanariv::library (DB-coupled scan and ReplayGain analysis).
# The only component that depends on both media (walk, fingerprints, tags,
# loudness decode) and db (batched upsert of the discovered rows), so db itself
# no longer links the decoder stack (W2). Consumers that only need the scan
# entry point depend on kanariv::contracts (ILibraryScanner) instead.
# kanariv::dsp is direct: the analyze pass integrates the per-track album
# block energies itself.
set(KANARIV_LIBRARY_MODULE_SOURCES
  modules/kanariv/library.cppm
  modules/kanariv/library/scan.cppm
  modules/kanariv/library/analyze.cppm
)
kanariv_add_component(library SOURCES src/library/scan.cpp src/library/analyze.cpp MODULE_SOURCES ${KANARIV_LIBRARY_MODULE_SOURCES} DEPS kanariv::contracts kanariv::db kanariv::dsp kanariv::media kanariv::types kanariv::utils Threads::Threads)
ca_set_module_warnings(library)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(library)
endif()
