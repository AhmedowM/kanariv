#pragma once

/**
 * @file service.hpp
 * @brief Umbrella header for the daemon service runtime.
 * @defgroup caudio_service caudio service runtime
 * @details Provides both service-hosting choices: `Service` for the desktop
 * daemon lifecycle and `Host` for an in-process service owned by another
 * application (for example a mobile foreground service). The included
 * classes own their worker/runtime resources through RAII; including this
 * header does not start a service.
 * @see caudio::service::Service
 * @see caudio::service::Host
 */

#include <caudio/service/host.hpp>
#include <caudio/service/service.hpp>
