#include <caudio/config.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/json.hpp>
#include <caudio/utils/log.hpp>
#include <caudio/utils/result.hpp>
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
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "config_detail.hpp"

namespace caudio::config::detail {

std::filesystem::path defaultDbPath() {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "caudio" / "library.db";
    }
#endif
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base;
    if (xdgData && xdgData[0] != '\0') {
        base = std::filesystem::path(xdgData) / "caudio";
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0')
            home = std::getenv("USERPROFILE");
        if (home && home[0] != '\0') {
            base = std::filesystem::path(home) / ".local" / "share" / "caudio";
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec) / "caudio";
            if (ec)
                base = std::filesystem::path("/tmp/caudio");
        }
    }
    return base / "library.db";
}

std::filesystem::path defaultConfigPath() {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "caudio" / "config.json";
    }
#endif
    const char* xdgCfg = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdgCfg && xdgCfg[0] != '\0') {
        base = std::filesystem::path(xdgCfg) / "caudio";
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0')
            home = std::getenv("USERPROFILE");
        if (home && home[0] != '\0') {
            base = std::filesystem::path(home) / ".config" / "caudio";
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec) / "caudio";
            if (ec)
                base = std::filesystem::path("/tmp/caudio");
        }
    }
    return base / "config.json";
}

std::filesystem::path legacyConfigPath() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "caudio" / "config.json";
    }
#endif
    return {};
}

caudio::utils::Expected<std::string> readFileString(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, "cannot open config")};
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

} // namespace caudio::config::detail

namespace caudio::config {

using caudio::utils::Json;

caudio::utils::Expected<Config> loadConfig(const std::filesystem::path& path) {
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
    if (!fileRes)
        return std::unexpected{fileRes.error()};
    std::string content = std::move(*fileRes);
    if (content.empty()) {
        return cfg;
    }
    try {
        const Json j = Json::parse(content);
        if (j.contains("dbPath") && j["dbPath"].isString()) {
            std::string s = j["dbPath"].get<std::string>();
            if (!s.empty())
                cfg.dbPath = std::filesystem::path(s);
        }
        if (j.contains("configPath") && j["configPath"].isString()) {
            // ignore - already set
        }
        if (j.contains("device") && j["device"].isString()) {
            cfg.device = j["device"].get<std::string>();
        }
        if (j.contains("logLevel") && j["logLevel"].isInteger()) {
            cfg.logLevel = j["logLevel"].get<int>();
        } else if (j.contains("logLevel") && j["logLevel"].isString()) {
            // Tolerate hand-written names ("info"); canonical form is numeric.
            if (auto lvl = caudio::utils::parseLogLevelValue(j["logLevel"].get<std::string>()))
                cfg.logLevel = std::to_underlying(*lvl);
        }
        if (j.contains("socketPath") && j["socketPath"].isString()) {
            std::string s = j["socketPath"].get<std::string>();
            if (!s.empty())
                cfg.socketPath = s;
        }
        if (j.contains("autoResumeOnPlay") && j["autoResumeOnPlay"].isBoolean()) {
            cfg.autoResumeOnPlay = j["autoResumeOnPlay"].get<bool>();
        }
        if (j.contains("clearStopsPlayback") && j["clearStopsPlayback"].isBoolean()) {
            cfg.clearStopsPlayback = j["clearStopsPlayback"].get<bool>();
        }
        if (j.contains("repeatDefault") && j["repeatDefault"].isString()) {
            std::string rd = j["repeatDefault"].get<std::string>();
            if (rd == "off" || rd == "one" || rd == "all")
                cfg.repeatDefault = rd;
        }
        if (j.contains("autostart") && j["autostart"].isBoolean()) {
            cfg.autostart = j["autostart"].get<bool>();
        }
        // legacy keys: db_path, log_level
        if (j.contains("db_path") && j["db_path"].isString()) {
            std::string s = j["db_path"].get<std::string>();
            if (!s.empty())
                cfg.dbPath = std::filesystem::path(s);
        }
        if (j.contains("log_level") && j["log_level"].isInteger()) {
            cfg.logLevel = j["log_level"].get<int>();
        } else if (j.contains("log_level") && j["log_level"].isString()) {
            if (auto lvl = caudio::utils::parseLogLevelValue(j["log_level"].get<std::string>()))
                cfg.logLevel = std::to_underlying(*lvl);
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Corrupt, e.what())};
    }
    return cfg;
}

