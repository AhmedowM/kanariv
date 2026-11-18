/**
 * @file queue_repo.cpp
 * @brief Queue facade over the position-based queue helpers.
 * @ingroup kanariv_db
 */

#include <cstddef>
#include <cstdint>
#include <db/queue.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <kanariv/db/database.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db_impl.hpp"

namespace kanariv::db {

kanariv::utils::Expected<void> Database::queueEnqueueBatch(int64_t qid,
                                                           std::span<const int64_t> tids) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = kanariv::db::DbTransaction::begin(impl_->db_.get());
    if (!txRes)
        return std::unexpected{txRes.error()};
    auto tx = std::move(*txRes);
    for (int64_t tid : tids) {
        auto r = kanariv::db::queueEnqueueLocked(impl_->db_.get(), qid, tid, -1);
        if (!r)
            return std::unexpected{r.error()};
    }
    return tx.commit();
}

kanariv::utils::Expected<void> Database::queueEnqueue(int64_t qid, int64_t tid, int64_t index) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->queueEnqueueLocked(qid, tid, index);
}

kanariv::utils::Expected<void> Database::Impl::queueEnqueueLocked(int64_t qid, int64_t tid,
                                                                  int64_t index) {
    return kanariv::db::queueEnqueueLocked(db_.get(), qid, tid, index);
}

kanariv::utils::Expected<kanariv::types::QueueItem> Database::queueDequeue(int64_t qid) {
    std::unique_lock lk{impl_->dbMutex_};
    return impl_->queueDequeueLocked(qid);
}

kanariv::utils::Expected<kanariv::types::QueueItem>
Database::Impl::queueDequeueLocked(int64_t qid) {
    return kanariv::db::queueDequeueLocked(db_.get(), qid);
}

kanariv::utils::Expected<void> Database::queueRemove(int64_t qid, int64_t index) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueRemoveLocked(impl_->db_.get(), qid, index);
}

kanariv::utils::Expected<void> Database::queueMove(int64_t qid, std::size_t from, std::size_t to) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = kanariv::db::DbTransaction::begin(impl_->db_.get());
    if (!txRes)
        return std::unexpected{txRes.error()};
    auto tx = std::move(*txRes);
    auto moved = kanariv::db::queueMoveLocked(impl_->db_.get(), qid, from, to);
    if (!moved)
        return std::unexpected{moved.error()};
    return tx.commit();
}

kanariv::utils::Expected<void> Database::queueClear(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueClearLocked(impl_->db_.get(), qid);
}

kanariv::utils::Expected<std::vector<kanariv::types::QueueItem>>
Database::listQueueItems(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueListLocked(impl_->db_.get(), qid);
}

kanariv::utils::Expected<std::vector<kanariv::types::TrackRow>>
Database::tracksForQueue(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::queueTracksLocked(impl_->db_.get(), qid);
}

kanariv::utils::Expected<kanariv::types::Queue> Database::getQueue(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::getQueueLocked(impl_->db_.get(), qid);
}

kanariv::utils::Expected<std::vector<kanariv::types::Queue>> Database::listQueues() {
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::listQueuesLocked(impl_->db_.get());
}

kanariv::utils::Expected<int64_t> Database::createQueue(std::string_view name, int64_t library_id) {
    if (name.empty())
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg)};
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::createQueueLocked(impl_->db_.get(), name, library_id);
}

kanariv::utils::Expected<void> Database::deleteQueue(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::deleteQueueLocked(impl_->db_.get(), qid);
}

kanariv::utils::Expected<void> Database::setQueueRepeat(int64_t qid,
                                                        kanariv::types::RepeatMode mode) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::setQueueRepeatLocked(impl_->db_.get(), qid, static_cast<int>(mode));
}

kanariv::utils::Expected<void> Database::setQueueTemp(int64_t qid, bool temp) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    return kanariv::db::setQueueTempLocked(impl_->db_.get(), qid, temp);
}

kanariv::utils::Expected<std::size_t> Database::queueCount(int64_t qid) {
    qid = kanariv::db::normalizeQueueId(qid);
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    return impl_->queueCountLocked(qid);
}

std::size_t Database::Impl::queueCountLocked(int64_t qid) {
    return kanariv::db::queueCountLocked(db_.get(), qid);
}

kanariv::utils::Expected<std::optional<size_t>> Database::queueIndexOf(int64_t qid,
                                                                       int64_t trackId) {
    qid = kanariv::db::normalizeQueueId(qid);
    if (trackId == 0)
        return std::nullopt;
    std::unique_lock lk{impl_->dbMutex_};
    if (!impl_->db_)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    return kanariv::db::queueIndexOfLocked(impl_->db_.get(), qid, trackId);
}
} // namespace kanariv::db
