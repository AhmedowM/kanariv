#include <chrono>
#include <cstdint>
#include <engine/history.hpp>

namespace kanariv::engine::detail {

uint64_t nowMs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t wallMs() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

bool shouldMarkPlayed(double duration, double played, bool marked, int pctThr,
                      int secsThr) noexcept {
    if (marked)
        return false;
    double pct = pctThr > 0 ? static_cast<double>(pctThr) / 100.0 : 0.6;
    double secs = secsThr > 0 ? static_cast<double>(secsThr) : 90.0;
    if (duration > 0.0 && played / duration >= pct)
        return true;
    if (played >= secs)
        return true;
    return false;
}

} // namespace kanariv::engine::detail
