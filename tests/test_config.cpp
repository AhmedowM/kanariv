#include <catch2/catch_test_macros.hpp>
#include <kanariv/config.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <algorithm>
#include <common.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "../src/config_detail.hpp"

namespace {

class ScopedEnv {
  public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_ = true;
            old_ = old;
        }
        set(value.c_str());
    }
    ~ScopedEnv() {
        set(had_ ? old_.c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : "");
#else
        if (value)
            setenv(name_.c_str(), value, 1);
        else
            unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    std::string old_;
    bool had_{false};
};

// Unique per process/run (and registered for atexit cleanup). Fixed names were
// reused across runs, so a leftover directory from a crashed run could collide.
std::filesystem::path freshTempDir(const char* name) {
    return kanariv::test_helpers::tempDirPath(name);
}

} // namespace

TEST_CASE("default config path follows platform data convention", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_paths");
#ifdef _WIN32
    ScopedEnv localApp("LOCALAPPDATA", tmp.string());
    ScopedEnv xdgCfg("XDG_CONFIG_HOME", (tmp / "xdg").string());
    auto cfg = kanariv::config::detail::defaultConfigPath();
    REQUIRE(cfg == tmp / "kanariv" / "config.json");
    REQUIRE(cfg.parent_path() == kanariv::config::detail::defaultDbPath().parent_path());
#else
    ScopedEnv xdgCfg("XDG_CONFIG_HOME", (tmp / "xdg").string());
    auto cfg = kanariv::config::detail::defaultConfigPath();
    REQUIRE(cfg == tmp / "xdg" / "kanariv" / "config.json");
#endif
}

#ifdef _WIN32
TEST_CASE("legacy Windows config is still read", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_legacy");
    auto newDir = tmp / "new";
    auto home = tmp / "home";
    auto legacyDir = home / ".config" / "kanariv";
    std::error_code ec;
    std::filesystem::create_directories(legacyDir, ec);
    REQUIRE(!ec);
    {
        std::ofstream out(legacyDir / "config.json");
        REQUIRE(out.good());
        out << "{}";
    }
    ScopedEnv localApp("LOCALAPPDATA", newDir.string());
    ScopedEnv userProfile("USERPROFILE", home.string());

    auto cfg = kanariv::config::loadConfig({});
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->configPath == legacyDir / "config.json");
}
#endif

TEST_CASE("config load/save playback-policy keys", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_policy");
    auto cfgPath = tmp / "config.json";

    // Write a config file with playback-policy keys.
    {
        std::ofstream out(cfgPath);
        REQUIRE(out.good());
        out << R"({
  "clearStopsPlayback": false,
  "repeatDefault": "all",
  "autostart": true
})";
    }

    auto loaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->clearStopsPlayback == false);
    REQUIRE(loaded->repeatDefault == kanariv::types::RepeatMode::All);
    REQUIRE(loaded->autostart == true);

    // Save and reload to verify round-trip.
    auto saved = kanariv::config::saveConfig(*loaded);
    REQUIRE(saved.has_value());

    auto reloaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->clearStopsPlayback == false);
    REQUIRE(reloaded->repeatDefault == kanariv::types::RepeatMode::All);
    REQUIRE(reloaded->autostart == true);
}

TEST_CASE("config separates crossfade from inter-track gap", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_crossfade");
    auto cfgPath = tmp / "config.json";

    SECTION("explicit keys are kept apart and round-trip") {
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << R"({
  "crossfadeMs": 700,
  "interTrackGapMs": 250
})";
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->crossfadeMs == 700);
        REQUIRE(loaded->interTrackGapMs == 250);

        REQUIRE(kanariv::config::saveConfig(*loaded).has_value());
        auto reloaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(reloaded.has_value());
        REQUIRE(reloaded->crossfadeMs == 700);
        REQUIRE(reloaded->interTrackGapMs == 250);
    }

    SECTION("legacy gap-only config keeps its crossfade behaviour") {
        // Before the split, interTrackGapMs configured the crossfade overlap.
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << R"({"interTrackGapMs": 400})";
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->crossfadeMs == 400);
        REQUIRE(loaded->interTrackGapMs == 0);
    }
}

TEST_CASE("config keeps the device id and name together", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_device");
    auto cfgPath = tmp / "config.json";

    SECTION("opaque id and display name round-trip") {
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << R"({
  "device": "alsa:68773a",
  "deviceName": "Built-in Audio"
})";
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->device == "alsa:68773a");
        REQUIRE(loaded->deviceName == "Built-in Audio");

        REQUIRE(kanariv::config::saveConfig(*loaded).has_value());
        auto reloaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(reloaded.has_value());
        REQUIRE(reloaded->device == "alsa:68773a");
        REQUIRE(reloaded->deviceName == "Built-in Audio");
    }

    SECTION("a legacy numeric id is preserved for the resolver") {
        // Old configs stored the enumeration index; it is interpreted at open
        // time, so loading must not guess a name or rewrite it.
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << R"({"device": "1"})";
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->device == "1");
        REQUIRE(loaded->deviceName.empty());
    }
}

