#pragma once
#include <cstdint>
#include <random>
#include <vector>

/**
 * @file shuffle.hpp
 * @brief Shuffle permutation helpers for the playback queue.
 * @ingroup kanariv_engine
 * @details Provides the shuffle primitive used by
 * Engine::setShuffleLocked and Engine::queueNextLocked. Permutations
 * are vectors of queue positions `[0, queueCount)` shuffled via
 * `std::ranges::shuffle` with `std::mt19937`.
 *
 * Two overloads are exported under `kanariv::engine::detail`:
 * - `shufflePerm(perm, rng)` uses a caller-supplied engine (e.g.
 *   `std::mt19937{42}` for deterministic tests).
 * - `shufflePerm(perm)` seeds `mt19937` from `std::random_device`
 *   for production randomness.
 *
 * Thread safety: helpers are stateless and operate on the caller's
 * vector; Engine holds `queueMutex_` while mutating `QueueState::perm`.
 */

namespace kanariv::engine::detail {

/**
 * @brief Shuffles a permutation in-place using the supplied RNG.
 * @ingroup kanariv_engine
 * @param perm Vector of positions to shuffle (typically `[0, n)`).
 * @param rng Mersenne Twister engine; caller controls seeding.
 * @details Thin wrapper over `std::ranges::shuffle(perm, rng)`.
 * Used by tests with `std::mt19937{42}` for deterministic ordering.
 * @par Thread safety
 * No internal synchronization; caller must hold `queueMutex_` if
 * `perm` is `QueueState::perm`.
 * @see shufflePerm(std::vector<int64_t>&)
 */
void shufflePerm(std::vector<int64_t>& perm, std::mt19937& rng);

/**
 * @brief Shuffles a permutation using a random_device-seeded engine.
 * @ingroup kanariv_engine
 * @param perm Vector of positions to shuffle.
 * @details Seeds `std::mt19937` from `std::random_device` then
 * delegates to `shufflePerm(perm, rng)`. Production path used by
 * Engine::setShuffleLocked.
 * @par Thread safety
 * No internal synchronization; caller must hold `queueMutex_`.
 * @see shufflePerm(std::vector<int64_t>&, std::mt19937&)
 */
void shufflePerm(std::vector<int64_t>& perm);

} // namespace kanariv::engine::detail
