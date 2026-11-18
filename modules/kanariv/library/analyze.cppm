module;
#include <kanariv/library/analyze.hpp>

export module kanariv.library:analyze;

// The explicit ReplayGain 2.0 pass: measure tracks with the dsp loudness meter
// and persist the gains through the store. Playback never triggers it.
export namespace kanariv::library {
using ::kanariv::library::analyzeLibrary;
using ::kanariv::library::AnalyzeOptions;
using ::kanariv::library::Analyzer;
using ::kanariv::library::AnalyzeReport;
} // namespace kanariv::library
