#pragma once

/**
 * @file null_history_store.hpp
 * @brief History adapter used when play history is switched off.
 * @ingroup kanariv_contracts
 * @details Consumers that keep `history` in `contracts::Storage` but do not
 * want plays recorded wire this adapter instead of the database-backed
 * implementation. Every write is accepted and discarded, every read is empty,
 * so call sites need no "history enabled" branch.
 */

#include <cstdint>
#include <expected>
#include <kanariv/contracts/history_store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <vector>

namespace kanariv::contracts {

/**
 * @brief Discards history writes and reports no history.
 * @ingroup kanariv_contracts
 */
class NullHistoryStore final : public IHistoryStore {
  public:
    /**
     * @brief Accepts and drops a play record.
     * @return Success.
     */
    std::expected<void, kanariv::utils::Error>
    markPlayed(int64_t /*track_id*/, int64_t /*started_at*/, int64_t /*completed_at*/,
               int64_t /*position_ms*/, double /*completion_pct*/, int64_t /*queue_id*/) override {
        return {};
    }

    /**
     * @brief Reports an empty history.
     * @return Empty vector.
     */
    std::expected<std::vector<kanariv::types::HistoryEntry>, kanariv::utils::Error>
    listHistory(int /*limit*/) override {
        return std::vector<kanariv::types::HistoryEntry>{};
    }

    /**
     * @brief Accepts and drops a clear request.
     * @return Success.
     */
    std::expected<void, kanariv::utils::Error> clearHistory() override {
        return {};
    }

    /// @brief Always usable.
    bool isOpen() const noexcept override {
        return true;
    }
};

} // namespace kanariv::contracts
