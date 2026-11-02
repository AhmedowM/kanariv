#include <sqlite3.h>

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <cstdint>
#include <db/statement.hpp>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace kanariv::db {

SqliteStatement::~SqliteStatement() {
    if (stmt_)
        sqlite3_finalize(stmt_);
}

SqliteStatement::SqliteStatement(SqliteStatement&& o) noexcept : stmt_(o.stmt_) {
    o.stmt_ = nullptr;
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& o) noexcept {
    if (this != &o) {
        if (stmt_)
            sqlite3_finalize(stmt_);
        stmt_ = o.stmt_;
        o.stmt_ = nullptr;
    }
    return *this;
}

std::expected<void, kanariv::utils::Error> SqliteStatement::prepare(sqlite3* db,
                                                                   std::string_view sql) {
    if (stmt_)
        sqlite3_finalize(stmt_);
    stmt_ = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "prepare failed";
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
    }
    return {};
}

void SqliteStatement::bindInt(int idx, int64_t v) {
    if (stmt_)
        sqlite3_bind_int64(stmt_, idx, v);
}

void SqliteStatement::bindDouble(int idx, double v) {
    if (stmt_)
        sqlite3_bind_double(stmt_, idx, v);
}

void SqliteStatement::bindText(int idx, std::string_view v) {
    if (!stmt_)
        return;
    if (v.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return;
    sqlite3_bind_text(stmt_, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

void SqliteStatement::bindBlob(int idx, std::span<const std::byte> data) {
    if (stmt_)
        sqlite3_bind_blob(stmt_, idx, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
}

void SqliteStatement::bindNull(int idx) {
    if (stmt_)
        sqlite3_bind_null(stmt_, idx);
}

bool SqliteStatement::step() {
    if (!stmt_)
        return false;
    int rc = sqlite3_step(stmt_);
    return rc == SQLITE_ROW;
}

int SqliteStatement::stepDone() {
    if (!stmt_)
        return SQLITE_ERROR;
    return sqlite3_step(stmt_);
}

int64_t SqliteStatement::columnInt(int idx) const {
    return stmt_ ? sqlite3_column_int64(stmt_, idx) : 0;
}

double SqliteStatement::columnDouble(int idx) const {
    return stmt_ ? sqlite3_column_double(stmt_, idx) : 0.0;
}

std::string SqliteStatement::columnText(int idx) const {
    if (!stmt_)
        return {};
    const unsigned char* txt = sqlite3_column_text(stmt_, idx);
    return txt ? std::string(reinterpret_cast<const char*>(txt)) : std::string{};
}

void SqliteStatement::reset() {
    if (stmt_) {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
}

sqlite3_stmt* SqliteStatement::get() const noexcept {
    return stmt_;
}

} // namespace kanariv::db