caudio::utils::Expected<void> saveConfig(const Config& cfg) {
    try {
        std::filesystem::path dir = cfg.configPath.parent_path();
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
        Json j;
        j["dbPath"] = cfg.dbPath.generic_string();
        j["device"] = cfg.device;
        j["logLevel"] = cfg.logLevel;
        j["autoResumeOnPlay"] = cfg.autoResumeOnPlay;
        j["clearStopsPlayback"] = cfg.clearStopsPlayback;
        j["repeatDefault"] = cfg.repeatDefault;
        j["autostart"] = cfg.autostart;
        if (!cfg.socketPath.empty())
            j["socketPath"] = cfg.socketPath;
        std::ofstream out(cfg.configPath);
        if (!out) {
            return std::unexpected{
                caudio::utils::makeError(caudio::utils::StatusCode::Io, "cannot write config")};
        }
        out << j.dump(2);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace caudio::config

namespace caudio::config::detail_paths {

std::string hex8ForDb(const std::filesystem::path& dbPath) {
    std::string input = dbPath.generic_string();
    if (input.empty())
        input = dbPath.string();
    const std::uint64_t raw = static_cast<std::uint64_t>(std::hash<std::string>{}(input));
    std::uint32_t hv = static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
    hv ^= static_cast<std::uint32_t>((raw >> 32) & 0xFFFFFFFFu);
    return std::format("{:08x}", hv);
}

std::filesystem::path baseDirForSocket() {
#ifdef _WIN32
    const char* localApp = std::getenv("LOCALAPPDATA");
    if (localApp && localApp[0] != '\0') {
        return std::filesystem::path(localApp) / "caudio";
    }
#endif
    const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRuntime && xdgRuntime[0] != '\0') {
        return std::filesystem::path(xdgRuntime) / "caudio";
    }
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    if (xdgData && xdgData[0] != '\0') {
        return std::filesystem::path(xdgData) / "caudio";
    }
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0')
        home = std::getenv("USERPROFILE");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "caudio";
    }
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec) / "caudio";
    if (ec)
        base = std::filesystem::path("/tmp/caudio");
    return base;
}

} // namespace caudio::config::detail_paths

namespace caudio::config {

caudio::utils::Expected<std::string> socketPathFor(const std::filesystem::path& dbPath) {
    try {
        std::string hex = detail_paths::hex8ForDb(dbPath);
#ifdef _WIN32
        return std::string("\\\\.\\pipe\\caudio-") + hex;
#else
        auto base = detail_paths::baseDirForSocket();
        return (base / ("caudio-" + hex + ".sock")).generic_string();
#endif
    } catch (const std::exception& e) {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, "socketPathFor failed")};
    }
}

caudio::utils::Expected<std::filesystem::path> pidPathFor(const std::filesystem::path& dbPath) {
    try {
        std::string hex = detail_paths::hex8ForDb(dbPath);
        auto base = detail_paths::baseDirForSocket();
        return base / ("caudio-" + hex + ".pid");
    } catch (const std::exception& e) {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, "pidPathFor failed")};
    }
}

caudio::utils::Expected<std::filesystem::path> lockPathFor(const std::filesystem::path& dbPath) {
    try {
        std::string hex = detail_paths::hex8ForDb(dbPath);
        auto base = detail_paths::baseDirForSocket();
        return base / ("caudio-" + hex + ".lock");
    } catch (const std::exception& e) {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, "lockPathFor failed")};
    }
}

caudio::utils::Expected<std::filesystem::path> logPathFor(const std::filesystem::path& dbPath) {
    try {
        auto parent = dbPath.parent_path();
        if (!parent.empty())
            return parent / "caudio.log";
        std::error_code ec;
        auto tmp = std::filesystem::temp_directory_path(ec);
        if (ec)
            return std::unexpected{
                caudio::utils::makeError(caudio::utils::StatusCode::Io, "logPathFor failed")};
        return tmp / "caudio.log";
    } catch (const std::exception& e) {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::Io, e.what())};
    } catch (...) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, "logPathFor failed")};
    }
}

} // namespace caudio::config
