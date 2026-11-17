/**
 * @file store.cpp
 * @brief Loading and saving the JSON config file.
 * @ingroup kanariv_config
 * @details Lenient, forward-compatible parsing (unknown keys are recorded, not
 * rejected) and an atomic save that never truncates the live file. Canonical
 * paths and the endpoint digest live in `paths.cpp`.
 */

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <kanariv/config.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "config_detail.hpp"

namespace {

using kanariv::types::RepeatMode;
using kanariv::types::ReplayGainMode;

/// Lowercases ASCII text; the config vocabulary is ASCII.
std::string lowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

/// Parses the config spelling of a repeat default (case-insensitive).
/// Unknown text is tolerated and means Off, matching the lenient loader.
RepeatMode parseRepeatDefault(std::string_view text) {
    const std::string lower = lowerAscii(text);
    if (lower == "all")
        return RepeatMode::All;
    if (lower == "one")
        return RepeatMode::One;
    return RepeatMode::Off;
}

/// Canonical (lowercase) config spelling written by saveConfig.
std::string_view repeatDefaultName(RepeatMode mode) noexcept {
    using enum RepeatMode;
    switch (mode) {
    case All:
        return "all";
    case One:
        return "one";
    case Off:
        break;
    }
    return "off";
}

/// Parses the config spelling of a ReplayGain mode (case-insensitive).
/// Unknown text is tolerated and means Off, matching the lenient loader.
ReplayGainMode parseReplayGain(std::string_view text) {
    const std::string lower = lowerAscii(text);
    if (lower == "track")
        return ReplayGainMode::Track;
    if (lower == "album")
        return ReplayGainMode::Album;
    return ReplayGainMode::Off;
}

/// Canonical (lowercase) config spelling written by saveConfig.
std::string_view replayGainName(ReplayGainMode mode) noexcept {
    using enum ReplayGainMode;
    switch (mode) {
    case Track:
        return "track";
    case Album:
        return "album";
    case Off:
        break;
    }
    return "off";
}

/// Every key loadConfig understands, including the legacy spellings.
/// Anything else is reported through Config::unknownKeys.
constexpr std::string_view knownConfigKeys[] = {
    "dbPath",        "configPath",      "device",           "deviceName",
    "logLevel",      "socketPath",      "autoResumeOnPlay", "clearStopsPlayback",
    "repeatDefault", "autostart",       "replayGain",       "fadeInMs",
    "fadeOutMs",     "interTrackGapMs", "crossfadeMs",      "db_path",
    "log_level",
};

/// Maps filesystem failures to the public status codes.
kanariv::utils::StatusCode statusForFsError(const std::error_code& ec) noexcept {
    if (ec == std::errc::no_space_on_device)
        return kanariv::utils::StatusCode::NoSpace;
    return kanariv::utils::StatusCode::Io;
}

} // namespace

