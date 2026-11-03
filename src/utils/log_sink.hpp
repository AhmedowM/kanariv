#pragma once

/**
 * @file log_sink.hpp
 * @brief Bounded rotating file sink for Logger (daemon log output).
 * @ingroup kanariv_utils
 * @details `FileLogSink` owns one log file plus a bounded set of rotated
 * generations (`path`, `path.1`, …). Rotation is size-triggered: when the
 * next line would exceed `maxBytes`, generations shift down (oldest
 * dropped) and a fresh current file starts. Total disk use is bounded by
 * `generations * maxBytes`, which matters because the daemon is long-lived.
 *
 * Line format: `[2026-10-13T12:34:56Z] [INFO ] [t:1a2b3c] message` —
 * UTC timestamp, fixed-width level, hashed thread id. Use through
 * `Logger`: `Logger(sink->callback(), level)`. The callback is thread-safe
 * (own mutex; Logger already invokes outside its lock) and never throws:
 * I/O failures degrade to dropped lines so logging can never crash the
 * daemon. Open failure returns nullptr — the caller falls back to silence.
 *
 * Thread safety: thread-safe; all state under an internal mutex.
 *
 * Internal header: lives in `src/`, not installed.
 */

#include <cstddef>
#include <filesystem>
#include <kanariv/utils/log.hpp>
#include <memory>

namespace kanariv::utils {

/** @brief Options for FileLogSink::open. @ingroup kanariv_utils */
struct FileSinkOptions {
    std::size_t maxBytes{1024 * 1024}; ///< Rotate past 1 MiB of current file.
    int generations{3};                ///< Total files kept (current + rotated).
    bool teeStderr{false};             ///< Also echo every line to stderr.
};

/**
 * @brief Rotating file sink behind a Logger::Callback.
 * @ingroup kanariv_utils
 * @details Create with open() (shared ownership — the Logger callback holds
 * a copy, so the sink outlives any ordering concerns). Not copyable.
 * @note KANARIV_API: service.dll installs this sink across the utils.dll
 * boundary.
 */
class KANARIV_API FileLogSink : public std::enable_shared_from_this<FileLogSink> {
  public:
    /**
     * @brief Opens (or creates) the log file for append.
     * @param path Log file path; parent must exist (caller creates it).
     * @param opts Rotation/tee options.
     * @return Sink, or nullptr when the file cannot be opened.
     * @details When the existing file already exceeds maxBytes it is
     * rotated immediately, so a stale oversized log never persists.
     */
    static std::shared_ptr<FileLogSink> open(const std::filesystem::path& path,
                                             const FileSinkOptions& opts = {});

    FileLogSink(const FileLogSink&) = delete;
    FileLogSink& operator=(const FileLogSink&) = delete;
    FileLogSink(FileLogSink&&) = delete;
    FileLogSink& operator=(FileLogSink&&) = delete;

    /** @brief Destructor (out-of-line: pimpl). */
    ~FileLogSink();

    /** @brief Logger-compatible callback writing formatted lines. */
    Logger::Callback callback();

    /** @brief True when the file is open. */
    bool valid() const noexcept;

    /** @brief Flushes the file (also done per line). */
    void flush();

  private:
    FileLogSink();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kanariv::utils
