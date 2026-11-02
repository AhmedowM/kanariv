module;
#include <kanariv/config.hpp>

export module kanariv.config;

export namespace kanariv::config {
using ::kanariv::config::Config;
using ::kanariv::config::loadConfig;
using ::kanariv::config::lockPathFor;
using ::kanariv::config::logPathFor;
using ::kanariv::config::pidPathFor;
using ::kanariv::config::RuntimePaths;
using ::kanariv::config::saveConfig;
using ::kanariv::config::socketPathFor;
} // namespace kanariv::config
