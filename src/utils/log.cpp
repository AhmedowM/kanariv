#include <kanariv/utils/log.hpp>
#include <mutex>
#include <utility>

namespace kanariv::utils {

bool Logger::getCallbackIfNeeded(LogLevel lvl, Callback& out) {
    std::lock_guard lk(mutex_);
    if (!callback_)
        return false;
    if (std::to_underlying(lvl) < std::to_underlying(minLevel_))
        return false;
    out = callback_;
    return true;
}

} // namespace kanariv::utils
