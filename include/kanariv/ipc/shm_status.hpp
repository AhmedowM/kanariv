#pragma once

/**
 * @file shm_status.hpp
 * @brief Seqlock shared-memory playback snapshot (daemon writes, clients poll).
 * @ingroup kanariv_ipc
 * @details Lets clients read the latest event-driven
 * position/state snapshot without an IPC round-trip. The daemon publishes
 * on dispatch only (no periodic writer), so position can go stale between
 * commands -- use the IPC status path when freshness matters. Writers publish
 * (lock-free atomics + seqlock sequence); readers get a plain `ShmStatus`
 * copy.
 *
 * This is the shared set of types; the writer takes plain data via
 * `ShmStatusUpdate` so `ipc` never depends on `engine::Engine` (the daemon
 * extracts the fields). Windows pattern as in the internal
 * `service/ipc_server.hpp`: local API declarations unless `windows.h` is
 * already included.
 *
 * @section shm_contract Layout and publication contract (layout v2 -- BREAKING)
 *
 * The segment is one `AtomicShmStatus` (see below), mapped read/write by the
 * single daemon and read-only by clients. Version 2 changes the publication
 * contract in three observable ways, so readers and writers must be rebuilt
 * together:
 *
 * 1. `duration` and `queueSize` are part of `ShmStatusUpdate` and are written
 *    inside the seqlock critical section (previously separate setters could
 *    interleave with `publish`, producing torn `position`/`duration` pairs).
 * 2. `title`/`artist` are published byte-wise through `std::atomic_ref`
 *    (previously a plain `memcpy` raced with the writer). Every publish
 *    rewrites the full buffer, so an empty view clears the previous string
 *    instead of leaving stale metadata behind.
 * 3. `ShmStatus::stale` reports that a reader gave up after a bounded number
 *    of seqlock retries (a crashed writer leaves `seq` odd forever). The
 *    snapshot is then default-initialised rather than spinning at 100% CPU.
 *
 * Writer protocol: store an odd `seq`, publish a full fence, write all fields
 * (relaxed atomics, byte-wise for strings), publish a release fence, store the
 * next even `seq`. Reader protocol: acquire-load an even `seq`, acquire fence,
 * read all fields, acquire fence, re-read `seq` and retry on mismatch.
 */

#include <atomic>
#include <bit>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace kanariv::ipc {

/** @brief Internal white-box accessor; defined in `src/ipc/shm_status_access.hpp`. */
struct ShmStatusAccess;

/**
 * @brief Plain snapshot copy (no atomics); what readers take home.
 * @ingroup kanariv_ipc
 * @details Strings are fixed `char` arrays (atomics can't hold them); writers
 * publish them byte-wise through `std::atomic_ref` under an odd seqlock
 * sequence, readers retry a bounded number of times on mismatch.
 */
struct KANARIV_API ShmStatus {
    uint64_t seq{0};      ///< Seqlock sequence; odd while writing, even when stable.
    double position{0.0}; ///< Current playback position in seconds.
    double duration{0.0}; ///< Current track duration in seconds.
    float volume{1.0f};   ///< Volume in the range [0, 1].
    bool muted{false};    ///< Whether output is muted.
    int state{0};         ///< `PlaybackState` value stored as an integer.
    int64_t track_id{0};  ///< Current track identifier, or zero when absent.
    size_t queueSize{0};  ///< Number of items in the active queue.
    char title[256]{0};   ///< Null-terminated current track title (empty when cleared).
    char artist[256]{0};  ///< Null-terminated current track artist (empty when cleared).
    /**
     * @brief True when the reader exhausted its retry budget.
     * @details Signals a crashed or wedged writer (`seq` never returned to
     * even) or pathological contention. All other fields are
     * default-initialised in that case, so callers should fall back to the IPC
     * status path.
     */
    bool stale{false};
};

/**
 * @brief Plain data handed to `ShmStatusWriter::publish` by the daemon.
 * @ingroup kanariv_ipc
 * @details Decouples the writer from `engine::Engine` (the daemon extracts
 * these fields; `ipc` never sees the engine). `title`/`artist` are views into
 * caller-owned storage, copied into the fixed arrays under the seqlock. Empty
 * views clear the shared string (no stale metadata survives a stop/track
 * change).
 */
