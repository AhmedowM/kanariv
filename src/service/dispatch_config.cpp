#include <algorithm>
#include <array>
#include <atomic>
#include <caudio/config.hpp>
#include <caudio/ipc/command.hpp>
#include <caudio/ipc/result.hpp>
#include <caudio/player/output.hpp>
#include <caudio/service/service.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/json.hpp>
#include <caudio/utils/log.hpp>
#include <caudio/utils/result.hpp>
#include <caudio/utils/ring.hpp>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "config/raw.hpp"
#include "service_impl.hpp"
#include "service_paths.hpp"

namespace caudio::service {

using namespace caudio::ipc;
using namespace caudio::config;
using caudio::ipc::ConfigExport;
using caudio::ipc::ConfigGet;
using caudio::ipc::ConfigImport;
using caudio::ipc::ConfigList;
using caudio::ipc::ConfigReset;
using caudio::ipc::ConfigSet;
using caudio::ipc::DeviceList;
using caudio::ipc::DeviceSet;
using caudio::ipc::DeviceTest;
using caudio::ipc::Shutdown;

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::ConfigGet& cmd) {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    auto vRes = caudio::config::detail::configGetRaw(p, cmd.key);
    if (!vRes)
        return std::unexpected{vRes.error()};
    return Result{ConfigValue{cmd.key, *vRes}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::ConfigSet& cmd) {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    // logLevel applies live: validate first (garbage is rejected, never
    // stored), canonicalize names to ints on write, then setLevel.
    bool isLevel = (cmd.key == "logLevel" || cmd.key == "log_level");
    caudio::utils::LogLevel newLevel = caudio::utils::LogLevel::Info;
    std::string storedValue = cmd.value;
    if (isLevel) {
        auto lvl = caudio::utils::parseLogLevelValue(cmd.value);
        if (!lvl)
            return std::unexpected{
                caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg,
                                         "invalid logLevel '" + cmd.value +
                                             "' (expected trace|debug|info|warn|error or 0-4)")};
        newLevel = *lvl;
        storedValue = std::to_string(std::to_underlying(newLevel));
    }
    auto sRes = caudio::config::detail::configSetRaw(p, cmd.key, storedValue);
    if (!sRes)
        return std::unexpected{sRes.error()};
    if (isLevel && logger_) {
        logger_->setLevel(newLevel);
        logger_->info("log level set to {}", caudio::utils::toString(newLevel));
    }
    return Result{Empty{}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::ConfigList&) {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    auto lRes = caudio::config::detail::configListRaw(p);
    if (!lRes)
        return std::unexpected{lRes.error()};
    ConfigValues cvs{};
    cvs.values.reserve(lRes->size());
    for (auto& kv : *lRes)
        cvs.values.push_back(ConfigValue{kv.key, kv.value});
    return Result{std::move(cvs)};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::ConfigExport& cmd) {
    auto src = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    std::filesystem::path dst{cmd.path};
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::NotFound, "config not found")};
    }
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, ec.message())};
    return Result{Empty{}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::ConfigImport& cmd) {
    std::filesystem::path src{cmd.path};
    auto dst = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::NotFound, "import path not found")};
    }
    // Validate BEFORE touching the live config: syntax, object shape, and
    // known-key types. Unknown keys are ignored (forward compatibility).
    std::string content;
    {
        std::ifstream ifs(src, std::ios::binary);
        if (!ifs)
            return std::unexpected{
                caudio::utils::makeError(caudio::utils::StatusCode::Io, "import file unreadable")};
        content =
            std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    auto parsed = caudio::utils::Json::tryParse(content);
    if (!parsed)
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::Corrupt,
                                                        "import is not valid JSON")};
    if (!parsed->isObject())
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg,
                                                        "import must be a JSON object")};
    auto checkString = [&](const char* key) -> std::optional<caudio::utils::Error> {
        auto v = parsed->at(key);
        if (v && !v->isString())
            return caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg,
                                            std::string("import key '") + key +
                                                "' must be a string");
        return std::nullopt;
    };
    for (const char* k : {"dbPath", "device", "socketPath", "db_path"}) {
        if (auto e = checkString(k))
            return std::unexpected{*e};
    }
    for (const char* k : {"logLevel", "log_level"}) {
        auto v = parsed->at(k);
        if (!v || v->isNull() || v->isNumber())
            continue;
        // Names are tolerated (loadConfig canonicalizes); garbage is not.
        if (v->isString() && caudio::utils::parseLogLevelValue(v->get<std::string>()))
            continue;
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg,
                                                        std::string("import key '") + k +
                                                            "' must be a number or level name")};
    }
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::Io, ec.message())};
    return Result{Empty{}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::ConfigReset& cmd) {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    if (cmd.key.has_value() && !cmd.key->empty()) {
        auto r = caudio::config::detail::configDeleteRaw(p, *cmd.key);
        if (!r)
            return std::unexpected{r.error()};
    } else if (cmd.key.has_value() && cmd.key->empty()) {
        return std::unexpected{
            caudio::utils::makeError(caudio::utils::StatusCode::InvalidArg, "empty key")};
    } else {
        auto r = caudio::config::detail::configResetAllRaw(p);
        if (!r)
            return std::unexpected{r.error()};
    }
    return Result{Empty{}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::Shutdown&) {
    shutdownRequested_.store(true, std::memory_order_release);
    // defer actual shutdown to run loop to avoid deadlock
    return Result{Empty{}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::DeviceList&) {
    auto devList = caudio::player::enumerateDevices();
    caudio::ipc::Devices result;
    result.devices = std::move(devList.devices);
    return Result{std::move(result)};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::DeviceSet& cmd) {
    // Validate device exists
    auto devList = caudio::player::enumerateDevices();
    bool found = false;
    for (const auto& d : devList.devices) {
        if (d.id == cmd.id) {
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::NotFound,
                                                        "device not found: " + cmd.id)};
    }
    // Save to config
    auto cfgPath = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    auto res = caudio::config::detail::configSetRaw(cfgPath, "device", cmd.id);
    if (!res) {
        return std::unexpected{res.error()};
    }
    // Update engine's device if running - the engine will pick it up on next playback
    // For now, just persist the config
    return Result{Empty{}};
}

std::expected<caudio::ipc::Result, caudio::utils::Error>
Service::Impl::handle(const caudio::ipc::DeviceTest& cmd) {
    // Get device ID to test
    std::string testId;
    if (cmd.id.has_value()) {
        testId = *cmd.id;
    } else {
        // Use current config device
        auto cfgPath = detail::resolveConfigPath(config_.configPath, config_.dbPath);
        auto devRes = caudio::config::detail::configGetRaw(cfgPath, "device");
        if (devRes) {
            testId = *devRes;
        } else {
            testId = "auto";
        }
    }
    // Enumeration check: verify the id resolves against listed devices.
    auto devList = caudio::player::enumerateDevices();
    bool found = false;
    for (const auto& d : devList.devices) {
        if (d.id == testId || (testId == "auto" && d.isDefault)) {
            found = true;
            break;
        }
    }
    if (!found && testId != "auto") {
        return std::unexpected{caudio::utils::makeError(caudio::utils::StatusCode::NotFound,
                                                        "device not found: " + testId)};
    }
    // Tone test: generate and play an audible sine wave through the device.
    if (cmd.tone) {
        constexpr std::uint32_t sampleRate = 48000;
        constexpr std::uint32_t channels = 2;
        constexpr double frequency = 440.0; // A4 concert pitch
        constexpr double durationSec = 1.0;
        constexpr std::size_t ringCapacity = 2048; // ~42 ms of audio

        caudio::utils::SpscRing<float> ring(ringCapacity, channels);
        auto out = caudio::player::AudioOutput::create(
            caudio::player::AudioOutput::Config{sampleRate, channels, &ring, 0.3f, nullptr});
        if (!out)
            return std::unexpected{out.error()};

        (*out)->start();

        const std::size_t totalFrames =
            sampleRate * static_cast<std::size_t>(durationSec);
        double phase = 0.0;
        const double phaseStep = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
        std::array<float, 2> frameBuf{};
        std::size_t framesWritten = 0;
        while (framesWritten < totalFrames) {
            const std::size_t framesToWrite =
                std::min<std::size_t>(ring.availableWrite(), totalFrames - framesWritten);
            if (framesToWrite == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            for (std::size_t f = 0; f < framesToWrite; ++f) {
                const float sample =
                    static_cast<float>(0.3 * std::sin(phase));
                frameBuf[0] = sample;
                frameBuf[1] = sample;
                ring.write(frameBuf.data(), 1);
                phase += phaseStep;
                if (phase > 2.0 * 3.14159265358979323846)
                    phase -= 2.0 * 3.14159265358979323846;
            }
            framesWritten += framesToWrite;
        }

        // Wait for the tone to finish playing through the device.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(durationSec * 1000) + 500));
        (*out)->stop();
        (*out)->shutdown();
    }
    return Result{Empty{}};
}

} // namespace caudio::service
