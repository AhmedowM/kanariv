#pragma once

/**
 * @file signals.hpp
 * @brief Process-wide interrupt handling and the stop source long loops observe.
 * @details Signal handlers may only store to a `volatile std::sig_atomic_t`
 * (calling a stop_source from a handler is undefined behaviour). A small
 * poller thread converts the flag into a `std::stop_source` request that every
 * long-running loop (foreground daemon, watch, live lyrics, preview) observes
 * through one `std::stop_token`.
 */

#include <chrono>
#include <csignal>
#include <stop_token>
#include <thread>

#include "cli_helpers.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace kanariv::app::cli::detail {

/**
 * @brief Async-signal-safe termination flag (handlers only store to it).
 * @ingroup kanariv_app
 */
inline volatile std::sig_atomic_t& terminationFlag() noexcept {
    static volatile std::sig_atomic_t flag = 0;
    return flag;
}

#ifdef _WIN32
/** @brief Console control handler: records Ctrl+C/Ctrl+Break/close. @ingroup kanariv_app */
inline BOOL WINAPI consoleCtrlHandler(DWORD type) noexcept {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        terminationFlag() = 1;
        return TRUE;
    default:
        return FALSE;
    }
}
#else
/** @brief SIGINT/SIGTERM handler: records the request only. @ingroup kanariv_app */
inline void signalHandler(int) noexcept {
    terminationFlag() = 1;
}
#endif

/** @brief Installs the process termination handlers once. @ingroup kanariv_app */
inline void installTerminationHandlers() {
    static const bool installed = [] {
#ifdef _WIN32
        ::SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
#endif
        return true;
    }();
    (void)installed;
}

/**
 * @brief Process stop state: one source, one token, one flag poller thread.
 * @ingroup kanariv_app
 * @details The poller observes the signal flag every 25 ms and requests stop,
 * so handlers stay trivial while every loop reacts within one poll. The
 * contract is that a signal exits with code 0 once loops have unwound (the
 * daemon host tears the service down through its stop token).
 */
class ProcessStop {
  public:
    /** @brief Installs handlers and starts the flag poller. @ingroup kanariv_app */
    ProcessStop() {
        installTerminationHandlers();
        poller_ = std::jthread([this](std::stop_token self) { pollLoop(self); });
    }
    /** @brief Stops and joins the poller. @ingroup kanariv_app */
    ~ProcessStop() {
        if (poller_.joinable())
            poller_.request_stop();
    }

    ProcessStop(const ProcessStop&) = delete;
    ProcessStop& operator=(const ProcessStop&) = delete;

    /** @brief Token every long-running loop observes. @ingroup kanariv_app */
    std::stop_token token() const noexcept {
        return source_.get_token();
    }
    /** @brief Requests stop directly (idempotent). @ingroup kanariv_app */
    void request() noexcept {
        source_.request_stop();
    }
    /** @brief True once a termination request was recorded. @ingroup kanariv_app */
    bool requested() const noexcept {
        return source_.stop_requested();
    }

  private:
    void pollLoop(std::stop_token self) {
        while (!self.stop_requested()) {
            if (source_.stop_requested())
                return;
            if (requestStopIfFlagged(terminationFlag() != 0, source_))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
    }

    std::stop_source source_;
    std::jthread poller_;
};

} // namespace kanariv::app::cli::detail
