/**
 * @file commands_playback.cpp
 * @brief Transport, status (incl. watch) and volume commands.
 */

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/print.hpp>
#include <string>
#include <string_view>
#include <variant>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"
#include "shell.hpp"

namespace kanariv::app::cli::detail {

void declarePlayback(CLI::App& cli, Options& o) {
    auto& p = o.playback;
    p.play = cli.add_subcommand("play", "Play current queue");
    addJsonFlag(p.play, p.playJson);
    p.pause = cli.add_subcommand("pause", "Pause playback");
    addJsonFlag(p.pause, p.pauseJson);
    p.resume = cli.add_subcommand("resume", "Resume playback");
    addJsonFlag(p.resume, p.resumeJson);
    p.restart = cli.add_subcommand("restart", "Restart current track");
    addJsonFlag(p.restart, p.restartJson);
    p.stop = cli.add_subcommand("stop", "Stop playback");
    addJsonFlag(p.stop, p.stopJson);
    p.next = cli.add_subcommand("next", "Next track");
    addJsonFlag(p.next, p.nextJson);
    p.prev = cli.add_subcommand("prev", "Prev track");
    addJsonFlag(p.prev, p.prevJson);
    p.seek = cli.add_subcommand("seek", "Seek to position");
    p.seek->add_option("time", p.seekStr, "mm:ss or seconds or +N/-N")->required();
    addJsonFlag(p.seek, p.seekJson);
    p.status = cli.add_subcommand("status", "Show status");
    addJsonFlag(p.status, p.statusJson);
    p.status->add_flag("--watch", p.statusWatch, "Continuous status stream (alias: --follow)");
    p.status->add_flag("--follow", p.statusFollow, "Continuous status stream (alias: --watch)");
    p.status->add_option("--interval", p.statusInterval, "Poll interval in ms");
    p.volume = cli.add_subcommand("volume", "Get/set volume");
    p.volume->add_option("level", p.volumeArg, "0-100|+N|-N|mute|unmute");
    addJsonFlag(p.volume, p.volumeJson);
}

std::optional<int> runPlayback(App& app, const config::Config& cfg, std::stop_token stop,
                               const Options& o) {
    const auto& p = o.playback;
    // Transport commands print one-line confirmations (no ids, no queue
    // positions -- see `info` / `queue list` for those). JSON dumps raw.
    if (p.play->parsed())
        return render(app.play(), p.playJson);
    if (p.pause->parsed())
        return render(app.pause(), p.pauseJson);
    if (p.resume->parsed())
        return render(app.resume(), p.resumeJson);
    if (p.restart->parsed())
        return render(app.restart(), p.restartJson);
    if (p.stop->parsed())
        return render(app.stop(), p.stopJson);
    if (p.next->parsed())
        return render(app.next(), p.nextJson);
    if (p.prev->parsed())
        return render(app.prev(), p.prevJson);
    if (p.seek->parsed()) {
        auto parsed = parseSeek(p.seekStr);
        if (!parsed) {
            kanariv::println(std::cerr,
                             "seek: '{}': {} (expected mm:ss, hh:mm:ss, seconds, or +N/-N)",
                             p.seekStr, parsed.error().message);
            return exitUsage;
        }
        double target = *parsed;
        // parseSeek trims whitespace, so detect +/- on the trimmed form too:
        // " +5" is a +5 delta, not absolute 5.
        std::string_view sv = p.seekStr;
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
            sv.remove_prefix(1);
        bool isRelative = !sv.empty() && (sv.front() == '+' || sv.front() == '-');
        return render(app.seek(target, isRelative), p.seekJson);
    }
    if (p.status->parsed()) {
        // NOTE: the watch loop stays in the shell on purpose: carriage-return
        // timestamps and ANSI output are terminal rendering, not reusable
        // behavior. Phase 1 will replace it with a status-stream API.
        const bool follow = p.statusWatch || p.statusFollow;
        if (p.statusWatch && p.statusFollow) {
            kanariv::println(std::cerr, "status: --watch and --follow are aliases; pass only one");
            return exitUsage;
        }
        if (follow) {
            if (p.statusInterval <= 0) {
                kanariv::println(std::cerr, "status: --interval must be positive (got {})",
                                 p.statusInterval);
                return exitUsage;
            }
            using namespace std::chrono;
            const bool ansi = useColor();
            kanariv::client::OutputFormatter fmt{p.statusJson};
            // One session (and its one connection) for the whole stream.
            kanariv::client::Session session{cfg.dbPath, cfg.socketPath};
            int failures = 0;
            while (!stop.stop_requested()) {
                // waitForStatus blocks until the status changes or the
                // interval elapses, avoiding wasteful polling. The transport
                // budget follows the interval (the session adds its margin),
                // so long intervals are not capped by the 2 s default timeout.
                auto res = session.waitForStatus(milliseconds{p.statusInterval});
                if (!res) {
                    // Transient transport failures (daemon restart, timeout)
                    // are retried with bounded backoff instead of ending the
                    // stream; one notice per outage keeps stderr readable.
                    ++failures;
                    if (failures == 1)
                        kanariv::println(std::cerr, "status: waiting for daemon ({}); retrying",
                                         res.error().message);
                    if (interruptibleSleep(stop, retryBackoff(failures - 1)))
                        break;
                    continue;
                }
                failures = 0;
                if (!p.statusJson) {
                    auto now = system_clock::now();
                    std::time_t t = system_clock::to_time_t(now);
                    std::tm tm{};
#ifdef _WIN32
                    localtime_s(&tm, &t);
#else
                    localtime_r(&t, &tm);
#endif
                    char buf[32];
                    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
                    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
                        if (ansi) {
                            kanariv::print(std::cout, "\r\33[2K[{}] {}", buf, statusLine(*st));
                        } else {
                            kanariv::println(std::cout, "[{}] {}", buf, statusLine(*st));
                        }
                        std::cout.flush();
                    } else {
                        kanariv::println(std::cout, "[{}]", buf);
                        fmt.print(*res, std::cout);
                    }
                } else {
                    // Compact JSON per line for streaming to avoid multi-line log spam
                    kanariv::println(std::cout, "{}", kanariv::ipc::toJson(*res).dump());
                }
                std::cout << std::flush;
            }
            return exitOk;
        }
        return render(app.status(), p.statusJson);
    }
    if (p.volume->parsed()) {
        auto pv = parseVolume(p.volumeArg);
        if (!pv) {
            kanariv::println(std::cerr, "volume: '{}': {} (expected 0-100, +N/-N, mute, or unmute)",
                             p.volumeArg, pv.error().message);
            return exitUsage;
        }
        return render(app.setVolume(*pv), p.volumeJson);
    }
    return std::nullopt;
}

} // namespace kanariv::app::cli::detail
