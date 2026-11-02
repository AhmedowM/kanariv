#pragma once

/**
 * @file service.hpp
 * @brief Umbrella header for the daemon service runtime.
 * @defgroup kanariv_service kanariv service runtime
 * @details Provides both service-hosting choices: `Service` for the desktop
 * daemon lifecycle and `Host` for an in-process service owned by another
 * application (for example a mobile foreground service). The included
 * classes own their worker/runtime resources through RAII; including this
 * header does not start a service.
 * @see kanariv::service::Service
 * @see kanariv::service::Host
 */

#include <kanariv/service/host.hpp>
#include <kanariv/service/service.hpp>
