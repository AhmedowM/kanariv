#pragma once

/**
 * @file app.hpp
 * @brief Application orchestrator shared by all frontends.
 * @ingroup kanariv_app
 * @details `App` owns the behavior moved out of the CLI shell: daemon
 * lifecycle (spawn, readiness, shutdown), transport policy (autostart,
 * retries) and, over time, every command dispatch path. Frontends (the CLI
 * shell today, TUIs tomorrow) keep only their own syntax and rendering and
 * call into this class.
 *
 * Phase 0 note: rendering still happens inside these methods (moved verbatim
 * from the shell); Phase 1 converts them to return data for callers to
 * render. Exit-code convention is CLI-compatible throughout: 0 ok, 1
 * runtime failure.
 *
 * Thread safety: thread-safe for concurrent calls; daemon interactions go
 * through the IPC client, which serializes per connection.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <kanariv/config.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kanariv::app {

/**
 * @brief Command outcome data for frontends to render.
 * @ingroup kanariv_app
 * @details handlers return this instead of printing. Frontends render the
 * result themselves (bare/JSON), print `line`, or stay silent:
 * result plus line is a confirmation, result alone is rendered by the
 * frontend, line
 * alone with toStderr is a warning (exit still 0), failed lines go to
 * stderr with exit 1, quiet skips human rendering of a carried result,
 * neither is silent success.
 */
struct KANARIV_API Outcome {
    std::optional<kanariv::ipc::Result> result; ///< Structured result for frontend rendering.
    std::optional<std::string> line;            ///< Human-readable line, when applicable.
    bool toStderr = false;                      ///< Render `line` on stderr instead of stdout.
    bool failed = false;                        ///< Indicates a runtime failure (exit code 1).
    bool quiet = false;                         ///< Suppresses human rendering of `result`.
    /** @brief Creates a successful warning outcome written to stderr. */
    static std::expected<Outcome, kanariv::utils::Error> warn(std::string message);
    /** @brief Creates a failed outcome written to stderr. */
    static std::expected<Outcome, kanariv::utils::Error> fail(std::string message);
};
/** @brief Handler return: outcome data, or the error to render. @ingroup kanariv_app */
using AppResult = std::expected<Outcome, kanariv::utils::Error>;

/**
 * @brief Multi-step batch outcome: stdout/stderr blobs plus JSON payload.
 * @ingroup kanariv_app
 * @details Batch commands (adds, removes, stats) accumulate output text
 * instead of printing; the frontend flushes each stream. err prints to
 * stderr in both modes, json renders under --json, out prints in text
 * mode, exitCode is the process exit code.
 */
struct KANARIV_API BatchReport {
    std::optional<ipc::Result> json; ///< Structured result for JSON mode.
    std::string out;                 ///< Human-readable stdout content.
    std::string err;                 ///< Human-readable stderr content.
    int exitCode = 0;                ///< Process-compatible exit code.
    /** @brief Creates a failed batch report with stderr content. */
    static std::expected<BatchReport, kanariv::utils::Error> fail(std::string message);
};
/** @brief Batch handler return. @ingroup kanariv_app */
using BatchResult = std::expected<BatchReport, kanariv::utils::Error>;

/** @brief Known tag fields accepted by tagValue. @ingroup kanariv_app */
inline constexpr std::array<std::string_view, 9> tagFields{"title",        "artist",      "album",
                                                           "album_artist", "genre",       "year",
                                                           "track_number", "disc_number", "cover"};

/** @brief True when field names a known tag field. @ingroup kanariv_app */
KANARIV_API bool isTagField(std::string_view field);

/**
 * @brief One selected tag value, human text plus single-key JSON dump.
 * @ingroup kanariv_app
 */
struct TagValue {
    std::string value;
    std::string json;
};

/**
 * @brief Listed queue tracks plus the id to highlight, for the frontend.
 * @ingroup kanariv_app
 */
struct TrackList {
    ipc::Result result;
    std::int64_t highlightId = 0;
};

