#pragma once

/**
 * @file version.hpp
 * @brief Version helpers in caudio::version -- wraps the CMake-generated
 * version_config.hpp constants.
 * @defgroup caudio_version caudio version
 * @details Thin header that exposes version constants plus helpers
 * `version()` / `versionString()`. Includes the CMake-generated
 * version_config.hpp for version constants.
 */

#include <caudio/version_config.hpp>
#include <string>
#include <string_view>

namespace caudio::version {

/**
 * @brief Returns library full version (git tag).
 * @ingroup caudio_version
 * @return String view of caudio::versionFull.
 */
inline std::string_view version() noexcept {
    return caudio::detail::versionFull;
}

/**
 * @brief Returns library full version as owned string.
 * @ingroup caudio_version
 * @return Copy of caudio::versionFull.
 */
inline std::string versionString() {
    return std::string(caudio::detail::versionFull);
}

/**
 * @brief Returns short version (PROJECT_VERSION).
 * @ingroup caudio_version
 * @return String view of caudio::detail::shortVersion.
 */
inline std::string_view shortVersion() noexcept {
    return caudio::detail::shortVersion;
}

/**
 * @brief Returns version commit hash (short).
 * @ingroup caudio_version
 * @return String view of caudio::detail::versionCommit.
 */
inline std::string_view versionCommit() noexcept {
    return caudio::detail::versionCommit;
}

/**
 * @brief Version major component.
 * @ingroup caudio_version
 * @return versionMajor.
 */
constexpr int versionMajor() noexcept {
    return caudio::detail::versionMajor;
}

/**
 * @brief Version minor component.
 * @ingroup caudio_version
 * @return Numeric minor component of the library version.
 */
constexpr int versionMinor() noexcept {
    return caudio::detail::versionMinor;
}

/**
 * @brief Version patch component.
 * @ingroup caudio_version
 * @return Numeric patch component of the library version.
 */
constexpr int versionPatch() noexcept {
    return caudio::detail::versionPatch;
}

} // namespace caudio::version
