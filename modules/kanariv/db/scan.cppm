module;
#include <kanariv/db/scan.hpp>

export module kanariv.db:scan;

// The db-side scan API is the persistence half only: `scanLibrary` upserts the
// tracks discovered by `kanariv::media::scanTracks`.
export namespace kanariv::db {
using ::kanariv::db::scanLibrary;
} // namespace kanariv::db

export namespace kanariv::db::detail {
using ::kanariv::media::hasAudioExt;
}
