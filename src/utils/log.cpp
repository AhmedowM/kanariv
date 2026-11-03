#include <kanariv/utils/log.hpp>
#include <mutex>
#include <utility>

namespace kanariv::utils {

auto Logger::getCallbackIfNeeded(LogLevel lvl, Callback& out) -> bool {
    std::scoped_lock lk(mutex_);
    if (!callback_) {
        return false;
    }
    if (std::to_underlying(lvl) < std::to_underlying(minLevel_)) {
        return false;
    }
    out = callback_;
    return true;
}

} // namespace kanariv::utils