/**
 * @brief Application orchestrator bound to one config (one database).
 * @ingroup kanariv_app
 * @see kanariv::config::Config
 */
class KANARIV_API App {
  public:
    /**
     * @brief Binds the orchestrator to a config.
     * @ingroup kanariv_app
     * @param cfg Resolved config (db/socket/log paths); copied.
     */
    explicit App(kanariv::config::Config cfg);
    /** @brief Dtor. @ingroup kanariv_app */
    ~App();

    /**
     * @brief Replaces the bound config (after frontend flag parsing).
     * @ingroup kanariv_app
     * @param cfg Fully resolved config; copied.
     * @details Frontends construct App before parsing their own flags, so
     * the bound config is stale until they call this once parsing plus
     * config-file reload plus socket derivation are done. Every daemon
     * interaction below must observe the final config.
     */
    void setConfig(kanariv::config::Config cfg);

    /**
     * @brief Sets argv[0] for daemon re-spawn path resolution.
     * @ingroup kanariv_app
     * @param argv0 Executable path as invoked (POSIX fallback only).
     * @details Frontends call this once from their entry point before any
     * spawn; on Windows the module path is used instead.
     */
    void setArgv0(std::string argv0);

    /**
     * @brief Injects the foreground daemon host (CLI owns service startup).
     * @ingroup kanariv_app
     * @param host Callable that runs the service in-process and blocks;
     *             when unset, `startDaemon(true)` returns an error.
     */
    void attachDaemonHost(std::function<AppResult(bool foreground)> host);

    /**
     * @brief Injects the local preview implementation (CLI owns the player-backed one).
     * @ingroup kanariv_app
     * @param host Callable that previews a local file and blocks until it
     *             finishes; when unset, `previewFile` returns an error.
     * @details The app layer stays free of the player dependency: the CLI
     * owns the player-backed preview and injects it here.
     */
    void attachPreviewHost(std::function<AppResult(const std::string& file)> host);

    /**
     * @brief Starts the daemon (foreground service or background spawn).
     * @ingroup kanariv_app
     * @param foreground Run the service in-process and block (child path).
     * @return Outcome data; success chatter is a line frontends skip in
     * JSON mode, failures are failed lines.
     * @details Background path waits out a previous daemon's teardown,
     * spawns once, then requires a full StatusReq round-trip (not just a
     * connect) within budget before reporting success.
     */
    AppResult startDaemon(bool foreground);

    /**
     * @brief Stops the daemon and waits for its teardown to complete.
     * @ingroup kanariv_app
     * @return Outcome data; "daemon stopped" line on success.
     */
    AppResult shutdownDaemon();

    /**
     * @brief Fetches the daemon playback status.
     * @ingroup kanariv_app
     * @return Status result, or the transport/server error.
     */
    AppResult status();

    /**
     * @brief Fetches daemon build and runtime information.
     * @ingroup kanariv_app
     * @return Info result, or the transport/server error.
     */
    AppResult info();

    /**
     * @brief Sets the output volume (absolute, mute, or relative delta).
     * @ingroup kanariv_app
     * @param volume Volume request to send.
     * @return Result, or the transport/server error.
     */
    AppResult setVolume(const kanariv::ipc::VolumeSet& volume);

    /**
     * @brief Shared play flow (used by play, and by resume when stopped).
     * @ingroup kanariv_app
     * @details Probes paused state first so the wording (resumed vs fresh)
     * matches; autostarts a down daemon via sendPlay().
     */
    AppResult play();

    /**
     * @brief Plays files immediately in a new queue.
     * @ingroup kanariv_app
     * @param files Pre-expanded file list (glob expansion stays in frontends).
     * @param save Keep the queue instead of purging it on shutdown.
     */
    AppResult playFiles(const std::vector<std::string>& files, bool save);

