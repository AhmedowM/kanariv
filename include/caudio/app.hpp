#pragma once

/**
 * @file app.hpp
 * @brief Umbrella header for the application orchestration library.
 * @defgroup caudio_app caudio application orchestration
 * @details Includes the public application facade and path-expansion helpers.
 * `caudio::app::App` coordinates client commands for frontends, while
 * `caudio::app::expandAddToken` provides the shared filesystem-token
 * semantics used when adding media to a queue. This header contains no
 * executable initialization; include it when a frontend needs the complete
 * application-facing API.
 * @see caudio::app::App
 * @see caudio::app::fmtClock
 * @see caudio::app::expandAddToken
 */

#include <caudio/app/app.hpp>
#include <caudio/app/paths.hpp>
