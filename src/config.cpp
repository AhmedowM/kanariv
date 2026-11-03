#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <kanariv/config.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "config_detail.hpp"

namespace kanariv::config::detail {

auto defaultDbPath() -> std::filesystem::path {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp != nullptr && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "kanariv" / "library.db";
    }
#endif
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base;
    if (xdgData != nullptr && xdgData[0] != '\0') {
        base = std::filesystem::path(xdgData) / "kanariv";
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            home = std::getenv("USERPROFILE");
        }
        if (home != nullptr && home[0] != '\0') {
            base = std::filesystem::path(home) / ".local" / "share" / "kanariv";
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec) / "kanariv";
            if (ec) {
                base = std::filesystem::path("/tmp/kanariv");
            }
        }
    }
    return base / "library.db";
}

auto defaultConfigPath() -> std::filesystem::path {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp != nullptr && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "kanariv" / "config.json";
    }
#endif
    const char* xdgCfg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdgCfg != nullptr && xdgCfg[0] != '\0') {
        base = std::filesystem::path(xdgCfg) / "kanariv";
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            home = std::getenv("USERPROFILE");
        }
        if (home != nullptr && home[0] != '\0') {
            base = std::filesystem::path(home) / ".config" / "kanariv";
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec) / "kanariv";
            if (ec) {
                base = std::filesystem::path("/tmp/kanariv");
            }
        }
    }
    return base / "config.json";
}

auto legacyConfigPath() -> std::filesystem::path {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "kanariv" / "config.json";
    }
#endif
    return {};
}

auto readFileString(const std::filesystem::path& p) -> kanariv::utils::Expected<std::string> {
    std::ifstream in(p);
    if (!in) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open config")};
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

} // namespace kanariv::config::detail

