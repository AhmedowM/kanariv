#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/player/output.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/utils/ring.hpp>
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

namespace kanariv::service {

using namespace kanariv::ipc;
using namespace kanariv::config;
using kanariv::ipc::ConfigExport;
using kanariv::ipc::ConfigGet;
using kanariv::ipc::ConfigImport;
using kanariv::ipc::ConfigList;
using kanariv::ipc::ConfigReset;
using kanariv::ipc::ConfigSet;
using kanariv::ipc::DeviceList;
using kanariv::ipc::DeviceSet;
using kanariv::ipc::DeviceTest;
using kanariv::ipc::Shutdown;

auto Service::Impl::handle(const kanariv::ipc::ConfigGet& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    auto vRes = kanariv::config::detail::configGetRaw(p, cmd.key);
    if (!vRes) {
        return std::unexpected{vRes.error()};
    }
    return Result{ConfigValue{.key = cmd.key, .value = *vRes}};
}

auto Service::Impl::handle(const kanariv::ipc::ConfigSet& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    // logLevel applies live: validate first (garbage is rejected, never
    // stored), canonicalize names to ints on write, then setLevel.
    bool isLevel = (cmd.key == "logLevel" || cmd.key == "log_level");
    kanariv::utils::LogLevel newLevel = kanariv::utils::LogLevel::Info;
    std::string storedValue = cmd.value;
    if (isLevel) {
        auto lvl = kanariv::utils::parseLogLevelValue(cmd.value);
        if (!lvl) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                          "invalid logLevel '" + cmd.value +
                                              "' (expected trace|debug|info|warn|error or 0-4)")};
        }
        newLevel = *lvl;
        storedValue = std::to_string(std::to_underlying(newLevel));
    }
    auto sRes = kanariv::config::detail::configSetRaw(p, cmd.key, storedValue);
    if (!sRes) {
        return std::unexpected{sRes.error()};
    }
    if (isLevel && logger_) {
        logger_->setLevel(newLevel);
        logger_->info("log level set to {}", kanariv::utils::toString(newLevel));
    }
    return Result{Empty{}};
}

auto Service::Impl::handle(const kanariv::ipc::ConfigList&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    auto lRes = kanariv::config::detail::configListRaw(p);
    if (!lRes) {
        return std::unexpected{lRes.error()};
    }
    ConfigValues cvs{};
    cvs.values.reserve(lRes->size());
    for (auto& kv : *lRes) {
        cvs.values.push_back(ConfigValue{.key = kv.key, .value = kv.value});
    }
    return Result{std::move(cvs)};
}

auto Service::Impl::handle(const kanariv::ipc::ConfigExport& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto src = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    std::filesystem::path dst{cmd.path};
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound, "config not found")};
    }
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, ec.message())};
    }
    return Result{Empty{}};
}

auto Service::Impl::handle(const kanariv::ipc::ConfigImport& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    std::filesystem::path src{cmd.path};
    auto dst = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "import path not found")};
    }
    // Validate BEFORE touching the live config: syntax, object shape, and
    // known-key types. Unknown keys are ignored (forward compatibility).
    std::string content;
    {
        std::ifstream ifs(src, std::ios::binary);
        if (!ifs) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                             "import file unreadable")};
        }
        content =
            std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    auto parsed = kanariv::utils::Json::tryParse(content);
    if (!parsed) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                         "import is not valid JSON")};
    }
    if (!parsed->isObject()) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "import must be a JSON object")};
    }
    auto checkString = [&](const char* key) -> std::optional<kanariv::utils::Error> {
        auto v = parsed->at(key);
        if (v && !v->isString()) {
            return kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                             std::string("import key '") + key +
                                                 "' must be a string");
        }
        return std::nullopt;
    };
    for (const char* k : {"dbPath", "device", "socketPath", "db_path"}) {
        if (auto e = checkString(k)) {
            return std::unexpected{*e};
        }
    }
    for (const char* k : {"logLevel", "log_level"}) {
        auto v = parsed->at(k);
        if (!v || v->isNull() || v->isNumber()) {
            continue;
        }
        // Names are tolerated (loadConfig canonicalizes); garbage is not.
        if (v->isString() && kanariv::utils::parseLogLevelValue(v->get<std::string>())) {
            continue;
        }
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         std::string("import key '") + k +
                                                             "' must be a number or level name")};
    }
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Io, ec.message())};
    }
    return Result{Empty{}};
}

auto Service::Impl::handle(const kanariv::ipc::ConfigReset& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto p = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    if (cmd.key.has_value() && !cmd.key->empty()) {
        auto r = kanariv::config::detail::configDeleteRaw(p, *cmd.key);
        if (!r) {
            return std::unexpected{r.error()};
        }
    } else if (cmd.key.has_value() && cmd.key->empty()) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "empty key")};
    } else {
        auto r = kanariv::config::detail::configResetAllRaw(p);
        if (!r) {
            return std::unexpected{r.error()};
        }
    }
    return Result{Empty{}};
}

