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
# INCLUDES vendor: src/media/scan.cpp and fingerprint.cpp include <blake3.h>.
# PRIVATE_DEPS carries the decode backend (FFmpeg, used only by
# src/media/decoders/ffmpeg.cpp via ffmpeg_impl.hpp) and BLAKE3 (scan discovery
# hash + sampled fingerprint). The helper forwards them to the object library
# for compile-time includes and to both flavours for linking; include dirs of
# imported targets are treated as SYSTEM, so FFmpeg headers stay exempt from
# our warning policy.
kanariv_add_component(media SOURCES ${KANARIV_MEDIA_SOURCES} MODULE_SOURCES ${KANARIV_MEDIA_MODULE_SOURCES} DEPS kanariv::utils kanariv::types PRIVATE_DEPS FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swresample blake3 INCLUDES vendor)
ca_set_module_warnings(media)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(media)
endif()
