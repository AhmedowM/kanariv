# dsp.cmake -- kanariv::dsp (signal processing over float sample blocks).
# KissFFT is a private implementation detail: no installed dsp header includes
# it, so the vendor include/target stays PRIVATE.
set(KANARIV_DSP_MODULE_SOURCES
  modules/kanariv/dsp.cppm
)
set(KANARIV_DSP_SOURCES
  src/dsp/fft.cpp
  src/dsp/loudness.cpp
)
kanariv_add_component(dsp SOURCES ${KANARIV_DSP_SOURCES} MODULE_SOURCES ${KANARIV_DSP_MODULE_SOURCES} DEPS kanariv::utils PRIVATE_DEPS kissfft)
ca_set_module_warnings(dsp)

if(KANARIV_BUILD_SHARED)
  kanariv_add_shared_variant(dsp)
endif()