auto Service::Impl::handle(const kanariv::ipc::Shutdown&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    shutdownRequested_.store(true, std::memory_order_release);
    // defer actual shutdown to run loop to avoid deadlock
    return Result{Empty{}};
}

auto Service::Impl::handle(const kanariv::ipc::DeviceList&)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    auto devList = kanariv::player::enumerateDevices();
    kanariv::ipc::Devices result;
    result.devices = std::move(devList.devices);
    return Result{std::move(result)};
}

auto Service::Impl::handle(const kanariv::ipc::DeviceSet& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    // Validate device exists
    auto devList = kanariv::player::enumerateDevices();
    bool found = false;
    for (const auto& d : devList.devices) {
        if (d.id == cmd.id) {
            found = true;
            break;
        }
    }
    if (!found) {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "device not found: " + cmd.id)};
    }
    // Save to config
    auto cfgPath = detail::resolveConfigPath(config_.configPath, config_.dbPath);
    auto res = kanariv::config::detail::configSetRaw(cfgPath, "device", cmd.id);
    if (!res) {
        return std::unexpected{res.error()};
    }
    // Update engine's device if running - the engine will pick it up on next playback
    // For now, just persist the config
    return Result{Empty{}};
}

auto Service::Impl::handle(const kanariv::ipc::DeviceTest& cmd)
    -> std::expected<kanariv::ipc::Result, kanariv::utils::Error> {
    // Get device ID to test
    std::string testId;
    if (cmd.id.has_value()) {
        testId = *cmd.id;
    } else {
        // Use current config device
        auto cfgPath = detail::resolveConfigPath(config_.configPath, config_.dbPath);
        auto devRes = kanariv::config::detail::configGetRaw(cfgPath, "device");
        if (devRes) {
            testId = *devRes;
        } else {
            testId = "auto";
        }
    }
    // Enumeration check: verify the id resolves against listed devices.
    auto devList = kanariv::player::enumerateDevices();
    bool found = false;
    for (const auto& d : devList.devices) {
        if (d.id == testId || (testId == "auto" && d.isDefault)) {
            found = true;
            break;
        }
    }
    if (!found && testId != "auto") {
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::NotFound,
                                                         "device not found: " + testId)};
    }
    // Tone test: generate and play an audible sine wave through the device.
    if (cmd.tone) {
        constexpr std::uint32_t sampleRate = 48000;
        constexpr std::uint32_t channels = 2;
        constexpr double frequency = 440.0; // A4 concert pitch
        constexpr double durationSec = 1.0;
        constexpr std::size_t ringCapacity = 2048; // ~42 ms of audio
        constexpr double msToSecDivisor = 1000.0;

        constexpr float testVolume = 0.3f;
        constexpr float testAmplitude = 0.3f;
        constexpr double twoPi = 2.0 * 3.14159265358979323846;
        constexpr int toneFinishBufferMs = 500;
        constexpr int sleepWhenRingFullMs = 1;

        kanariv::utils::SpscRing<float> ring(ringCapacity, channels);
        auto out = kanariv::player::AudioOutput::create(
            kanariv::player::AudioOutput::Config{.sampleRate = sampleRate,
                                                 .channels = channels,
                                                 .ring = &ring,
                                                 .volume = testVolume,
                                                 .logger = nullptr});
        if (!out) {
            return std::unexpected{out.error()};
        }

        (*out)->start();

        const std::size_t totalFrames = sampleRate * static_cast<std::size_t>(durationSec);
        double phase = 0.0;
        const double phaseStep = twoPi * frequency / sampleRate;
        std::array<float, 2> frameBuf{};
        std::size_t framesWritten = 0;
        while (framesWritten < totalFrames) {
            const std::size_t framesToWrite =
                std::min<std::size_t>(ring.availableWrite(), totalFrames - framesWritten);
            if (framesToWrite == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepWhenRingFullMs));
                continue;
            }
            for (std::size_t f = 0; f < framesToWrite; ++f) {
                const auto sample = static_cast<float>(testAmplitude * std::sin(phase));
                frameBuf[0] =
                    sample; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                frameBuf[1] =
                    sample; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                ring.write(frameBuf.data(), 1);
                phase += phaseStep;
                if (phase > twoPi) {
                    phase -= twoPi;
                }
            }
            framesWritten += framesToWrite;
        }

        // Wait for the tone to finish playing through the device.
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(durationSec * msToSecDivisor) + toneFinishBufferMs));
        (*out)->stop();
        (*out)->shutdown();
    }
    return Result{Empty{}};
}

} // namespace kanariv::service
