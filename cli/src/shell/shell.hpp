#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <kanariv/app.hpp>
#include <kanariv/config.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/utils/error.hpp>
#include <memory>
#include <ostream>
#include <stop_token>
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
kanariv::utils::Expected<double> parseTime(std::string_view s);
/// @brief Parses absolute or `+`/`-` relative seeks into seconds-or-delta.
kanariv::utils::Expected<double> parseSeek(std::string_view s);
/// @brief Parses `0-100`/`+n`/`-n`/`mute`/`unmute`/empty(show) into VolumeSet.
kanariv::utils::Expected<kanariv::ipc::VolumeSet> parseVolume(std::string_view s);
/// @brief parseTime result as a duration.
std::chrono::duration<double> parseDuration(std::string_view s);

} // namespace detail

using detail::parseSeek;
using detail::parseTime;
using detail::parseVolume;

class Shell {
  public:
    /**
     * @brief Binds the shell to a config and the process stop token.
     * @param cfg Resolved config (pre-parse; refreshed in run()).
     * @param stop Process stop token observed by every long-running loop;
     *             empty when no signal handling is installed.
     */
    explicit Shell(kanariv::config::Config cfg, std::stop_token stop = {});
    ~Shell();
    int run(int argc, char** argv);

  private:
    kanariv::config::Config config_;
    kanariv::app::App app_;
    std::unique_ptr<CLI::App> cli_;
    std::stop_token stop_;
};

} // namespace kanariv::app::cli
