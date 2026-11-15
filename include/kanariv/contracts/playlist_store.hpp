#pragma once

/**
 * @file playlist_store.hpp
 * @brief Playlist persistence port.
 * @ingroup kanariv_contracts
 * @details Playlist containers and their ordered membership.
 *
 * @par Error behavior
 * Implementations use `StatusCode::NotFound` for absent playlists or items,
 * `InvalidArg` for empty names and out-of-range positions, and `Io` or
 * `Internal` for backend failures.
 */

#include <cstdint>
#include <expected>
#include <kanariv/contracts/store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace kanariv::contracts {

/**
 * @brief Playlist containers and membership.
 * @ingroup kanariv_contracts
 */
class IPlaylistStore : public IStore {
  public:
    /**
     * @brief Lists playlists.
     * @return Playlist values, or a backend error.
     */
    virtual std::expected<std::vector<kanariv::types::Playlist>, kanariv::utils::Error>
    listPlaylists() = 0;

    /**
     * @brief Lists tracks in a playlist ordered by position.
     * @param pid Playlist identifier.
     * @return Track values, or `NotFound`/a backend error.
     */
    virtual std::expected<std::vector<kanariv::types::Track>, kanariv::utils::Error>
    playlistGetTracks(int64_t pid) = 0;

    /**
     * @brief Loads a playlist by identifier.
     * @param id Playlist identifier.
     * @return Playlist, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<kanariv::types::Playlist, kanariv::utils::Error>
    getPlaylist(int64_t id) = 0;

    /**
     * @brief Creates a playlist and returns its identifier.
     * @param name Display name; implementations reject empty names.
     * @param type Playlist type, where zero denotes a manual playlist.
     * @param smart_query Smart-playlist query; empty for a manual playlist.
     * @return New identifier, or `InvalidArg`/a backend error.
     */
    virtual std::expected<int64_t, kanariv::utils::Error>
    createPlaylist(std::string_view name, int type = 0, std::string_view smart_query = {}) = 0;

    /**
     * @brief Adds a track to a playlist.
     * @param pid Playlist identifier.
     * @param tid Track identifier.
     * @param pos Zero-based insertion position, or `-1` to append.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> playlistAddTrack(int64_t pid, int64_t tid,
                                                                        int64_t pos = -1) = 0;

    /**
     * @brief Removes a track from a playlist.
     * @param pid Playlist identifier.
     * @param tid Track identifier.
     * @return Success, or `NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> playlistRemoveTrack(int64_t pid,
                                                                           int64_t tid) = 0;

    /**
     * @brief Moves a playlist item between positions.
     * @param pid Playlist identifier.
     * @param from Existing zero-based position.
     * @param to Destination zero-based position.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> playlistReorder(int64_t pid, int64_t from,
                                                                       int64_t to) = 0;

    /**
     * @brief Changes a playlist name.
     * @param id Playlist identifier.
     * @param newName Replacement display name.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> renamePlaylist(int64_t id,
                                                                      std::string_view newName) = 0;

    /**
     * @brief Creates a playlist from an ordered list of track identifiers.
     * @param name Display name.
     * @param trackIds Track identifiers in playlist order.
     * @return New playlist identifier, or an error; implementations must
     * create the playlist and its items atomically.
     */
    virtual std::expected<int64_t, kanariv::utils::Error>
    createPlaylistFromTracks(std::string_view name, std::span<const int64_t> trackIds) = 0;

    /**
     * @brief Deletes a playlist by identifier.
     * @param id Playlist identifier.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> deletePlaylist(int64_t id) = 0;
};

} // namespace kanariv::contracts
