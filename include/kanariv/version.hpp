#pragma once

/**
 * @file version.hpp
 * @brief Version helpers in kanariv::version -- wraps the CMake-generated
 * version_config.hpp constants.
 * @defgroup kanariv_version kanariv version
 * @details Thin header that exposes version constants plus helpers
 * `version()` / `versionString()`. Includes the CMake-generated
 * version_config.hpp for version constants.
 */

#include <kanariv/version_config.hpp>
#include <string>
#include <string_view>

namespace kanariv::version {

/**
 * @brief Returns library full version (git tag).
 * @ingroup kanariv_version
 * @return String view of kanariv::versionFull.
 */
inline std::string_view version() noexcept {
    return kanariv::detail::versionFull;
}

/**
 * @brief Returns library full version as owned string.
 * @ingroup kanariv_version
 * @return Copy of kanariv::versionFull.
 */
inline std::string versionString() {
    return std::string(kanariv::detail::versionFull);
}

/**
 * @brief Returns short version (PROJECT_VERSION).
 * @ingroup kanariv_version
 * @return String view of kanariv::detail::shortVersion.
 */
inline std::string_view shortVersion() noexcept {
    return kanariv::detail::shortVersion;
}

/**
 * @brief Returns version commit hash (short).
 * @ingroup kanariv_version
 * @return String view of kanariv::detail::versionCommit.
 */
inline std::string_view versionCommit() noexcept {
    return kanariv::detail::versionCommit;
}

/**
 * @brief Version major component.
 * @ingroup kanariv_version
 * @return versionMajor.
 */
constexpr int versionMajor() noexcept {
    return kanariv::detail::versionMajor;
}

/**
 * @brief Version minor component.
 * @ingroup kanariv_version
 * @return Numeric minor component of the library version.
 */
constexpr int versionMinor() noexcept {
    return kanariv::detail::versionMinor;
}

/**
 * @brief Version patch component.
 * @ingroup kanariv_version
 * @return Numeric patch component of the library version.
 */
constexpr int versionPatch() noexcept {
    return kanariv::detail::versionPatch;
}

} // namespace kanariv::version
