#pragma once

/**
 * @file utils.hpp
 * @brief Main header for kanariv.utils -- includes all utility partitions.
 * @defgroup kanariv_utils kanariv utilities
 * @details This header includes all public partitions of kanariv.utils:
 * - result: StatusCode, toString, formatter
 * - error: Error, Expected, makeError
 * - log: LogLevel, Logger
 * - math: clampVolume, toHex, fromHex
 * - ring: SpscRing
 * - thread: sleepFor, sleepForMs, setThreadName
 * - print: portable print/println facade (see print.hpp)
 * - json: opaque Json/JsonRef value type (nlohmann stays behind src/ walls)
 * - record_json: Track <-> Json codec shared by db and ipc
 * - generator: portable Generator (libc++ lacks std::generator)
 *
 * Internal-only helpers (MpscQueue, MoveOnlyFunction, the rotating file
 * sink) live in src/utils/ and are NOT installed.
 */

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/generator.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/print.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
#include <kanariv/utils/thread.hpp>