namespace kanariv::config {

using kanariv::utils::Json;

auto loadConfig(const std::filesystem::path& path) -> kanariv::utils::Expected<Config> {
    Config cfg{};
    cfg.dbPath = detail::defaultDbPath();
    cfg.configPath = path.empty() ? detail::defaultConfigPath() : path;
    cfg.device = "auto";
    cfg.logLevel = 2;

    std::filesystem::path cfgFile = cfg.configPath;
    std::error_code ec;
    if (path.empty() && !std::filesystem::exists(cfgFile, ec)) {
        // Older Windows installs kept the config next to $HOME/.config.
        // Keep reading it (and keep writing there) rather than silently
        // starting from defaults; fresh installs use the new default path.
        auto legacy = detail::legacyConfigPath();
        if (!legacy.empty()) {
            std::error_code legacyEc;
            if (std::filesystem::exists(legacy, legacyEc)) {
                cfg.configPath = legacy;
                cfgFile = legacy;
            }
        }
    }
    if (!std::filesystem::exists(cfgFile, ec)) {
        // no config file -> return defaults
        return cfg;
    }

    auto fileRes = detail::readFileString(cfgFile);
    if (!fileRes) {
        return std::unexpected{fileRes.error()};
    }
    std::string content = std::move(*fileRes);
    if (content.empty()) {
        return cfg;
    }
    try {
        const Json j = Json::parse(content);
        if (j.contains("dbPath") && j["dbPath"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            auto s = j["dbPath"].get<std::string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (!s.empty()) {
                cfg.dbPath = std::filesystem::path(s);
            }
        }
        if (j.contains("configPath") && j["configPath"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // ignore - already set
        }
        if (j.contains("device") && j["device"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            cfg.device = j["device"].get<std::string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("logLevel") && j["logLevel"].isInteger()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            cfg.logLevel = j["logLevel"].get<int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        } else if (j.contains("logLevel") && j["logLevel"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // Tolerate hand-written names ("info"); canonical form is numeric.
            if (auto lvl = kanariv::utils::parseLogLevelValue(j["logLevel"].get<std::string>())) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                cfg.logLevel = std::to_underlying(*lvl);
            }
        }
        if (j.contains("socketPath") && j["socketPath"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            auto s = j["socketPath"].get<std::string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (!s.empty()) {
                cfg.socketPath = s;
            }
        }
        if (j.contains("autoResumeOnPlay") && j["autoResumeOnPlay"].isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            cfg.autoResumeOnPlay = j["autoResumeOnPlay"].get<bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("clearStopsPlayback") && j["clearStopsPlayback"].isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            cfg.clearStopsPlayback = j["clearStopsPlayback"].get<bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("repeatDefault") && j["repeatDefault"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            auto rd = j["repeatDefault"].get<std::string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (rd == "off" || rd == "one" || rd == "all") {
                cfg.repeatDefault = rd;
            }
        }
        if (j.contains("autostart") && j["autostart"].isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            cfg.autostart = j["autostart"].get<bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        // legacy keys: db_path, log_level
        if (j.contains("db_path") && j["db_path"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            auto s = j["db_path"].get<std::string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (!s.empty()) {
                cfg.dbPath = std::filesystem::path(s);
            }
        }
        if (j.contains("log_level") && j["log_level"].isInteger()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            cfg.logLevel = j["log_level"].get<int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        } else if (j.contains("log_level") && j["log_level"].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (auto lvl = kanariv::utils::parseLogLevelValue(j["log_level"].get<std::string>())) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                cfg.logLevel = std::to_underlying(*lvl);
            }
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
    return cfg;
}

auto saveConfig(const Config& cfg) -> kanariv::utils::Expected<void> {
    try {
        std::filesystem::path dir = cfg.configPath.parent_path();
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
        Json j;
        j["dbPath"] = cfg.dbPath.generic_string(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["device"] = cfg.device; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["logLevel"] = cfg.logLevel; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["autoResumeOnPlay"] = cfg.autoResumeOnPlay; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["clearStopsPlayback"] = cfg.clearStopsPlayback; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["repeatDefault"] = cfg.repeatDefault; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["autostart"] = cfg.autostart; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!cfg.socketPath.empty()) {
            j["socketPath"] = cfg.socketPath; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        std::ofstream out(cfg.configPath);
        if (!out) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot write config")};
        }
        out << j.dump(2);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace kanariv::config

namespace kanariv::config::detail_paths {

constexpr std::uint32_t hashMask32 = 0xFFFFFFFFu;
constexpr int hashShift32 = 32;

auto hex8ForDb(const std::filesystem::path& dbPath) -> std::string {
    std::string input = dbPath.generic_string();
    if (input.empty()) {
        input = dbPath.string();
    }
    const std::uint64_t raw = static_cast<std::uint64_t>(std::hash<std::string>{}(input));
    auto hv = static_cast<std::uint32_t>(raw & hashMask32);
    hv ^= static_cast<std::uint32_t>((raw >> hashShift32) & hashMask32);
    return std::format("{:08x}", hv);
}

auto baseDirForSocket() -> std::filesystem::path {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp != nullptr && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "kanariv";
    }
#endif
    const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRuntime != nullptr && xdgRuntime[0] != '\0') {
        return std::filesystem::path(xdgRuntime) / "kanariv";
    }
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    if (xdgData != nullptr && xdgData[0] != '\0') {
        return std::filesystem::path(xdgData) / "kanariv";
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        home = std::getenv("USERPROFILE");
    }
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "kanariv";
    }
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec) / "kanariv";
    if (ec) {
        base = std::filesystem::path("/tmp/kanariv");
    }
    return base;
}

} // namespace kanariv::config::detail_paths

namespace kanariv::config {

auto socketPathFor(const std::filesystem::path& dbPath) -> kanariv::utils::Expected<std::string> {
    try {
        std::string hex = detail_paths::hex8ForDb(dbPath);
#ifdef _WIN32
        return std::string(R"(\\.\pipe\kanariv-)") + hex;
#else
        auto base = detail_paths::baseDirForSocket();
        return (base / ("kanariv-" + hex + ".sock")).generic_string();
#endif
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "socketPathFor failed")};
    }
}

auto pidPathFor(const std::filesystem::path& dbPath)
    -> kanariv::utils::Expected<std::filesystem::path> {
    try {
        std::string hex = detail_paths::hex8ForDb(dbPath);
        auto base = detail_paths::baseDirForSocket();
        return base / ("kanariv-" + hex + ".pid");
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "pidPathFor failed")};
    }
}

auto lockPathFor(const std::filesystem::path& dbPath)
    -> kanariv::utils::Expected<std::filesystem::path> {
    try {
        std::string hex = detail_paths::hex8ForDb(dbPath);
        auto base = detail_paths::baseDirForSocket();
        return base / ("kanariv-" + hex + ".lock");
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "lockPathFor failed")};
    }
}

auto logPathFor(const std::filesystem::path& dbPath)
    -> kanariv::utils::Expected<std::filesystem::path> {
    try {
        auto parent = dbPath.parent_path();
        if (!parent.empty()) {
            return parent / "kanariv.log";
        }
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (ec) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "logPathFor failed")};
        }
        return tmp / "kanariv.log";
    } catch (const std::exception& e) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "logPathFor failed")};
    }
}

} // namespace kanariv::config
