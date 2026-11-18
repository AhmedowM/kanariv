#pragma once

/**
 * @file media.hpp
 * @brief Umbrella header for the kanariv.media module.
 * @ingroup kanariv_media
 * @defgroup kanariv_media kanariv media
 *
 * Media input module providing:
 * - Reader abstractions (FileReader, MemoryReader) for input sources
 * - FFmpeg-based Decoder for all common formats
 * - Metadata extraction and audio-extension helpers
 * - Library scan discovery: filesystem walk, fingerprinting, tag extraction
 *   (persistence of the result lives in `kanariv::library::Scanner`)
 * - ReplayGain 2.0 analysis: decode a file, measure EBU R128 loudness and
 *   true peak (`kanariv::library` turns block energies into album gain)
 */

#include <kanariv/media/audio_ext.hpp>
#include <kanariv/media/decoder.hpp>
#include <kanariv/media/loudness.hpp>
#include <kanariv/media/reader.hpp>
#include <kanariv/media/scan.hpp>
