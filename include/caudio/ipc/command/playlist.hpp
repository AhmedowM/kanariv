#pragma once

/**
 * @file playlist.hpp
 * @brief Playlist commands for the caudio IPC protocol.
 * @ingroup caudio_ipc
 */

#include <cstdint>
#include <optional>
#include <string>

namespace caudio::ipc {

// ---------------------------------------------------------------------------
// Playlist Commands
// ---------------------------------------------------------------------------

/**
 * @struct PlaylistList
 * @brief List all playlists.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistList"}
 *
 * @return Playlists object with vector of playlists.
 */
struct PlaylistList final {};

/**
 * @struct PlaylistTracks
 * @brief List tracks in a specific playlist.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistTracks", "pid": 1}
 *
 * @param pid Playlist ID.
 * @return PlaylistData object with tracks and format info.
 */
struct PlaylistTracks final {
    /**
     * @brief Playlist ID.
     */
    int64_t pid{0};
};

/**
 * @struct PlaylistCreate
 * @brief Create an empty playlist, optionally a smart playlist.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistCreate", "name": "mix"}
 *
 * @param name Display name for the new playlist.
 * @param smart_query Smart-playlist filter expression (empty = manual playlist).
 * @return PlaylistCreated object with the new playlist id and name.
 */
struct PlaylistCreate final {
    /**
     * @brief Display name for the new playlist.
     */
    std::string name{};
    /**
     * @brief Smart-playlist filter expression (empty = manual playlist).
     * See the smart-query grammar in caudio/db/smart.hpp.
     */
    std::string smart_query{};
};

/**
 * @struct PlaylistAdd
 * @brief Append a library track to a playlist.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistAdd", "pid": 1, "track_id": 7}
 *
 * @param pid Playlist ID.
 * @param track_id Library track ID.
 * @return Empty (success with no data).
 */
struct PlaylistAdd final {
    /**
     * @brief Playlist ID.
     */
    int64_t pid{0};
    /**
     * @brief Library track ID.
     */
    int64_t track_id{0};
};

/**
 * @struct PlaylistLoad
 * @brief Load a playlist into the active queue and optionally start playback.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistLoad", "pid": 1, "play": true}
 *
 * @param pid Playlist ID to load.
 * @param play If true, switch to the new queue and start playback.
 * @param replace If true, replace the active queue instead of a new queue.
 * @return PlaylistLoaded object with the target queue id and status.
 */
struct PlaylistLoad final {
    /**
     * @brief Playlist ID to load.
     */
    int64_t pid{0};
    /**
     * @brief If true, switch to the target queue and start playback.
     */
    bool play{false};
    /**
     * @brief If true, replace the active queue instead of a new queue.
     */
    bool replace{false};
};

/**
 * @struct PlaylistSave
 * @brief Save the active queue as a new playlist.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistSave", "name": "My Playlist", "queue_id": 1}
 *
 * @param name Name for the new playlist.
 * @param queue_id Queue ID to save. If omitted, saves the active queue.
 * @return Empty (success with no data).
 */
struct PlaylistSave final {
    /**
     * @brief Name for the new playlist.
     */
    std::string name{};
    /**
     * @brief Queue ID to save. If omitted, saves the active queue.
     */
    std::optional<int64_t> queue_id{};
};

/**
 * @struct PlaylistDelete
 * @brief Delete a playlist by ID.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistDelete", "pid": 1}
 *
 * @param pid Playlist ID to delete.
 * @return Empty (success with no data).
 */
struct PlaylistDelete final {
    /**
     * @brief Playlist ID to delete.
     */
    int64_t pid{0};
};

/**
 * @struct PlaylistRename
 * @brief Rename an existing playlist.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistRename", "pid": 1, "newName": "New Name"}
 *
 * @param pid Playlist ID to rename.
 * @param newName New name for the playlist.
 * @return Empty (success with no data).
 */
struct PlaylistRename final {
    /**
     * @brief Playlist ID to rename.
     */
    int64_t pid{0};
    /**
     * @brief New name for the playlist.
     */
    std::string newName{};
};

/**
 * @struct PlaylistExport
 * @brief Export a playlist to a file (M3U format).
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistExport", "pid": 1, "path": "/tmp/playlist.m3u", "format": "m3u"}
 *
 * @param pid Playlist ID to export.
 * @param path Output file path.
 * @param format Export format (currently only "m3u" supported).
 * @return Empty (success with no data).
 */
struct PlaylistExport final {
    /**
     * @brief Playlist ID to export.
     */
    int64_t pid{0};
    /**
     * @brief Output file path.
     */
    std::string path{};
    /**
     * @brief Export format (currently only "m3u" supported).
     */
    std::string format{"m3u"};
};

/**
 * @struct PlaylistImport
 * @brief Import a playlist from an M3U file.
 * @ingroup caudio_ipc
 *
 * @json_example {"type": "PlaylistImport", "path": "/tmp/playlist.m3u", "name": "Imported"}
 *
 * @param path Path to the M3U file.
 * @param name Optional name for the imported playlist. If omitted, derived from the filename.
 * @return Empty (success with no data).
 */
struct PlaylistImport final {
    /**
     * @brief Path to the M3U file.
     */
    std::string path{};
    /**
     * @brief Optional name for the imported playlist.
     * If omitted, derived from the filename.
     */
    std::optional<std::string> name{};
};

} // namespace caudio::ipc
