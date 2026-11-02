#pragma once

/**
 * @file app.hpp
 * @brief Umbrella header for the application orchestration library.
 * @defgroup kanariv_app kanariv application orchestration
 * @details Includes the public application facade and path-expansion helpers.
 * `kanariv::app::App` coordinates client commands for frontends, while
 * `kanariv::app::expandAddToken` provides the shared filesystem-token
 * semantics used when adding media to a queue. This header contains no
 * executable initialization; include it when a frontend needs the complete
 * application-facing API.
 * @see kanariv::app::App
 * @see kanariv::app::fmtClock
 * @see kanariv::app::expandAddToken
 */

#include <kanariv/app/app.hpp>
#include <kanariv/app/paths.hpp>
