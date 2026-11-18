#pragma once

/**
 * @file config.hpp
 * @brief Configuration management: canonical paths, load/save, and raw key/value access.
 * @ingroup kanariv_config
 * @defgroup kanariv_config kanariv configuration
 */

#include <kanariv/config/runtime.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/types/wire.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace kanariv::config {

/**
 * @brief Derives the local IPC endpoint for a database.
 *
 * Endpoint identity is the FNV-1a 64-bit digest of the canonical UTF-8 form of the
 * database path, written as 16 lowercase hex digits:
 * `std::filesystem::path(dbPath).generic_u8string()` (`/` separators) when that
 * conversion succeeds, and the raw path spelling otherwise. The digest is specified
 * here rather than inherited from `std::hash`, so a daemon and a client built with
 * different C++ runtimes still agree. Socket, PID, and lock names all
 * use this one digest.
 * - Windows uses a named pipe derived from the database path; pid/lock are files under
 *   `%LOCALAPPDATA%/kanariv`
 * - POSIX socket/pid/lock are under $XDG_RUNTIME_DIR/kanariv or $XDG_DATA_HOME/kanariv or
 *   ~/.local/share/kanariv. On POSIX the runtime directory is created owner-only when
 *   absent, and an existing directory is refused (not chmod'ed) when it is not a
 *   directory owned by the current user or is writable by group/other.
 *
 * @param dbPath Database path used as the stable identity for the endpoint.
 * @return Named-pipe or Unix-socket path, or an error when the platform
 *         runtime directory cannot be determined.
 * @warning Endpoint rename (upgrade note): earlier releases derived an 8-hex identifier
 * from `std::hash<std::string>`, which is implementation-defined and therefore toolchain
 * dependent. After upgrading, `kanariv-<old-id>.sock`, `.pid`, and `.lock` files are
 * orphaned (Windows named pipes are recreated under the new name). Remove the stale files
 * manually; the library never deletes them. A daemon started with the new binary and a CLI
 * built from an older revision (or a tool that re-derives the old `std::hash` name) will not
 * find each other: upgrade both, or set `socketPath` explicitly.
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
    /** @brief Audio device id ("auto" for default).
     *  Opaque stable id from DeviceList; configs written before ids became
     *  opaque stored a numeric enumeration index, which is still accepted. */
    std::string device{"auto"};
    /** @brief Display name paired with `device`. Used when the id is no
     *  longer enumerated (for example after a backend change); if neither
     *  resolves, playback falls back to the default device with a warning. */
    std::string deviceName{};
    /** @brief Log level (0=trace,1=debug,2=info,3=warn,4=error). Default: 2 (info). */
    int logLevel{2};
    /** @brief Optional explicit socket path. If empty, derived from dbPath. */
    std::string socketPath{};
    /** @brief If true, play() after daemon restart resumes the persisted track/position
     *  from DB instead of advancing to the next queue track. Default: false. */
    bool autoResumeOnPlay{false};
    /** @brief When the active queue is cleared, also stop playback. Default: true. */
    bool clearStopsPlayback{true};
    /** @brief Default repeat mode for newly created queues. Default: Off.
     *  The config file spells it `"off"`, `"one"`, or `"all"` (case-insensitive on load;
     *  unknown text falls back to Off without failing the load). */
    kanariv::types::RepeatMode repeatDefault{kanariv::types::RepeatMode::Off};
    /** @brief When a track is added to an empty queue, auto-start playback. Default: false. */
    bool autostart{false};
    /**
     * @brief ReplayGain normalization mode. Default: Off.
     * @details Loaded from `"off"`, `"track"`, or `"album"` (case-insensitive;
     * unknown text falls back to Off without failing the load) and written
     * back by saveConfig in lowercase. Reads `TrackRow::replayGainTrack` /
     * `replayGainAlbum` at load time; those columns are written only by the
     * library's explicit ReplayGain analysis pass, so before it has run every
     * mode behaves as Off.
     */
    kanariv::types::ReplayGainMode replayGain{kanariv::types::ReplayGainMode::Off};
    /** @brief Fade-in duration in ms (0 = off). Default: 0. */
    int fadeInMs{0};
    /** @brief Fade-out duration in ms (0 = off). Default: 0. */
    int fadeOutMs{0};
    /** @brief Equal-power crossfade overlap in ms (0 = off). Default: 0.
     *  The next track is preloaded and mixed in over the last crossfadeMs of
     *  the outgoing one. */
    int crossfadeMs{0};
    /** @brief Inter-track silence in ms (0 = gapless). Default: 0.
     *  Held after the outgoing tail drains, before the next track starts;
     *  ignored when crossfadeMs > 0. Legacy configs (no crossfadeMs key)
     *  keep their old behaviour: a non-zero interTrackGapMs is read as the
     *  crossfade overlap. */
    int interTrackGapMs{0};
    /**
     * @brief Keys present in the config file that this build does not know.
     * @details Filled by loadConfig and never written by saveConfig. Entry
     * points that own a logger report them once at startup, so a typo
     * (`fadeInMS`, `replaygain`) is visible instead of silently ignored.
     */
    std::vector<std::string> unknownKeys{};
};

/**
 * @brief Load configuration from file with defaults.
 * @param path Config file path (empty = use default).
 * @return Config with defaults merged from file on success, Error on failure.
 *
 * Reads JSON config file, falls back to defaults for missing keys.
 * Supports legacy keys: db_path, log_level (snake_case).
 * `logLevel` accepts names and numbers; numbers are clamped to the logger
 * vocabulary (0–4) so `9` cannot silence logging. `repeatDefault` and
 * `replayGain` are parsed to their enums case-insensitively (unknown text
 * means Off). Keys this build does not recognise are not an error: they are
 * collected in `Config::unknownKeys` for the caller to warn about.
 */
KANARIV_API kanariv::utils::Expected<Config> loadConfig(const std::filesystem::path& path);

/**
 * @brief Save configuration to file.
 * @param cfg Config to save.
 * @return void on success, Error on failure.
 *
 * Creates parent directories if needed (creation errors are reported, not ignored).
 * Writes all configuration keys with 2-space indentation to a sibling
 * `<configPath>.tmp`, checks every stream operation, then renames the temporary file
 * over `configPath`, so a crash cannot truncate the live config. The temp file is
 * removed on failure. Rename is atomic on POSIX filesystems and on Windows (both MSVC
 * and MinGW map it to `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`); only when the
 * filesystem rejects the rename is a documented non-atomic copy-over fallback used.
 */
KANARIV_API kanariv::utils::Expected<void> saveConfig(const Config& cfg);

} // namespace kanariv::config
