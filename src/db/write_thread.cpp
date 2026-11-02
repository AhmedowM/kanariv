#include <sqlite3.h>

#include <atomic>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <db/statement.hpp>
#include <db/stmt_helpers.hpp>
#include <db/write_thread.hpp>
#include <expected>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <utils/function.hpp>
#include <utils/mpsc_queue.hpp>

namespace kanariv::db {

// Private implementation: all heavyweight members (queue, worker thread,
// synchronization) live here so the internal header stays include-light.
struct WriterThread::Impl {
    explicit Impl(std::size_t writeBatchSize)
        : queue_(std::make_unique<kanariv::utils::MpscQueue<WriteOperation>>(
              writeBatchSize > 0 ? writeBatchSize : 256)) {}

    void worker(std::stop_token st);

    std::unique_ptr<kanariv::utils::MpscQueue<WriteOperation>> queue_;
    sqlite3* db_{nullptr};
    std::jthread thread_{};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<int> in_flight_{0};
};

// Out-of-line: the header only forward-declares SqliteStatement.
WriteOperation::WriteOperation() = default;

WriteOperation::WriteOperation(
    std::string s, std::unique_ptr<SqliteStatement> st,
    kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)> c)
    : sql(std::move(s)), stmt(std::move(st)), cb(std::move(c)) {}

WriteOperation::WriteOperation(WriteOperation&&) noexcept = default;
WriteOperation& WriteOperation::operator=(WriteOperation&&) noexcept = default;
WriteOperation::~WriteOperation() = default;

WriterThread::WriterThread(std::size_t writeBatchSize)
    : impl_(std::make_unique<Impl>(writeBatchSize)) {}

WriterThread::~WriterThread() {
    close();
}

// Move leaves the source empty (null impl_); its accessors are null-safe.
WriterThread::WriterThread(WriterThread&& o) noexcept : impl_(std::move(o.impl_)) {}

WriterThread& WriterThread::operator=(WriterThread&& o) noexcept {
    if (this != &o)
        impl_ = std::move(o.impl_);
    return *this;
}

void WriterThread::open(sqlite3* db) {
    if (impl_->thread_.joinable())
        return;
    impl_->db_ = db;
    impl_->thread_ = std::jthread{[p = impl_.get()](std::stop_token st) { p->worker(st); }};
}

void WriterThread::close() {
    if (!impl_)
        return;
    if (impl_->thread_.joinable()) {
        impl_->thread_.request_stop();
        impl_->cv_.notify_all();
        impl_->thread_.join();
    }
    while (true) {
        auto v = impl_->queue_->pop();
        if (!v)
            break;
    }
}

std::expected<void, kanariv::utils::Error> WriterThread::push(
    std::string sql, std::unique_ptr<SqliteStatement> stmt,
    kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)> cb) {
    WriteOperation op{std::move(sql), std::move(stmt), std::move(cb)};
    auto r = impl_->queue_->push(std::move(op));
    if (!r)
        return std::unexpected{r.error()};
    {
        std::scoped_lock lk(impl_->mtx_);
        impl_->cv_.notify_one();
    }
    return {};
}

std::expected<void, kanariv::utils::Error> WriterThread::push(
    std::string sql,
    kanariv::utils::MoveOnlyFunction<void(std::expected<void, kanariv::utils::Error>)> cb) {
    return push(std::move(sql), std::unique_ptr<SqliteStatement>{}, std::move(cb));
}

std::expected<void, kanariv::utils::Error> WriterThread::flush() {
    std::unique_lock<std::mutex> lk(impl_->mtx_);
    bool done = impl_->cv_.wait_for(lk, std::chrono::milliseconds{200}, [this] {
        return impl_->queue_->empty() && impl_->in_flight_.load(std::memory_order_acquire) == 0;
    });
    if (done)
        return {};
    if (impl_->queue_->empty() && impl_->in_flight_.load(std::memory_order_acquire) == 0)
        return {};
    return std::unexpected{
        kanariv::utils::makeError(kanariv::utils::StatusCode::Busy, "flush timeout")};
}

bool WriterThread::empty() const {
    return !impl_ || impl_->queue_->empty();
}

std::size_t WriterThread::size() const {
    return impl_ ? impl_->queue_->size() : 0;
}

void WriterThread::Impl::worker(std::stop_token st) {
    while (!st.stop_requested()) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds{200},
                     [this, &st] { return !queue_->empty() || st.stop_requested(); });
        if (st.stop_requested() && queue_->empty())
            break;
        lk.unlock();
        auto popped = queue_->pop();
        if (!popped.has_value()) {
            if (st.stop_requested())
                break;
            continue;
        }
        WriteOperation op = std::move(popped.value());
        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        kanariv::utils::Error err{};
        bool ok = true;
        if (op.stmt) {
            int rc = op.stmt->stepDone();
            if (rc != SQLITE_DONE && rc != SQLITE_ROW && rc != SQLITE_OK) {
                err = kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                               sqlite3_errmsg(db_));
                ok = false;
            }
        } else if (!op.sql.empty() && db_) {
            char* e = nullptr;
            internal::SqliteErrGuard guard{e};
            int rc = sqlite3_exec(db_, op.sql.c_str(), nullptr, nullptr, &e);
            if (rc != SQLITE_OK) {
                err = kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                               e ? e : sqlite3_errmsg(db_));
                ok = false;
            }
        }
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> notifyLk(mtx_);
            cv_.notify_all();
        }
        if (op.cb) {
            if (ok)
                op.cb(std::expected<void, kanariv::utils::Error>{});
            else
                op.cb(std::unexpected{err});
        }
    }
}

} // namespace kanariv::db
