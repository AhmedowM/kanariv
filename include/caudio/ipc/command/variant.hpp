#pragma once

/**
 * @file variant.hpp
 * @brief Command variant and alternative concepts for the caudio IPC protocol.
 * @ingroup caudio_ipc
 * @note This header is fully inline/template; no .cpp is required.
 */

#include <caudio/ipc/command/bookmark.hpp>
#include <caudio/ipc/command/config.hpp>
#include <caudio/ipc/command/device.hpp>
#include <caudio/ipc/command/history.hpp>
#include <caudio/ipc/command/library.hpp>
#include <caudio/ipc/command/playback.hpp>
#include <caudio/ipc/command/playlist.hpp>
#include <caudio/ipc/command/queue.hpp>
#include <caudio/ipc/command/system.hpp>
#include <caudio/ipc/command/tag.hpp>
#include <caudio/ipc/command/track.hpp>
#include <type_traits>
#include <variant>

namespace caudio::ipc {

/**
 * @brief Variant type representing any valid CLI command.
 *
 * Used for JSON serialization/deserialization and type-safe dispatch.
 * All command structs are empty or contain only data members (no behavior).
 * @ingroup caudio_ipc
 *
 * @see toJson for serialization
 * @see commandFromJson for deserialization
 * @see protocol.cppm for IPC framing
 */
using Command =
    std::variant<Play, Pause, Resume, Restart, Stop, Next, Prev, Seek, StatusReq, VolumeSet,
                 QueueList, QueueQueues, QueueCreate, QueueDelete, PlayFiles, QueueSwitch, QueueAdd,
                 QueueRemove, QueueMove, QueueClear, QueueShuffle, QueueRepeat, PlaylistList,
                 PlaylistCreate, PlaylistAdd, PlaylistTracks, PlaylistLoad, PlaylistSave,
                 PlaylistDelete, PlaylistRename, PlaylistExport, PlaylistImport, LibraryScan,
                 LibrarySearch, LibraryStats, LibraryAdd, LibraryRemove, LibraryList, TagEdit,
                 TagGet, ConfigGet, ConfigSet, ConfigList, ConfigExport, ConfigImport, ConfigReset,
                 HistoryList, HistoryClear, BookmarkList, BookmarkAdd, BookmarkRemove, BookmarkGoto,
                 Shutdown, Preview, DeviceList, DeviceSet, DeviceTest, Info, TrackRate>;

// helper concepts
namespace detail {

template <typename T, typename Variant>
struct is_alternative : std::false_type {};

template <typename T, typename... Ts>
struct is_alternative<T, std::variant<Ts...>> : std::disjunction<std::is_same<T, Ts>...> {};

template <typename T>
inline constexpr bool is_alternative_v = is_alternative<T, Command>::value;

template <typename T>
concept is_command_alternative = is_alternative_v<T>;

} // namespace detail

/**
 * @brief Concept matching any valid command alternative type.
 *
 * Constrains template parameters to types that are part of the Command variant.
 * @ingroup caudio_ipc
 */
template <typename T>
concept CommandAlternative = detail::is_alternative_v<T>;

/**
 * @brief Concept matching any valid command type (including cvref-qualified).
 *
 * Convenience concept that strips cvref qualifiers before checking CommandAlternative.
 * @ingroup caudio_ipc
 */
template <typename T>
concept CommandType = CommandAlternative<std::remove_cvref_t<T>>;

} // namespace caudio::ipc
