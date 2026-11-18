# combined is the shared libkanariv umbrella: utils/config/media/player/db/
# library/engine/ipc/service/client/app linked into a single shared lib. It is
# built only when KANARIV_BUILD_SHARED and KANARIV_BUILD_COMBINED (default ON)
# are both enabled; the per-component *_shared targets always exist alongside
# it.
#
# Links component object files directly instead of WHOLE_ARCHIVE-ing static
# archives. This avoids duplicate symbols from overlapping transitive deps
# (player->media, db->media, engine->player+media+utils, all->utils) and
# removes the need for --allow-multiple-definition.
if(KANARIV_BUILD_SHARED AND KANARIV_BUILD_COMBINED)
  add_library(combined SHARED)
  add_library(kanariv::combined ALIAS combined)
  set_target_properties(combined PROPERTIES OUTPUT_NAME kanariv VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR} PDB_NAME kanariv_)
  if(MSVC)
    # The CLI executable is also named kanariv and uses the default kanariv.ilk.
    # Avoid an incremental-link state collision when Ninja links both targets.
    target_link_options(combined PRIVATE /INCREMENTAL:NO)
  endif()
  target_include_directories(combined PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(combined PUBLIC cxx_std_23)
  # combined is a DLL like the component shared variants, so its consumers take
  # the __declspec(dllimport) branch of KANARIV_API.
  target_compile_definitions(combined INTERFACE KANARIV_USING_DLL)
  # Object libraries are compiled once with PIC; linking them directly pulls
  # each object file exactly once. Vendor static libs (sqlite3, blake3) are
  # linked normally -- the linker pulls only needed objects.
  target_sources(combined PRIVATE
    $<TARGET_OBJECTS:utils_objects>
    $<TARGET_OBJECTS:config_objects>
    $<TARGET_OBJECTS:dsp_objects>
    $<TARGET_OBJECTS:media_objects>
    $<TARGET_OBJECTS:player_objects>
    $<TARGET_OBJECTS:db_objects>
    $<TARGET_OBJECTS:library_objects>
    $<TARGET_OBJECTS:engine_objects>
    $<TARGET_OBJECTS:ipc_objects>
    $<TARGET_OBJECTS:service_objects>
    $<TARGET_OBJECTS:client_objects>
    $<TARGET_OBJECTS:app_objects>
  )
  target_link_libraries(combined PRIVATE
    kanariv::sqlite3
    blake3
    kissfft
    tag
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample
    Threads::Threads)
  if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(combined PRIVATE rt)
  endif()
  # combined carries service_objects, so it needs the same platform libraries
  # (WinRT/COM, MediaPlayer framework, or libdbus-1) the service component
  # links. The list lives in one helper so the two cannot drift.
  kanariv_service_platform_libs(combined)
  if(KANARIV_ENABLE_SANITIZERS)
    kanariv_apply_sanitizers(combined)
  endif()
endif()
