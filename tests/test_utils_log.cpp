#include <catch2/catch_test_macros.hpp>
#include <kanariv/utils/log.hpp>
#include <common.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../src/utils/log_sink.hpp"

namespace kanariv::utils::test {

bool log_injected() {
    std::vector<std::pair<LogLevel, std::string>> out;
    Logger logger(
        [&](LogLevel lvl, std::string_view msg) { out.emplace_back(lvl, std::string(msg)); });
    logger.log(LogLevel::Info, "hello {}", 42);
    if (out.size() != 1)
        return false;
    if (out[0].second.compare("hello 42") != 0)
        return false;
    if (out[0].first != LogLevel::Info)
        return false;
    return true;
}

bool log_level_filter() {
    std::vector<std::pair<LogLevel, std::string>> out;
    Logger logger(
        [&](LogLevel lvl, std::string_view msg) { out.emplace_back(lvl, std::string(msg)); },
        LogLevel::Warn);
    logger.log(LogLevel::Debug, "debug msg");
    logger.log(LogLevel::Info, "info msg");
    if (!out.empty())
        return false;
    logger.log(LogLevel::Warn, "warn msg");
    if (out.size() != 1)
        return false;
    if (out[0].first != LogLevel::Warn)
        return false;
    logger.log(LogLevel::Error, "error msg");
    if (out.size() != 2)
        return false;
    return true;
}

bool log_convenience() {
    std::vector<std::pair<LogLevel, std::string>> out;
    Logger logger(
        [&](LogLevel lvl, std::string_view msg) { out.emplace_back(lvl, std::string(msg)); },
        LogLevel::Trace);
    logger.trace("trc {}", 0);
    logger.debug("dbg {}", 1);
    logger.info("inf {}", 2);
    logger.warn("wrn {}", 3);
    logger.error("err {}", 4);
    if (out.size() != 5)
        return false;
    if (out[0].first != LogLevel::Trace)
        return false;
    if (out[0].second.compare("trc 0") != 0)
        return false;
    if (out[1].first != LogLevel::Debug)
        return false;
    if (out[1].second.compare("dbg 1") != 0)
        return false;
    if (out[2].first != LogLevel::Info)
        return false;
    if (out[3].first != LogLevel::Warn)
        return false;
    if (out[4].first != LogLevel::Error)
        return false;
    return true;
}

bool log_set_callback_level() {
    Logger logger;
    std::vector<std::string> out;
    logger.log(LogLevel::Info, "no cb yet");
    if (!out.empty())
        return false;
    logger.setCallback([&](LogLevel, std::string_view msg) { out.emplace_back(msg); });
    logger.log(LogLevel::Info, "now captured");
    if (out.size() != 1)
        return false;
    if (out[0].compare("now captured") != 0)
        return false;
    logger.setLevel(LogLevel::Error);
    logger.log(LogLevel::Warn, "filtered");
    if (out.size() != 1)
        return false;
    logger.log(LogLevel::Error, "passes");
    if (out.size() != 2)
        return false;
    return true;
}

bool log_null_safe() {
    Logger logger(nullptr);
    logger.log(LogLevel::Info, "no crash");
    logger.debug("also no crash");
    return true;
}

bool log_toString_level() {
    if (toString(LogLevel::Trace).compare("Trace") != 0)
        return false;
    if (toString(LogLevel::Debug).compare("Debug") != 0)
        return false;
    if (toString(LogLevel::Info).compare("Info") != 0)
        return false;
    if (toString(LogLevel::Warn).compare("Warn") != 0)
        return false;
    if (toString(LogLevel::Error).compare("Error") != 0)
        return false;
    return true;
}

bool log_parse_level() {
    // CLI --log-level names map 1:1 to enumerators (trace=0 ... error=4).
    const std::pair<std::string_view, LogLevel> cases[] = {
        {"trace", LogLevel::Trace}, {"debug", LogLevel::Debug}, {"info", LogLevel::Info},
        {"warn", LogLevel::Warn},   {"error", LogLevel::Error},
    };
    for (const auto& [name, lvl] : cases) {
        auto got = parseLogLevel(name);
        if (!got || *got != lvl)
            return false;
        if (std::to_underlying(*got) != std::to_underlying(lvl))
            return false;
    }
    if (std::to_underlying(LogLevel::Trace) != 0)
        return false;
    if (std::to_underlying(LogLevel::Error) != 4)
        return false;
    if (parseLogLevel("bogus").has_value())
        return false;
    if (parseLogLevel("INFO").has_value())
        return false;
    if (parseLogLevel("").has_value())
        return false;
    return true;
}

bool log_trace_filter() {
    std::vector<std::pair<LogLevel, std::string>> out;
    Logger logger(
        [&](LogLevel lvl, std::string_view msg) { out.emplace_back(lvl, std::string(msg)); });
    // Default minLevel is Debug: trace is filtered, everything else passes.
    logger.trace("filtered");
    logger.debug("passes");
    if (out.size() != 1)
        return false;
    if (out[0].first != LogLevel::Debug)
        return false;
    return true;
}

bool log_sink_rotation() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto dir = fs::temp_directory_path(ec) / "kanariv-logtest";
    if (ec)
        return false;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec)
        return false;
    kanariv::test_helpers::registerTempPath(dir);
    FileSinkOptions opts;
    opts.maxBytes = 128;
    opts.generations = 3;
    auto sink = FileLogSink::open(dir / "kanariv.log", opts);
    if (!sink || !sink->valid())
        return false;
    Logger logger(sink->callback());
    for (int i = 0; i < 40; ++i)
        logger.info("line number {} padding padding", i);
    // Bounded: current + 2 generations exist, the 4th never does.
    if (!fs::exists(dir / "kanariv.log", ec))
        return false;
    if (!fs::exists(dir / "kanariv.log.1", ec))
        return false;
    if (!fs::exists(dir / "kanariv.log.2", ec))
        return false;
    if (fs::exists(dir / "kanariv.log.3", ec))
        return false;
    // Format: level tag plus the message text.
    std::ifstream in(dir / "kanariv.log");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.find("[INFO ]") == std::string::npos)
        return false;
    if (content.find("[t:") == std::string::npos)
        return false;
    if (content.find("line number") == std::string::npos)
        return false;
    in.close();
    fs::remove_all(dir, ec);
    return true;
}

