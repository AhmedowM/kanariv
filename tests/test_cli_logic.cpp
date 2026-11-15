/**
 * @file test_cli_logic.cpp
 * @brief Unit tests for the CLI's pure policy helpers (cli_helpers.hpp).
 * @details Covers the exit-code table, retry backoff, the ANSI/TTY gate and
 * stop-flag polling without spawning a process or touching audio.
 */

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <kanariv/utils/error.hpp>
#include <stop_token>

#include "cli_helpers.hpp"

using namespace kanariv::app::cli::detail;
using kanariv::utils::Error;
using kanariv::utils::makeError;
using kanariv::utils::StatusCode;

TEST_CASE("cli exit-code table maps status codes", "[cli][logic]") {
    REQUIRE(exitOk == 0);
    REQUIRE(exitRuntime == 1);
    REQUIRE(exitUsage == 2);
    REQUIRE(exitDaemonDown == 105);
    REQUIRE(exitInvalidArgument == 106);
    REQUIRE(exitNotFound == 109);

    REQUIRE(exitCodeForError(makeError(StatusCode::InvalidArg, "bad flag")) == 106);
    REQUIRE(exitCodeForError(makeError(StatusCode::NotFound, "no such id")) == 109);
    // State carries daemon-down and other bad-state failures: a failed
    // daemon start reports State and therefore exits 105.
    REQUIRE(exitCodeForError(makeError(StatusCode::State, "daemon not running")) == 105);
    // Everything else stays a generic runtime failure.
    for (auto code : {StatusCode::Io, StatusCode::Corrupt, StatusCode::Busy, StatusCode::Internal,
                      StatusCode::NoMem, StatusCode::AlreadyExists, StatusCode::Unsupported,
                      StatusCode::Device, StatusCode::NoSpace, StatusCode::Ok}) {
        INFO("status code: " << kanariv::utils::toString(code));
        REQUIRE(exitCodeForError(Error{code, "x"}) == 1);
    }
}

TEST_CASE("cli retry backoff is bounded and monotonic", "[cli][logic]") {
    REQUIRE(retryBackoff(0) == retryBackoffBase);
    REQUIRE(retryBackoff(-3) == retryBackoffBase);
    auto previous = std::chrono::milliseconds{0};
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto delay = retryBackoff(attempt);
        INFO("attempt: " << attempt);
        REQUIRE(delay >= previous);
        REQUIRE(delay <= retryBackoffCap);
        REQUIRE(delay >= retryBackoffBase);
        previous = delay;
    }
    REQUIRE(retryBackoff(1) == retryBackoffBase * 2);
    REQUIRE(retryBackoff(2) == retryBackoffBase * 4);
    REQUIRE(retryBackoff(100) == retryBackoffCap);
}

TEST_CASE("cli ANSI gate needs a TTY and no NO_COLOR", "[cli][logic]") {
    REQUIRE_FALSE(ansiEnabled(false, false)); // piped redirect: plain output
    REQUIRE_FALSE(ansiEnabled(false, true));
    REQUIRE_FALSE(ansiEnabled(true, true)); // NO_COLOR wins on a TTY
    REQUIRE(ansiEnabled(true, false));
}

TEST_CASE("cli stop-flag polling requests stop once", "[cli][logic]") {
    std::stop_source src;
    REQUIRE_FALSE(requestStopIfFlagged(false, src));
    REQUIRE_FALSE(src.stop_requested());
    REQUIRE(requestStopIfFlagged(true, src));
    REQUIRE(src.stop_requested());
    // Idempotent: a poller may observe the same flag again.
    REQUIRE_FALSE(requestStopIfFlagged(true, src));
    REQUIRE(src.stop_requested());
}

TEST_CASE("cli interruptible sleep observes a stop request", "[cli][logic]") {
    std::stop_source src;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(interruptibleSleep(src.get_token(), std::chrono::milliseconds{20}));
    REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{20});

    src.request_stop();
    const auto stoppedAt = std::chrono::steady_clock::now();
    REQUIRE(interruptibleSleep(src.get_token(), std::chrono::milliseconds{5000}));
    // A stopped token must not sleep anywhere near the full duration.
    REQUIRE(std::chrono::steady_clock::now() - stoppedAt < std::chrono::seconds{1});
}
