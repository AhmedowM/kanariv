#pragma once

/**
 * @file write_thread.hpp
 * @brief Background write batching for SQLite.
 * @ingroup kanariv_db
 * @details Single-consumer background thread that serializes
 * `WriteOperation`s queued via an `MpscQueue<WriteOperation>`. Batch size
 * is set at construction (`writeBatchSize`, default 256) and forwarded to the
 * queue capacity. The queue is bounded: `push()` returns `Busy` when full.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <utils/function.hpp>

namespace kanariv::db {

/**
 * @brief Single write operation submitted to `WriterThread`.
 * @ingroup kanariv_db
 * @details Either `stmt` or `sql` is executed; `cb` is invoked outside
 * the worker lock after execution with success or `Corrupt` on failure.
 */
struct WriteOperation {
    std::string sql;                       ///< Raw SQL (used when `stmt` is null).
    std::unique_ptr<SqliteStatement> stmt; ///< Prepared statement to step.
    kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)>
        cb; ///< Completion callback.

    // All special members are out-of-line (write_thread.cpp): the header only
    // forward-declares SqliteStatement, which is insufficient for inline
    // construction/destruction of the unique_ptr member.
    WriteOperation();
    /**
     * @brief Constructs a write operation.
     * @ingroup kanariv_db
     * @param s SQL text.
     * @param st Prepared statement (may be null if using `s`).
     * @param c Completion callback (may be empty).
     */
    WriteOperation(
        std::string s, std::unique_ptr<SqliteStatement> st,
        kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)> c);
    WriteOperation(const WriteOperation&) = delete;
    WriteOperation& operator=(const WriteOperation&) = delete;
    WriteOperation(WriteOperation&&) noexcept;
    WriteOperation& operator=(WriteOperation&&) noexcept;
    ~WriteOperation();
};

/**
 * @brief Background writer that serializes SQLite writes off the hot path.
 * @ingroup kanariv_db
 * @details Thread-safety and batching:
 * - `queue_` is a bounded `MpscQueue<WriteOperation>` with capacity
 *   `writeBatchSize` (default 256, clamped to >=1). `push()` fails with
 *   `Busy` when full.
 * - `db_` is a raw `sqlite3*` borrowed from `Database`; set in `open()` and
 *   cleared on move/close. No ownership.
 * - `thread_` is a `std::jthread` running `worker()` with a `stop_token`.
 * - `mtx_` + `cv_` coordinate `flush()` and the worker's wait. `in_flight_`
 *   counts ops currently being executed (outside the lock).
 * - Worker pops one op at a time (not batched under a single transaction in
 *   the current implementation; batching is at the queue-capacity level).
 * - `close()` requests stop, notifies, joins, then drains remaining ops.
 * - Move operations transfer queue/db/thread/in_flight; source is left empty.
 */
class WriterThread final {
  public:
    /**
     * @brief Constructs the writer with given batch/queue capacity.
     * @ingroup kanariv_db
     * @param writeBatchSize Queue capacity; 0 is normalized to 256.
     */
    explicit WriterThread(std::size_t writeBatchSize = 256);

    /** @brief Closes the thread and drains the queue. @ingroup kanariv_db */
    ~WriterThread();

    WriterThread(const WriterThread&) = delete;
    WriterThread& operator=(const WriterThread&) = delete;

    /**
     * @brief Move-constructs, transferring queue/db/thread/in_flight.
     * @ingroup kanariv_db
     * @param o Source; left with null db and empty thread.
     */
    WriterThread(WriterThread&& o) noexcept;

    /**
     * @brief Move-assigns; closes current thread first.
     * @ingroup kanariv_db
     * @param o Source.
     * @return *this
     */
    WriterThread& operator=(WriterThread&& o) noexcept;

    /**
     * @brief Starts the background thread for the given handle.
     * @ingroup kanariv_db
     * @param db Borrowed sqlite3 handle (must outlive the thread).
     * @details No-op if already running (`thread_.joinable()`).
     * @par Thread safety
     * Not thread-safe with concurrent `open`/`close`; caller must serialize.
     */
    void open(sqlite3* db);

    /**
     * @brief Stops the thread and drains remaining ops.
     * @ingroup kanariv_db
     * @details Requests stop, notifies `cv_`, joins. Remaining queued ops
     * are popped and destroyed (their statements finalized).
     */
    void close();

    /**
     * @brief Enqueues a write operation.
     * @ingroup kanariv_db
     * @param sql SQL text (used if `stmt` is null).
     * @param stmt Prepared statement to execute (may be null).
     * @param cb Completion callback invoked after execution.
     * @return Success or `Error` with `StatusCode::Busy` if queue is full.
     * @par Thread safety
     * Thread-safe (MPSC); multiple producers may call concurrently.
     */
    std::expected<void, kanariv::utils::Error>
    push(std::string sql, std::unique_ptr<SqliteStatement> stmt,
         kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)> cb);
    /**
     * @brief Enqueues a raw-SQL write operation (no prepared statement).
     * @ingroup kanariv_db
     * @param sql SQL text to execute.
     * @param cb Completion callback invoked after execution.
     * @return Success or `Error` with `StatusCode::Busy` if queue is full.
     * @details Convenience overload: passing (or moving) a statement needs
     * the complete `SqliteStatement` type, which is src-private. Most
     * callers — including all downstream users — want this overload.
     * @par Thread safety
     * Thread-safe (MPSC); multiple producers may call concurrently.
     */
    std::expected<void, kanariv::utils::Error>
    push(std::string sql,
         kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)> cb);

    /**
     * @brief Waits until the queue is empty and no op is in flight.
     * @ingroup kanariv_db
     * @return Success, or `Error` with `StatusCode::Busy` on 200 ms timeout.
     * @details Waits on `cv_` with predicate `queue_->empty() && in_flight_ == 0`.
     * Re-checks predicate after timeout to avoid spurious failure.
     * @par Thread safety
     * Thread-safe; blocks caller.
     */
    std::expected<void, kanariv::utils::Error> flush();

    /**
     * @brief Checks if the queue is empty.
     * @ingroup kanariv_db
     * @return true if no pending ops.
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief Returns number of pending ops.
     * @ingroup kanariv_db
     * @return Queue size.
     */
    [[nodiscard]] std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::db
