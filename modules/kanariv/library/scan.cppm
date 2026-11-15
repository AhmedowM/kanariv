module;
#include <kanariv/library/scan.hpp>

export module kanariv.library:scan;

// The scan orchestration: `Scanner` walks a library root with the media stack
// and upserts the result through the store. Media-free consumers depend on
// `kanariv.contracts` (ILibraryScanner) instead.
export namespace kanariv::library {
using ::kanariv::library::Scanner;
} // namespace kanariv::library
