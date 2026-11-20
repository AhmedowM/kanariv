#pragma once

/**
 * @file ring.hpp
 * @brief SPSC ring buffer with cache-line isolation and SPSC invariants.
 * @ingroup kanariv_utils
 * @note This header is fully inline/template; no .cpp is required.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

namespace kanariv::utils {

/**
 * @brief Cache-line size used to pad atomics against false sharing.
 * @ingroup kanariv_utils
 * @details Uses default 64 bytes (common x86-64 line size). Must be power-of-two.
 */
inline constexpr std::size_t ringCacheLine = 64UZ;
static_assert((ringCacheLine & (ringCacheLine - 1)) == 0, "ringCacheLine must be power-of-2");

/**
 * @brief Single-producer single-consumer ring buffer for trivially copyable types.
 * @ingroup kanariv_utils
 * @tparam T Element type; must be trivially copyable (memcpy-able).
 * @details SPSC invariants and atomics:
 * - `wr_` and `rd_` are monotonically increasing frame counters, cache-line
 *   padded (`alignas(ringCacheLine)`) to avoid false sharing.
 * - `write()` is producer-only: loads `wr_` relaxed (writer owns it), loads
 *   `rd_` acquire to observe consumer progress; stores `wr_` release.
 * - `read()` is consumer-only: loads `rd_` relaxed (reader owns it), loads
 *   `wr_` acquire; stores `rd_` release.
 * - `availableRead()` / `availableWrite()` use the same relaxed/acquire
 *   pairing for lock-free size queries.
 * - `read()` bumps `wake_` and notifies after committing, so a producer
 *   parked in `waitForRead()` (instead of polling for space) wakes on the
 *   consumer's progress. `wake_` is consumer-written, producer-read; the
 *   consumer's notify never takes `waitMtx_`, so the data callback stays
 *   lock-free.
 * - `reset()` requires external synchronization (caller must stop both
 *   producer and consumer or hold engine `decodeMtx_`); concurrent
 *   reset with read/write is a data race. No fence beyond release stores.
 * - Data movement uses `memcpy` with wrap-around; capacity is in frames.
 *
 * @par Thread safety
 * Exactly one producer may call `write()` and exactly one consumer may call
 * `read()`. Capacity queries may be called by either side, but the result is a
 * momentary observation rather than a reservation. Copying is disabled.
 * `waitForRead()`/`readSequence()` are producer-only.
 *
 * @par Lifetime
 * The ring owns its storage. Any spans or pointers passed to an operation must
 * remain valid for that call only; no caller-owned sample storage is retained.
 *
 * @see ringCacheLine
 */
template <typename T>
    requires std::is_trivially_copyable_v<T>
class SpscRing {
  public:
    /**
     * @brief Deleted default constructor; capacity must be specified.
     * @ingroup kanariv_utils
     */
    SpscRing() = delete;

