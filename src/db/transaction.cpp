#include <sqlite3.h>

#include <db/stmt_helpers.hpp>
#include <db/transaction.hpp>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <utility>

namespace kanariv::db {

DbTransaction::DbTransaction(sqlite3* db, bool committed) : db_(db), committed_(committed) {}

auto DbTransaction::begin(sqlite3* db) -> std::expected<DbTransaction, kanariv::utils::Error> {
    if (db == nullptr) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "null db")};
    }
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err != nullptr ? err : "BEGIN failed";
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
    }
    return DbTransaction{db, false};
}

DbTransaction::~DbTransaction() {
    if (db_ != nullptr && !committed_) {
        char* err = nullptr;
        internal::SqliteErrGuard guard{err};
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
    }
}

DbTransaction::DbTransaction(DbTransaction&& o) noexcept
    : db_(std::exchange(o.db_, nullptr)), committed_(std::exchange(o.committed_, true)) {}

auto DbTransaction::operator=(DbTransaction&& o) noexcept -> DbTransaction& {
    if (this != &o) {
        if (db_ != nullptr && !committed_) {
            char* err = nullptr;
            internal::SqliteErrGuard guard{err};
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
        }
        db_ = std::exchange(o.db_, nullptr);
        committed_ = std::exchange(o.committed_, true);
    }
    return *this;
}

auto DbTransaction::commit() -> std::expected<void, kanariv::utils::Error> {
    if (db_ == nullptr) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "no active DbTransaction"));
    }
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err != nullptr ? err : "commit failed";
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg));
    }
    committed_ = true;
    db_ = nullptr;
    return {};
}

auto DbTransaction::rollback() -> std::expected<void, kanariv::utils::Error> {
    if (db_ == nullptr) {
        return std::unexpected(kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "no active DbTransaction"));
    }
    char* err = nullptr;
    internal::SqliteErrGuard guard{err};
    int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err != nullptr ? err : "rollback failed";
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg));
    }
    committed_ = true;
    db_ = nullptr;
    return {};
}

} // namespace kanariv::db
