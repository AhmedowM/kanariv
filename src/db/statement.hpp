#pragma once

#include <kanariv/utils.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

/**
 * @file statement.hpp
 * @brief RAII wrapper for sqlite3_stmt.
 * @ingroup kanariv_db
 * @details Non-copyable, movable. Owns a sqlite3_stmt* and finalizes on
 * destruction/move. Provides prepare/bind/step/column/reset helpers.
 * All operations are thin wrappers over the SQLite C API; no locking is
 * performed here -- callers must hold the appropriate Database mutex.
 */

// Forward declarations for SQLite handles (sqlite3.h stays in .cpp files).
struct sqlite3;
struct sqlite3_stmt;

namespace kanariv::db {

/**
 * @brief RAII prepared statement.
 * @ingroup kanariv_db
 * @details Wraps sqlite3_stmt*; finalized on destruction. Move transfers
 * ownership. All bind/step methods are no-ops if no statement is prepared.
 * Thread-safety: not thread-safe; external synchronization required.
 */
class SqliteStatement final {
  public:
    /** @brief Default-constructs with no statement. @ingroup kanariv_db */
    SqliteStatement() = default;
    /** @brief Finalizes the statement if active. @ingroup kanariv_db */
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    /**
     * @brief Move-constructs, transferring ownership.
     * @ingroup kanariv_db
     * @param o Source; left with nullptr.
     */
    SqliteStatement(SqliteStatement&& o) noexcept;
    /**
     * @brief Move-assigns, finalizing any existing statement.
     * @ingroup kanariv_db
     * @param o Source.
     * @return *this
     */
    SqliteStatement& operator=(SqliteStatement&& o) noexcept;
    /**
     * @brief Prepares a statement.
     * @ingroup kanariv_db
     * @param db SQLite handle.
     * @param sql SQL text.
     * @return Success or Error with StatusCode::Internal and sqlite error message.
     */
    [[nodiscard]] std::expected<void, kanariv::utils::Error> prepare(sqlite3* db,
                                                                    std::string_view sql);
    /**
     * @brief Binds an int64 at 1-based index.
     * @ingroup kanariv_db
     * @param idx Parameter index (1-based).
     * @param v Value.
     */
    void bindInt(int idx, int64_t v);
    /**
     * @brief Binds a double at 1-based index.
     * @ingroup kanariv_db
     * @param idx Parameter index.
     * @param v Value.
     */
    void bindDouble(int idx, double v);
    /**
     * @brief Binds text at 1-based index (SQLITE_TRANSIENT).
     * @ingroup kanariv_db
     * @param idx Parameter index.
     * @param v Text view to copy.
     */
    void bindText(int idx, std::string_view v);
    /**
     * @brief Binds a blob at 1-based index (SQLITE_TRANSIENT).
     * @ingroup kanariv_db
     * @param idx Parameter index.
     * @param data Bytes to copy.
     */
    void bindBlob(int idx, std::span<const std::byte> data);
    /**
     * @brief Binds NULL at 1-based index.
     * @ingroup kanariv_db
     * @param idx Parameter index.
     */
    void bindNull(int idx);
    /**
     * @brief Steps to the next row.
     * @ingroup kanariv_db
     * @return true if a row is available (SQLITE_ROW), false otherwise.
     */
    [[nodiscard]] bool step();
    /**
     * @brief Steps expecting completion (SQLITE_DONE).
     * @ingroup kanariv_db
     * @return SQLite result code (SQLITE_DONE on success, SQLITE_ERROR if no statement).
     */
    [[nodiscard]] int stepDone();
    /**
     * @brief Reads an int64 column.
     * @ingroup kanariv_db
     * @param idx Column index (0-based).
     * @return Column value or 0 if no statement.
     */
    int64_t columnInt(int idx) const;
    /**
     * @brief Reads a double column.
     * @ingroup kanariv_db
     * @param idx Column index.
     * @return Column value or 0.0 if no statement.
     */
    double columnDouble(int idx) const;
    /**
     * @brief Reads a text column as string.
     * @ingroup kanariv_db
     * @param idx Column index.
     * @return Column text or empty if null/no statement.
     */
    std::string columnText(int idx) const;
    /**
     * @brief Resets the statement and clears bindings for reuse.
     * @ingroup kanariv_db
     */
    void reset();
    /**
     * @brief Returns the raw sqlite3_stmt*.
     * @ingroup kanariv_db
     * @return Borrowed pointer, may be null.
     */
    [[nodiscard]] sqlite3_stmt* get() const noexcept;

  private:
    sqlite3_stmt* stmt_{nullptr}; ///< Owned handle, finalized on destruction.
};

} // namespace kanariv::db
