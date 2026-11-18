#pragma once

/**
 * @file library.hpp
 * @brief Aggregate include for the kanariv.library module.
 * @ingroup kanariv_library
 * @defgroup kanariv_library kanariv library orchestration
 * @details Library scanning spans media discovery and store persistence, and
 * the explicit ReplayGain 2.0 pass measures tracks over the same seam; this
 * component is the only place that depends on both.
 */

#include <kanariv/library/analyze.hpp>
#include <kanariv/library/scan.hpp>