TEST_CASE("config defaults for playback-policy keys", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_defaults");
    auto cfgPath = tmp / "config.json";

    // Empty config file -> defaults.
    {
        std::ofstream out(cfgPath);
        REQUIRE(out.good());
        out << "{}";
    }

    auto loaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->clearStopsPlayback == true);                       // default
    REQUIRE(loaded->repeatDefault == kanariv::types::RepeatMode::Off); // default
    REQUIRE(loaded->autostart == false);                               // default
}

TEST_CASE("config repeatDefault parses case-insensitively to RepeatMode", "[config]") {
    using kanariv::types::RepeatMode;
    struct Case {
        const char* json_text;
        RepeatMode expected;
    };
    const Case cases[] = {
        {"\"off\"", RepeatMode::Off}, {"\"all\"", RepeatMode::All}, {"\"one\"", RepeatMode::One},
        {"\"ALL\"", RepeatMode::All}, {"\"One\"", RepeatMode::One}, {"\"bogus\"", RepeatMode::Off},
        {"\"\"", RepeatMode::Off},
    };
    for (const auto& c : cases) {
        auto tmp = freshTempDir("kanariv_test_config_repeat");
        auto cfgPath = tmp / "config.json";
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << R"({"repeatDefault": )" << c.json_text << "}";
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->repeatDefault == c.expected);
    }
}

TEST_CASE("config repeatDefault ignores non-string values", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_repeat_type");
    auto cfgPath = tmp / "config.json";
    {
        std::ofstream out(cfgPath);
        REQUIRE(out.good());
        out << R"({"repeatDefault": 2})";
    }
    auto loaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->repeatDefault == kanariv::types::RepeatMode::Off);
}

TEST_CASE("config replayGain parses to ReplayGainMode", "[config]") {
    using kanariv::types::ReplayGainMode;
    struct Case {
        const char* json_text;
        ReplayGainMode expected;
    };
    const Case cases[] = {
        {"\"off\"", ReplayGainMode::Off},     {"\"track\"", ReplayGainMode::Track},
        {"\"album\"", ReplayGainMode::Album}, {"\"TRACK\"", ReplayGainMode::Track},
        {"\"Album\"", ReplayGainMode::Album}, {"\"bogus\"", ReplayGainMode::Off},
    };
    for (const auto& c : cases) {
        auto tmp = freshTempDir("kanariv_test_config_replaygain");
        auto cfgPath = tmp / "config.json";
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << R"({"replayGain": )" << c.json_text << "}";
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->replayGain == c.expected);
    }
}

TEST_CASE("config replayGain round-trips as a lowercase name", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_replaygain_save");
    auto cfgPath = tmp / "config.json";
    kanariv::config::Config cfg{};
    cfg.configPath = cfgPath;
    cfg.dbPath = tmp / "library.db";
    cfg.replayGain = kanariv::types::ReplayGainMode::Album;

    REQUIRE(kanariv::config::saveConfig(cfg).has_value());
    std::ifstream in(cfgPath);
    REQUIRE(in.good());
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    REQUIRE(content.find("\"replayGain\"") != std::string::npos);
    REQUIRE(content.find("\"album\"") != std::string::npos);

    auto loaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->replayGain == kanariv::types::ReplayGainMode::Album);
}

TEST_CASE("config logLevel is clamped to the logger vocabulary", "[config]") {
    struct Case {
        const char* json_text;
        int expected;
    };
    const Case cases[] = {
        {"{\"logLevel\": 9}", 4},        {"{\"logLevel\": -1}", 0},
        {"{\"logLevel\": 3}", 3},        {"{\"log_level\": 9}", 4},
        {"{\"log_level\": -7}", 0},      {"{\"logLevel\": \"warn\"}", 3},
        {"{\"logLevel\": \"info\"}", 2},
    };
    for (const auto& c : cases) {
        auto tmp = freshTempDir("kanariv_test_config_loglevel");
        auto cfgPath = tmp / "config.json";
        {
            std::ofstream out(cfgPath);
            REQUIRE(out.good());
            out << c.json_text;
        }
        auto loaded = kanariv::config::loadConfig(cfgPath);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->logLevel == c.expected);
    }
}

