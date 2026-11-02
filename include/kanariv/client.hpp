#pragma once

/**
 * @file client.hpp
 * @brief Umbrella header for the daemon client SDK.
 * @defgroup kanariv_client kanariv client SDK
 * @details Includes the synchronous client facade and all command/result
 * types needed to communicate with a running kanariv service. Constructing a
 * client does not launch a daemon; callers choose whether to connect to an
 * existing endpoint or start a service separately.
 * @see kanariv::client::Client
 */

#include <kanariv/client/client.hpp>
