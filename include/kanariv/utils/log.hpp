#pragma once

/**
 * @file log.hpp
 * @brief Thread-safe logger with level filtering and format support.
 * @ingroup kanariv_utils
 */

#include <format>
#include <functional>
#include <kanariv/fwd.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kanariv::utils {

/**
 * @brief Log severity levels ordered low to high.
 * @ingroup kanariv_utils
 * @details Filtering is `lvl >= minLevel`. Underlying values are used for
 * comparison via `std::to_underlying`. Values match the CLI `--log-level`
 * names 1:1 (trace=0 … error=4); see parseLogLevel.
 */
enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

/**
 * @brief Converts a LogLevel to a human-readable string view.
 * @ingroup kanariv_utils
 * @param lvl Level to stringify.
 * @return Non-owning view; `"Unknown"` for out-of-range values.
 */
constexpr std::string_view toString(LogLevel lvl) noexcept {
    switch (lvl) {
    case LogLevel::Trace:
        return "Trace";
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warn:
        return "Warn";
    case LogLevel::Error:
        return "Error";
    default:
        return "Unknown";
    }
}

/**
 * @brief Parses a CLI `--log-level` name to a LogLevel.
 * @ingroup kanariv_utils
 * @param name Lowercase level name (`trace|debug|info|warn|error`).
 * @return The level, or nullopt for unknown names (caller reports usage).
 * @details Single source of truth for the name mapping — the shell and any
 * future config/import validation must use this, never a local if-chain.
 */
constexpr std::optional<LogLevel> parseLogLevel(std::string_view name) noexcept {
    if (name == "trace")
        return LogLevel::Trace;
    if (name == "debug")
        return LogLevel::Debug;
    if (name == "info")
        return LogLevel::Info;
    if (name == "warn")
        return LogLevel::Warn;
    if (name == "error")
        return LogLevel::Error;
    return std::nullopt;
}

/**
 * @brief Parses a log-level value: names or 0–4 integers.
 * @ingroup kanariv_utils
 * @param value Level name (`trace|debug|info|warn|error`) or integer string.
 * @return The level, or nullopt for garbage (caller reports usage).
 * @details Accepts everything parseLogLevel does plus `"0"`…`"4"`, so config
 * values (`config set logLevel`) accept both forms while the canonical
 * stored form stays numeric. Single source of truth — the daemon handler
 * and loadConfig must use this, never ad-hoc parsing.
 */
constexpr std::optional<LogLevel> parseLogLevelValue(std::string_view value) noexcept {
    if (auto l = parseLogLevel(value))
        return l;
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '4')
        return static_cast<LogLevel>(value[0] - '0');
    return std::nullopt;
}

/**
 * @brief Thread-safe logger with callback and level filtering.
 * @ingroup kanariv_utils
 * @details Lock ordering: a single `mutable std::mutex` protects both
 * `callback_` and `minLevel_`. All mutators (`setCallback`, `setLevel`)
 * and observers (`level`, `getCallbackIfNeeded`) lock the mutex. `log()`
 * copies the callback under lock then invokes outside the lock to avoid
 * deadlocks if the callback re-enters Logger.
 *
 * Thread-safety: concurrent `log`/`setCallback`/`setLevel` is safe.
 * Callback invocation is not synchronized beyond the copy; callback must
 * be thread-safe if it touches shared state.
 *
 * @see LogLevel
 * @see toString
 */
class KANARIV_API Logger {
  public:
    /**
     * @brief Callback invoked for each enabled log message.
     * @ingroup kanariv_utils
     * @param level Severity of the message.
     * @param message Non-owning view of the log text (valid for call duration).
     */
    using Callback = std::function<void(LogLevel, std::string_view)>;

    /**
     * @brief Constructs a logger.
     * @ingroup kanariv_utils
     * @param cb Optional callback; null means messages are dropped.
     * @param minLevel Minimum level to emit (default Debug = all).
     */
    explicit Logger(Callback cb = nullptr, LogLevel minLevel = LogLevel::Debug)
        : callback_(std::move(cb)), minLevel_(minLevel) {}

    /**
     * @brief Replaces the callback atomically.
     * @ingroup kanariv_utils
     * @param cb New callback (may be null to disable logging).
     * @details Locks mutex_, moves callback in.
     */
    void setCallback(Callback cb) {
        std::lock_guard<std::mutex> lk(mutex_);
        callback_ = std::move(cb);
    }

