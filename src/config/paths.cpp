/**
 * @file paths.cpp
 * @brief Canonical config/runtime paths and the endpoint digest.
 * @ingroup kanariv_config
 * @details XDG/Windows directory resolution, the FNV-1a endpoint identifier and
 * the socket/pid/lock/log path helpers. Loading and saving the config file
 * live in `store.cpp`.
 */

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <kanariv/config.hpp>
#include <kanariv/utils/error.hpp>

#include "config_detail.hpp"

namespace kanariv::config::detail {

std::filesystem::path defaultDbPath() {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "kanariv" / "library.db";
    }
#endif
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base;
    if (xdgData && xdgData[0] != '\0') {
        base = std::filesystem::path(xdgData) / "kanariv";
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0')
            home = std::getenv("USERPROFILE");
        if (home && home[0] != '\0') {
            base = std::filesystem::path(home) / ".local" / "share" / "kanariv";
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec) / "kanariv";
            if (ec)
                base = std::filesystem::path("/tmp/kanariv");
        }
    }
    return base / "library.db";
}

std::filesystem::path defaultConfigPath() {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "kanariv" / "config.json";
    }
#endif
    const char* xdgCfg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdgCfg && xdgCfg[0] != '\0') {
        base = std::filesystem::path(xdgCfg) / "kanariv";
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0')
            home = std::getenv("USERPROFILE");
        if (home && home[0] != '\0') {
            base = std::filesystem::path(home) / ".config" / "kanariv";
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec) / "kanariv";
            if (ec)
                base = std::filesystem::path("/tmp/kanariv");
        }
    }
    return base / "config.json";
}

std::filesystem::path legacyConfigPath() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "kanariv" / "config.json";
    }
#endif
    return {};
}

namespace {

constexpr std::uint64_t fnvOffsetBasis = 0xcbf2'9ce4'8422'2325ULL;
constexpr std::uint64_t fnvPrime = 0x0000'0100'0000'01b3ULL;

/// FNV-1a 64-bit over the given bytes (specified, toolchain independent).
std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = fnvOffsetBasis;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= fnvPrime;
    }
    return hash;
}

/// Canonical endpoint input bytes: UTF-8 with `/` separators when the path can be
/// converted; otherwise the raw spelling of the supplied input.
std::string canonicalEndpointInput(const std::filesystem::path& dbPath) {
    try {
        const std::u8string utf8 = dbPath.generic_u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    } catch (const std::exception&) {
        // Invalid native encoding; fall back to the raw input spelling below.
    } catch (...) {
    }
    try {
        return dbPath.string();
    } catch (...) {
        return {};
    }
}

} // namespace

std::string endpointIdForDb(const std::filesystem::path& dbPath) {
    return std::format("{:016x}", fnv1a64(canonicalEndpointInput(dbPath)));
}

namespace {

#ifndef _WIN32
/// Verifies the runtime directory, creating it owner-only when absent.
/// A pre-existing directory is accepted only when this user owns it and no
/// group/other write bit is set; it is refused rather than chmod'ed, because
/// on the shared temp fallback a pre-created directory is exactly the
/// endpoint-squatting case this guard exists for.
kanariv::utils::Expected<std::filesystem::path> secureRuntimeDir(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    const auto reject = [&dir](const std::string& why) {
        return kanariv::utils::Expected<std::filesystem::path>{std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                      "runtime directory '" + dir.generic_string() + "' " + why)}};
    };
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0) {
        if (errno != ENOENT)
            return reject(std::string("cannot be inspected: ") + std::strerror(errno));
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
            return reject("cannot be created: " + ec.message());
        std::error_code permEc;
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, permEc);
        if (permEc)
            return reject("cannot be restricted: " + permEc.message());
        if (::lstat(dir.c_str(), &st) != 0)
            return reject(std::string("cannot be inspected: ") + std::strerror(errno));
    }
    if (!S_ISDIR(st.st_mode))
        return reject("is not a directory");
    if (st.st_uid != ::geteuid())
        return reject("is not owned by the current user");
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return reject("is writable by other users");
    return dir;
}
#endif

} // namespace

kanariv::utils::Expected<std::filesystem::path> baseDirForSocket() {
    std::filesystem::path base;
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0')
        return std::filesystem::path(localApp) / "kanariv";
#endif
    const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR");
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0')
        home = std::getenv("USERPROFILE");
    if (xdgRuntime && xdgRuntime[0] != '\0')
        base = std::filesystem::path(xdgRuntime) / "kanariv";
    else if (xdgData && xdgData[0] != '\0')
        base = std::filesystem::path(xdgData) / "kanariv";
    else if (home && home[0] != '\0')
        base = std::filesystem::path(home) / ".local" / "share" / "kanariv";
    else {
        std::error_code ec;
        base = std::filesystem::temp_directory_path(ec) / "kanariv";
        if (ec)
            base = std::filesystem::path("/tmp/kanariv");
    }
#ifndef _WIN32
    return secureRuntimeDir(base);
#else
    return base;
#endif
}

} // namespace kanariv::config::detail

namespace kanariv::config {

kanariv::utils::Expected<std::string> socketPathFor(const std::filesystem::path& dbPath) {
    try {
        std::string id = detail::endpointIdForDb(dbPath);
#ifdef _WIN32
        return std::string("\\\\.\\pipe\\kanariv-") + id;
#else
        auto base = detail::baseDirForSocket();
        if (!base)
            return std::unexpected{base.error()};
        return (*base / ("kanariv-" + id + ".sock")).generic_string();
#endif
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "socketPathFor: unknown exception")};
    }
}

kanariv::utils::Expected<std::filesystem::path> pidPathFor(const std::filesystem::path& dbPath) {
    try {
        std::string id = detail::endpointIdForDb(dbPath);
        auto base = detail::baseDirForSocket();
        if (!base)
            return std::unexpected{base.error()};
        return *base / ("kanariv-" + id + ".pid");
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "pidPathFor: unknown exception")};
    }
}

kanariv::utils::Expected<std::filesystem::path> lockPathFor(const std::filesystem::path& dbPath) {
    try {
        std::string id = detail::endpointIdForDb(dbPath);
        auto base = detail::baseDirForSocket();
        if (!base)
            return std::unexpected{base.error()};
        return *base / ("kanariv-" + id + ".lock");
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "lockPathFor: unknown exception")};
    }
}

kanariv::utils::Expected<std::filesystem::path> logPathFor(const std::filesystem::path& dbPath) {
    try {
        auto parent = dbPath.parent_path();
        if (!parent.empty())
            return parent / "kanariv.log";
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (ec)
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Io,
                "logPathFor: cannot determine temp dir: " + ec.message())};
        return tmp / "kanariv.log";
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                         "logPathFor: unknown exception")};
    }
}

} // namespace kanariv::config
