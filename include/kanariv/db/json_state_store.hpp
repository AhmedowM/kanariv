#pragma once

/**
 * @file json_state_store.hpp
 * @brief JSON-sidecar adapter for the playback-state port.
 * @ingroup kanariv_db
 * @details Engine state is a fixed record with no relational needs, so it does
 * not belong in SQLite: this adapter writes one versioned JSON document next to
 * the database (`<database>.state.json`). The write is temp-file + fsync +
 * rename, and the previous document is kept as `.state.json.prev` so a torn or
 * corrupt file falls back to the last good generation instead of losing the
 * volume and resume position.
 *
 * `makePlaybackStateStore()` is what `makeStorage()` uses. On first use with an
 * existing database it imports the legacy `engine_state` row once, then the
 * sidecar is the only state written.
 *
 * @par Thread safety
 * `load()` and `save()` serialize on an internal mutex; the engine calls them
 * from its monitor thread and command handlers.
 */

#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/contracts/playback_state_store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/engine.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <mutex>
#include <optional>

namespace kanariv::db {

/**
 * @brief Plays back engine state to and from a JSON sidecar.
 * @ingroup kanariv_db
 */
class KANARIV_API JsonPlaybackStateStore final : public kanariv::contracts::IPlaybackStateStore {
  public:
    /**
     * @brief Creates the adapter.
     * @param path Sidecar file path; empty keeps the state in memory only.
     * @param logger Non-owning logger (may be null); must outlive the store.
     */
    explicit JsonPlaybackStateStore(std::filesystem::path path = {},
                                    kanariv::utils::Logger* logger = nullptr);
    ~JsonPlaybackStateStore() override;
    JsonPlaybackStateStore(const JsonPlaybackStateStore&) = delete;
    JsonPlaybackStateStore& operator=(const JsonPlaybackStateStore&) = delete;

    /**
     * @brief Reads the sidecar, falling back to the previous generation.
     * @return State snapshot. A missing or unreadable file is success with a
     * default snapshot, so a damaged sidecar never blocks startup.
     */
    std::expected<kanariv::types::EngineState, kanariv::utils::Error> load() override;

    /**
     * @brief Replaces the stored snapshot atomically.
     * @param state State snapshot to write.
     * @return Success, or `Io` when the document cannot be written.
     */
    std::expected<void, kanariv::utils::Error>
    save(const kanariv::types::EngineState& state) override;

    /// @brief Always usable: the sidecar (or in-memory mode) needs no open step.
    bool isOpen() const noexcept override;

    /// @brief Returns the logger supplied at construction.
    kanariv::utils::Logger* logger() const noexcept override;

    /**
     * @brief Path of the sidecar document (empty in in-memory mode).
     * @return Borrowed path reference.
     */
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

  private:
    std::filesystem::path path_;
    kanariv::utils::Logger* logger_{nullptr};
    mutable std::mutex mutex_;
    std::optional<kanariv::types::EngineState> memory_;
};

/**
 * @brief Builds the playback-state port for a database.
 * @ingroup kanariv_db
 * @param db Database whose siblings receive the sidecar.
 * @return Store writing `<database>.state.json`, an in-memory store for a
 * database without a filesystem path, or the JSON store seeded once from the
 * legacy `engine_state` row when the sidecar does not exist yet.
 * @details Never returns null.
 */
KANARIV_API std::shared_ptr<kanariv::contracts::IPlaybackStateStore>
makePlaybackStateStore(Database& db);

} // namespace kanariv::db
