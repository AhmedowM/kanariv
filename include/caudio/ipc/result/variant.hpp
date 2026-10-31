#pragma once

/**
 * @file variant.hpp
 * @brief Result variant and expected-reply alias for the caudio IPC protocol.
 * @ingroup caudio_ipc
 */

#include <caudio/fwd.hpp>
#include <caudio/ipc/result/bookmark.hpp>
#include <caudio/ipc/result/config.hpp>
#include <caudio/ipc/result/device.hpp>
#include <caudio/ipc/result/history.hpp>
#include <caudio/ipc/result/library.hpp>
#include <caudio/ipc/result/playback.hpp>
#include <caudio/ipc/result/playlist.hpp>
#include <caudio/ipc/result/queue.hpp>
#include <caudio/ipc/result/special.hpp>
#include <variant>

namespace caudio::ipc {

/**
 * @brief Variant type representing any valid CLI result or error.
 *
 * Used for JSON serialization/deserialization. Contains all possible
 * result types plus Empty for successful operations with no data,
 * and CliError for error responses.
 * @ingroup caudio_ipc
 *
 * @see toJson for serialization
 * @see resultFromJson for deserialization
 * @see protocol.cppm for IPC framing
 */
using Result =
    std::variant<Status, QueueTracks, Queues, QueueCreated, PlaylistLoaded, VolumeInfo,
                 caudio::types::LibraryStats, Tracks, SearchResults, Playlists, PlaylistData,
                 PlaylistCreated, PlaylistImportReport, ScanReport, LibraryMaintenanceReport,
                 ConfigValue, ConfigValues, SingleTrack, TrackInfo, History, Bookmarks, Empty,
                 CliError, Devices>;

/**
 * @brief Expected type for results that can fail with a CLI error.
 * @ingroup caudio_ipc
 *
 * Used for functions that return either a Result or a CliError.
 * @see caudio::utils::Error
 */
using ExpectedReply = caudio::utils::Expected<Result>;

} // namespace caudio::ipc
