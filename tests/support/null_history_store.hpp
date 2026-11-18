#pragma once

/**
 * @file null_history_store.hpp
 * @brief Test double: history adapter used when play history is switched off.
 * @details Test-only Null Object for `contracts::IHistoryStore`. Every write is
 * accepted and discarded, every read is empty, so a test that does not care
 * about history can still hand a complete `contracts::Storage` aggregate to a
 * consumer without a "history enabled" branch.
 */

#include <cstdint>
#include <expected>
#include <kanariv/contracts/history_store.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <vector>

namespace kanariv::test_helpers {

/**
 * @brief Discards history writes and reports no history.
 */
class NullHistoryStore final : public kanariv::contracts::IHistoryStore {
  public:
    kanariv::utils::Expected<void> markPlayed(int64_t /*track_id*/, int64_t /*started_at*/,
                                              int64_t /*completed_at*/, int64_t /*position_ms*/,
                                              double /*completion_pct*/,
                                              int64_t /*queue_id*/) override {
        return {};
    }

    kanariv::utils::Expected<std::vector<kanariv::types::HistoryEntry>>
    listHistory(int /*limit*/) override {
        return std::vector<kanariv::types::HistoryEntry>{};
    }

    kanariv::utils::Expected<void> clearHistory() override {
        return {};
    }

    bool isOpen() const noexcept override {
        return true;
    }
};

} // namespace kanariv::test_helpers
