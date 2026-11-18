#pragma once

/**
 * @file dsp.hpp
 * @brief Umbrella header for the kanariv.dsp module.
 * @ingroup kanariv_dsp
 * @defgroup kanariv_dsp kanariv dsp
 *
 * Signal processing over float sample blocks, independent of where the samples
 * came from:
 * - FFT plans (real and complex) with windowing helpers
 * - EBU R128 integrated loudness and true peak (ReplayGain 2.0)
 *
 * `kanariv::media` stays the file/descriptor side (read, decode, tags, scan)
 * and depends on this module when it needs analysis, not the other way round.
 */

#include <kanariv/dsp/fft.hpp>
#include <kanariv/dsp/loudness.hpp>
