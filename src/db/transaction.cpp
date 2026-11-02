#include <sqlite3.h>

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <string>
#include <utility>

namespace kanariv::db {

DbTransaction::DbTransaction(sqlite3* db, bool committed) : db_(db), committed_(committed) {}

std::expected<DbTransaction, kanariv::utils::Error> DbTransaction::begin(sqlite3* db) {
    if (!db) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db")};
    }
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "BEGIN failed";
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
    }
    return DbTransaction{db, false};
}

DbTransaction::~DbTransaction() {
    if (db_ && !committed_) {
        char* err = nullptr;
        internal::SqliteErrGuard guard{err};
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
    }
}

DbTransaction::DbTransaction(DbTransaction&& o) noexcept
    : db_(std::exchange(o.db_, nullptr)), committed_(std::exchange(o.committed_, true)) {}

DbTransaction& DbTransaction::operator=(DbTransaction&& o) noexcept {
    if (this != &o) {
        if (db_ && !committed_) {
            char* err = nullptr;
            internal::SqliteErrGuard guard{err};
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
        }
        db_ = std::exchange(o.db_, nullptr);
        committed_ = std::exchange(o.committed_, true);
    }
    return *this;
}

std::expected<void, kanariv::utils::Error> DbTransaction::commit() {
    if (!db_) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        "no active DbTransaction"));
    }
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "commit failed";
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg));
    }
    committed_ = true;
    db_ = nullptr;
    return {};
}

std::expected<void, kanariv::utils::Error> DbTransaction::rollback() {
    if (!db_) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                        "no active DbTransaction"));
    }
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "rollback failed";
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg));
    }
    committed_ = true;
    db_ = nullptr;
    return {};
}

} // namespace kanariv::db
