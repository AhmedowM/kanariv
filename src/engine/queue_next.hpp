#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <optional>
#include <span>
#include <vector>

/**
 * @file queue_next.hpp
 * @brief Read-only "what plays next" resolution for the playback queue.
 * @ingroup kanariv_engine
 * @details Internal header (lives in `src/`, not installed). Extracted from
 * `Engine::Impl::queueNextLocked` so the cursor/permutation arithmetic can be
 * peeked without mutating the queue and unit-tested directly (linear walking,
 * shuffle, repeat-one and wrap) without a database or decode thread.
 *
 * `planNext` maps the pair (shuffle flag, repeat mode, cursor, permutation) to
 * the queue listing index of the next entry plus the cursor value a caller
 * commits once that entry is actually accepted. It performs no I/O: item
 * counts and a fresh permutation are requested through caller-supplied
 * callables that are only invoked when the plan genuinely needs them.
 *
 * Thread safety: stateless pure function over caller-owned values. The engine
 * calls it under `queueMutex_`; committing the returned plan (writing
 * `QueueState::cursor`/`perm`) is a separate step and must happen under the
 * same lock.
 */

namespace kanariv::engine::detail {

/**
 * @brief Planned next queue entry and the cursor change that selects it.
 * @ingroup kanariv_engine
 */
struct NextPlan {
    /// Zero-based listing index (`queueTrackAt` argument) of the entry.
    std::int64_t position{0};
    /// Cursor value to commit once the entry is accepted.
    std::size_t cursor{0};
    /// Fresh permutation to install verbatim; set only for the first shuffle
    /// use (empty `perm`), so `position` is that permutation's head.
    std::optional<std::vector<std::int64_t>> newPerm{};
};

/**
 * @brief Resolves the next queue entry without mutating any queue state.
 * @ingroup kanariv_engine
 * @param shuffle Shuffle flag (`QueueState::shuffle`).
 * @param repeat Repeat mode (`QueueState::repeat`).
 * @param cursor Current cursor (`QueueState::cursor`).
 * @param perm Active shuffle permutation; empty when shuffle is off.
 * @param count Callable returning the queue's item count. Invoked only for a
 * linear queue or the first shuffle use.
 * @param makePerm Callable taking an item count and returning a permutation of
 * `[0, count)`. Invoked only for the first shuffle use.
 * @return The plan, or an error (NotFound for an empty queue, Internal when
 * `makePerm` yields nothing).
 * @details Rules, matching the historical `queueNextLocked` behaviour:
 * - linear: the entry at `cursor`, advancing to `cursor + 1`;
 * - linear wrap (cursor past the end, repeat Off/All): entry 0, cursor 1;
 * - linear repeat-one at the end: replay the last entry, cursor unchanged;
 * - shuffle: the entry at `perm[cursor]`, advancing to `cursor + 1`;
 * - shuffle wrap (repeat Off/All): replay `perm[0]` of the *existing* cycle,
 *   cursor 1 (no reshuffle);
 * - shuffle repeat-one at the end: replay the entry the cursor still trails,
 *   cursor unchanged;
 * - empty permutation with shuffle on: a fresh permutation is generated and
 *   its head is planned (`newPerm` set, cursor 1).
 * @par Thread safety
 * Pure; no shared state. Callers holding a queue lock may pass live values.
 * @see Engine::Impl::peekNextLocked
 */
template <typename CountFn, typename PermFn>
std::expected<NextPlan, kanariv::utils::Error>
planNext(bool shuffle, kanariv::types::RepeatMode repeat, std::size_t cursor,
         std::span<const std::int64_t> perm, CountFn&& count, PermFn&& makePerm) {
    NextPlan plan;
    if (shuffle) {
        if (perm.empty()) {
            const std::size_t n = static_cast<std::size_t>(count());
            if (n == 0)
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
            plan.newPerm = makePerm(n);
            if (!plan.newPerm || plan.newPerm->empty())
                return std::unexpected(
                    kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no perm"));
            plan.position = (*plan.newPerm)[0];
            plan.cursor = 1;
            return plan;
        }
        if (cursor >= perm.size()) {
            if (repeat == kanariv::types::RepeatMode::One) {
                // Repeat-one replays the entry the cursor still trails; the
                // cursor must not advance or the replayed entry is lost.
                std::size_t idx = perm.size() - 1;
                if (cursor > 0 && cursor <= perm.size())
                    idx = cursor - 1;
                plan.position = perm[idx];
                plan.cursor = cursor;
                return plan;
            }
            // Wrap to the head of the current permutation (no reshuffle until
            // the shuffle flag is toggled), consuming perm[0].
            plan.position = perm[0];
            plan.cursor = 1;
            return plan;
        }
        plan.position = perm[cursor];
        plan.cursor = cursor + 1;
        return plan;
    }
    const std::size_t n = static_cast<std::size_t>(count());
    if (n == 0)
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "empty queue"));
    if (cursor >= n) {
        if (repeat == kanariv::types::RepeatMode::One) {
            std::size_t idx = n - 1;
            if (cursor > 0 && cursor <= n)
                idx = cursor - 1;
            plan.position = static_cast<std::int64_t>(idx);
            plan.cursor = cursor;
            return plan;
        }
        plan.position = 0;
        plan.cursor = 1;
        return plan;
    }
    plan.position = static_cast<std::int64_t>(cursor);
    plan.cursor = cursor + 1;
    return plan;
}

} // namespace kanariv::engine::detail