struct KANARIV_API ShmStatusUpdate final {
    double position{0.0};    ///< Current playback position in seconds.
    double duration{0.0};    ///< Current track duration in seconds (zero when absent).
    float volume{1.0f};      ///< Volume in the range [0, 1].
    bool muted{false};       ///< Whether output is muted.
    int state{0};            ///< `PlaybackState` value stored as an integer.
    int64_t track_id{0};     ///< Current track identifier, or zero when absent.
    size_t queueSize{0};     ///< Number of items in the active queue.
    std::string_view title;  ///< Current track title; empty clears the buffer.
    std::string_view artist; ///< Current track artist; empty clears the buffer.
};

/**
 * @brief Lock-free shared-memory view (`bit_cast` doubles/floats through integers).
 * @ingroup kanariv_ipc
 * @details Cache-line aligned; fits one page (static_assert below).
 *
 * Layout v2 (BREAKING -- see the file-level contract):
 * - `seq` is the seqlock word: odd while `publish()` is writing, even when the
 *   rest of the segment is stable.
 * - Every scalar field is an atomic; `position`/`duration` are `double` and
 *   `volume` is `float` bit-cast through their integer atomics.
 * - `title`/`artist` are plain `char[256]` regions. They are *not* read or
 *   written directly: both sides use `std::atomic_ref<char>` byte-wise, which
 *   keeps the copies race-free while the seqlock rejects torn snapshots.
 * - A writer creates/resets the segment exclusively, so a fresh segment is all
 *   zeroes (`seq == 0`, empty strings) and never inherits a crashed writer's
 *   odd `seq`.
 */
struct KANARIV_API alignas(64) AtomicShmStatus {
    std::atomic<uint64_t> seq{0};      ///< Seqlock sequence (odd = write in progress).
    std::atomic<uint64_t> position{0}; ///< `bit_cast` of the position `double`.
    std::atomic<uint64_t> duration{0}; ///< `bit_cast` of the duration `double`.
    std::atomic<uint32_t> volume{0};   ///< `bit_cast` of the volume `float`.
    std::atomic<bool> muted{false};    ///< Muted flag.
    std::atomic<int> state{0};         ///< `PlaybackState` value.
    std::atomic<int64_t> track_id{0};  ///< Current track id, zero when absent.
    std::atomic<size_t> queueSize{0};  ///< Items in the active queue.
    char title[256]{0};                ///< Seqlock-protected, published via `atomic_ref`.
    char artist[256]{0};               ///< Seqlock-protected, published via `atomic_ref`.
};

static_assert(sizeof(AtomicShmStatus) <= 4096, "AtomicShmStatus should fit in one page");

/**
 * @brief Loads a double from an atomic bit representation.
 * @param a Atomic storage to read.
 * @return Reconstructed double value.
 * @ingroup kanariv_ipc
 */
inline double atomicLoadDouble(const std::atomic<uint64_t>& a) noexcept {
    return std::bit_cast<double>(a.load(std::memory_order_acquire));
}
/**
 * @brief Stores a double in an atomic bit representation.
 * @param a Atomic storage to update.
 * @param v Value to store.
 * @par Thread safety
 * The caller must provide synchronization with readers according to the
 * surrounding seqlock protocol.
 * @ingroup kanariv_ipc
 */
inline void atomicStoreDouble(std::atomic<uint64_t>& a, double v) noexcept {
    a.store(std::bit_cast<uint64_t>(v), std::memory_order_relaxed);
}
/**
 * @brief Loads a float from an atomic bit representation.
 * @param a Atomic storage to read.
 * @return Reconstructed float value.
 * @ingroup kanariv_ipc
 */
inline float atomicLoadFloat(const std::atomic<uint32_t>& a) noexcept {
    return std::bit_cast<float>(a.load(std::memory_order_acquire));
}
/**
 * @brief Stores a float in an atomic bit representation.
 * @param a Atomic storage to update.
 * @param v Value to store.
 * @ingroup kanariv_ipc
 */
inline void atomicStoreFloat(std::atomic<uint32_t>& a, float v) noexcept {
    a.store(std::bit_cast<uint32_t>(v), std::memory_order_relaxed);
}

/**
 * @brief Client-side read view of the shared-memory status segment.
 * @ingroup kanariv_ipc
 * @details Exposes only snapshot reads.
 */
