#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <db/database_access.hpp>
#include <db/statement.hpp>
#include <db/transaction.hpp>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <kanariv/db/database.hpp>
#include <kanariv/db/json.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/db.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/status_format.hpp>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include "db_impl.hpp"

namespace kanariv::db {

using kanariv::utils::Json;

kanariv::utils::Expected<void> exportJson(Database& db, const std::filesystem::path& outPath) {
    auto tracks = db.listTracks();
    if (!tracks)
        return std::unexpected{tracks.error()};
    Json root;
    root["tracks"] = Json::array();
    for (auto& t : *tracks) {
        root["tracks"].push_back(kanariv::utils::trackToJson(t));
    }
    std::ofstream f(outPath, std::ios::binary);
    if (!f)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open output")};
    f << root.dump(2);
    if (!f)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "write failed")};
    return {};
}

kanariv::utils::Expected<void> importJson(Database& db, const std::filesystem::path& inPath) {
    std::ifstream f(inPath, std::ios::binary);
    if (!f)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, "cannot open input")};
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty())
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "empty file")};
    Json root;
    try {
        root = Json::parse(content);
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
    if (!root.contains("tracks"))
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "missing tracks array")};
    const Json tracks = root["tracks"];
    if (!tracks.isArray()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, "missing tracks array")};
    }
    std::unique_lock lk{DatabaseAccess::mutex(db)};
    sqlite3* h = DatabaseAccess::handleLocked(db);
    if (!h)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no db")};
    auto txRes = DbTransaction::begin(h);
    if (!txRes)
        return std::unexpected{txRes.error()};
    DbTransaction tx = std::move(*txRes);

    // Prepared once and reused for every row. Every failure is reported with
    // its row number and aborts the whole import (the transaction rolls back),
    // so no row is ever dropped silently.
    SqliteStatement insertStmt;
    if (auto e = insertStmt.prepare(h, insertTrackSql); !e)
        return std::unexpected{e.error()};
    SqliteStatement updateStmt;
    if (auto e = updateStmt.prepare(h, updateTrackSql); !e)
        return std::unexpected{e.error()};
    SqliteStatement findByFingerprint;
    if (auto e = findByFingerprint.prepare(h, "SELECT id FROM tracks WHERE fingerprint=?"); !e)
        return std::unexpected{e.error()};

    std::size_t inserted = 0;
    std::size_t updated = 0;
    for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
        const auto rowError = [ti](std::string_view detail) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                             "import row " + std::to_string(ti) +
                                                                 ": " + std::string(detail))};
        };
        auto jt = tracks.at(ti);
        if (!jt)
            return rowError("invalid track object");
        auto tr = kanariv::utils::trackFromJson(*jt);
        if (!tr)
            return rowError(tr.error().message);
        kanariv::types::TrackRow t = *tr;
        // A row without a path cannot be a track: reject it explicitly instead
        // of storing an unusable empty-path row.
        if (t.path.empty())
            return rowError("missing path");
        const auto fpSpan = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(t.fingerprint.data()), t.fingerprint.size()};

        insertStmt.reset();
        bindTrackColumns(insertStmt, t, 1);
        const int insertRc = insertStmt.stepDone();
        insertStmt.reset();
        if (insertRc == SQLITE_DONE) {
            ++inserted;
            continue;
        }
        if (insertRc != SQLITE_CONSTRAINT)
            return rowError(sqlite3_errmsg(h));

        // The only UNIQUE column is fingerprint: import updates the existing
        // row in place. A constraint without a matching row is a real error.
        findByFingerprint.reset();
        findByFingerprint.bindBlob(1, fpSpan);
        const bool found = findByFingerprint.step();
        const int64_t existing = found ? findByFingerprint.columnInt(0) : 0;
        findByFingerprint.reset();
        if (!found)
            return rowError("duplicate fingerprint has no matching row");

        updateStmt.reset();
        bindTrackColumns(updateStmt, t, 1);
        updateStmt.bindInt(29, existing);
        const int updateRc = updateStmt.stepDone();
        updateStmt.reset();
        if (updateRc != SQLITE_DONE)
            return rowError(std::string("update failed: ") + sqlite3_errmsg(h));
        ++updated;
    }
    if (auto c = tx.commit(); !c)
        return std::unexpected{c.error()};
    if (auto* logger = db.logger())
        logger->info("json import complete inserted={} updated={}", inserted, updated);
    return {};
}

kanariv::utils::Expected<void> Database::exportJson(const std::filesystem::path& outPath) {
    return kanariv::db::exportJson(*this, outPath);
}

kanariv::utils::Expected<void> Database::importJson(const std::filesystem::path& inPath) {
    return kanariv::db::importJson(*this, inPath);
}

} // namespace kanariv::db
