#include <chrono>
#include <cstdint>
#include <engine/history.hpp>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/result.hpp>
#include <memory>
#include <utility>

namespace kanariv::engine::detail {

constexpr double defaultPctThreshold = 0.6;
constexpr double defaultSecsThreshold = 90.0;

auto nowMs() noexcept -> uint64_t {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

auto wallMs() noexcept -> uint64_t {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

auto shouldMarkPlayed(double duration, double played, bool marked, int pctThr, int secsThr) noexcept
    -> bool {
    if (marked) {
        return false;
    }
    double pct = pctThr > 0 ? static_cast<double>(pctThr) / 100.0 : defaultPctThreshold;
    double secs = secsThr > 0 ? static_cast<double>(secsThr) : defaultSecsThreshold;
    if (duration > 0.0 && played / duration >= pct) {
        return true;
    }
    if (played >= secs) {
        return true;
    }
    return false;
}

} // namespace kanariv::engine::detail

namespace kanariv::engine {

History::History(std::shared_ptr<kanariv::contracts::IDatabase> db) : db_(std::move(db)) {}

auto History::listHistory(int limit) -> History::ExpectedEntries {
    if (!db_ || !db_->isOpen()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    return db_->engineHistoryList(limit);
}

auto History::clearHistory() -> History::ExpectedVoid {
    if (!db_ || !db_->isOpen()) {
        return std::unexpected(
            kanariv::utils::makeError(kanariv::utils::StatusCode::State, "no db"));
    }
    return db_->engineHistoryClear();
}

} // namespace kanariv::engine
