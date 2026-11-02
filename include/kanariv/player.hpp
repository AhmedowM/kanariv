#pragma once

/**
 * @file player.hpp
 * @brief Umbrella header for the kanariv.player module.
 * @ingroup kanariv_player
 * @defgroup kanariv_player kanariv player
 *
 * Audio playback module providing:
 * - Media reader/decoder re-exports (see @ref kanariv_media)
 * - Lock-free SPSC ring buffer for decode/audio thread communication
 * - miniaudio-based AudioOutput for device playback
 * - Player with gapless playback, seeking, and state management
 */

#include <kanariv/media.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/player/player.hpp>
