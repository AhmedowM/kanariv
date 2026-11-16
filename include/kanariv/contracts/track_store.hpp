#pragma once

/**
 * @file track_store.hpp
 * @brief TrackRow record, bookmark and lyrics persistence port.
 * @ingroup kanariv_contracts
 * @details TrackRow identity (id, fingerprint, path, durable source), the track
 * record itself, and the per-track annotations (bookmarks, lyrics).
 *
 * @par Error behavior
 * Implementations use `StatusCode::NotFound` for absent rows, `InvalidArg` for
 * invalid caller input (empty path/source key, rating outside 0..5), and `Io`,
 * `Corrupt` or `Internal` for backend failures.
 */

#include <array>
#include <cstdint>
#include <expected>
#include <kanariv/contracts/store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <string_view>
#include <vector>

namespace kanariv::contracts {

/**
 * @brief TrackRow rows and their per-track annotations.
 * @ingroup kanariv_contracts
 */
class ITrackStore : public IStore {
  public:
    /**
     * @brief Loads a track by id.
     * @return TrackRow on success, or NotFound when no row has this id.
     */
    virtual std::expected<kanariv::types::TrackRow, kanariv::utils::Error> getTrack(int64_t id) = 0;

    /**
     * @brief Inserts a track and returns its new row identifier.
     * @param t TrackRow value to persist.
     * @return The new identifier, or an error such as `AlreadyExists`,
     * `InvalidArg`, or `Internal`.
     */
    virtual std::expected<int64_t, kanariv::utils::Error>
    insertTrack(const kanariv::types::TrackRow& t) = 0;

    /**
     * @brief Finds a track by its 32-byte content fingerprint.
     * @param fp Fingerprint to look up.
     * @return The matching track, or `NotFound` when no row matches.
     */
    virtual std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    findByFingerprint(const std::array<uint8_t, 32>& fp) = 0;

    /**
     * @brief Finds a track by filesystem path.
     * @param path Path key used by the store.
     * @return The matching track, or `NotFound` when no row matches.
     */
    virtual std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    findByPath(std::string_view path) = 0;

    /**
     * @brief Finds a track by durable source identity.
     * @param source Source kind and stable key; path rows use the filesystem
     * path as the key after normalization.
     * @return TrackRow on success, or NotFound.
     */
    virtual std::expected<kanariv::types::TrackRow, kanariv::utils::Error>
    findBySource(const kanariv::types::SourceRef& source) = 0;

    /**
     * @brief Updates an existing track row.
     * @param t TrackRow containing the row identifier and fields to persist.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error>
    updateTrack(const kanariv::types::TrackRow& t) = 0;

    /**
     * @brief Deletes a track by identifier.
     * @param id TrackRow identifier.
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> deleteTrack(int64_t id) = 0;

    /**
     * @brief Sets the user rating for a track.
     * @param id TrackRow identifier.
     * @param rating Rating value (0..5).
     * @return Success, or `InvalidArg`, `NotFound`, or a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> setRating(int64_t id, int32_t rating) = 0;

    /**
     * @brief Lists bookmarks belonging to a track.
     * @param tid TrackRow identifier.
     * @return Bookmarks ordered by the implementation, or a backend error.
     */
    virtual std::expected<std::vector<kanariv::types::Bookmark>, kanariv::utils::Error>
    listBookmarks(int64_t tid) = 0;

    /**
     * @brief Loads a bookmark by identifier.
     * @param id Bookmark identifier.
     * @return Bookmark, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<kanariv::types::Bookmark, kanariv::utils::Error>
    bookmarkGet(int64_t id) = 0;

    /**
     * @brief Adds a bookmark at a position in milliseconds.
     * @param tid TrackRow identifier.
     * @param pos Position in milliseconds.
     * @param note Optional user note.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> bookmarkAdd(int64_t tid, int64_t pos,
                                                                   std::string_view note = {}) = 0;

    /**
     * @brief Removes a bookmark by identifier.
     * @param id Bookmark identifier.
     * @return Success, or `InvalidArg`/`NotFound`/a backend error.
     */
    virtual std::expected<void, kanariv::utils::Error> bookmarkRemove(int64_t id) = 0;

    /**
     * @brief Sets lyrics for a track (upsert by track id).
     * @param tid TrackRow id.
     * @param lrc_text Lyrics text (LRC format when synced).
     * @param is_synced True when lrc_text contains time-coded lines.
     * @param source Provenance label (e.g. "local", "import").
     * @return Success or Error.
     */
    virtual std::expected<void, kanariv::utils::Error>
    lyricsSet(int64_t tid, std::string_view lrc_text, bool is_synced, std::string_view source) = 0;

    /**
     * @brief Gets lyrics for a track.
     * @param tid TrackRow id.
     * @return Lyrics, or NotFound when no lyrics exist for the track.
     */
    virtual std::expected<kanariv::types::Lyrics, kanariv::utils::Error> lyricsGet(int64_t tid) = 0;

    /**
     * @brief Removes lyrics for a track.
     * @param tid TrackRow id.
     * @return Success, or NotFound when no lyrics exist for the track.
     */
    virtual std::expected<void, kanariv::utils::Error> lyricsRemove(int64_t tid) = 0;
};

} // namespace kanariv::contracts