bool log_sink_open_failure() {
    // Directories are not log files: open fails gracefully, no crash.
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / "kanariv-logtest-dir";
    if (ec)
        return false;
    std::filesystem::create_directories(dir, ec);
    kanariv::test_helpers::registerTempPath(dir);
    auto sink = FileLogSink::open(dir);
    std::filesystem::remove_all(dir, ec);
    return sink == nullptr;
}

} // namespace kanariv::utils::test

using namespace kanariv::utils::test;

TEST_CASE("Logger injected callback", "[utils][log]") {
    REQUIRE(log_injected());
}

TEST_CASE("Logger LogLevel filtering", "[utils][log]") {
    REQUIRE(log_level_filter());
}

TEST_CASE("Logger convenience methods", "[utils][log]") {
    REQUIRE(log_convenience());
}

TEST_CASE("Logger setCallback and setLevel", "[utils][log]") {
    REQUIRE(log_set_callback_level());
}

TEST_CASE("Logger null callback safe", "[utils][log]") {
    REQUIRE(log_null_safe());
}

TEST_CASE("Logger toString LogLevel", "[utils][log]") {
    REQUIRE(log_toString_level());
}

TEST_CASE("Logger parseLogLevel mapping", "[utils][log]") {
    REQUIRE(log_parse_level());
}

TEST_CASE("Logger parseLogLevelValue accepts ints", "[utils][log]") {
    using kanariv::utils::LogLevel;
    using kanariv::utils::parseLogLevelValue;
    REQUIRE(parseLogLevelValue("0") == LogLevel::Trace);
    REQUIRE(parseLogLevelValue("2") == LogLevel::Info);
    REQUIRE(parseLogLevelValue("4") == LogLevel::Error);
    REQUIRE(parseLogLevelValue("info") == LogLevel::Info);
    REQUIRE(!parseLogLevelValue("5").has_value());
    REQUIRE(!parseLogLevelValue("loud").has_value());
    REQUIRE(!parseLogLevelValue("").has_value());
    REQUIRE(!parseLogLevelValue("INFO").has_value());
}

TEST_CASE("Logger trace filtered by default", "[utils][log]") {
    REQUIRE(log_trace_filter());
}

TEST_CASE("FileLogSink rotation stays bounded", "[utils][log]") {
    REQUIRE(log_sink_rotation());
}

TEST_CASE("FileLogSink open failure is silent", "[utils][log]") {
    REQUIRE(log_sink_open_failure());
}

TEST_CASE("Logger enabled fast check", "[utils][log]") {
    kanariv::utils::Logger logger(nullptr, kanariv::utils::LogLevel::Warn);
    // No callback: nothing would be emitted at any level.
    REQUIRE(!logger.enabled(kanariv::utils::LogLevel::Error));
    logger.setCallback([](kanariv::utils::LogLevel, std::string_view) {});
    REQUIRE(!logger.enabled(kanariv::utils::LogLevel::Info));
    REQUIRE(logger.enabled(kanariv::utils::LogLevel::Warn));
    REQUIRE(logger.enabled(kanariv::utils::LogLevel::Error));
    logger.setLevel(kanariv::utils::LogLevel::Trace);
    REQUIRE(logger.enabled(kanariv::utils::LogLevel::Trace));
}
