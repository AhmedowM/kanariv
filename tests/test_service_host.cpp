#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <common.hpp>
#include <filesystem>
#include <fstream>
#include <kanariv/config/runtime.hpp>
#include <kanariv/contracts/database.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/service/host.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

using namespace kanariv::service;
using namespace kanariv::utils;
using namespace kanariv::test_helpers;

namespace {

ServiceConfig inProcessConfig(const std::filesystem::path& dbPath,
                              const std::filesystem::path& logsDir) {
    ServiceConfig cfg;
    cfg.inProcess = true;
    cfg.logToStderr = false;
    cfg.runtime.database = dbPath;
    cfg.runtime.logs = logsDir;
    return cfg;
}

} // namespace

TEST_CASE("host create starts service in-process", "[service][host]") {
    auto dir = tempDirPath("host_start");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes.has_value());
    auto& host = *hostRes;
    REQUIRE(host->running());
    REQUIRE(host->ready());
    REQUIRE_FALSE(host->error().has_value());
    REQUIRE(host->service().running());
    host->stop();
    REQUIRE_FALSE(host->running());
}

TEST_CASE("host requestStop then wait", "[service][host]") {
    auto dir = tempDirPath("host_stop");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes.has_value());
    auto& host = *hostRes;
    REQUIRE(host->running());

    host->requestStop();
    REQUIRE(host->wait());
    REQUIRE_FALSE(host->running());
    REQUIRE_FALSE(host->error().has_value());
}

TEST_CASE("host stop then re-start is a fresh host", "[service][host]") {
    auto dir = tempDirPath("host_restart");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    {
        auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
        REQUIRE(hostRes.has_value());
        auto& host = *hostRes;
        REQUIRE(host->running());
        host->stop();
    }
    // Destructor calls stop(); a new host can bind the same in-process db path.
    auto hostRes2 = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes2.has_value());
    auto& host2 = *hostRes2;
    REQUIRE(host2->running());
    host2->stop();
}

TEST_CASE("host destroy stops worker without explicit stop", "[service][host]") {
    auto dir = tempDirPath("host_destroy");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    {
        auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
        REQUIRE(hostRes.has_value());
        auto& host = *hostRes;
        REQUIRE(host->running());
    }
    // Out of scope: destructor must join the worker and stop the loop.
}

TEST_CASE("host missing database path fails cleanly", "[service][host]") {
    auto dir = tempDirPath("host_fail");
    // A path whose parent is a file (not a directory) cannot be created.
    auto blocker = dir / "blocker";
    {
        std::ofstream f(blocker);
        REQUIRE(f);
    }
    auto badDb = blocker / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(badDb, logsDir));
    REQUIRE_FALSE(hostRes.has_value());
}

TEST_CASE("host exposes service for direct commands", "[service][host]") {
    auto dir = tempDirPath("host_cmd");
    auto dbPath = dir / "library.db";
    auto logsDir = dir / "logs";
    std::filesystem::create_directories(logsDir);

    auto hostRes = Host::create(inProcessConfig(dbPath, logsDir));
    REQUIRE(hostRes.has_value());
    auto& host = *hostRes;
    // The wrapped service is reachable for host-driven command dispatch.
    REQUIRE(host->service().running());
    host->stop();
}

TEST_CASE("runtime paths empty is a no-op", "[service][runtime]") {
    kanariv::config::RuntimePaths empty;
    REQUIRE(empty.empty());
    REQUIRE(empty.ensureDirectories().has_value());
}

TEST_CASE("runtime paths ensure directories", "[service][runtime]") {
    auto dir = tempDirPath("runtime");
    kanariv::config::RuntimePaths rp;
    rp.database = dir / "sub" / "library.db";
    rp.logs = dir / "logs";
    rp.cache = dir / "cache";
    rp.importedMedia = dir / "imported";
    REQUIRE(rp.ensureDirectories().has_value());
    REQUIRE(std::filesystem::is_directory(dir / "logs"));
    REQUIRE(std::filesystem::is_directory(dir / "cache"));
    REQUIRE(std::filesystem::is_directory(dir / "imported"));
    REQUIRE(std::filesystem::exists(dir / "sub")); // database parent created
}