namespace kanariv::config::detail {

kanariv::utils::Expected<std::string> readFileString(const std::filesystem::path& p) {
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

kanariv::utils::Expected<Config> loadConfig(const std::filesystem::path& path) {
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
        if (j.contains("deviceName") && j["deviceName"].isString()) {
            cfg.deviceName = j["deviceName"].get<std::string>();
        }
        if (j.contains("logLevel") && j["logLevel"].isInteger()) {
            // Clamp instead of rejecting: 9 must not silence logging.
            cfg.logLevel =
                std::to_underlying(kanariv::utils::clampLogLevel(j["logLevel"].get<int>()));
        } else if (j.contains("logLevel") && j["logLevel"].isString()) {
            // Tolerate hand-written names ("info"); canonical form is numeric.
            if (auto lvl = kanariv::utils::parseLogLevelValue(j["logLevel"].get<std::string>()))
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
            cfg.repeatDefault = parseRepeatDefault(j["repeatDefault"].get<std::string>());
        }
        if (j.contains("autostart") && j["autostart"].isBoolean()) {
            cfg.autostart = j["autostart"].get<bool>();
        }
        if (j.contains("replayGain") && j["replayGain"].isString()) {
            cfg.replayGain = parseReplayGain(j["replayGain"].get<std::string>());
        }
        if (j.contains("fadeInMs") && j["fadeInMs"].isInteger()) {
            cfg.fadeInMs = j["fadeInMs"].get<int>();
        }
        if (j.contains("fadeOutMs") && j["fadeOutMs"].isInteger()) {
            cfg.fadeOutMs = j["fadeOutMs"].get<int>();
        }
        if (j.contains("interTrackGapMs") && j["interTrackGapMs"].isInteger()) {
            cfg.interTrackGapMs = j["interTrackGapMs"].get<int>();
        }
        if (j.contains("crossfadeMs") && j["crossfadeMs"].isInteger()) {
            cfg.crossfadeMs = j["crossfadeMs"].get<int>();
        } else if (cfg.interTrackGapMs > 0) {
            // Legacy meaning: before the split, interTrackGapMs configured the
            // crossfade overlap. A config without crossfadeMs keeps the
            // audible behaviour it had; the key now means real silence.
            cfg.crossfadeMs = cfg.interTrackGapMs;
            cfg.interTrackGapMs = 0;
        }
        // legacy keys: db_path, log_level
        if (j.contains("db_path") && j["db_path"].isString()) {
            std::string s = j["db_path"].get<std::string>();
            if (!s.empty())
                cfg.dbPath = std::filesystem::path(s);
        }
        if (j.contains("log_level") && j["log_level"].isInteger()) {
            cfg.logLevel =
                std::to_underlying(kanariv::utils::clampLogLevel(j["log_level"].get<int>()));
        } else if (j.contains("log_level") && j["log_level"].isString()) {
            if (auto lvl = kanariv::utils::parseLogLevelValue(j["log_level"].get<std::string>()))
                cfg.logLevel = std::to_underlying(*lvl);
        }
        // Unknown keys are a typo signal, not an error: keep loading, but record
        // them so an owner with a logger (the daemon) can warn at startup.
        for (std::size_t i = 0, n = j.size(); i < n; ++i) {
            auto key = j.keyAt(i);
            if (!key)
                continue;
            if (std::find(std::begin(knownConfigKeys), std::end(knownConfigKeys), *key) ==
                std::end(knownConfigKeys))
                cfg.unknownKeys.push_back(std::move(*key));
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
    return cfg;
}

kanariv::utils::Expected<void> saveConfig(const Config& cfg) {
    namespace fs = std::filesystem;
    fs::path tmp{};
    try {
        const fs::path dir = cfg.configPath.parent_path();
        if (!dir.empty()) {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec) {
                return std::unexpected{kanariv::utils::makeError(
                    statusForFsError(ec), "cannot create config directory '" +
                                              dir.generic_string() + "': " + ec.message())};
            }
        }
        Json j;
        j["dbPath"] = cfg.dbPath.generic_string();
        j["device"] = cfg.device;
        j["deviceName"] = cfg.deviceName;
        j["logLevel"] = cfg.logLevel;
        j["autoResumeOnPlay"] = cfg.autoResumeOnPlay;
        j["clearStopsPlayback"] = cfg.clearStopsPlayback;
        j["repeatDefault"] = repeatDefaultName(cfg.repeatDefault);
        j["autostart"] = cfg.autostart;
        j["replayGain"] = replayGainName(cfg.replayGain);
        j["fadeInMs"] = cfg.fadeInMs;
        j["fadeOutMs"] = cfg.fadeOutMs;
        j["interTrackGapMs"] = cfg.interTrackGapMs;
        j["crossfadeMs"] = cfg.crossfadeMs;
        if (!cfg.socketPath.empty())
            j["socketPath"] = cfg.socketPath;
        const std::string payload = j.dump(2);

        // Write beside the target first so a failure never truncates the live config.
        tmp = cfg.configPath;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                                 "cannot write config '" +
                                                                     tmp.generic_string() + "'")};
            }
            out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            out.flush();
            if (!out.good()) {
                out.close();
                std::error_code rmEc;
                fs::remove(tmp, rmEc);
                return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                                 "cannot write config '" +
                                                                     tmp.generic_string() + "'")};
            }
            out.close();
            if (out.fail()) {
                std::error_code rmEc;
                fs::remove(tmp, rmEc);
                return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                                 "cannot close config '" +
                                                                     tmp.generic_string() + "'")};
            }
        }

        std::error_code renameEc;
        fs::rename(tmp, cfg.configPath, renameEc);
        if (renameEc) {
            // POSIX rename and the MSVC/MinGW implementations replace the target
            // atomically. Filesystems that reject that keep this documented
            // non-atomic fallback: copy over, then drop the temp file.
            std::error_code copyEc;
            fs::copy_file(tmp, cfg.configPath, fs::copy_options::overwrite_existing, copyEc);
            std::error_code rmEc;
            fs::remove(tmp, rmEc);
            if (copyEc) {
                return std::unexpected{kanariv::utils::makeError(
                    statusForFsError(copyEc),
                    "cannot replace config '" + cfg.configPath.generic_string() +
                        "': " + renameEc.message() + " (copy fallback: " + copyEc.message() + ")")};
            }
        }
        return {};
    } catch (const std::exception& e) {
        // Never leave a partially written temp file behind.
        std::error_code rmEc;
        if (!tmp.empty())
            fs::remove(tmp, rmEc);
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace kanariv::config
