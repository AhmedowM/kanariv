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