class KANARIV_API ShmStatusReader {
  public:
    /** @brief Result of opening a segment. */
    using ExpectedShm = std::expected<ShmStatusReader, kanariv::utils::Error>;

    /**
     * @brief Opens an existing segment read-only.
     * @ingroup kanariv_ipc
     * @param name Platform segment name (derived from the database path).
     * @return Reader, or an `Io` error when the segment is absent or smaller
     * than the current layout (incompatible stale segment).
     */
    static ExpectedShm openReadOnly(const std::string& name);

    /** @brief Unmaps and closes (idempotent). */
    ~ShmStatusReader();

    ShmStatusReader(const ShmStatusReader&) = delete;
    ShmStatusReader& operator=(const ShmStatusReader&) = delete;

    /** @brief Move-constructs, transferring the mapping. */
    ShmStatusReader(ShmStatusReader&& other) noexcept;
    /** @brief Move-assigns, transferring the mapping. */
    ShmStatusReader& operator=(ShmStatusReader&& other) noexcept;

    /** @brief True when the mapping is live. */
    bool valid() const noexcept;
    /** @brief Segment name. */
    const std::string& name() const noexcept;
    /**
     * @brief Seqlock reader: returns a snapshot copy of the status.
     * @return Consistent snapshot, or a zero-initialised snapshot with
     * `stale == true` when the writer did not release the sequence within the
     * bounded retry budget (crashed/wedged writer). Never spins unbounded.
     */
    ShmStatus snapshot() const;

  private:
    /** @brief Private default ctor; use openReadOnly(). */
    ShmStatusReader() = default;

    friend struct ShmStatusAccess;

    void close() noexcept;

    AtomicShmStatus* map_{nullptr};
    std::size_t size_{0};
    std::string name_;
    std::intptr_t handle_{-1};
};

/**
 * @brief Daemon-side write view of the shared-memory status segment.
 * @ingroup kanariv_ipc
 * @details Exposes only publishing.
 */
class KANARIV_API ShmStatusWriter {
  public:
    /** @brief Result of creating a segment. */
    using ExpectedShm = std::expected<ShmStatusWriter, kanariv::utils::Error>;

    /**
     * @brief Creates the segment by name, exclusively, and resets it.
     * @ingroup kanariv_ipc
     * @param name Platform segment name (derived from the database path).
     * @details A stale segment left by a crashed daemon is replaced (POSIX
     * `O_CREAT | O_EXCL`, `EEXIST` -> unlink and recreate; Windows
     * `ERROR_ALREADY_EXISTS` -> reset the header). The initialized segment has
     * `seq == 0` and empty strings, so a reader can never adopt an odd
     * sequence or old metadata.
     */
    static ExpectedShm create(const std::string& name);

    /** @brief Unmaps and closes (idempotent). */
    ~ShmStatusWriter();

    ShmStatusWriter(const ShmStatusWriter&) = delete;
    ShmStatusWriter& operator=(const ShmStatusWriter&) = delete;

    /** @brief Move-constructs, transferring the mapping. */
    ShmStatusWriter(ShmStatusWriter&& other) noexcept;
    /** @brief Move-assigns, transferring the mapping. */
    ShmStatusWriter& operator=(ShmStatusWriter&& other) noexcept;

    /** @brief True when the mapping is live. */
    bool valid() const noexcept;
    /** @brief Segment name. */
    const std::string& name() const noexcept;

    /**
     * @brief Seqlock writer: publishes the complete status in one critical section.
     * @param u Plain data; no `engine::Engine` needed at this layer.
     * @details Writes position/duration/volume/muted/state/track id/queue size
     * and both strings. Strings are always rewritten (an empty view clears the
     * buffer), so readers never observe metadata from a previous track. The
     * previous separate `setDuration`/`setQueueSize` setters were removed
     * because they bypassed the seqlock.
     */
    void publish(const ShmStatusUpdate& u) noexcept;

  private:
    /** @brief Private default ctor; use create(). */
    ShmStatusWriter() = default;

    friend struct ShmStatusAccess;

    void close() noexcept;

    AtomicShmStatus* map_{nullptr};
    std::size_t size_{0};
    std::string name_;
    bool create_{false};
    std::intptr_t handle_{-1};
};

} // namespace kanariv::ipc