    /**
     * @brief Sets the minimum level to emit.
     * @ingroup kanariv_utils
     * @param lvl New minimum; messages with `lvl < minLevel` are dropped.
     * @details Locks mutex_.
     */
    void setLevel(LogLevel lvl) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        minLevel_ = lvl;
    }

    /**
     * @brief Returns the current minimum level.
     * @ingroup kanariv_utils
     * @return Current minLevel (copy under lock).
     */
    [[nodiscard]] LogLevel level() const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return minLevel_;
    }

    /**
     * @brief Fast check whether a level would be emitted.
     * @ingroup kanariv_utils
     * @param lvl Severity to test.
     * @return true when a callback is set and `lvl >= minLevel`.
     * @details Use to skip expensive message-argument computation: the
     * `log()` overloads filter before formatting, but arguments are still
     * evaluated at the call site. `if (logger && logger->enabled(Debug))`
     * avoids that cost on hot paths.
     */
    [[nodiscard]] bool enabled(LogLevel lvl) const noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        return callback_ && std::to_underlying(lvl) >= std::to_underlying(minLevel_);
    }

    /**
     * @brief Emits a pre-formatted message if enabled.
     * @ingroup kanariv_utils
     * @param lvl Severity of the message.
     * @param msg Message view (forwarded to callback verbatim).
     * @details Copies callback under lock via getCallbackIfNeeded(), returns
     * early if no callback or filtered by minLevel. Invocation happens
     * outside the lock.
     */
    void log(LogLevel lvl, std::string_view msg) {
        Callback cbCopy;
        if (bool ok = getCallbackIfNeeded(lvl, cbCopy); !ok)
            return;
        cbCopy(lvl, msg);
    }

    /**
     * @brief Emits a formatted message if enabled.
     * @ingroup kanariv_utils
     * @tparam Args Format argument types.
     * @param lvl Severity.
     * @param fmt Compile-time format string (`std::format_string`).
     * @param args Arguments forwarded to `std::format`.
     * @details Filtering and callback copy happen under lock before
     * formatting. Formatting is done outside the lock to minimize hold time.
     */
    template <typename... Args>
    void log(LogLevel lvl, std::format_string<Args...> fmt, Args&&... args) {
        Callback cbCopy;
        if (bool ok = getCallbackIfNeeded(lvl, cbCopy); !ok)
            return;
        std::string s = std::format(fmt, std::forward<Args>(args)...);
        cbCopy(lvl, s);
    }

    /**
     * @brief Emits a formatted Trace message.
     * @ingroup kanariv_utils
     * @tparam Args Format argument types.
     * @param fmt Format string.
     * @param args Format arguments.
     */
    template <typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Trace, fmt, std::forward<Args>(args)...);
    }
    /**
     * @brief Emits a formatted Debug message.
     * @ingroup kanariv_utils
     * @tparam Args Format argument types.
     * @param fmt Format string.
     * @param args Format arguments.
     */
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }
    /**
     * @brief Emits a formatted Info message.
     * @ingroup kanariv_utils
     * @tparam Args Format argument types.
     * @param fmt Format string.
     * @param args Format arguments.
     */
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }
    /**
     * @brief Emits a formatted Warn message.
     * @ingroup kanariv_utils
     * @tparam Args Format argument types.
     * @param fmt Format string.
     * @param args Format arguments.
     */
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Warn, fmt, std::forward<Args>(args)...);
    }
    /**
     * @brief Emits a formatted Error message.
     * @ingroup kanariv_utils
     * @tparam Args Format argument types.
     * @param fmt Format string.
     * @param args Format arguments.
     */
    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) {
        log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief Emits a Trace message view.
     * @ingroup kanariv_utils
     * @param msg Message view.
     */
    void trace(std::string_view msg) {
        log(LogLevel::Trace, msg);
    }
    /**
     * @brief Emits a Debug message view.
     * @ingroup kanariv_utils
     * @param msg Message view.
     */
    void debug(std::string_view msg) {
        log(LogLevel::Debug, msg);
    }
    /**
     * @brief Emits an Info message view.
     * @ingroup kanariv_utils
     * @param msg Message view.
     */
    void info(std::string_view msg) {
        log(LogLevel::Info, msg);
    }
    /**
     * @brief Emits a Warn message view.
     * @ingroup kanariv_utils
     * @param msg Message view.
     */
    void warn(std::string_view msg) {
        log(LogLevel::Warn, msg);
    }
    /**
     * @brief Emits an Error message view.
     * @ingroup kanariv_utils
     * @param msg Message view.
     */
    void error(std::string_view msg) {
        log(LogLevel::Error, msg);
    }

  private:
    /**
     * @brief Checks if logging is enabled and copies callback.
     * @param lvl Message level to test.
     * @param out Receives callback copy if enabled.
     * @return true if callback present and lvl >= minLevel.
     * @details Locking: acquires mutex_, checks callback and level filter
     * via `std::to_underlying`, copies callback to `out`. Caller invokes
     * outside the lock.
     */
    bool getCallbackIfNeeded(LogLevel lvl, Callback& out);

    mutable std::mutex mutex_;           ///< Protects callback_ and minLevel_.
    Callback callback_;                  ///< User-provided sink; null = disabled.
    LogLevel minLevel_{LogLevel::Debug}; ///< Minimum level to emit.
};

} // namespace kanariv::utils
