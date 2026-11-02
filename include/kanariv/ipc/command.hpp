#pragma once

/**
 * @file command.hpp
 * @brief CLI command types and variants for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 *
 * Defines all command structs that can be sent from a client to the
 * kanariv service daemon. Commands are serialized as JSON over the
 * IPC socket/named pipe connection.
 *
 * ## IPC Protocol Structure
 *
 * Commands are sent as JSON objects with a "type" field identifying the command.
 * The full IPC message format is:
 * - 4-byte big-endian length prefix (uint32_t)
 * - JSON payload containing an IpcRequest with:
 *   - `id`: uint32_t request identifier
 *   - `cmd`: Command object with `type` and command-specific fields
 *
 * @see kanariv::ipc::protocol for serialization/deserialization functions
 * @see kanariv::ipc::Result for response types
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
#include <kanariv/ipc/command/variant.hpp>
