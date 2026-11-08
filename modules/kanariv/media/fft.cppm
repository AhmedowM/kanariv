module;
#include <kanariv/media/fft.hpp>

export module kanariv.media:fft;

export namespace kanariv::media::fft {
using ::kanariv::media::fft::FftPlan;
using ::kanariv::media::fft::fftReal;
using ::kanariv::media::fft::ifftReal;
} // namespace kanariv::media::fft

export namespace kanariv::media::fft::window {
using ::kanariv::media::fft::window::applyInPlace;
using ::kanariv::media::fft::window::hamming;
using ::kanariv::media::fft::window::hann;
} // namespace kanariv::media::fft::window