    /** @brief Pause playback (warns instead of failing when idle). @ingroup kanariv_app */
    AppResult pause();
    /**
     * @brief Resume playback (forgiving: warns when playing, plays from
     * cursor when stopped).
     * @ingroup kanariv_app
     */
    AppResult resume();
    /** @brief Restart the current track. @ingroup kanariv_app */
    AppResult restart();
    /** @brief Stop playback. @ingroup kanariv_app */
    AppResult stop();
    /** @brief Advance to the next track. @ingroup kanariv_app */
    AppResult next();
    /** @brief Move to the previous track (warns at queue start). @ingroup kanariv_app */
    AppResult prev();
    /**
     * @brief Seek to an absolute position, or by delta when relative.
     * @ingroup kanariv_app
     * @param target Seconds (absolute) or delta (relative to now).
     * @param isRelative Resolve target against the current position first.
     * @details Silent on success; argument grammar (mm:ss, +/-) is parsed
     * by frontends, the relative flag arrives pre-detected.
     */
    AppResult seek(double target, bool isRelative);

    /**
     * @brief Lists queues (id, name, track counts, active/temp markers).
     * @ingroup kanariv_app
     */
    AppResult queueList();

    /**
     * @brief Lists tracks in the active queue (added or playback order).
     * @ingroup kanariv_app
     * @param order "added" for insertion order, anything else for playback.
     */
    std::expected<TrackList, kanariv::utils::Error> queueTracks(const std::string& order);

    /** @brief Switches the active queue. @ingroup kanariv_app */
    AppResult queueSwitch(std::int64_t qid);

    /** @brief Creates a queue container. @ingroup kanariv_app */
    AppResult queueCreate(const std::string& name);

    /** @brief Deletes a queue container (active queue guarded). @ingroup kanariv_app */
    AppResult queueDelete(std::int64_t qid);

    /**
     * @brief Adds to the queue via playlist, id/search, or paths.
     * @ingroup kanariv_app
     * @param paths PATH/glob/folder selectors (PATH mode only).
     * @param id Library id (id mode) or search query (search mode).
     * @param search Treat id as an FTS query instead of a numeric id.
     * @param playlist Source playlist id (playlist mode, 0 = off).
     * @param replace Overwrite the active queue first (playlist mode only).
     * @param recursive Descend into folders (PATH mode only).
     * @details Mode validation travels with the handler, verbatim from the
     * shell; glob expansion uses the shared paths utilities.
     */
    BatchResult queueAdd(const std::vector<std::string>& paths, const std::string& id, bool search,
                         std::int64_t playlist, bool replace, bool recursive);

    /**
     * @brief Removes from the queue by id, position, or paths.
     * @ingroup kanariv_app
     * @details Selectors are exclusive; PATH mode matches
     * separator-insensitively against an insertion-order snapshot.
     */
    BatchResult queueRemove(const std::string& id, const std::string& pos,
                            const std::vector<std::string>& paths, bool recursive);

    /** @brief Moves a track within the queue. @ingroup kanariv_app */
    AppResult queueMove(std::size_t from, std::size_t to);

    /** @brief Clears the active queue. @ingroup kanariv_app */
    AppResult queueClear();

    /**
     * @brief Sets shuffle (on/off/empty toggles to resulting state).
     * @ingroup kanariv_app
     */
    AppResult queueShuffle(const std::string& mode);

    /**
     * @brief Sets repeat (off/one/all; empty cycles off->all->one->off).
     * @ingroup kanariv_app
     */
    AppResult queueRepeat(const std::string& mode);

    /**
     * @brief Lists playlists.
     * @ingroup kanariv_app
     */
    AppResult playlistList();

    /**
     * @brief Lists tracks on a playlist.
     * @ingroup kanariv_app
     */
    AppResult playlistTracks(std::int64_t pid);

    /**
     * @brief Creates an empty playlist.
     * @ingroup kanariv_app
     * @param name Playlist name.
     * @param smart_query Optional smart-playlist filter expression.
     */
    AppResult playlistCreate(const std::string& name, const std::string& smart_query = {});

