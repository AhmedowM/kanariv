#pragma once

/**
 * @file mpsc_queue.hpp
 * @brief Multi-producer single-consumer bounded queue.
 * @ingroup kanariv_utils
 * @details Internal header: lives in `src/`, not installed. Used by the
 * engine event queue and the database writer thread.
 */

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace kanariv::utils {

/**
 * @brief Bounded MPSC queue with blocking and non-blocking operations.
 * @ingroup kanariv_utils
 * @tparam T Element type (stored as `std::optional<T>` internally).
 * @details Thread-safety and lock ordering:
 * - Single `mutable std::mutex mutex_` protects `cap_`, `buf_`, `wr_`, `rd_`.
 *   All public methods lock it (`lock_guard` or `unique_lock`).
 * - `wr_`/`rd_` are monotonic counters (not modulo indices); index is
 *   `counter % cap_`. `size()` is `wr_ - rd_` under lock.
 * - `condition_variable_any cv_` is notified on every successful push/emplace
 *   while still holding the lock (then unlocked). `waitPop()` waits with
 *   predicate `wr_ != rd_` and respects `std::stop_token`.
 * - Capacity is fixed at construction (0 is normalized to 1). No resize.
 *
 * @see Error
 * @see StatusCode
 */
template <typename T>
class MpscQueue {
  public:
    /**
     * @brief Constructs a queue with given capacity.
     * @ingroup kanariv_utils
     * @param capacity Maximum number of elements (0 is normalized to 1).
     */
    explicit MpscQueue(std::size_t capacity = 64) : cap_(capacity), buf_(capacity) {
        if (cap_ == 0)
            cap_ = 1;
        if (buf_.size() != cap_)
            buf_.resize(cap_);
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;

    ~MpscQueue() = default;

    /**
     * @brief Returns capacity.
     * @ingroup kanariv_utils
     * @return Capacity (normalized, never 0).
     */
    [[nodiscard]] std::size_t capacity() const noexcept {
        return cap_;
    }

    /**
     * @brief Returns number of elements currently queued.
     * @ingroup kanariv_utils
     * @return `wr_ - rd_` (computed under lock).
     */
    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return wr_ - rd_;
    }

    /**
     * @brief Checks if queue is empty.
     * @ingroup kanariv_utils
     * @return true if `wr_ == rd_` (under lock).
     */
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return wr_ == rd_;
    }

    /**
     * @brief Pushes a copy into the queue (non-blocking).
     * @ingroup kanariv_utils
     * @tparam U Value type (defaults to T); must be copy-constructible.
     * @param value Value to copy.
     * @return `Expected<void>` success (`{}`) or error `Busy` if full.
     * @details Locks mutex_, checks `used >= cap_`, emplaces at `wr_ % cap_`,
     * increments `wr_`, notifies `cv_`. Error code: `StatusCode::Busy`
     * with message "queue full" when capacity reached.
     */
    template <typename U = T>
    [[nodiscard]] Expected<void> push(const U& value)
        requires std::is_copy_constructible_v<U>
    {
        std::unique_lock<std::mutex> lk(mutex_);
        std::size_t used = wr_ - rd_;
        if (used >= cap_) {
            return std::unexpected(Error{StatusCode::Busy, std::string_view{"queue full"}});
        }
        buf_[wr_ % cap_].emplace(value);
        ++wr_;
        cv_.notify_one();
        return {};
    }

    /**
     * @brief Pushes by move into the queue (non-blocking).
     * @ingroup kanariv_utils
     * @param value Value to move.
     * @return `Expected<void>` success or `Busy` if full.
     * @details Same locking/notification as copy-push. Error code:
     * `StatusCode::Busy` when full.
     */
    [[nodiscard]] Expected<void> push(T&& value) {
        std::unique_lock<std::mutex> lk(mutex_);
        std::size_t used = wr_ - rd_;
        if (used >= cap_) {
            return std::unexpected(Error{StatusCode::Busy, std::string_view{"queue full"}});
        }
        buf_[wr_ % cap_].emplace(std::move(value));
        ++wr_;
        cv_.notify_one();
        return {};
    }

    /**
     * @brief Constructs an element in-place (non-blocking).
     * @ingroup kanariv_utils
     * @tparam Args Constructor argument types for T.
     * @param args Arguments forwarded to T's constructor.
     * @return `Expected<void>` success or `Busy` if full.
     * @details Locks, checks capacity, emplaces at `wr_ % cap_`, notifies.
     */
    template <typename... Args>
    [[nodiscard]] Expected<void> emplace(Args&&... args) {
        std::unique_lock<std::mutex> lk(mutex_);
        std::size_t used = wr_ - rd_;
        if (used >= cap_) {
            return std::unexpected(Error{StatusCode::Busy, std::string_view{"queue full"}});
        }
        buf_[wr_ % cap_].emplace(std::forward<Args>(args)...);
        ++wr_;
        cv_.notify_one();
        return {};
    }

    /**
     * @brief Pops an element (non-blocking).
     * @ingroup kanariv_utils
     * @return `Expected<T>` with value on success, or `State` if empty.
     * @details Locks, checks `wr_ == rd_`, moves value from `rd_ % cap_`,
     * resets optional, increments `rd_`. Error code: `StatusCode::State`
     * with message "queue empty" when no element.
     */
    [[nodiscard]] Expected<T> pop() {
        std::unique_lock<std::mutex> lk(mutex_);
        if (wr_ == rd_) {
            return std::unexpected(Error{StatusCode::State, std::string_view{"queue empty"}});
        }
        T val = std::move(*buf_[rd_ % cap_]);
        buf_[rd_ % cap_].reset();
        ++rd_;
        return val;
    }

    /**
     * @brief Blocking pop that waits until an element arrives or stop requested.
     * @ingroup kanariv_utils
     * @param st Optional stop_token; if stop requested, returns nullopt.
     * @return `std::optional<T>` with value, or nullopt if stopped/empty.
     * @details Uses `cv_.wait(lk, st, predicate)` with predicate `wr_ != rd_`.
     * After wakeup, re-checks `stop_requested()` and `wr_ == rd_` before
     * extracting. Increments `rd_` and resets slot. Designed for service threads
     * that need cooperative cancellation via `std::stop_token` (e.g., writer thread,
     * decode thread). The stop_token is typically obtained from a `std::jthread`.
     */
    // Blocking pop with stop support (for service threads using jthread/stop_token)
    [[nodiscard]] std::optional<T> waitPop(std::stop_token st = {}) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, st, [this] { return wr_ != rd_; });
        if (st.stop_requested())
            return std::nullopt;
        if (wr_ == rd_)
            return std::nullopt;
        T val = std::move(*buf_[rd_ % cap_]);
        buf_[rd_ % cap_].reset();
        ++rd_;
        return val;
    }

  private:
    std::size_t cap_;                   ///< Fixed capacity (normalized).
    std::vector<std::optional<T>> buf_; ///< Circular storage [cap_].
    std::size_t wr_{0};                 ///< Monotonic write counter (mutex-protected).
    std::size_t rd_{0};                 ///< Monotonic read counter (mutex-protected).
    mutable std::mutex mutex_;          ///< Protects cap_/buf_/wr_/rd_.
    std::condition_variable_any cv_;    ///< Notified on push/emplace.
};

} // namespace kanariv::utils
