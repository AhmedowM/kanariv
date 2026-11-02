/**
 * @file tag_writer.hpp
 * @brief Audio file tag writing via TagLib (service-local).
 * @details Full-state writer: persists all eight editable tag fields of a
 * track row into the audio file. The service merges the edited field into
 * the row first, writes the file, then re-registers (rescan) so the DB row
 * always mirrors file bytes. Formats outside the writable set report
 * InvalidArg and leave both file and DB untouched.
 */
#pragma once

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <filesystem>

namespace kanariv::contracts {
// Defined in contracts/database.hpp (full Database API); reference params need only this.
class IDatabase;
} // namespace kanariv::contracts

namespace kanariv::service::detail {

/**
 * @brief Reports whether TagLib can write tags for a path.
 * @param p Audio file path (extension check, case-insensitive).
 */
bool canWriteFileTags(const std::filesystem::path& p);

/**
 * @brief Writes a track row's tag fields into its audio file.
 * @param p Audio file path.
 * @param t Track row carrying the desired tag state (id/path ignored).
 * @return Success or Error (InvalidArg for unsupported formats, Io for
 * TagLib open/save failures).
 */
kanariv::utils::Expected<void> writeFileTags(const std::filesystem::path& p,
                                            const kanariv::types::Track& t);

/**
 * @brief Re-registers a track row after its file was tag-edited.
 * @param db Database.
 * @param t Row carrying the merged tag fields, path, and id.
 * @return Updated row or Error.
 * @details Tag fields stay as merged (what was written); fingerprint, size,
 * mtime, and audio technicals refresh from the new bytes. The id is stable:
 * lookup is path-keyed, never fingerprint-keyed (the fingerprint changed).
 */
kanariv::utils::Expected<kanariv::types::Track> reregisterTrackRow(kanariv::contracts::IDatabase& db,
                                                                 kanariv::types::Track t);

} // namespace kanariv::service::detail