    /**
     * @brief Adds ids/paths to a playlist (resolving files via the library).
     * @ingroup kanariv_app
     * @param pid Target playlist id.
     * @param ids Library id strings (validated numeric here).
     * @param paths PATH/glob/folder selectors, expanded via paths utilities.
     * @param recursive Descend into folders (PATH mode only).
     */
    BatchResult playlistAdd(std::int64_t pid, const std::vector<std::string>& ids,
                            const std::vector<std::string>& paths, bool recursive);

    /**
     * @brief Loads a playlist into a queue (optionally replacing/playing).
     * @ingroup kanariv_app
     */
    AppResult playlistLoad(std::int64_t pid, bool play, bool replace);

    /**
     * @brief Saves a queue as a playlist.
     * @ingroup kanariv_app
     * @param name Playlist name.
     * @param qid Source queue id, or nullopt for the active queue.
     */
    AppResult playlistSave(const std::string& name, std::optional<std::int64_t> qid);

    /** @brief Deletes a playlist. @ingroup kanariv_app */
    AppResult playlistDelete(std::int64_t pid);
    /** @brief Renames a playlist. @ingroup kanariv_app */
    AppResult playlistRename(std::int64_t pid, const std::string& name);

    /**
     * @brief Exports a playlist to an m3u/pls/json file.
     * @ingroup kanariv_app
     */
    BatchResult playlistExport(std::int64_t pid, const std::string& path,
                               const std::string& format);

    /**
     * @brief Imports a playlist file.
     * @ingroup kanariv_app
     * @param path Playlist file path.
     * @param name Playlist name, or nullopt to derive from the file.
     */
    AppResult playlistImport(const std::string& path, std::optional<std::string> name);

    /**
     * @brief Scans a path into the library.
     * @ingroup kanariv_app
     * @param path Scan root, or nullopt for the configured library path.
     * @param fullHash Fingerprint full content instead of sampling.
     */
    AppResult libraryScan(std::optional<std::string> path, bool fullHash);

    /**
     * @brief Searches the library (filenames included).
     * @ingroup kanariv_app
     */
    AppResult librarySearch(const std::string& query, int limit);

    /**
     * @brief Shows library stats, optionally with queue/playlist breakdowns.
     * @ingroup kanariv_app
     * @param topTracks Top-N tracks by total plays; nullopt or non-positive = totals only.
     * @param topAlbums Top-N albums by total plays; nullopt or non-positive = not requested.
     * @param topGenres Top-N genres by total plays; nullopt or non-positive = not requested.
     * @param topArtists Top-N artists by total plays; nullopt or non-positive = not requested.
     * @param queues Queue selectors ("all" or numeric ids).
     * @param playlists Playlist ids to include in the breakdown.
     */
    BatchResult libraryStats(std::optional<int> topTracks, std::optional<int> topAlbums,
                             std::optional<int> topGenres, std::optional<int> topArtists,
                             const std::vector<std::string>& queues,
                             const std::vector<std::int64_t>& playlists);

    /**
     * @brief Lists library tracks with optional filters.
     * @ingroup kanariv_app
     * @details Empty strings mean no filter; translated to nullopts here so
     * frontends pass plain values.
     */
    AppResult libraryList(const std::string& query, int limit, int offset, const std::string& sort,
                          const std::string& order, const std::string& artist,
                          const std::string& album, const std::string& genre,
                          std::optional<std::int32_t> rating = {});

    /**
     * @brief Lists favorited (rated) library tracks.
     * @ingroup kanariv_app
     * @param minRating Minimum rating (0..5), defaults to 1.
     * @param limit Maximum results (0 = unlimited).
     */
    AppResult libraryFavorites(std::int32_t minRating, int limit);

    /**
     * @brief Library maintenance: report or prune missing/duplicate tracks.
     * @ingroup kanariv_app
     * @param action "report" or "prune".
     * @param dryRun Report only, no changes (default true).
     * @param libraryId Library id (default: 1).
     */
    AppResult libraryMaintenance(const std::string& action, bool dryRun, std::int64_t libraryId);

