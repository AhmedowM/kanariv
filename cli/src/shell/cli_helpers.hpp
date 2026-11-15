#pragma once

/**
 * @file cli_helpers.hpp
 * @brief Pure CLI policy helpers: exit codes, retry backoff, ANSI gate, stop polling.
 * @details These helpers hold the decisions every render/loop path shares. They
 * are free of platform calls and I/O so tests can exercise them without
 * spawning a process; shell.cpp supplies the observable inputs (TTY,
 * NO_COLOR, the signal flag).
 */

#include <algorithm>
#include <chrono>
#include <kanariv/utils/error.hpp>
#include <stop_token>
#include <thread>

namespace kanariv::app::cli::detail {

/** @brief Success. @ingroup kanariv_app */
inline constexpr int exitOk = 0;
/** @brief Runtime failure (including a failed or partial batch). @ingroup kanariv_app */
inline constexpr int exitRuntime = 1;
/** @brief Daemon down or in a state that rejects the command. @ingroup kanariv_app */
inline constexpr int exitDaemonDown = 105;
/** @brief Argument rejected by daemon-side validation. @ingroup kanariv_app */
inline constexpr int exitInvalidArgument = 106;
/** @brief Requested track/queue/playlist/config key not found. @ingroup kanariv_app */
inline constexpr int exitNotFound = 109;

/**
 * @brief The one exit-code table for process status.
 * @ingroup kanariv_app
 * @param e Error to map.
 * @return 106 invalid argument, 109 not found, 105 daemon down/bad state,
 *         1 for anything else.
 * @details Every path (single outcomes, batches, JSON renders, watch/lyrics
 * loops) derives process status from here: `ipc::Result` keeps an embedded
 * error alternative, and re-deriving 0/1 from it produced codes that
 * disagreed with this table.
 */
inline int exitCodeForError(const kanariv::utils::Error& e) {
    using kanariv::utils::StatusCode;
    switch (e.code) {
    case StatusCode::InvalidArg:
        return exitInvalidArgument;
    case StatusCode::NotFound:
        return exitNotFound;
    case StatusCode::State:
        return exitDaemonDown;
    default:
        return exitRuntime;
    }
}

/** @brief First retry delay after a transient transport failure. @ingroup kanariv_app */
inline constexpr std::chrono::milliseconds retryBackoffBase{200};
/** @brief Upper bound for the retry delay (never grows past this). @ingroup kanariv_app */
inline constexpr std::chrono::milliseconds retryBackoffCap{2000};

/**
 * @brief Bounded exponential backoff between transient transport retries.
 * @ingroup kanariv_app
 * @param attempt Zero-based failure index (0 = first failure).
 * @return Base delay for attempt 0, doubled per attempt, capped at the cap.
 */
inline std::chrono::milliseconds retryBackoff(int attempt) {
    auto delay = retryBackoffBase;
    for (int i = 0; i < attempt && delay < retryBackoffCap; ++i)
        delay *= 2;
    return std::min(delay, retryBackoffCap);
}

/**
 * @brief ANSI control gate: interactive stdout only, and NO_COLOR honoured.
 * @ingroup kanariv_app
 * @param stdoutIsTty True when stdout is a terminal.
 * @param noColor True when NO_COLOR is set in the environment.
 * @return True when ANSI escapes (colour or line clearing) may be emitted.
 */
inline constexpr bool ansiEnabled(bool stdoutIsTty, bool noColor) {
    return stdoutIsTty && !noColor;
}

/**
 * @brief One stop-flag poll: requests stop when the signal flag is set.
 * @ingroup kanariv_app
 * @param flagged Value of the async-signal-safe termination flag.
 * @param src Process stop source to request.
 * @return True on the first request; false when the flag is clear or stop was
 *         already requested (idempotent, so a poller may call it repeatedly).
 */
inline bool requestStopIfFlagged(bool flagged, std::stop_source& src) noexcept {
    if (!flagged)
        return false;
    return src.request_stop();
}

/**
 * @brief Sleeps up to @p d, waking early when @p st is stop-requested.
 * @ingroup kanariv_app
 * @param st Stop token observed in short slices.
 * @param d Maximum sleep.
 * @return True when the sleep was cut short by a stop request.
 */
inline bool interruptibleSleep(std::stop_token st, std::chrono::milliseconds d) {
    using namespace std::chrono;
    const auto slice = milliseconds{25};
    const auto deadline = steady_clock::now() + d;
    while (!st.stop_requested()) {
        // Round up so accumulated truncation cannot cut the sleep short of d.
        const auto left = ceil<milliseconds>(deadline - steady_clock::now());
        if (left <= milliseconds::zero())
            return false;
        std::this_thread::sleep_for(std::min(slice, left));
    }
    return true;
}

} // namespace kanariv::app::cli::detail
