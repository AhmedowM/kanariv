#pragma once

/**
 * @file result.hpp
 * @brief CLI result types and variants for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 *
 * Defines all result structs that can be returned from the kanariv service
 * daemon to a client. Results are serialized as JSON over the IPC
 * socket/named pipe connection.
 *
 * ## IPC Protocol Structure
 *
 * Results are sent as JSON objects with a "type" field identifying the result type.
 * The full IPC message format is:
 * - 4-byte big-endian length prefix (uint32_t)
 * - JSON payload containing an IpcReply with:
 *   - `id`: uint32_t request identifier (matches request)
 *   - `ok`: boolean indicating success
 *   - `result`: Result object (if ok=true) or `error`: Error object (if ok=false)
 *
 * @see kanariv::ipc::protocol for serialization/deserialization functions
 * @see kanariv::ipc::Command for request types
 */

#include <kanariv/ipc/result/bookmark.hpp>
#include <kanariv/ipc/result/config.hpp>
#include <kanariv/ipc/result/device.hpp>
#include <kanariv/ipc/result/history.hpp>
#include <kanariv/ipc/result/library.hpp>
#include <kanariv/ipc/result/playback.hpp>
#include <kanariv/ipc/result/playlist.hpp>
#include <kanariv/ipc/result/queue.hpp>
#include <kanariv/ipc/result/special.hpp>
#include <kanariv/ipc/result/variant.hpp>
