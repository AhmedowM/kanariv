#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <db/statement.hpp>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace kanariv::db {

SqliteStatement::~SqliteStatement() {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
}

SqliteStatement::SqliteStatement(SqliteStatement&& o) noexcept : stmt_(o.stmt_) {
    o.stmt_ = nullptr;
}

auto SqliteStatement::operator=(SqliteStatement&& o) noexcept -> SqliteStatement& {
    if (this != &o) {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
        stmt_ = o.stmt_;
        o.stmt_ = nullptr;
    }
    return *this;
}

auto SqliteStatement::prepare(sqlite3* db, std::string_view sql)
    -> std::expected<void, kanariv::utils::Error> {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
    }
    stmt_ = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db != nullptr ? sqlite3_errmsg(db) : "prepare failed";
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
    }
    return {};
}

void SqliteStatement::bindInt(int idx, int64_t v) {
    if (stmt_ != nullptr) {
        sqlite3_bind_int64(stmt_, idx, v);
    }
}

void SqliteStatement::bindDouble(int idx, double v) {
    if (stmt_ != nullptr) {
        sqlite3_bind_double(stmt_, idx, v);
    }
}

void SqliteStatement::bindText(int idx, std::string_view v) {
    if (stmt_ == nullptr) {
        return;
    }
    if (v.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return;
    }
    sqlite3_bind_text(stmt_, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

void SqliteStatement::bindBlob(int idx, std::span<const std::byte> data) {
    if (stmt_ != nullptr) {
        sqlite3_bind_blob(stmt_, idx, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
    }
}

void SqliteStatement::bindNull(int idx) {
    if (stmt_ != nullptr) {
        sqlite3_bind_null(stmt_, idx);
    }
}

auto SqliteStatement::step() -> bool {
    if (stmt_ == nullptr) {
        return false;
    }
    int rc = sqlite3_step(stmt_);
    return rc == SQLITE_ROW;
}

auto SqliteStatement::stepDone() -> int {
    if (stmt_ == nullptr) {
        return SQLITE_ERROR;
    }
    return sqlite3_step(stmt_);
}

auto SqliteStatement::columnInt(int idx) const -> int64_t {
    return stmt_ != nullptr ? sqlite3_column_int64(stmt_, idx) : 0;
}

auto SqliteStatement::columnDouble(int idx) const -> double {
    return stmt_ != nullptr ? sqlite3_column_double(stmt_, idx) : 0.0;
}

auto SqliteStatement::columnText(int idx) const -> std::string {
    if (stmt_ == nullptr) {
        return {};
    }
    const unsigned char* txt = sqlite3_column_text(stmt_, idx);
    return txt != nullptr ? std::string(reinterpret_cast<const char*>(txt))
                          : std::string{}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

void SqliteStatement::reset() {
    if (stmt_ != nullptr) {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
}

auto SqliteStatement::get() const noexcept -> sqlite3_stmt* {
    return stmt_;
}

} // namespace kanariv::db
