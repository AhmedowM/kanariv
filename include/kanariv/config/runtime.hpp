#pragma once

/**
 * @file runtime.hpp
 * @brief Explicit, platform-neutral runtime directories for a host process.
 * @ingroup kanariv_config
 *
 * Desktop discovery (XDG variables, HOME, LOCALAPPDATA, temp directories) is
 * convenient for a CLI but wrong for a sandboxed host such as Android. This
 * structure lets a host provide every persistent location up front. The core
 * validates and creates the supplied paths; it never invents Android or
 * desktop defaults from this object.
 *
 * All fields are optional. An empty field means "not supplied by the host";
 * callers that retain desktop behavior may leave the whole structure empty.
 * `logs` is a directory: the service writes `kanariv.log` inside it. `cache`
 * and `importedMedia` are reserved for host-side import/copy workflows and are
 * not touched by the playback engine.
 */

#include <filesystem>
#include <kanariv/fwd.hpp>
#include <string>

namespace kanariv::config {

/**
 * @struct RuntimePaths
 * @brief Application-private directories and optional transport path.
 * @ingroup kanariv_config
 *
 * Field meanings:
 * - `database`: SQLite file (not a directory).
 * - `configuration`: configuration file or its parent; hosts decide which.
 * - `logs`: directory for service log files.
 * - `cache`: disposable cache directory.
 * - `importedMedia`: directory for app-private imported media copies.
 * - `socket`: explicit local transport endpoint, when the platform uses one.
 *
 * The Android adapter is expected to map these to `Context.filesDir`,
 * `Context.noBackupFilesDir`, and `Context.cacheDir`. The core does not infer
 * any of them.
 */
struct KANARIV_API RuntimePaths {
    /** @brief SQLite database file. Empty = use the caller's existing config. */
    std::filesystem::path database{};
    /** @brief Configuration file or directory, as agreed by the host and caller. */
    std::filesystem::path configuration{};
    /** @brief Log directory; the service writes `kanariv.log` here. */
    std::filesystem::path logs{};
    /** @brief Disposable cache directory. */
    std::filesystem::path cache{};
    /** @brief Directory for app-private imported media copies. */
    std::filesystem::path importedMedia{};
    /** @brief Explicit local socket/named-pipe endpoint, if used. */
    std::string socket{};

    /**
     * @brief Checks whether every field is empty.
     * @return true when the host supplied no runtime paths.
     */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief Creates the directories implied by the supplied paths.
     * @return Success, or Error with `StatusCode::Io` when a directory cannot
     *         be created.
     *
     * Creates the parent directory of `database`/`configuration`, and the
     * `logs`, `cache`, and `importedMedia` directories themselves. Empty
     * fields are skipped. Existing directories are accepted. An empty object
     * is a successful no-op.
     * @par Thread safety
     * The operation is safe when each call uses an independent
     * `RuntimePaths` value. Concurrent calls targeting the same paths rely on
     * the platform filesystem's directory-creation semantics.
     */
    [[nodiscard]] kanariv::utils::Expected<void> ensureDirectories() const;
};

} // namespace kanariv::config
