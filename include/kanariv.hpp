#pragma once

/**
 * @file kanariv.hpp
 * @brief Umbrella header for kanariv -- modern C++23 music player library.
 * @defgroup kanariv kanariv
 *
 * @details kanariv is a cross-platform, offline-first music player library
 * written in modern C++23. It provides layered libraries (this header
 * includes all of them):
 *
 * - @ref kanariv_types "kanariv.types" -- Dependency-free shared value types:
 *   wire enums (RepeatMode, PlaybackState) and plain-data records (TrackRow,
 *   Playlist, QueueItem, HistoryEntry, Bookmark) used across all layers.
 * - @ref kanariv_utils "kanariv.utils" -- Core utilities: error handling (`std::expected`),
 *   lock-free SPSC ring, thread helpers, logging, opaque JSON value.
 * - @ref kanariv_media "kanariv.media" -- Media input: reader abstractions,
 *   FFmpeg-based decoder, metadata extraction, audio-extension helpers,
 *   library scan discovery (filesystem walk, fingerprint, tags).
 * - @ref kanariv_player "kanariv.player" -- Audio playback: miniaudio output,
 *   gapless playback. Reader/decoder are re-exported from `kanariv.media`.
 * - @ref kanariv_db "kanariv.db" -- Database layer: SQLite with WAL, track/playlist/queue
 *   management, full-text search (FTS5), JSON import/export.
 * - @ref kanariv_library "kanariv.library" -- Library scan orchestration: walks a
 *   configured root with the media stack and upserts the discovered tracks.
 * - @ref kanariv_engine "kanariv.engine" -- Playback engine: state machine, shuffle/repeat,
 *   gapless transition, history marking, persistence, event queue.
 * - @ref kanariv_ipc "kanariv.ipc" -- Daemon wire protocol: Command/Result
 *   variants, JSON serialization, length-prefixed framing.
 * - @ref kanariv_client "kanariv.client" -- Daemon SDK: Client, IPC transport,
 *   output formatting (used by the CLI and third-party frontends).
 * - @ref kanariv_service "kanariv.service" -- Daemon runtime: Service owning
 *   Engine, Database and the IPC server.
 * - config -- JSON config file, socket/pid/lock path derivation.
 *
 * Layer rule (each line lists its direct kanariv dependencies; the
 * authoritative declarations live in cmake/components/<component>.cmake):
 * @code
 * kanariv.types     (no deps)
 * kanariv.utils     (types)
 * kanariv.contracts (types, utils)
 * kanariv.media     (utils, types)
 * kanariv.player    (utils, types, media)
 * kanariv.db        (contracts, utils, types)
 * kanariv.library   (contracts, db, media, utils)
 * kanariv.engine    (contracts, utils, types; private: player, media)
 * kanariv.ipc       (utils, types)
 * kanariv.config    (types, utils)
 * kanariv.client    (config, ipc, utils)
 * kanariv.service   (config, ipc, utils, contracts; private: engine, db, media, player)
 * kanariv.app       (config, ipc, utils; private: client, media, service, db, player, library)
 * @endcode
 *
 * Threading model:
 * - `utils::SpscRing` -- lock-free SPSC (decode thread ->' audio callback)
 * - `Engine` -- `queueMutex_` (try_lock) for queue state, `decodeMtx_` for
 *   decoder/ring vs seek, `dbMutex_` for DB, `cbMutex_` for callbacks.
 * - `Database` -- `dbMutex_` (mutex) serializes all SQLite access, including
 *   the prepared-statement cache.
 * - Background writer -- single consumer, bounded MPSC queue,
 *   `condition_variable` + `stop_token`.
 *
 * Error handling: All fallible public APIs return `kanariv::utils::Expected<T>`
 * with `StatusCode` enum (Ok, InvalidArg, NotFound, Unsupported, Io, Device,
 * State, NoMem, Internal, AlreadyExists, Busy, Corrupt, NoSpace).
 *
 * Build: CMake 3.28+, C++23 modules, Ninja. Vendored deps: SQLite (FTS5),
 * miniaudio, BLAKE3, FFmpeg (required). JSON (nlohmann backend) is private
 * to the build -- downstream needs no JSON package.
 *
 * @see kanariv.types
 * @see kanariv.utils
 * @see kanariv.media
 * @see kanariv.player
 * @see kanariv.db
 * @see kanariv.library
 * @see kanariv.engine
 * @see kanariv.ipc
 * @see kanariv.client
 * @see kanariv.service
 * @see kanariv.config
 */

#include <kanariv/app.hpp>
#include <kanariv/client.hpp>
#include <kanariv/config.hpp>
#include <kanariv/db.hpp>
#include <kanariv/engine.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc.hpp>
#include <kanariv/library.hpp>
#include <kanariv/media.hpp>
#include <kanariv/player.hpp>
#include <kanariv/service.hpp>
#include <kanariv/types.hpp>
#include <kanariv/utils.hpp>
#include <kanariv/version.hpp>
