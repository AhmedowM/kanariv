#pragma once
#include <atomic>
#include <kanariv/utils/ring.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
/**
 * @file common.hpp
 * @brief Shared test helpers (temp paths, busy-wait, NOAUDIO switch).
 * @ingroup kanariv_test
 */

/// @brief Test-only helpers for headless/CI audio control.
namespace kanariv::test {

/**
 * @brief Returns true when audio device tests should be skipped.
 * @details Checks compile definition `KANARIV_TEST_NOAUDIO_BUILD` first, then
 * falls back to env `KANARIV_TEST_NOAUDIO` for values `1`, `true`, `True`.
 * When true, tests that would call `AudioOutput::create` / `ma_device_start`
 * and beep should `SKIP` instead. Set in CI via `KANARIV_TEST_NOAUDIO=1`.
 * @return true if `KANARIV_TEST_NOAUDIO_BUILD` is defined or
 * `KANARIV_TEST_NOAUDIO` is `1`/`true`/`True`.
 */
inline bool noAudio() noexcept {
#ifdef KANARIV_TEST_NOAUDIO_BUILD
    return true;
#endif
    const char* v = std::getenv("KANARIV_TEST_NOAUDIO");
    return v && (std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "True");
}

/**
 * @brief Creates a dummy SPSC ring without touching the audio device.
 * @param sampleRate Sample rate hint (unused, for API symmetry with AudioOutput::Config).
 * @param channels Channel count for the ring (default 2).
 * @param frames Capacity in frames (default 8192).
 * @return `kanariv::utils::SpscRing<float>` ready for `fillForTest` exercises.
 * @details Test-only helper -- does NOT call `AudioOutput::create` or
 * `ma_device_init`/`ma_device_start`, so no beep. Example:
 * `auto ring = kanariv::test::makeDummyRing(48000, 2);`
 */
inline kanariv::utils::SpscRing<float> makeDummyRing(uint32_t sampleRate = 48000,
                                                    uint32_t channels = 2, size_t frames = 8192) {
    (void)sampleRate;
    return kanariv::utils::SpscRing<float>{frames, channels};
}

} // namespace kanariv::test

/**
 * @brief Catch2 helper: skip current test when `KANARIV_TEST_NOAUDIO=1`.
 * @details Usage: `KANARIV_SKIP_IF_NOAUDIO();` at top of a TEST_CASE that
 * would otherwise beep via `AudioOutput::create`. Expands to `SKIP(...)`.
 */
#define KANARIV_SKIP_IF_NOAUDIO()                                                                   \
    do {                                                                                           \
        if (kanariv::test::noAudio()) {                                                             \
            SKIP("KANARIV_TEST_NOAUDIO=1 - audio device test skipped (no beep)");                   \
        }                                                                                          \
    } while (0)

namespace kanariv::test_helpers {

/**
 * @brief Paths created by the test helpers, removed at process exit.
 * @details Deliberately leaked (function-local `new`) so the registry outlives
 * static destructors; `std::atexit` cleanup then observes a valid vector.
 */
inline std::vector<std::filesystem::path>& tempCleanupPaths() {
    static auto* paths = new std::vector<std::filesystem::path>{};
    return *paths;
}

/**
 * @brief Removes every registered temp path, retrying briefly.
 * @details A daemon or SQLite handle may still be closing on Windows; without
 * the retry a passing test leaks its directory.
 */
inline void cleanupTempPaths() noexcept {
    std::error_code ec;
    auto& paths = tempCleanupPaths();
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::filesystem::remove_all(*it, ec);
            if (!std::filesystem::exists(*it))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
}

inline void registerTempPath(const std::filesystem::path& p) {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(cleanupTempPaths); });
    tempCleanupPaths().push_back(p);
}

inline std::filesystem::path tempDbPath(const std::string& prefix) {
    static std::atomic<int> ctr{0};
    auto dir = std::filesystem::temp_directory_path();
    std::string name = prefix + "_" + std::to_string(ctr.fetch_add(1)) + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       ".db";
    auto p = dir / name;
    registerTempPath(p);
    registerTempPath(std::filesystem::path(p.string() + "-wal"));
    registerTempPath(std::filesystem::path(p.string() + "-shm"));
    return p;
}

inline std::filesystem::path tempDirPath(const std::string& prefix) {
    static std::atomic<int> ctr2{1000};
    auto dir = std::filesystem::temp_directory_path();
    // Unique across runs, not just within one process: counter alone
    // restarts at 1000 every run, so a leaked daemon/config from an aborted
    // run would collide (same socket hash) with the next run. Timestamp +
    // pid make reuse practically impossible, even under parallel ctest.
#ifdef _WIN32
    auto pid = static_cast<unsigned long long>(::_getpid());
#else
    auto pid = static_cast<unsigned long long>(::getpid());
#endif
    std::string name = prefix + "_" + std::to_string(ctr2.fetch_add(1)) + "_" +
                       std::to_string(pid) + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto p = dir / name;
    std::filesystem::create_directories(p);
    registerTempPath(p);
    return p;
}

inline void busyWaitUntil(std::function<bool()> pred,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{2000},
                          std::chrono::milliseconds interval = std::chrono::milliseconds{10}) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > timeout)
            break;
        std::this_thread::sleep_for(interval);
    }
}

inline void safeRemoveDb(const std::string& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p + "-wal", ec);
    std::filesystem::remove(p + "-shm", ec);
}

} // namespace kanariv::test_helpers
