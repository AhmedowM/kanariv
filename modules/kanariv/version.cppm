module;
#include <kanariv/version.hpp>

export module kanariv.version;

export namespace kanariv::version {
using ::kanariv::version::shortVersion;
using ::kanariv::version::version;
using ::kanariv::version::versionCommit;
using ::kanariv::version::versionMajor;
using ::kanariv::version::versionMinor;
using ::kanariv::version::versionPatch;
using ::kanariv::version::versionString;
} // namespace kanariv::version

export namespace kanariv::detail {
using ::kanariv::detail::shortVersion;
using ::kanariv::detail::versionCommit;
using ::kanariv::detail::versionFull;
using ::kanariv::detail::versionMajor;
using ::kanariv::detail::versionMinor;
using ::kanariv::detail::versionPatch;
} // namespace kanariv::detail
