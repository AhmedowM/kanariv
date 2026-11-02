#pragma once

#include <kanariv/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/utils/error.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace CLI {
class App;
}

namespace kanariv::app::cli {

namespace detail {

// CLI-internal helpers (adapt parse.hpp string-errors into utils::Error).

/// @brief Parses `s`/`mm:ss`/`hh:mm:ss` into seconds (see parse.hpp).
std::expected<double, kanariv::utils::Error> parseTime(std::string_view s);
/// @brief Parses absolute or `+`/`-` relative seeks into seconds-or-delta.
std::expected<double, kanariv::utils::Error> parseSeek(std::string_view s);
/// @brief Parses `0-100`/`+n`/`-n`/`mute`/`unmute`/empty(show) into VolumeSet.
std::expected<kanariv::ipc::VolumeSet, kanariv::utils::Error> parseVolume(std::string_view s);
/// @brief parseTime result as a duration.
std::chrono::duration<double> parseDuration(std::string_view s);

} // namespace detail

using detail::parseSeek;
using detail::parseTime;
using detail::parseVolume;

class Shell {
  public:
    explicit Shell(kanariv::config::Config cfg);
    ~Shell();
    int run(int argc, char** argv);

  private:
    kanariv::config::Config config_;
    kanariv::app::App app_;
    std::unique_ptr<CLI::App> cli_;
};

} // namespace kanariv::app::cli
