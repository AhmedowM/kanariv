module;
#include <kanariv/dsp.hpp>

export module kanariv.dsp;

export namespace kanariv::dsp::fft {
using ::kanariv::dsp::fft::FftPlan;
using ::kanariv::dsp::fft::fftReal;
using ::kanariv::dsp::fft::ifftReal;
} // namespace kanariv::dsp::fft

export namespace kanariv::dsp::fft::window {
using ::kanariv::dsp::fft::window::applyInPlace;
using ::kanariv::dsp::fft::window::hamming;
using ::kanariv::dsp::fft::window::hann;
} // namespace kanariv::dsp::fft::window

export namespace kanariv::dsp::loudness {
using ::kanariv::dsp::loudness::integratedLoudness;
using ::kanariv::dsp::loudness::kReplayGain2ReferenceLufs;
using ::kanariv::dsp::loudness::linearToDbtp;
using ::kanariv::dsp::loudness::LoudnessMeter;
using ::kanariv::dsp::loudness::LoudnessResult;
using ::kanariv::dsp::loudness::replayGainDb;
} // namespace kanariv::dsp::loudness
