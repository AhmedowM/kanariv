#include <catch2/catch_test_macros.hpp>
#include <common.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <kanariv/config.hpp>
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

std::filesystem::path freshTempDir(const char* name) {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / name;
    REQUIRE(!ec);
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE(!ec);
    kanariv::test_helpers::registerTempPath(dir);
    return dir;
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
    REQUIRE(loaded->repeatDefault == "all");
    REQUIRE(loaded->autostart == true);

    // Save and reload to verify round-trip.
    auto saved = kanariv::config::saveConfig(*loaded);
    REQUIRE(saved.has_value());

    auto reloaded = kanariv::config::loadConfig(cfgPath);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->clearStopsPlayback == false);
    REQUIRE(reloaded->repeatDefault == "all");
    REQUIRE(reloaded->autostart == true);
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
    REQUIRE(loaded->clearStopsPlayback == true); // default
    REQUIRE(loaded->repeatDefault == "off");     // default
    REQUIRE(loaded->autostart == false);         // default
}
