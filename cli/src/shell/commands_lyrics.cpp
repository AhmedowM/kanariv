/**
 * @file commands_lyrics.cpp
 * @brief Lyrics commands, including the live LRC display.
 */

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/print.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"
#include "shell.hpp"

namespace kanariv::app::cli::detail {

namespace {

struct LrcLine {
    double seconds{0.0};
    std::string text;
};

// Parse LRC text into timestamped lines.
// Handles [mm:ss.xx], [mm:ss.ms], and [mm:ss] timestamp formats.
std::vector<LrcLine> parseLrc(const std::string& lrc_text) {
    std::vector<LrcLine> lines;
    std::istringstream iss(lrc_text);
    std::string raw;
    while (std::getline(iss, raw)) {
        // Strip trailing \r (Windows CRLF)
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        if (raw.empty() || raw[0] != '[')
            continue;
        auto close = raw.find(']');
        if (close == std::string::npos || close < 9)
            continue;
        std::string ts = raw.substr(1, close - 1);
        auto colon = ts.find(':');
        if (colon == std::string::npos)
            continue;
        try {
            int minutes = std::stoi(ts.substr(0, colon));
            std::string rest = ts.substr(colon + 1);
            auto dot = rest.find('.');
            if (dot == std::string::npos) {
                // [mm:ss] no fractional part
                int seconds = std::stoi(rest);
                double total = minutes * 60.0 + seconds;
                std::string text = raw.substr(close + 1);
                while (!text.empty() && text[0] == ' ')
                    text.erase(0, 1);
                if (!text.empty())
                    lines.push_back({total, text});
            } else {
                int seconds = std::stoi(rest.substr(0, dot));
                std::string frac = rest.substr(dot + 1);
                // Truncate/pad fractional part to milliseconds
                while (frac.size() < 3)
                    frac += '0';
                frac.resize(3);
                int ms = std::stoi(frac.substr(0, 3));
                double total = minutes * 60.0 + seconds + ms / 1000.0;
                std::string text = raw.substr(close + 1);
                while (!text.empty() && text[0] == ' ')
                    text.erase(0, 1);
                if (!text.empty())
                    lines.push_back({total, text});
            }
        } catch (...) {
            // Skip malformed timestamp lines
        }
    }
    return lines;
}

/**
 * @brief Streams time-coded lyrics until a bounded exit condition fires.
 * @details Exits 0 on: stop requested (Ctrl+C), playback Stopped, the track
 * changing, or the last line plus a grace window elapsing. Transient transport
 * failures retry with bounded backoff (one stderr notice per outage) instead of
 * aborting; a persistent outage exits 1. ANSI line-clearing is only emitted on
 * a terminal (NO_COLOR/user redirects get one plain line per lyric change).
 */
int renderLyricsLive(const kanariv::ipc::Lyrics& ly, const std::filesystem::path& dbPath,
                     const std::filesystem::path& socketPath, std::stop_token stop, bool ansi) {
    auto lines = parseLrc(ly.lrc_text);
    if (lines.empty()) {
        kanariv::println(std::cerr, "lyrics get --live: no time-coded lyrics found");
        return exitRuntime;
    }

    using namespace std::chrono;
    // Persistent-outage bound: ~20 retries with capped backoff is roughly 30 s.
    constexpr int maxRetries = 20;
    constexpr double tailGrace = 10.0; // seconds after the last timestamp
    // A session reuses one connection: this loop polls every 500 ms, so a
    // per-call connect would churn a daemon thread on every lyric update.
    kanariv::client::Session session{dbPath, socketPath.string()};
    std::size_t shown = lines.size(); // last printed line index (size = none)
    int failures = 0;
    while (!stop.stop_requested()) {
        auto pres = session.waitForStatus(milliseconds{500});
        if (!pres) {
            ++failures;
            if (failures == 1)
                kanariv::println(std::cerr,
                                 "lyrics get --live: lost connection to daemon ({}); retrying",
                                 pres.error().message);
            if (failures > maxRetries) {
                kanariv::println(std::cerr, "lyrics get --live: lost connection to daemon");
                return exitRuntime;
            }
            if (interruptibleSleep(stop, retryBackoff(failures - 1)))
                break;
            continue;
        }
        failures = 0;
        auto* st = std::get_if<kanariv::ipc::Status>(&*pres);
        if (st == nullptr)
            continue;
        // A different track means these lyrics no longer apply.
        if (st->track_id != 0 && ly.track_id != 0 && st->track_id != ly.track_id)
            return exitOk;
        if (st->state == kanariv::types::PlaybackState::Stopped)
            return exitOk;
        if (st->state != kanariv::types::PlaybackState::Playing)
            continue; // paused: leave the last line on screen
        const double pos = st->pos;
        std::size_t idx = 0;
        bool found = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].seconds > pos)
                break;
            idx = i;
            found = true;
        }
        if (found && idx != shown) {
            shown = idx;
            if (ansi) {
                kanariv::print(std::cout, "\r\33[2K{}", lines[idx].text);
                std::cout.flush();
            } else {
                kanariv::println(std::cout, "{}", lines[idx].text);
            }
        }
        // Lyrics exhausted: the last line is displayed and playback has moved
        // past its timestamp by the grace window with no track change.
        if (shown + 1 == lines.size() && pos > lines.back().seconds + tailGrace)
            return exitOk;
    }
    return exitOk;
}

} // namespace