TEST_CASE("config reports unknown keys without failing the load", "[config]") {
    auto tmp = freshTempDir("kanariv_test_config_unknown");
    auto cfgPath = tmp / "config.json";
    {
        std::ofstream out(cfgPath);
        REQUIRE(out.good());
        out << R"({
  "dbPath": "library.db",
  "fadeInMS": 100,
  "replaygain": "album",
  "log_level": "warn"
})";
    }
    auto loaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(loaded.has_value());
    const auto& unknown = loaded->unknownKeys;
    REQUIRE(unknown.size() == 2);
    REQUIRE(std::find(unknown.begin(), unknown.end(), "fadeInMS") != unknown.end());
    REQUIRE(std::find(unknown.begin(), unknown.end(), "replaygain") != unknown.end());
    // Legacy spellings are understood, not reported.
    REQUIRE(std::find(unknown.begin(), unknown.end(), "log_level") == unknown.end());

    // Diagnostics are never persisted: the rewritten file is clean.
    REQUIRE(kanariv::config::saveConfig(*loaded).has_value());
    auto reloaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->unknownKeys.empty());
}

TEST_CASE("endpoint id is a stable FNV-1a digest", "[config][endpoint]") {
    using kanariv::config::detail::endpointIdForDb;
    // Fixed golden values: changing the digest or its canonicalisation renames every
    // on-disk endpoint, so an update here must be deliberate and documented.
    REQUIRE(endpointIdForDb(std::filesystem::path("data/library.db")) == "4f477ef70d3ca00b");
    REQUIRE(endpointIdForDb(std::filesystem::path("C:/kanariv/library.db")) == "1b00c40520a0a75b");
    REQUIRE(endpointIdForDb(std::filesystem::path{}) == "cbf29ce484222325");
    // Path composition and separator normalisation do not change the identity.
    REQUIRE(endpointIdForDb(std::filesystem::path("data") / "library.db") ==
            endpointIdForDb(std::filesystem::path("data/library.db")));
}

TEST_CASE("socket/pid/lock share one endpoint digest", "[config][endpoint]") {
    const auto dbPath = std::filesystem::path("data") / "library.db";
    const std::string id = kanariv::config::detail::endpointIdForDb(dbPath);
    REQUIRE(id.size() == 16);

    auto pid = kanariv::config::pidPathFor(dbPath);
    REQUIRE(pid.has_value());
    REQUIRE(pid->filename().string() == "kanariv-" + id + ".pid");

    auto lock = kanariv::config::lockPathFor(dbPath);
    REQUIRE(lock.has_value());
    REQUIRE(lock->filename().string() == "kanariv-" + id + ".lock");

    auto sock = kanariv::config::socketPathFor(dbPath);
    REQUIRE(sock.has_value());
#ifdef _WIN32
    REQUIRE(*sock == "\\\\.\\pipe\\kanariv-" + id);
#else
    REQUIRE(sock->ends_with("kanariv-" + id + ".sock"));
#endif
}

TEST_CASE("config save replaces atomically without temp leftovers", "[config][save]") {
    auto tmp = freshTempDir("kanariv_test_config_save");
    auto cfgPath = tmp / "config.json";
    kanariv::config::Config cfg{};
    cfg.configPath = cfgPath;
    cfg.dbPath = tmp / "library.db";
    cfg.device = "device-one";
    cfg.repeatDefault = kanariv::types::RepeatMode::One;

    REQUIRE(kanariv::config::saveConfig(cfg).has_value());
    auto tmpPath = cfgPath;
    tmpPath += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(tmpPath));

    auto first = kanariv::config::loadConfig(cfgPath);
    REQUIRE(first.has_value());
    REQUIRE(first->device == "device-one");
    REQUIRE(first->repeatDefault == kanariv::types::RepeatMode::One);

    // Saving over an existing file replaces its content (and writes lowercase names).
    cfg.device = "device-two";
    cfg.repeatDefault = kanariv::types::RepeatMode::Off;
    REQUIRE(kanariv::config::saveConfig(cfg).has_value());
    REQUIRE_FALSE(std::filesystem::exists(tmpPath));

    auto second = kanariv::config::loadConfig(cfgPath);
    REQUIRE(second.has_value());
    REQUIRE(second->device == "device-two");
    REQUIRE(second->repeatDefault == kanariv::types::RepeatMode::Off);

    std::ifstream in(cfgPath);
    REQUIRE(in.good());
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    REQUIRE(content.find("\"repeatDefault\"") != std::string::npos);
    REQUIRE(content.find("\"off\"") != std::string::npos);
}

TEST_CASE("config save reports an unwritable destination", "[config][save]") {
    // A regular file standing in for the config directory fails on every platform
    // (a read-only directory does not reliably block the owner on Windows).
    auto tmp = freshTempDir("kanariv_test_config_unwritable");
    auto blocker = tmp / "blocker";
    {
        std::ofstream out(blocker);
        REQUIRE(out.good());
        out << "not a directory";
    }
    kanariv::config::Config cfg{};
    cfg.configPath = blocker / "config.json";

    const auto saved = kanariv::config::saveConfig(cfg);
    REQUIRE_FALSE(saved.has_value());
    REQUIRE(saved.error().code == kanariv::utils::StatusCode::Io);

    auto tmpPath = cfg.configPath;
    tmpPath += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(tmpPath));
}
