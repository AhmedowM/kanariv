#pragma once

/**
 * @file variant.hpp
 * @brief Command variant and alternative concepts for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 * @details `Command` is the closed set of request payloads accepted by the
 * daemon. Adding a command requires adding its payload type here and teaching
 * the protocol serializer and service dispatcher about that alternative.
 * The concepts below are intended for generic serializers and dispatch code;
 * they do not perform runtime validation of a payload's field values.
 * @note This header is fully inline/template; no .cpp is required.
 */

#include <kanariv/ipc/command/bookmark.hpp>
#include <kanariv/ipc/command/config.hpp>
#include <kanariv/ipc/command/device.hpp>
#include <kanariv/ipc/command/history.hpp>
#include <kanariv/ipc/command/library.hpp>
#include <kanariv/ipc/command/playback.hpp>
#include <kanariv/ipc/command/playlist.hpp>
#include <kanariv/ipc/command/queue.hpp>
#include <kanariv/ipc/command/system.hpp>
#include <kanariv/ipc/command/tag.hpp>
#include <kanariv/ipc/command/track.hpp>
#include <type_traits>
#include <variant>

namespace kanariv::ipc {

/**
 * @brief Variant type representing any valid CLI command.
 *
 * Used for JSON serialization/deserialization and type-safe dispatch.
 * All command structs are empty or contain only data members (no behavior).
 * @ingroup kanariv_ipc
 *
 * @see toJson for serialization
 * @see commandFromJson for deserialization
 * @see protocol.cppm for IPC framing
 * @par Ownership
 * The variant owns its active command payload and is safe to move between
 * request-building and transport code.
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
                 Shutdown, Preview, DeviceList, DeviceSet, DeviceTest, Info, TrackRate,
                 LibraryMaintenance, StatusWait>;

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
 * @ingroup kanariv_ipc
 */
template <typename T>
concept CommandAlternative = detail::is_alternative_v<T>;

/**
 * @brief Concept matching any valid command type (including cvref-qualified).
 *
 * Convenience concept that strips cvref qualifiers before checking CommandAlternative.
 * @ingroup kanariv_ipc
 */
template <typename T>
concept CommandType = CommandAlternative<std::remove_cvref_t<T>>;

} // namespace kanariv::ipc
