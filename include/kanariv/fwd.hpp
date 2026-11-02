#pragma once

/**
 * @file fwd.hpp
 * @brief Shared forward declarations and the canonical `Expected<T>` alias.
 * @ingroup kanariv
 * @details Centralizes forward declarations so that multiple public headers
 * do not repeat the same `struct sqlite3;` / `class Logger;` / `class Database;`
 * blocks. Include this instead of re-declaring in each header. Also defines the
 * single canonical `kanariv::utils::Expected<T>` alias so public headers can name
 * fallible return types without pulling the heavier `utils/error.hpp` (which
 * drags `<format>`, `<string>` and the `Error` formatter).
 * @note This header is fully inline; no .cpp is required.
 */

#include <cstdint>
#include <expected>
#include <type_traits>

#if defined(_WIN32)
#if defined(KANARIV_BUILDING_DLL)
#define KANARIV_API __declspec(dllexport)
#else
#define KANARIV_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define KANARIV_API __attribute__((visibility("default")))
#else
#define KANARIV_API
#endif

/** @brief Opaque SQLite database handle (defined by SQLite). */
struct sqlite3;
/** @brief Opaque SQLite prepared-statement handle (defined by SQLite). */
struct sqlite3_stmt;

namespace kanariv::config {
/** @brief Persisted application configuration. */
struct Config;
} // namespace kanariv::config

namespace kanariv::db {
/** @brief Database implementation. */
class Database;
/** @brief SQLite statement wrapper. */
class SqliteStatement;
} // namespace kanariv::db

namespace kanariv::contracts {
/** @brief Abstract database contract used by the engine and service. */
class IDatabase;
} // namespace kanariv::contracts

namespace kanariv::engine {
/** @brief Playback engine. */
class Engine;
} // namespace kanariv::engine

namespace kanariv::media {
/** @brief Seekable or streaming media reader. */
class Reader;
/** @brief Audio decoder. */
class Decoder;
} // namespace kanariv::media

namespace kanariv::player {
/** @brief Audio-device output sink. */
class AudioOutput;
} // namespace kanariv::player

namespace kanariv::service {
// IpcServer is internal (src/service/ipc_server.hpp); the public
// `kanariv/service/service.hpp` only names it in Service's private members,
// so the forward declaration stays here.
/** @brief Host adapter for running a service in a platform process. */
class Host;
/** @brief IPC server implementation used by the service. */
class IpcServer;
/** @brief Long-running daemon service. */
class Service;
} // namespace kanariv::service

namespace kanariv::ipc {
/** @brief Shared-memory playback status record. */
struct Status;
/** @brief Read-only shared-memory status mapping. */
class ShmStatusReader;
/** @brief Writable shared-memory status mapping. */
class ShmStatusWriter;
} // namespace kanariv::ipc

namespace kanariv::types {
/** @brief Queue repeat policy. */
enum class RepeatMode : std::uint8_t;
/** @brief Playback state-machine value. */
enum class PlaybackState : std::uint8_t;
/** @brief Persisted audio track record. */
struct Track;
/** @brief Persisted playlist record. */
struct Playlist;
/** @brief Queue entry record. */
struct QueueItem;
/** @brief Playback history record. */
struct HistoryEntry;
/** @brief Bookmark record. */
struct Bookmark;
/** @brief Queue record. */
struct Queue;
/** @brief Library summary record. */
struct Library;
/** @brief Track search query. */
struct TrackQuery;
/** @brief History search query. */
struct HistoryQuery;
/** @brief Playback statistics. */
struct PlaybackStats;
/** @brief Engine state snapshot. */
struct EngineState;
/** @brief Engine event category. */
enum class EngineEventType : std::uint8_t;
/** @brief Engine event payload. */
struct EngineEvent;
/** @brief Engine callback collection. */
struct EngineCallbacks;
/** @brief Engine construction options. */
struct EngineConfig;
} // namespace kanariv::types

namespace kanariv::utils {
template <typename T>
    requires std::is_trivially_copyable_v<T>
class SpscRing;
template <typename T>
class Generator;
class Logger;
class Json;
class JsonRef;
enum class StatusCode : std::uint8_t;
struct Error;

/**
 * @brief Canonical alias for `std::expected<T, Error>`.
 * @ingroup kanariv
 * @tparam T Value type on success.
 * @details Defined here (single definition) so public headers can name
 * fallible return types as `Expected<T>` with only `fwd.hpp`, without
 * including `utils/error.hpp`. `Error` may be incomplete at any declaration
 * that merely returns or takes `Expected<T>`; only definitions need it whole.
 */
template <typename T>
using Expected = std::expected<T, Error>;
} // namespace kanariv::utils
