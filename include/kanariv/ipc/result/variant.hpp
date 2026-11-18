#pragma once

/**
 * @file variant.hpp
 * @brief Result variant and expected-reply alias for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 * @details The active alternative identifies the reply projection returned
 * for a command. `Empty` represents a successful command with no payload.
 * Serialization preserves the selected alternative's wire discriminator.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/ipc/result/bookmark.hpp>
#include <kanariv/ipc/result/config.hpp>
#include <kanariv/ipc/result/device.hpp>
#include <kanariv/ipc/result/history.hpp>
#include <kanariv/ipc/result/library.hpp>
#include <kanariv/ipc/result/lyrics.hpp>
#include <kanariv/ipc/result/playback.hpp>
#include <kanariv/ipc/result/playlist.hpp>
#include <kanariv/ipc/result/queue.hpp>
#include <kanariv/ipc/result/special.hpp>
#include <kanariv/ipc/result/tag.hpp>
#include <variant>

namespace kanariv::ipc {

/**
 * @brief Variant type representing any successful CLI result.
 *
 * Used for JSON serialization/deserialization. Contains all possible
 * result types plus Empty for successful operations with no data.
 * @ingroup kanariv_ipc
 *
 * @see toJson for serialization
 * @see resultFromJson for deserialization
 * @see protocol.cppm for IPC framing
 * @par Ownership
 * The variant owns its active result payload; vectors and strings are copied
 * or moved with the normal `std::variant` value semantics.
 */
using Result =
    std::variant<Status, QueueTracks, QueueAddReport, QueueRemoveReport, Queues, QueueCreated,
                 PlaylistLoaded, VolumeInfo, kanariv::types::LibraryStats, Tracks, SearchResults,
                 Playlists, PlaylistData, PlaylistCreated, PlaylistImportReport, ScanReport,
                 AnalyzeReport, LibraryMaintenanceReport, ConfigValue, ConfigValues, SingleTrack,
                 TrackInfo, History, Bookmarks, Lyrics, TagBatchEditResult, Empty, Devices>;

/**
 * @brief Expected type for replies that can fail.
 * @ingroup kanariv_ipc
 *
 * `IpcReply.result` is the only error channel: a daemon-side failure is the
 * `unexpected` side, never a `Result` alternative.
 * @see kanariv::utils::Error
 */
using ExpectedReply = kanariv::utils::Expected<Result>;

} // namespace kanariv::ipc
