# player.cmake -- kanariv::player (miniaudio output + gapless playback).
# The module re-exports kanariv.media and keeps reader/decoder aliases in
# kanariv.player; their partition sources are retained for compatibility but
# are no longer compiled into this target.
set(KANARIV_PLAYER_MODULE_SOURCES
  modules/kanariv/player.cppm
  modules/kanariv/player/core.cppm
  modules/kanariv/player/output.cppm
)
set(KANARIV_PLAYER_SOURCES
  src/player/output.cpp
  src/player/core.cpp
)
kanariv_add_component(player SOURCES ${KANARIV_PLAYER_SOURCES} MODULE_SOURCES ${KANARIV_PLAYER_MODULE_SOURCES} DEPS kanariv::utils kanariv::types INCLUDES vendor PRIVATE_DEPS kanariv::media)
# GCC cannot parse Apple's ObjC-blocks system headers pulled in by miniaudio's
# CoreAudio backend; Apple Clang-only. Local mac devs (Xcode Clang) keep full
# audio; GNU-on-macOS CI builds fall back to the other backends (NOAUDIO anyway).
target_compile_definitions(player PRIVATE $<$<AND:$<PLATFORM_ID:Darwin>,$<CXX_COMPILER_ID:GNU>>:MA_NO_COREAUDIO>)
target_compile_definitions(player_objects PRIVATE $<$<AND:$<PLATFORM_ID:Darwin>,$<CXX_COMPILER_ID:GNU>>:MA_NO_COREAUDIO>)
# Trimmed miniaudio: device playback +
# enumeration only. No decoding/encoding (FFmpeg owns that), no generation,
# resource manager, node graph, engine, or built-in wav/flac/mp3 decoders.
# miniaudio.h is included only by src/player/miniaudio_impl.cpp +
# src/player/output.cpp, so PRIVATE covers every consumer.
target_compile_definitions(player PRIVATE
  MA_NO_DECODING=1
  MA_NO_ENCODING=1
  MA_NO_GENERATION=1
  MA_NO_RESOURCE_MANAGER=1
  MA_NO_NODE_GRAPH=1
  MA_NO_ENGINE=1
  MA_NO_WAV=1
  MA_NO_FLAC=1
  MA_NO_MP3=1
)
target_compile_definitions(player_objects PRIVATE
  MA_NO_DECODING=1
  MA_NO_ENCODING=1
  MA_NO_GENERATION=1
  MA_NO_RESOURCE_MANAGER=1
  MA_NO_NODE_GRAPH=1
  MA_NO_ENGINE=1
  MA_NO_WAV=1
  MA_NO_FLAC=1
  MA_NO_MP3=1
)
if(KANARIV_ENABLE_MODULES)
  ca_set_module_warnings(player)
endif()

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(player EXTRA_DEPS kanariv::utils_shared kanariv::media_shared)
endif()