    /**
     * @brief Constructs a ring with given capacity and channel count.
     * @ingroup kanariv_utils
     * @param capacityFrames Number of frames (capacity in frames, not samples).
     * @param channels Number of channels per frame (default 1).
     * @details `capacityFrames==0` yields an empty buffer where all ops are no-ops.
     */
    SpscRing(std::size_t capacityFrames, std::uint32_t channels = 1)
        : cap_(capacityFrames), channels_(channels), buf_(capacityFrames * channels) {
        if (cap_ > 0 && (cap_ & (cap_ - 1)) == 0)
            mask_ = cap_ - 1;
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /**
     * @brief Returns capacity in frames.
     * @ingroup kanariv_utils
     * @return Capacity passed at construction.
     */
    [[nodiscard]] std::size_t capacity() const noexcept {
        return cap_;
    }
    /**
     * @brief Returns channel count per frame.
     * @ingroup kanariv_utils
     * @return Channels per frame.
     */
    [[nodiscard]] std::uint32_t channels() const noexcept {
        return channels_;
    }

    /**
     * @brief Writes frames from a span (producer-only).
     * @ingroup kanariv_utils
     * @param data Span of interleaved samples (`size()` must be multiple of channels).
     * @return Number of frames actually written (may be < requested if ring full).
     * @details Computes frames = `data.size()/channels`; delegates to pointer overload.
     * No lock; relies on SPSC atomic protocol.
     */
    std::size_t write(std::span<const T> data) noexcept {
        if (data.empty() || cap_ == 0)
            return 0;
        std::size_t frames = data.size() / channels_;
        if (frames == 0)
            return 0;
        return write(data.data(), frames);
    }

    /**
     * @brief Writes frames from a pointer (producer-only).
     * @ingroup kanariv_utils
     * @param data Pointer to interleaved samples.
     * @param frames Number of frames to write.
     * @return Frames written (clamped to free space).
     * @details Atomics: `wr_` relaxed (owned), `rd_` acquire; `wr_` release on commit.
     * Handles wrap-around with two `memcpy` copies.
     */
    std::size_t write(const T* data, std::size_t frames) noexcept {
        if (!data || frames == 0 || cap_ == 0)
            return 0;
        std::size_t wr = wr_.load(std::memory_order_relaxed); // writer owns wr
        std::size_t rd = rd_.load(std::memory_order_acquire);
        std::size_t used = wr - rd;
        if (used > cap_)
            used = cap_;
        std::size_t free = cap_ - used;
        if (free == 0)
            return 0;
        if (frames > free)
            frames = free;
        std::size_t wrIdx = mask_ != 0 ? (wr & mask_) : (wr % cap_);
        std::size_t ch = static_cast<std::size_t>(channels_);
        std::size_t first = cap_ - wrIdx;
        if (first > frames)
            first = frames;
        // memcpy wrap like ca_ring.c:89
        std::memcpy(buf_.data() + wrIdx * ch, data, first * ch * sizeof(T));
        if (frames > first) {
            std::memcpy(buf_.data(), data + first * ch, (frames - first) * ch * sizeof(T));
        }
        wr_.store(wr + frames, std::memory_order_release);
        return frames;
    }

    /**
     * @brief Reads frames into a span (consumer-only).
     * @ingroup kanariv_utils
     * @param out Span to fill; `out.size()/channels` is max frames to read.
     * @return Frames actually read (0 if empty).
     * @details Delegates to pointer overload.
     */
    std::size_t read(std::span<T> out) noexcept {
        if (out.empty() || cap_ == 0)
            return 0;
        std::size_t frames = out.size() / channels_;
        if (frames == 0)
            return 0;
        return read(out.data(), frames);
    }

    /**
     * @brief Reads frames into a pointer (consumer-only).
     * @ingroup kanariv_utils
     * @param out Destination pointer for interleaved samples.
     * @param frames Max frames to read.
     * @return Frames read (clamped to available).
     * @details Atomics: `rd_` relaxed (owned), `wr_` acquire; `rd_` release on commit.
     * Handles wrap-around with two `memcpy` copies.
     */
    std::size_t read(T* out, std::size_t frames) noexcept {
        if (!out || frames == 0 || cap_ == 0)
            return 0;
        std::size_t rd = rd_.load(std::memory_order_relaxed); // reader owns rd
        std::size_t wr = wr_.load(std::memory_order_acquire);
        std::size_t avail = wr - rd;
        if (avail > cap_)
            avail = cap_;
        if (avail == 0)
            return 0;
        if (frames > avail)
            frames = avail;
        std::size_t rdIdx = mask_ != 0 ? (rd & mask_) : (rd % cap_);
        std::size_t ch = static_cast<std::size_t>(channels_);
        std::size_t first = cap_ - rdIdx;
        if (first > frames)
            first = frames;
        std::memcpy(out, buf_.data() + rdIdx * ch, first * ch * sizeof(T));
        if (frames > first) {
            std::memcpy(out + first * ch, buf_.data(), (frames - first) * ch * sizeof(T));
        }
        rd_.store(rd + frames, std::memory_order_release);
        // Consumer-side signal: the freed space (and the shortened tail) is
        // what a producer parked in waitForRead() is waiting for. The audio
        // callback only notifies; it never takes the wait mutex, so the
        // real-time path stays lock-free.
        wake_.fetch_add(1, std::memory_order_release);
        waitCv_.notify_one();
        return frames;
    }

    /**
     * @brief Returns frames available for reading (consumer view).
     * @ingroup kanariv_utils
     * @return Available frames (0..cap_).
     * @details `rd_` relaxed (owned), `wr_` acquire.
     */
    [[nodiscard]] std::size_t availableRead() const noexcept {
        // consumer view: rd relaxed (owned), wr acquire (sync with producer)
        std::size_t rd = rd_.load(std::memory_order_relaxed);
        std::size_t wr = wr_.load(std::memory_order_acquire);
        std::size_t avail = wr - rd;
        if (avail > cap_)
            avail = cap_;
        return avail;
    }

    /**
     * @brief Returns frames available for writing (producer view).
     * @ingroup kanariv_utils
     * @return Free frames (0..cap_).
     * @details `wr_` relaxed (owned), `rd_` acquire.
     */
    [[nodiscard]] std::size_t availableWrite() const noexcept {
        // producer view: wr relaxed (owned), rd acquire (sync with consumer)
        std::size_t wr = wr_.load(std::memory_order_relaxed);
        std::size_t rd = rd_.load(std::memory_order_acquire);
        std::size_t avail = wr - rd;
        if (avail > cap_)
            avail = cap_;
        return cap_ - avail;
    }

    /**
     * @brief Returns the consumer's read counter (producer view).
     * @ingroup kanariv_utils
     * @return Monotonic count of successful read() calls.
     * @details Sample it *before* checking availableWrite()/availableRead(),
     * then hand it to waitForRead(). The ordering makes any read that lands
     * after the sample visible to the wait, so the producer cannot miss a
     * wake-up by sampling late.
     */
    [[nodiscard]] std::uint64_t readSequence() const noexcept {
        return wake_.load(std::memory_order_acquire);
    }

    /**
     * @brief Blocks until the consumer reads after @p since, or times out.
     * @ingroup kanariv_utils
     * @param since Value returned by readSequence() before the space/tail check.
     * @param maxWait Upper bound on the wait; the caller re-checks its own
     * state afterwards. The bound only matters when the consumer stopped
     * without a state transition (device stall).
     * @return True when a read happened after @p since, false on timeout.
     * @details Producer-only. Replaces polling availableWrite() with the
     * consumer's signal; a state machine that stops consuming must still bound
     * the wait or wake the producer itself.
     * @par Thread safety
     * Must be called by the producer side only.
     */
    template <typename Rep, typename Period>
    bool waitForRead(std::uint64_t since, std::chrono::duration<Rep, Period> maxWait) {
        if (wake_.load(std::memory_order_acquire) != since)
            return true;
        std::unique_lock<std::mutex> lk(waitMtx_);
        const bool changed = waitCv_.wait_for(
            lk, maxWait, [this, since] { return wake_.load(std::memory_order_acquire) != since; });
        return changed;
    }

    /**
     * @brief Resets read and write counters to zero.
     * @ingroup kanariv_utils
     * @details Requires external synchronization: caller must ensure producer
     * and consumer are stopped/paused. The playback pipeline resets it only
     * after bumping its generation and parking the producer, so no in-flight
     * chunk can still write. No internal lock; concurrent reset with
     * read/write is a data race. Uses release stores; no fence needed as
     * caller guarantees quiescence.
     */
    // Requires external sync: producer & consumer stopped, or the owner's
    // generation bump has parked the producer (Pipeline::seek/open/stop).
    void reset() noexcept {
        // No fence needed: caller guarantees no concurrent read/write.
        rd_.store(0, std::memory_order_release);
        wr_.store(0, std::memory_order_release);
    }

  private:
    std::size_t cap_{0};        ///< Capacity in frames.
    std::uint32_t channels_{1}; ///< Channels per frame.
    std::vector<T> buf_{};      ///< Interleaved storage [cap_*channels].
    alignas(ringCacheLine) std::atomic<std::size_t> wr_{0}; ///< Producer index (monotonic).
    alignas(ringCacheLine) std::atomic<std::size_t> rd_{0}; ///< Consumer index (monotonic).
    /** Consumer read counter for waitForRead(); never reset (monotonic). */
    alignas(ringCacheLine) std::atomic<std::uint64_t> wake_{0};
    /// Producer-side wait pair; read() notifies without taking waitMtx_.
    std::mutex waitMtx_;
    std::condition_variable waitCv_;
    std::size_t mask_{0}; ///< cap_-1 when cap_ is a power of two, else 0 (use modulo).
};

} // namespace kanariv::utils
