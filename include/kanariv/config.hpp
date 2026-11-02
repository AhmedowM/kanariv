#pragma once

/**
 * @file config.hpp
 * @brief Configuration management: canonical paths, load/save, and raw key/value access.
 * @ingroup kanariv_config
 * @defgroup kanariv_config kanariv configuration
 */

#include <kanariv/config/runtime.hpp>
#include <kanariv/fwd.hpp>
#include <filesystem>
#include <string>

namespace kanariv::config {

/**
 * @brief Canonical path helpers: single source for socket/pid/lock derived from dbPath.
 * All three use hash of dbPath.generic_string() + XDG/LOCALAPPDATA base dir.
 * - Windows uses a named pipe derived from the database path; pid/lock are files under
 * `%LOCALAPPDATA%/kanariv`
 * - POSIX socket/pid/lock are under $XDG_RUNTIME_DIR/kanariv or $XDG_DATA_HOME/kanariv or
 * ~/.local/share/kanariv
 */
/**
 * @brief Derives the local IPC endpoint for a database.
 * @param dbPath Database path used as the stable identity for the endpoint.
 * @return Named-pipe or Unix-socket path, or an error when the platform
 *         runtime directory cannot be determined.
 */
KANARIV_API kanariv::utils::Expected<std::string> socketPathFor(const std::filesystem::path& dbPath);
/**
 * @brief Derives the PID-file path for a database.
 * @param dbPath Database path used as the stable identity.
 * @return PID-file path, or an error when the runtime directory is unavailable.
 */
KANARIV_API kanariv::utils::Expected<std::filesystem::path>
pidPathFor(const std::filesystem::path& dbPath);
/**
 * @brief Derives the single-instance lock-file path for a database.
 * @param dbPath Database path used as the stable identity.
 * @return Lock-file path, or an error when the runtime directory is unavailable.
 */
KANARIV_API kanariv::utils::Expected<std::filesystem::path>
lockPathFor(const std::filesystem::path& dbPath);
/**
 * @brief Daemon log file path for a database: `<db-dir>/kanariv.log`.
 * @param dbPath Database path whose parent directory receives the log.
 * @return Log-file path, or an error if the path cannot be derived.
 * Falls back to the temp dir when the db path has no parent.
 */
KANARIV_API kanariv::utils::Expected<std::filesystem::path>
logPathFor(const std::filesystem::path& dbPath);

/**
 * @brief User-facing configuration loaded from config.json.
 * @ingroup kanariv_config
 */
struct Config {
    /** @brief Database file path. Default: derived from XDG/LOCALAPPDATA. */
    std::filesystem::path dbPath{};
    /** @brief Config file path. Default: derived from XDG/LOCALAPPDATA. */
    std::filesystem::path configPath{};
    /** @brief Audio device ID ("auto" for default). */
    std::string device{"auto"};
    /** @brief Log level (0=trace,1=debug,2=info,3=warn,4=error). Default: 2 (info). */
    int logLevel{2};
    /** @brief Optional explicit socket path. If empty, derived from dbPath. */
    std::string socketPath{};
    /** @brief If true, play() after daemon restart resumes the persisted track/position
     *  from DB instead of advancing to the next queue track. Default: false. */
    bool autoResumeOnPlay{false};
    /** @brief When the active queue is cleared, also stop playback. Default: true. */
    bool clearStopsPlayback{true};
    /** @brief Default repeat mode for newly created queues: "off", "one", or "all". Default: "off". */
    std::string repeatDefault{"off"};
    /** @brief When a track is added to an empty queue, auto-start playback. Default: false. */
    bool autostart{false};
};

/**
 * @brief Load configuration from file with defaults.
 * @param path Config file path (empty = use default).
 * @return Config with defaults merged from file on success, Error on failure.
 *
 * Reads JSON config file, falls back to defaults for missing keys.
 * Supports legacy keys: db_path, log_level (snake_case).
 */
KANARIV_API kanariv::utils::Expected<Config> loadConfig(const std::filesystem::path& path);

/**
 * @brief Save configuration to file.
 * @param cfg Config to save.
 * @return void on success, Error on failure.
 *
 * Creates parent directories if needed. Writes JSON with 2-space indentation.
 * Only writes dbPath, device, logLevel, and socketPath (if non-empty).
 */
KANARIV_API kanariv::utils::Expected<void> saveConfig(const Config& cfg);

} // namespace kanariv::config