    /** @brief Adds a file or directory to the library. @ingroup kanariv_app */
    AppResult libraryAdd(const std::string& path, bool recursive);

    /**
     * @brief Removes a track from the library (id or path).
     * @ingroup kanariv_app
     * @details Resolves a human label first so the confirmation names it.
     */
    AppResult libraryRemove(const std::string& query);

    /**
     * @brief Edits one track tag field (database row).
     * @ingroup kanariv_app
     */
    AppResult tagEdit(std::string idOrPath, const std::string& field, const std::string& value);

    /**
     * @brief Reads the full track tag record.
     * @ingroup kanariv_app
     * @param id Track id.
     * @param cover When true, include cover art (base64-encoded) and MIME type.
     * @return Outcome data; result carries SingleTrack with cover_mime/cover_data
     *   populated when cover=true and cover art is available.
     */
    AppResult tagGet(const std::string& idOrPath, bool cover = false);

    /**
     * @brief Reads one tag field as human text plus single-key JSON.
     * @ingroup kanariv_app
     * @param id Track id.
     * @param field Must name a known field (see isTagField); frontends
     * validate before calling, unknown names fail as InvalidArg.
     */
    std::expected<TagValue, kanariv::utils::Error> tagValue(std::string idOrPath,
                                                            const std::string& field);

    /**
     * @brief Sets the user rating (0..5) on a library track.
     * @ingroup kanariv_app
     * @param id Library track id.
     * @param rating Rating value (0..5).
     * @return Outcome data, or the transport/server error.
     */
    AppResult trackRate(std::string idOrPath, std::int32_t rating);

    /** @brief Lists playback history entries. @ingroup kanariv_app */
    AppResult historyList(int limit);

    /** @brief Clears all playback history. @ingroup kanariv_app */
    AppResult historyClear();

    /**
     * @brief Adds a bookmark (position defaults to the current position).
     * @ingroup kanariv_app
     */
    AppResult bookmarkAdd(std::optional<double> position, std::string note,
                          std::optional<int64_t> track);
    /** @brief Lists bookmarks for a track (default: current track). @ingroup kanariv_app */
    AppResult bookmarkList(std::optional<int64_t> track);
    /** @brief Removes a bookmark by id. @ingroup kanariv_app */
    AppResult bookmarkRemove(int64_t id);
    /** @brief Plays the bookmarked track and seeks to its position. @ingroup kanariv_app */
    AppResult bookmarkGoto(int64_t id);

    /** @brief Lists audio output devices. @ingroup kanariv_app */
    AppResult deviceList();

    /** @brief Sets the default audio output device. @ingroup kanariv_app */
    AppResult deviceSet(const std::string& id);

    /**
     * @brief Checks an audio device is available.
     * @ingroup kanariv_app
     * @param id Device id, or nullopt for the default device.
     */
    AppResult deviceTest(std::optional<std::string> id, bool tone = false);

    /**
     * @brief Reads a config value from the local file (no daemon needed).
     * @ingroup kanariv_app
     */
    AppResult configGet(const std::string& key);

    /** @brief Sets a config value (daemon-side). @ingroup kanariv_app */
    AppResult configSet(const std::string& key, const std::string& value);

    /** @brief Lists config entries. @ingroup kanariv_app */
    AppResult configList();

    /** @brief Exports config to a file. @ingroup kanariv_app */
    AppResult configExport(const std::string& path);

    /** @brief Imports config from a file (validated first). @ingroup kanariv_app */
    AppResult configImport(const std::string& path);

    /**
     * @brief Resets config to defaults (one key or all).
     * @ingroup kanariv_app
     * @param key Key to reset, or nullopt for everything.
     */
    AppResult configReset(std::optional<std::string> key);

    /**
     * @brief Plays a file ephemerally without touching the daemon/queue.
     * @ingroup kanariv_app
     * @details Delegates to the injected preview host (see
     * attachPreviewHost); returns an error when no host is configured.
     */
    AppResult previewFile(const std::string& file);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::app
