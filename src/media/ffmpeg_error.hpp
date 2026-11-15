#pragma once

/**
 * @file ffmpeg_error.hpp
 * @brief Public-facing text for an FFmpeg/`swresample` error code.
 * @ingroup kanariv_media
 * @details Internal header: lives in `src/`, not installed. FFmpeg's own log
 * output is suppressed process-wide (`av_log_set_level(AV_LOG_QUIET)` in the
 * decoder), so `av_strerror` is the only remaining source of the *reason* a
 * file failed to open, probe, read or decode. Every FFmpeg failure path must
 * route this text into its `Error::message` or the logger (M4).
 */

#include <string>

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wglobal-module"
#endif
extern "C" {
#include <libavutil/error.h>
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif

namespace kanariv::media::internal {

/**
 * @brief Formats an FFmpeg error code as human-readable text.
 * @ingroup kanariv_media
 * @param err Negative `AVERROR` value returned by an FFmpeg call.
 * @return `av_strerror` text (e.g. "Invalid data found when processing input"),
 * or `"unknown FFmpeg error <n>"` when the code has no description.
 */
inline std::string ffErrorString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(err, buf, sizeof(buf)) < 0)
        return "unknown FFmpeg error " + std::to_string(err);
    return std::string(buf);
}

} // namespace kanariv::media::internal
