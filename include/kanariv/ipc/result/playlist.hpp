#pragma once

/**
 * @file playlist.hpp
 * @brief Playlist results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 */

#include <cstddef>
#include <cstdint>
#include <kanariv/ipc/result/playback.hpp>
#include <kanariv/types/record.hpp>
#include <string>
#include <vector>

namespace kanariv::ipc {

/**
 * @struct PlaylistLoaded
 * @brief Playlist load target and status (returned by PlaylistLoad).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "PlaylistLoaded", "queue_id": 3, "status": {...}}
 *
 * @param queue_id Target queue id (new queue, or active queue with replace).
 * @param status Playback status after loading.
 */
struct PlaylistLoaded final {
    /**
     * @brief Target queue id.
     */
    int64_t queue_id{0};
    /**
     * @brief Playback status after loading.
     */
    Status status{};
};

/**
 * @struct Playlists
 * @brief List of playlists (returned by PlaylistList).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "Playlists", "playlists": [{"id": 1, "name": "My Playlist", ...}]}
 *
 * @param playlists Vector of playlists.
 */
struct Playlists final {
    /**
     * @brief Vector of playlists.
     */
    std::vector<kanariv::types::Playlist> playlists{};
};

/**
 * @struct PlaylistData
 * @brief Playlist contents with format info (returned by PlaylistExport, PlaylistTracks).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "PlaylistData", "tracks": [...], "format": "m3u"}
 *
 * @param tracks Tracks in the playlist.
 * @param format Export format (e.g., "m3u").
 */
struct PlaylistData final {
    /**
     * @brief Tracks in the playlist.
     */
    std::vector<kanariv::types::Track> tracks{};
    /**
     * @brief Export format (e.g., "m3u").
     */
    std::string format{};
};

/**
 * @struct PlaylistCreated
 * @brief New playlist id (returned by PlaylistCreate).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "PlaylistCreated", "id": 2, "name": "mix"}
 *
 * @param id New playlist id.
 * @param name New playlist name.
 */
struct PlaylistCreated final {
    /**
     * @brief New playlist id.
     */
    int64_t id{0};
    /**
     * @brief New playlist name.
     */
    std::string name{};
};

/**
 * @struct PlaylistImportReport
 * @brief Playlist import outcome (returned by PlaylistImport).
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "PlaylistImportReport", "pid": 2, "matched": 9}
 *
 * @param pid Created playlist id.
 * @param name Created playlist name.
 * @param tracks Imported tracks.
 * @param matched Lines matched to library tracks.
 * @param skipped Lines with no library match.
 * @param duplicates Lines already present (skipped).
 */
struct PlaylistImportReport final {
    /**
     * @brief Created playlist id.
     */
    int64_t pid{0};
    /**
     * @brief Created playlist name.
     */
    std::string name{};
    /**
     * @brief Imported tracks.
     */
    std::vector<kanariv::types::Track> tracks{};
    /**
     * @brief Lines matched to library tracks.
     */
    std::size_t matched{0};
    /**
     * @brief Lines with no library match.
     */
    std::size_t skipped{0};
    /**
     * @brief Lines already present (skipped).
     */
    std::size_t duplicates{0};
};

} // namespace kanariv::ipc
