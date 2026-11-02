# media.cmake -- kanariv::media (reader/decoder/FFmpeg metadata/scan discovery).
set(KANARIV_MEDIA_MODULE_SOURCES
  modules/kanariv/media.cppm
  modules/kanariv/media/reader.cppm
  modules/kanariv/media/source.cppm
  modules/kanariv/media/decoder.cppm
  modules/kanariv/media/audio_ext.cppm
  modules/kanariv/media/scan.cppm
)
set(KANARIV_MEDIA_SOURCES
  src/media/reader.cpp
  src/media/source.cpp
  src/media/decoder.cpp
  src/media/decoders/ffmpeg.cpp
  src/media/scan.cpp
  src/media/fingerprint.cpp
)
kanariv_add_component(media SOURCES ${KANARIV_MEDIA_SOURCES} MODULE_SOURCES ${KANARIV_MEDIA_MODULE_SOURCES} DEPS kanariv::utils kanariv::types)
ca_set_module_warnings(media)
# FFmpeg (sole decoder backend) is used only by src/media/decoders/ffmpeg.cpp
# (via ffmpeg_impl.hpp), so link it here PRIVATE. STATIC private deps propagate
# as $<LINK_ONLY:...> to the final executables, so consumers still resolve
# libav symbols.
target_link_libraries(media PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample)
# BLAKE3 moved here with the scan discovery half (src/media/scan.cpp full-file
# hash + src/media/fingerprint.cpp sampled hash); db no longer hashes.
target_link_libraries(media PRIVATE blake3)

# Propagate FFmpeg include dirs to the object library so it can compile
# media's .cpp files against FFmpeg headers. The object library is not a
# link target, so target_link_libraries would not give it compile-time
# includes. Needed in both static and shared builds because media_objects
# compiles the same sources as the static media library.
# SYSTEM: FFmpeg headers trigger /WX warnings (C4244) under MSVC; treat as
# system includes so they don't fail warnings-as-errors builds.
get_target_property(_ff_inc FFmpeg::avcodec INTERFACE_INCLUDE_DIRECTORIES)
if(_ff_inc)
  target_include_directories(media_objects SYSTEM PRIVATE ${_ff_inc})
endif()

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(media EXTRA_DEPS kanariv::utils_shared)
  if(KANARIV_BUILD_STATIC)
    target_link_libraries(media_shared PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample blake3)
  endif()
endif()