void declareLyrics(CLI::App& cli, Options& o) {
    auto& ly = o.lyrics;
    ly.lyrics = cli.add_subcommand("lyrics", "Lyrics operations");
    ly.set = ly.lyrics->add_subcommand("set", "Set lyrics for a track");
    ly.set->add_option("text", ly.setText, "Lyrics text (LRC format when synced)");
    ly.set->add_flag("--synced", ly.setSynced, "Text is time-coded LRC");
    ly.set->add_option("--file", ly.setFile, "Read lyrics from .lrc file");
    ly.set->add_option("--source", ly.setSource, "Provenance label (default: local)");
    ly.set->add_option("--track", ly.setTrack, "Track id or path (default: current track)");
    addJsonFlag(ly.set, ly.setJson);
    ly.get = ly.lyrics->add_subcommand("get", "Get lyrics for a track");
    ly.get->add_option("--track", ly.getTrack, "Track id or path (default: current track)");
    ly.get->add_option("--out", ly.getOut, "Write lyrics to .lrc file");
    ly.get->add_flag("--live", ly.getLive, "Display live lyrics synced to playback");
    addJsonFlag(ly.get, ly.getJson);
    ly.remove = ly.lyrics->add_subcommand("remove", "Remove lyrics for a track");
    ly.remove->add_option("--track", ly.removeTrack, "Track id or path (default: current track)");
    addJsonFlag(ly.remove, ly.removeJson);
}

std::optional<int> runLyrics(App& app, const config::Config& cfg, std::stop_token stop,
                             const Options& o) {
    const auto& ly = o.lyrics;
    if (!ly.lyrics->parsed())
        return std::nullopt;
    if (ly.set->parsed()) {
        std::string lrc_text = ly.setText;
        bool synced = ly.setSynced;
        if (!ly.setFile.empty()) {
            std::ifstream ifs(ly.setFile, std::ios::binary);
            if (!ifs) {
                kanariv::println(std::cerr, "lyrics set: cannot open file '{}'", ly.setFile);
                return exitRuntime;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            lrc_text = ss.str();
            // .lrc files are inherently time-coded
            synced = true;
        } else if (lrc_text.empty()) {
            kanariv::println(std::cerr, "lyrics set: provide TEXT or --file <path>");
            return exitUsage;
        }
        return render(app.lyricsSet(std::move(lrc_text), synced, ly.setSource,
                                    ly.setTrack.empty() ? std::nullopt
                                                        : std::optional<std::string>{ly.setTrack}),
                      ly.setJson);
    }
    if (ly.get->parsed()) {
        auto res = app.lyricsGet(ly.getTrack.empty() ? std::nullopt
                                                     : std::optional<std::string>{ly.getTrack});
        if (!res)
            return renderError(res.error());
        auto* outcome = &*res;
        if (!outcome->result) {
            kanariv::println(std::cerr, "lyrics get: no result from daemon");
            return exitRuntime;
        }
        auto& ipc_res = *outcome->result;
        if (ly.getLive) {
            if (auto* data = std::get_if<kanariv::ipc::Lyrics>(&ipc_res))
                return renderLyricsLive(*data, cfg.dbPath, cfg.socketPath, stop, useColor());
            kanariv::println(std::cerr, "lyrics get --live: no lyrics data");
            return exitRuntime;
        }
        if (auto* data = std::get_if<kanariv::ipc::Lyrics>(&ipc_res)) {
            if (!ly.getOut.empty()) {
                std::ofstream ofs(ly.getOut, std::ios::binary | std::ios::trunc);
                if (!ofs) {
                    kanariv::println(std::cerr, "lyrics get: cannot write to file '{}'", ly.getOut);
                    return exitRuntime;
                }
                ofs << data->lrc_text;
                ofs.flush();
                kanariv::println(std::cout, "Wrote lyrics ({} bytes) to {}", data->lrc_text.size(),
                                 ly.getOut);
                return exitOk;
            }
            if (ly.getJson) {
                kanariv::println("{}", kanariv::ipc::toJson(ipc_res).dump());
                return exitOk;
            }
            std::string line;
            if (!data->lrc_text.empty())
                line = std::format("Lyrics for track {} ({}):\n{}", data->track_id,
                                   data->is_synced ? "synced" : "plain", data->lrc_text);
            else
                line = std::format("No lyrics text for track {}", data->track_id);
            kanariv::println(std::cout, "{}", line);
            return exitOk;
        }
        return render(std::move(res), ly.getJson);
    }
    if (ly.remove->parsed())
        return render(app.lyricsRemove(ly.removeTrack.empty()
                                           ? std::nullopt
                                           : std::optional<std::string>{ly.removeTrack}),
                      ly.removeJson);
    std::cout << ly.lyrics->help() << "\n";
    return exitOk;
}

} // namespace kanariv::app::cli::detail
