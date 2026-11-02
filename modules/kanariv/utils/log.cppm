module;
#include <kanariv/utils/log.hpp>

export module kanariv.utils:log;

export namespace kanariv::utils {
using ::kanariv::utils::Logger;
using ::kanariv::utils::LogLevel;
using ::kanariv::utils::parseLogLevel;
using ::kanariv::utils::toString;
} // namespace kanariv::utils
