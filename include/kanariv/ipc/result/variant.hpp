#pragma once

/**
 * @file variant.hpp
 * @brief Result variant and expected-reply alias for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 * @details The active alternative identifies the reply projection returned
 * for a command. `Empty` represents a successful command with no payload and
 * `CliError` represents a protocol-level failure returned by the daemon.
 * Serialization preserves the selected alternative's wire discriminator.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/ipc/result/bookmark.hpp>
#include <kanariv/ipc/result/config.hpp>
#include <kanariv/ipc/result/device.hpp>
#include <kanariv/ipc/result/history.hpp>
#include <kanariv/ipc/result/library.hpp>
#include <kanariv/ipc/result/playback.hpp>
#include <kanariv/ipc/result/playlist.hpp>
#include <kanariv/ipc/result/queue.hpp>
#include <kanariv/ipc/result/special.hpp>
#include <variant>

namespace kanariv::ipc {

/**
 * @brief Variant type representing any valid CLI result or error.
 *
 * Used for JSON serialization/deserialization. Contains all possible
 * result types plus Empty for successful operations with no data,
 * and CliError for error responses.
 * @ingroup kanariv_ipc
 *
 * @see toJson for serialization
 * @see resultFromJson for deserialization
 * @see protocol.cppm for IPC framing
 * @par Ownership
 * The variant owns its active result payload; vectors and strings are copied
 * or moved with the normal `std::variant` value semantics.
 */
using Result = std::variant<Status, QueueTracks, Queues, QueueCreated, PlaylistLoaded, VolumeInfo,
                            kanariv::types::LibraryStats, Tracks, SearchResults, Playlists,
                            PlaylistData, PlaylistCreated, PlaylistImportReport, ScanReport,
                            LibraryMaintenanceReport, ConfigValue, ConfigValues, SingleTrack,
                            TrackInfo, History, Bookmarks, Empty, CliError, Devices>;

/**
 * @brief Expected type for results that can fail with a CLI error.
 * @ingroup kanariv_ipc
 *
 * Used for functions that return either a Result or a CliError.
 * @see kanariv::utils::Error
 */
using ExpectedReply = kanariv::utils::Expected<Result>;

} // namespace kanariv::ipc
