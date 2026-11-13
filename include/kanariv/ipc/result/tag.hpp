#pragma once

/**
 * @file tag.hpp
 * @brief Tag batch-edit results for the kanariv IPC protocol.
 * @ingroup kanariv_ipc
 * @details The payload owns a snapshot of per-entry batch-edit results.
 * It can therefore be retained after the client connection or database
 * operation that produced it has ended.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kanariv::ipc {

// ---------------------------------------------------------------------------
// Tag Batch-Edit Results
// ---------------------------------------------------------------------------

/**
 * @struct TagBatchEditEntryResult
 * @brief Per-entry result of a TagBatchEdit command.
 * @ingroup kanariv_ipc
 *
 * @json_example {"id": 123, "field": "artist", "ok": true}
 *
 * @param id Resolved track id (nullopt if identification failed).
 * @param path Path used to identify the track (if id was not given).
 * @param field Tag field that was attempted.
 * @param ok True when the entry was applied successfully.
 * @param error Human-readable error message when ok is false.
 */
struct TagBatchEditEntryResult final {
    std::optional<int64_t> id{};
    std::optional<std::string> path{};
    std::string field{};
    bool ok{false};
    std::string error{};
};

/**
 * @struct TagBatchEditResult
 * @brief Aggregate result of a TagBatchEdit command.
 * @ingroup kanariv_ipc
 *
 * @json_example {"type": "TagBatchEditResult", "results": [...], "succeeded": 2, "failed": 0}
 *
 * @param results Per-entry success/failure details.
 * @param succeeded Number of entries applied successfully.
 * @param failed Number of entries that failed.
 */
struct TagBatchEditResult final {
    std::vector<TagBatchEditEntryResult> results{};
    int64_t succeeded{0};
    int64_t failed{0};
};

} // namespace kanariv::ipc
