#include <cstdio>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#include <CLI/CLI.hpp>
#include <app/detail.hpp>
#include <kanariv/app/app.hpp>
#include <kanariv/app/paths.hpp>
#include <kanariv/client/client.hpp>
#include <kanariv/config.hpp>
#include <kanariv/db/database.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/service/service.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/print.hpp>
#include <kanariv/utils/result.hpp>
#include <kanariv/version.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "output_formatter.hpp"
#include "parse.hpp"
#include "preview.hpp"
#include "shell.hpp"

namespace kanariv::app::cli {

namespace detail {
// Single source: delegate to kanariv::app::cli::parse::parseTime (parse.hpp) and adapt error type.
// parse.hpp returns expected<double,string>; app layer wraps string into utils::Error.
std::expected<double, kanariv::utils::Error> parseTime(std::string_view s) {
    auto r = kanariv::app::cli::parse::parseTime(s);
    if (!r)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, r.error())};
    return *r;
}
// Single source: delegate to kanariv::app::cli::parse::parseSeek and adapt error type.
std::expected<double, kanariv::utils::Error> parseSeek(std::string_view s) {
    auto r = kanariv::app::cli::parse::parseSeek(s);
    if (!r)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, r.error())};
    return *r;
}
// Single source: delegate to kanariv::app::cli::parse::parseVolume and adapt error type.
std::expected<kanariv::ipc::VolumeSet, kanariv::utils::Error> parseVolume(std::string_view s) {
    auto r = kanariv::app::cli::parse::parseVolume(s);
    if (!r)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, r.error())};
    kanariv::app::cli::parse::ParsedVolume pv = *r;
    kanariv::ipc::VolumeSet vs{};
    vs.level = pv.level;
    vs.mute = pv.mute;
    vs.deltaPct = pv.deltaPct;
    return vs;
}
std::chrono::duration<double> parseDuration(std::string_view s) {
    auto t = parseTime(s);
    if (!t)
        return std::chrono::duration<double>{0};
    return std::chrono::duration<double>{*t};
}

std::string statusLine(const kanariv::ipc::Status& st) {
    std::string s{std::format("{}", st.state)};
    std::string who;
    if (!st.artist.empty() && !st.title.empty())
        who = st.artist + " - " + st.title;
    else
        who = st.artist + st.title;
    if (!who.empty())
        s += " " + who;
    s += " [" + kanariv::utils::formatClock(st.pos) + "/" + kanariv::utils::formatClock(st.dur) + "]";
    return s;
}

// Map StatusCode to a finer exit code (F-13).
// 0 = success, 1 = general error, 105 = daemon not running / bad state,
// 106 = invalid argument, 109 = not found.
int exitCodeForError(const kanariv::utils::Error& e) {
    switch (e.code) {
        case kanariv::utils::StatusCode::InvalidArg:
            return 106;
        case kanariv::utils::StatusCode::NotFound:
            return 109;
        case kanariv::utils::StatusCode::State:
            return 105;
        default:
            return 1;
    }
}

int renderError(const kanariv::utils::Error& e) {
    std::string text;
    kanariv::app::detail::renderErrorInto(text, e);
    kanariv::println(std::cerr, "{}", text);
    return exitCodeForError(e);
}

int renderJson(const kanariv::ipc::Result& r) {
    OutputFormatter fmt{true};
    fmt.print(r, std::cout);
    return 0;
}

// Terminal color detection for renderers (NO_COLOR honored).
bool useColor() {
    if (std::getenv("NO_COLOR") != nullptr)
        return false;
#ifdef _WIN32
    return ::_isatty(::_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

// Renders a Phase-1 outcome: errors (with hint), JSON results, human
// lines, bare results, or silent success, in that order.
int render(kanariv::app::AppResult res, bool asJson, std::string_view columns = {}) {
    if (!res)
        return renderError(res.error());
    auto outcome = std::move(*res);
    if (outcome.failed) {
        if (outcome.line)
            kanariv::println(std::cerr, "{}", *outcome.line);
        return 1;
    }
    if (outcome.quiet && !asJson)
        return 0;
    if (asJson && outcome.result)
        return renderJson(*outcome.result);
    if (outcome.line) {
        if (outcome.toStderr)
            kanariv::println(std::cerr, "{}", *outcome.line);
        else
            kanariv::println(std::cout, "{}", *outcome.line);
        return 0;
    }
    if (outcome.result) {
        OutputFormatter fmt{false};
        if (!columns.empty())
            fmt.setColumns(columns);
        fmt.print(*outcome.result, std::cout);
    }
    return 0;
}

// Flushes a batch report: stderr blob, JSON payload under --json,
// stdout blob in text mode, then the recorded exit code.
int renderBatch(kanariv::app::BatchResult res, bool asJson) {
    if (!res)
        return renderError(res.error());
    auto& rep = *res;
    if (!rep.err.empty())
        kanariv::println(std::cerr, "{}", rep.err);
    if (asJson) {
        if (rep.json) {
            renderJson(*rep.json);
            return 0;
        }
        return rep.exitCode;
    }
    if (!rep.out.empty())
        kanariv::println(std::cout, "{}", rep.out);
    return rep.exitCode;
}

// Prints version plus build provenance for the `version` subcommand. Human
// output is a labelled block; --json emits one flat object.
int renderVersion(bool asJson) {
#if defined(__clang__)
    const std::string compiler = std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
    const std::string compiler = std::string("MSVC ") + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
    const std::string compiler = std::string("GCC ") + __VERSION__;
#else
    const std::string compiler = "unknown";
#endif
#if defined(_LIBCPP_VERSION)
    constexpr std::string_view runtime = "libc++";
#elif defined(_MSVC_STL_VERSION)
    constexpr std::string_view runtime = "MSVC STL";
#elif defined(__GLIBCXX__)
    constexpr std::string_view runtime = "libstdc++";
#else
    constexpr std::string_view runtime = "unknown";
#endif
#if defined(_WIN32)
    constexpr std::string_view platform = "windows";
#elif defined(__APPLE__)
    constexpr std::string_view platform = "macos";
#elif defined(__linux__)
    constexpr std::string_view platform = "linux";
#else
    constexpr std::string_view platform = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr std::string_view arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    constexpr std::string_view arch = "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    constexpr std::string_view arch = "x86";
#else
    constexpr std::string_view arch = "unknown";
#endif
#if defined(NDEBUG)
    constexpr std::string_view build = "release";
#else
    constexpr std::string_view build = "debug";
#endif
    const std::string full(kanariv::version::version());
    const std::string shortv(kanariv::version::shortVersion());
    const std::string commit(kanariv::version::versionCommit());
    const std::string stdv = std::to_string(__cplusplus);
    if (asJson) {
        kanariv::println(
            R"({{"version":"{}","short":"{}","commit":"{}","compiler":"{}","std":"{}","runtime":"{}","build":"{}","os":"{}","arch":"{}"}})",
            full, shortv, commit, compiler, stdv, runtime, build, platform, arch);
        return 0;
    }
    kanariv::println("kanariv {}", full);
    kanariv::println("  short:    {}", shortv);
    kanariv::println("  commit:   {}", commit);
    kanariv::println("  compiler: {}", compiler);
    kanariv::println("  std:      {}", stdv);
    kanariv::println("  runtime:  {}", runtime);
    kanariv::println("  build:    {}", build);
    kanariv::println("  os:       {}", platform);
    kanariv::println("  arch:     {}", arch);
    return 0;
}

// ---------------------------------------------------------------------------
// Live lyrics display
// ---------------------------------------------------------------------------

struct LrcLine {
    double seconds{0.0};
    std::string text;
};

// Parse LRC text into timestamped lines.
// Handles [mm:ss.xx], [mm:ss.ms], and [mm:ss] timestamp formats.
static std::vector<LrcLine> parseLrc(const std::string& lrc_text) {
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

int renderLyricsLive(const kanariv::ipc::Lyrics& ly,
                     const std::filesystem::path& dbPath,
                     const std::filesystem::path& socketPath) {
    auto lines = parseLrc(ly.lrc_text);
    if (lines.empty()) {
        kanariv::println(std::cerr,
                        "lyrics get --live: no time-coded lyrics found");
        return 1;
    }

    using namespace std::chrono;
    while (true) {
        kanariv::client::Client client{dbPath, socketPath.string()};
        auto pres =
            client.waitForStatus(milliseconds{500});
        if (!pres) {
            kanariv::println(std::cerr,
                            "lyrics get --live: lost connection to daemon");
            return 1;
        }
        if (auto* st = std::get_if<kanariv::ipc::Status>(&*pres)) {
            if (st->state == kanariv::types::PlaybackState::Playing) {
                double pos = st->pos;
                const LrcLine* current = nullptr;
                for (const auto& line : lines) {
                    if (line.seconds <= pos)
                        current = &line;
                    else
                        break;
                }
                if (current && !current->text.empty())
                    kanariv::print(std::cout, "\r\33[2K{}", current->text);
                else
                    kanariv::print(std::cout, "\r\33[2K");
                std::cout.flush();
            }
            // If not playing, just keep polling silently.
        }
    }
}

} // namespace detail
using detail::parseSeek;
using detail::parseTime;
using detail::parseVolume;

Shell::Shell(kanariv::config::Config cfg)
    : config_(cfg), app_(std::move(cfg)),
      cli_(std::make_unique<CLI::App>("kanariv - terminal player")) {
    cli_->set_version_flag("--version", std::string(kanariv::version::version()));
}

Shell::~Shell() = default;

int Shell::run(int argc, char** argv) {
    if (argc > 0 && argv && argv[0])
        app_.setArgv0(argv[0]);
    std::string configPathStr;
    std::string logLevelStr;
    std::string deviceStr;
    cli_->add_option("--config", configPathStr, "Config file");
    cli_->add_option("--log-level", logLevelStr, "trace|debug|info|warn|error");
    cli_->add_option("--device", deviceStr, "Audio output device");
    std::vector<std::string> posPaths;
    bool posSave = false;
    cli_->add_option("path", posPaths, "Audio file, folder, or glob (direct play)");
    cli_->add_flag("--save", posSave, "Keep the direct-play queue (default: temporary)");
    bool daemonFlag = false;
    cli_->add_flag("--daemon", daemonFlag, "Internal daemon flag")->group("");
    bool globalFg = false;
    cli_->add_flag("--foreground", globalFg, "")->group("");
    bool fg = false;
    auto* startCmd = cli_->add_subcommand("start", "Start daemon");
    startCmd->add_flag("--foreground", fg, "Run in foreground");
    auto* shutdownCmd = cli_->add_subcommand("shutdown", "Stop daemon");
    bool playJson = false;
    auto* playCmd = cli_->add_subcommand("play", "Play current queue");
    playCmd->add_flag("--json", playJson, "JSON output");
    bool pauseJson = false;
    auto* pauseCmd = cli_->add_subcommand("pause", "Pause playback");
    pauseCmd->add_flag("--json", pauseJson, "JSON output");
    bool resumeJson = false;
    auto* resumeCmd = cli_->add_subcommand("resume", "Resume playback");
    resumeCmd->add_flag("--json", resumeJson, "JSON output");
    bool restartJson = false;
    auto* restartCmd = cli_->add_subcommand("restart", "Restart current track");
    restartCmd->add_flag("--json", restartJson, "JSON output");
    bool stopJson = false;
    auto* stopCmd = cli_->add_subcommand("stop", "Stop playback");
    stopCmd->add_flag("--json", stopJson, "JSON output");
    bool nextJson = false;
    auto* nextCmd = cli_->add_subcommand("next", "Next track");
    nextCmd->add_flag("--json", nextJson, "JSON output");
    bool prevJson = false;
    auto* prevCmd = cli_->add_subcommand("prev", "Prev track");
    prevCmd->add_flag("--json", prevJson, "JSON output");
    std::string seekStr;
    bool seekJson = false;
    auto* seekCmd = cli_->add_subcommand("seek", "Seek to position");
    seekCmd->add_option("time", seekStr, "mm:ss or seconds or +N/-N")->required();
    seekCmd->add_flag("--json", seekJson, "JSON output");
    bool jsonFlag = false;
    bool statusWatch = false;
    int statusInterval = 1000;
    auto* statusCmd = cli_->add_subcommand("status", "Show status");
    statusCmd->add_flag("--json", jsonFlag, "JSON output");
    statusCmd->add_flag("--watch", statusWatch, "Continuous polling");
    statusCmd->add_flag("--follow", statusWatch, "Alias for --watch");
    statusCmd->add_option("--interval", statusInterval, "Poll interval in ms");
    std::string volumeArg;
    bool volumeJson = false;
    auto* volumeCmd = cli_->add_subcommand("volume", "Get/set volume");
    volumeCmd->add_option("level", volumeArg, "0-100|+N|-N|mute|unmute");
    volumeCmd->add_flag("--json", volumeJson, "JSON output");
    auto* queueCmd = cli_->add_subcommand("queue", "Queue operations");
    bool qJson = false;
    auto* qList = queueCmd->add_subcommand("list", "List all queues");
    qList->add_flag("--json", qJson, "JSON output");
    bool qQueuesJson = false;
    auto* qQueuesAlias = queueCmd->add_subcommand("queues", "List all queues (alias)")->group("");
    qQueuesAlias->add_flag("--json", qQueuesJson, "JSON output");
    std::string qTracksOrder = "playback";
    bool qTracksJson = false;
    auto* qTracks = queueCmd->add_subcommand("tracks", "List tracks in active queue");
    qTracks->add_option("--order", qTracksOrder, "Track order: added|playback")
        ->check(CLI::IsMember({"added", "playback"}));
    qTracks->add_flag("--json", qTracksJson, "JSON output");
    std::int64_t qSwitchId = 0;
    bool qSwitchJson = false;
    auto* qSwitch = queueCmd->add_subcommand("switch", "Switch active queue");
    qSwitch->add_option("qid", qSwitchId, "Queue id")->required();
    qSwitch->add_flag("--json", qSwitchJson, "JSON output");
    std::vector<std::string> qAddPaths;
    std::string qAddId;
    bool qAddSearch = false;
    bool qAddRecursive = false;
    std::int64_t qAddPlaylist = 0;
    bool qAddReplace = false;
    bool qAddJson = false;
    auto* qAdd = queueCmd->add_subcommand("add", "Add to queue");
    qAdd->add_option("path", qAddPaths, "File, folder, or glob (repeatable)");
    qAdd->add_option("--id", qAddId, "Library track id");
    qAdd->add_flag("--search", qAddSearch, "Treat path as FTS query (single)");
    qAdd->add_flag("--recursive", qAddRecursive, "Recurse into folders");
    qAdd->add_option("--playlist", qAddPlaylist, "Playlist id to append");
    qAdd->add_flag("--replace", qAddReplace, "Clear queue first (with --playlist)");
    qAdd->add_flag("--json", qAddJson, "JSON output");
    std::vector<std::string> qRemovePaths;
    std::string qRemoveId;
    std::string qRemovePos;
    bool qRemoveRecursive = false;
    bool qRemoveJson = false;
    auto* qRemove = queueCmd->add_subcommand("remove", "Remove from queue");
    qRemove->add_option("path", qRemovePaths, "File, folder, or glob (repeatable)");
    qRemove->add_option("--id", qRemoveId, "Library track id");
    qRemove->add_option("--pos", qRemovePos, "Queue position");
    qRemove->add_flag("--recursive", qRemoveRecursive, "Recurse into folders");
    qRemove->add_flag("--json", qRemoveJson, "JSON output");
    std::string qCreateName;
    bool qCreateJson = false;
    auto* qCreate = queueCmd->add_subcommand("create", "Create a new queue");
    qCreate->add_option("name", qCreateName, "Queue name")->required();
    qCreate->add_flag("--json", qCreateJson, "JSON output");
    std::int64_t qDeleteQid = 0;
    bool qDeleteJson = false;
    auto* qDelete = queueCmd->add_subcommand("delete", "Delete a queue");
    qDelete->add_option("qid", qDeleteQid, "Queue id")->required();
    qDelete->add_flag("--json", qDeleteJson, "JSON output");
    std::size_t qFrom = 0, qTo = 0;
    bool qMoveJson = false;
    auto* qMove = queueCmd->add_subcommand("move", "Move within queue");
    qMove->add_option("from", qFrom, "from index")->required();
    qMove->add_option("to", qTo, "to index")->required();
    qMove->add_flag("--json", qMoveJson, "JSON output");
    bool qClearJson = false;
    auto* qClear = queueCmd->add_subcommand("clear", "Clear queue");
    qClear->add_flag("--json", qClearJson, "JSON output");
    std::string qShuffleArg;
    bool qShuffleJson = false;
    auto* qShuffle = queueCmd->add_subcommand("shuffle", "Set shuffle");
    qShuffle->add_option("mode", qShuffleArg, "on|off");
    qShuffle->add_flag("--json", qShuffleJson, "JSON output");
    std::string qRepeatArg;
    bool qRepeatJson = false;
    auto* qRepeat = queueCmd->add_subcommand("repeat", "Set repeat");
    qRepeat->add_option("mode", qRepeatArg, "off|one|all");
    qRepeat->add_flag("--json", qRepeatJson, "JSON output");
    auto* plCmd = cli_->add_subcommand("playlist", "Playlist operations");
    bool plJson = false;
    auto* plList = plCmd->add_subcommand("list", "List playlists");
    plList->add_flag("--json", plJson, "JSON output");
    std::int64_t plTracksPid = 0;
    bool plTracksJson = false;
    auto* plTracks = plCmd->add_subcommand("tracks", "Tracks in playlist");
    plTracks->add_option("pid", plTracksPid, "Playlist id")->required();
    plTracks->add_flag("--json", plTracksJson, "JSON output");
    std::string plCreateName;
    std::string plCreateSmart;
    bool plCreateJson = false;
    auto* plCreate = plCmd->add_subcommand("create", "Create an empty playlist");
    plCreate->add_option("name", plCreateName, "Playlist name")->required();
    plCreate->add_option("--smart", plCreateSmart, "Smart-playlist filter expression");
    plCreate->add_flag("--json", plCreateJson, "JSON output");
    std::int64_t plAddPid = 0;
    std::vector<std::string> plAddIds;
    std::vector<std::string> plAddPaths;
    bool plAddRecursive = false;
    bool plAddJson = false;
    auto* plAdd = plCmd->add_subcommand("add", "Add tracks to playlist");
    plAdd->add_option("pid", plAddPid, "Playlist id")->required();
    plAdd->add_option("--id", plAddIds, "Library track id (repeatable)");
    plAdd->add_option("path", plAddPaths, "File, folder, or glob (repeatable)");
    plAdd->add_flag("--recursive", plAddRecursive, "Recurse into folders");
    plAdd->add_flag("--json", plAddJson, "JSON output");
    std::int64_t plLoadPid = 0;
    bool plLoadPlay = false;
    bool plLoadReplace = false;
    bool plLoadJson = false;
    auto* plLoad = plCmd->add_subcommand("load", "Load playlist into queue");
    plLoad->add_option("pid", plLoadPid, "Playlist id")->required();
    plLoad->add_flag("--play", plLoadPlay, "Play after load");
    plLoad->add_flag("--replace", plLoadReplace, "Replace active queue");
    plLoad->add_flag("--json", plLoadJson, "JSON output");
    std::string plSaveName;
    std::int64_t plSaveQid = 0;
    bool plSaveJson = false;
    auto* plSave = plCmd->add_subcommand("save", "Save queue as playlist");
    plSave->add_option("name", plSaveName, "Playlist name")->required();
    plSave->add_option("--queue", plSaveQid, "Queue id");
    plSave->add_flag("--json", plSaveJson, "JSON output");
    std::int64_t plDeletePid = 0;
    bool plDeleteJson = false;
    auto* plDelete = plCmd->add_subcommand("delete", "Delete playlist");
    plDelete->add_option("pid", plDeletePid, "Playlist id")->required();
    plDelete->add_flag("--json", plDeleteJson, "JSON output");
    std::int64_t plRenamePid = 0;
    std::string plRenameName;
    bool plRenameJson = false;
    auto* plRename = plCmd->add_subcommand("rename", "Rename playlist");
    plRename->add_option("pid", plRenamePid, "Playlist id")->required();
    plRename->add_option("name", plRenameName, "New name")->required();
    plRename->add_flag("--json", plRenameJson, "JSON output");
    std::int64_t plExportPid = 0;
    std::string plExportPath;
    std::string plExportFormat = "m3u";
    auto* plExport = plCmd->add_subcommand("export", "Export playlist to file");
    plExport->add_option("pid", plExportPid, "Playlist id")->required();
    plExport->add_option("path", plExportPath, "Output file path")->required();
    plExport->add_option("--format", plExportFormat, "Format: m3u|pls|json")
        ->check(CLI::IsMember({"m3u", "pls", "json"}));
    std::string plImportPath;
    std::string plImportName;
    bool plImportJson = false;
    auto* plImport = plCmd->add_subcommand("import", "Import playlist from file");
    plImport->add_option("path", plImportPath, "Input file path")->required();
    plImport->add_option("--name", plImportName, "Playlist name (default: filename)");
    plImport->add_flag("--json", plImportJson, "JSON output");
    auto* libCmd = cli_->add_subcommand("library", "Library operations");
    std::string libScanPath;
    bool libScanFullHash = false;
    bool libScanJson = false;
    auto* libScan = libCmd->add_subcommand("scan", "Scan library");
    libScan->add_option("--path", libScanPath, "Scan path (default: library music dir)");
    libScan->add_flag("--full-hash", libScanFullHash, "Full-file BLAKE3 (slower)");
    libScan->add_flag("--json", libScanJson, "JSON output");
    std::string libSearchQuery;
    int libSearchLimit = 50;
    bool libSearchJson = false;
    auto* libSearch = libCmd->add_subcommand("search", "Search library");
    libSearch->add_option("query", libSearchQuery, "FTS query")->required();
    libSearch->add_option("--limit", libSearchLimit, "Limit");
    libSearch->add_flag("--json", libSearchJson, "JSON output");
    bool libStatsJson = false;
    int libStatsTopTracks = -1;
    int libStatsTopAlbums = 0;
    int libStatsTopGenres = 0;
    int libStatsTopArtists = 0;
    std::vector<std::string> libStatsQueues;
    std::vector<std::int64_t> libStatsPlaylists;
    auto* libStats = libCmd->add_subcommand("stats", "Library stats");
    libStats->add_flag("--json", libStatsJson, "JSON output");
    libStats->add_option("--top-tracks", libStatsTopTracks,
                         "Top-N most played tracks (0 = totals only)");
    libStats->add_option("--top-albums", libStatsTopAlbums,
                         "Top-N albums by total plays (0 = not requested)");
    libStats->add_option("--top-genres", libStatsTopGenres,
                         "Top-N genres by total plays (0 = not requested)");
    libStats->add_option("--top-artists", libStatsTopArtists,
                         "Top-N artists by total plays (multi-artist tracks split)");
    libStats->add_option("--queue", libStatsQueues, "Queue overview: all or id (repeatable)");
    libStats->add_option("--playlist", libStatsPlaylists, "Playlist overview by id (repeatable)");
    std::string libAddPath;
    bool libAddRecursive = false;
    bool libAddJson = false;
    auto* libAdd = libCmd->add_subcommand("add", "Add file or directory to library");
    libAdd->add_option("path", libAddPath, "File or directory path")->required();
    libAdd->add_flag("--recursive", libAddRecursive, "Recurse into subdirectories");
    libAdd->add_flag("--json", libAddJson, "JSON output");
    std::string libRemoveQuery;
    bool libRemoveJson = false;
    auto* libRemove = libCmd->add_subcommand("remove", "Remove track from library");
    libRemove->add_option("id", libRemoveQuery, "Track id or path")->required();
    libRemove->add_flag("--json", libRemoveJson, "JSON output");
    int libListLimit = 50;
    int libListOffset = 0;
    std::string libListQuery;
    std::string libListSort;
    std::string libListOrder;
    std::string libListColumns;
    std::string libListArtist;
    std::string libListAlbum;
    std::string libListGenre;
    int libListRating = 0;
    bool libListJson = false;
    auto* libList = libCmd->add_subcommand("list", "List all tracks in library");
    libList->add_option("--query", libListQuery, "Search query (title, artist, album, genre)");
    libList->add_option("--sort", libListSort, "Sort field: id, title, artist, album, year, rating, play_count, duration, date_added");
    libList->add_option("--order", libListOrder, "Sort direction: asc or desc");
    libList->add_option("--columns", libListColumns, "Column allowlist: id,title,artist,album,genre,year,rating,duration,path");
    libList->add_option("--artist", libListArtist, "Filter by artist");
    libList->add_option("--album", libListAlbum, "Filter by album");
    libList->add_option("--genre", libListGenre, "Filter by genre");
    libList->add_option("--rating", libListRating, "Filter by user rating (0-5)")
        ->check(CLI::Range(0, 5));
    libList->add_option("--limit", libListLimit, "Limit results");
    libList->add_option("--offset", libListOffset, "Offset for pagination");
    libList->add_flag("--json", libListJson, "JSON output");
    int libFavMinRating = 1;
    int libFavLimit = 50;
    bool libFavJson = false;
    auto* libFav = libCmd->add_subcommand("favorites", "List favorited (rated) tracks");
    libFav->add_option("--min-rating", libFavMinRating, "Minimum rating (0-5)")
        ->check(CLI::Range(0, 5));
    libFav->add_option("--limit", libFavLimit, "Limit results");
    libFav->add_flag("--json", libFavJson, "JSON output");
    std::string libMaintAction = "report";
    bool libMaintDryRun = true;
    std::int64_t libMaintLibraryId = 1;
    bool libMaintJson = false;
    auto* libMaint = libCmd->add_subcommand("maintenance",
                                            "Report or prune missing/duplicate tracks");
    libMaint->add_option("action", libMaintAction, "report or prune")
        ->check(CLI::IsMember({"report", "prune"}));
    libMaint->add_flag("--dry-run", libMaintDryRun, "Report only, no changes");
    libMaint->add_option("--library", libMaintLibraryId, "Library id (default: 1)");
    libMaint->add_flag("--json", libMaintJson, "JSON output");
    auto* tagCmd = cli_->add_subcommand("tag", "Tag operations");
    std::string tagEditId;
    std::string tagEditField;
    std::string tagEditValue;
    bool tagEditJson = false;
    auto* tagEdit = tagCmd->add_subcommand("edit", "Edit track tag");
    tagEdit->add_option("id", tagEditId, "Track id or file path")->required();
    tagEdit
        ->add_option("field", tagEditField,
                     "Field (title,artist,album,album_artist,genre,year,track_number,disc_number)")
        ->required()
        ->check(CLI::IsMember({"title", "artist", "album", "album_artist", "genre", "year",
                               "track_number", "disc_number"}));
    tagEdit->add_option("value", tagEditValue, "New value")->required();
    tagEdit->add_flag("--json", tagEditJson, "JSON output");
    std::string tagGetId;
    std::string tagGetField;
    bool tagGetJson = false;
    std::string tagGetOut;
    auto* tagGet = tagCmd->add_subcommand("get", "Get track tags");
    tagGet->add_option("id", tagGetId, "Track id or file path")->required();
    tagGet->add_option("field", tagGetField, "Single field (default: all)");
    tagGet->add_flag("--json", tagGetJson, "JSON output");
    tagGet->add_option("--out", tagGetOut, "Output file path (cover art only)");
    std::string tagBatchFile;
    bool tagBatchDryRun = false;
    bool tagBatchJson = false;
    auto* tagBatch = tagCmd->add_subcommand("batch", "Batch-edit tags from a file");
    tagBatch->add_option("--file", tagBatchFile, "Tag assignments file (JSON or id|field|value lines)")
        ->required();
    tagBatch->add_flag("--dry-run", tagBatchDryRun, "Validate only, do not write");
    tagBatch->add_flag("--json", tagBatchJson, "JSON output");
    auto* trackCmd = cli_->add_subcommand("track", "Track operations");
    std::string trRateId;
    int trRateRating = 0;
    bool trRateJson = false;
    auto* trRate = trackCmd->add_subcommand("rate", "Rate a track (0-5)");
    trRate->add_option("id", trRateId, "Track id or file path")->required();
    trRate->add_option("rating", trRateRating, "Rating (0-5)")
        ->check(CLI::Range(0, 5));
    trRate->add_flag("--json", trRateJson, "JSON output");
    auto* historyCmd = cli_->add_subcommand("history", "Playback history operations");
    bool historyJson = false;
    int historyLimit = 50;
    auto* historyList = historyCmd->add_subcommand("list", "List playback history");
    historyList->add_flag("--json", historyJson, "JSON output");
    historyList->add_option("--limit", historyLimit, "Limit entries");
    auto* historyClear = historyCmd->add_subcommand("clear", "Clear playback history");
    bool historyClearJson = false;
    historyClear->add_flag("--json", historyClearJson, "JSON output");
    auto* bookmarkCmd = cli_->add_subcommand("bookmark", "Bookmark operations");
    std::string bmAddTime;
    std::string bmAddNote;
    std::int64_t bmAddTrack = 0;
    bool bmAddJson = false;
    auto* bmAdd = bookmarkCmd->add_subcommand("add", "Add bookmark at a position");
    bmAdd->add_option("time", bmAddTime, "mm:ss or seconds (default: current position)");
    bmAdd->add_option("--note", bmAddNote, "Bookmark note");
    bmAdd->add_option("--track", bmAddTrack, "Track id (default: current track)");
    bmAdd->add_flag("--json", bmAddJson, "JSON output");
    std::int64_t bmListTrack = 0;
    bool bmListJson = false;
    auto* bmList = bookmarkCmd->add_subcommand("list", "List bookmarks");
    bmList->add_option("--track", bmListTrack, "Track id (default: current track)");
    bmList->add_flag("--json", bmListJson, "JSON output");
    std::int64_t bmRemoveId = 0;
    bool bmRemoveJson = false;
    auto* bmRemove = bookmarkCmd->add_subcommand("remove", "Remove a bookmark");
    bmRemove->add_option("id", bmRemoveId, "Bookmark id")->required();
    bmRemove->add_flag("--json", bmRemoveJson, "JSON output");
    std::int64_t bmGotoId = 0;
    bool bmGotoJson = false;
    auto* bmGoto = bookmarkCmd->add_subcommand("goto", "Play and seek to a bookmark");
    bmGoto->add_option("id", bmGotoId, "Bookmark id")->required();
    bmGoto->add_flag("--json", bmGotoJson, "JSON output");
    auto* lyricsCmd = cli_->add_subcommand("lyrics", "Lyrics operations");
    std::string lyricsSetText;
    bool lyricsSetSynced = false;
    std::string lyricsSetSource = "local";
    std::string lyricsSetTrack;
    std::string lyricsSetFile;
    bool lyricsSetJson = false;
    auto* lyricsSet = lyricsCmd->add_subcommand("set", "Set lyrics for a track");
    lyricsSet->add_option("text", lyricsSetText, "Lyrics text (LRC format when synced)");
    lyricsSet->add_flag("--synced", lyricsSetSynced, "Text is time-coded LRC");
    lyricsSet->add_option("--file", lyricsSetFile, "Read lyrics from .lrc file");
    lyricsSet->add_option("--source", lyricsSetSource, "Provenance label (default: local)");
    lyricsSet->add_option("--track", lyricsSetTrack, "Track id or path (default: current track)");
    lyricsSet->add_flag("--json", lyricsSetJson, "JSON output");
    std::string lyricsGetTrack;
    std::string lyricsGetOut;
    bool lyricsGetLive = false;
    bool lyricsGetJson = false;
    auto* lyricsGet = lyricsCmd->add_subcommand("get", "Get lyrics for a track");
    lyricsGet->add_option("--track", lyricsGetTrack, "Track id or path (default: current track)");
    lyricsGet->add_option("--out", lyricsGetOut, "Write lyrics to .lrc file");
    lyricsGet->add_flag("--live", lyricsGetLive, "Display live lyrics synced to playback");
    lyricsGet->add_flag("--json", lyricsGetJson, "JSON output");
    std::string lyricsRemoveTrack;
    bool lyricsRemoveJson = false;
    auto* lyricsRemove = lyricsCmd->add_subcommand("remove", "Remove lyrics for a track");
    lyricsRemove->add_option("--track", lyricsRemoveTrack, "Track id or path (default: current track)");
    lyricsRemove->add_flag("--json", lyricsRemoveJson, "JSON output");
    std::string previewFile;
    bool previewKeepPlaying = false;
    auto* previewCmd = cli_->add_subcommand("preview", "Preview file (ephemeral)");
    previewCmd->add_option("file", previewFile, "File path")->required();
    previewCmd->add_flag("--keep-playing", previewKeepPlaying,
                         "Do not pause a playing daemon while previewing");
    previewCmd->group("");
    auto* cfgCmd = cli_->add_subcommand("config", "Config operations");
    std::string cfgGetKey;
    bool cfgGetJson = false;
    auto* cfgGet = cfgCmd->add_subcommand("get", "Get config value");
    cfgGet->add_option("key", cfgGetKey, "Key")->required();
    cfgGet->add_flag("--json", cfgGetJson, "JSON output");
    std::string cfgSetKey, cfgSetVal;
    auto* cfgSet = cfgCmd->add_subcommand("set", "Set config value");
    cfgSet->add_option("key", cfgSetKey, "Key")->required();
    cfgSet->add_option("value", cfgSetVal, "Value")->required();
    bool cfgListJson = false;
    auto* cfgList = cfgCmd->add_subcommand("list", "List config");
    cfgList->add_flag("--json", cfgListJson, "JSON output");
    std::string cfgExportPath;
    bool cfgExportJson = false;
    auto* cfgExport = cfgCmd->add_subcommand("export", "Export config");
    cfgExport->add_option("path", cfgExportPath, "Path")->required();
    cfgExport->add_flag("--json", cfgExportJson, "JSON output");
    std::string cfgImportPath;
    bool cfgImportJson = false;
    auto* cfgImport = cfgCmd->add_subcommand("import", "Import config");
    cfgImport->add_option("path", cfgImportPath, "Path")->required();
    cfgImport->add_flag("--json", cfgImportJson, "JSON output");
    std::string cfgResetKey;
    bool cfgResetJson = false;
    auto* cfgReset = cfgCmd->add_subcommand("reset", "Reset config to defaults");
    cfgReset->add_option("key", cfgResetKey, "Key to reset (omit to reset all)");
    cfgReset->add_flag("--json", cfgResetJson, "JSON output");
    // Device commands
    auto* deviceCmd = cli_->add_subcommand("device", "Audio device operations");
    bool devJson = false;
    auto* devList = deviceCmd->add_subcommand("list", "List audio output devices");
    devList->add_flag("--json", devJson, "JSON output");
    std::string devSetId;
    bool devSetJson = false;
    auto* devSet = deviceCmd->add_subcommand("set", "Set default audio device");
    devSet->add_option("id", devSetId, "Device ID")->required();
    devSet->add_flag("--json", devSetJson, "JSON output");
    std::string devTestId;
    bool devTestJson = false;
    bool devTestTone = false;
    auto* devTest = deviceCmd->add_subcommand("test", "Check audio device is available");
    devTest->add_option("--id", devTestId, "Device ID (default: current config)");
    devTest->add_flag("--tone", devTestTone, "Play an audible test tone");
    devTest->add_flag("--json", devTestJson, "JSON output");
    bool infoJson = false;
    auto* infoCmd = cli_->add_subcommand("info", "Show current track info");
    infoCmd->add_flag("--json", infoJson, "JSON output");
    bool versionJson = false;
    auto* versionCmd = cli_->add_subcommand("version", "Show version and build details");
    versionCmd->add_flag("--json", versionJson, "JSON output");
    try {
        cli_->parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return cli_->exit(e);
    }
    if (versionCmd->parsed())
        return detail::renderVersion(versionJson);
    if (!configPathStr.empty())
        config_.configPath = std::filesystem::path(configPathStr);
    {
        auto loaded = kanariv::config::loadConfig(config_.configPath);
        if (loaded) {
            config_ = *loaded;
            if (!configPathStr.empty())
                config_.configPath = std::filesystem::path(configPathStr);
            // If config file didn't exist, persist defaults for next run
            std::error_code ec2;
            if (!std::filesystem::exists(config_.configPath, ec2)) {
                std::error_code ec3;
                auto parent = config_.configPath.parent_path();
                if (!parent.empty())
                    std::filesystem::create_directories(parent, ec3);
                (void)kanariv::config::saveConfig(config_);
            }
        } else {
            // loadConfig failed (e.g. corrupt); keep current config_ which has robust defaults
            // ensure directories exist for db and config
            std::error_code ec2;
            if (!config_.dbPath.empty()) {
                auto parent = config_.dbPath.parent_path();
                if (!parent.empty())
                    std::filesystem::create_directories(parent, ec2);
            }
        }
    }
    if (!deviceStr.empty())
        config_.device = deviceStr;
    if (!logLevelStr.empty()) {
        auto lvl = kanariv::utils::parseLogLevel(logLevelStr);
        if (!lvl) {
            kanariv::println(std::cerr,
                            "--log-level: invalid '{}' (expected trace|debug|info|warn|error)",
                            logLevelStr);
            return 1;
        }
        config_.logLevel = std::to_underlying(*lvl);
    }
    if (config_.socketPath.empty() && !config_.dbPath.empty()) {
        auto sp = kanariv::config::socketPathFor(config_.dbPath);
        if (sp)
            config_.socketPath = *sp;
    }
    // Ensure db parent dir exists before any operation (fixes "unable to open database file")
    {
        std::error_code ec2;
        auto parent = config_.dbPath.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec2);
    }
    // The App orchestrator was bound to the pre-parse config in the Shell
    // ctor; sync it now that reload plus socket derivation are final. Every
    // daemon interaction below must observe these paths.
    app_.setConfig(config_);
    // Inject the local preview host: the CLI (not app) owns the player-backed
    // preview, so the app layer stays free of the player dependency.
    app_.attachPreviewHost(detail::runLocalPreview);
    // Inject the foreground daemon host: the CLI (not app) owns service startup.
    app_.attachDaemonHost([this](bool foreground) -> kanariv::app::AppResult {
        kanariv::service::ServiceConfig scfg;
        scfg.dbPath = config_.dbPath;
        scfg.socketPath = config_.socketPath;
        scfg.configPath = config_.configPath;
        scfg.logLevel = config_.logLevel;
        scfg.logToStderr = foreground;
        scfg.autoResumeOnPlay = config_.autoResumeOnPlay;
        auto db = kanariv::db::Database::open(scfg.dbPath.generic_string());
        if (!db) {
            return kanariv::app::Outcome::fail(std::format(
                "start failed: {} (db: {})", db.error().message, config_.dbPath.generic_string()));
        }
        auto svc = kanariv::service::Service::create(scfg, std::move(db.value()));
        if (!svc) {
            return kanariv::app::Outcome::fail(std::format(
                "start failed: {} (db: {})", svc.error().message, config_.dbPath.generic_string()));
        }
        auto starting = std::format("starting daemon foreground at {} db={}", config_.socketPath,
                                    config_.dbPath.generic_string());
        std::stop_source ss;
        auto res = svc.value()->run(ss.get_token());
        if (!res) {
            return kanariv::app::Outcome::fail(std::format("daemon error: {} (dbPath={})",
                                                          res.error().message,
                                                          config_.dbPath.generic_string()));
        }
        return kanariv::app::Outcome{std::nullopt, std::move(starting), false, false};
    });
    // Internal daemon mode: if --daemon present, run Service foreground immediately (child process)
    if (daemonFlag) {
        return detail::render(app_.startDaemon(true), false);
    }
    // Direct play: `kanariv PATH... [--save]` with no subcommand. Files are
    // fingerprinted into the library; the queue itself is temporary unless
    // --save keeps it. Output is human-readable text.
    bool anySub = false;
    for (auto* sc : cli_->get_subcommands()) {
        if (sc->parsed()) {
            anySub = true;
            break;
        }
    }
    if (!posPaths.empty() || posSave) {
        if (anySub) {
            kanariv::println(std::cerr, "paths and --save take no subcommand");
            return 1;
        }
        if (posPaths.empty()) {
            kanariv::println(std::cerr, "need a PATH (audio file, folder, or glob)");
            return 1;
        }
        std::vector<std::string> files;
        std::vector<std::string> unmatched;
        for (auto& tok : posPaths)
            kanariv::app::expandAddToken(tok, false, files, unmatched);
        bool hardFail = false;
        for (auto& u : unmatched) {
            if (kanariv::app::hasGlobChars(u)) {
                kanariv::println(std::cerr, "No files matched: {}", u);
            } else {
                std::error_code ec;
                if (std::filesystem::is_directory(u, ec) && !ec)
                    kanariv::println(std::cerr, "No files matched: {}", u);
                else {
                    kanariv::println(std::cerr, "no such file: {}", u);
                    hardFail = true;
                }
            }
        }
        if (files.empty())
            return hardFail ? 1 : 0;
        return detail::render(app_.playFiles(files, posSave), false);
    }
    if (startCmd->parsed())
        return detail::render(app_.startDaemon(fg || globalFg), false);
    if (shutdownCmd->parsed())
        return detail::render(app_.shutdownDaemon(), false);
    // Transport commands print one-line confirmations (no ids, no queue
    // positions -- see `info` / `queue list` for those). JSON dumps raw.
    if (playCmd->parsed())
        return detail::render(app_.play(), playJson);
    if (pauseCmd->parsed())
        return detail::render(app_.pause(), pauseJson);
    if (resumeCmd->parsed())
        return detail::render(app_.resume(), resumeJson);
    if (restartCmd->parsed())
        return detail::render(app_.restart(), restartJson);
    if (stopCmd->parsed())
        return detail::render(app_.stop(), stopJson);
    if (nextCmd->parsed())
        return detail::render(app_.next(), nextJson);
    if (prevCmd->parsed())
        return detail::render(app_.prev(), prevJson);
    if (seekCmd->parsed()) {
        auto parsed = detail::parseSeek(seekStr);
        if (!parsed) {
            kanariv::println(std::cerr,
                            "seek: '{}': {} (expected mm:ss, hh:mm:ss, seconds, or +N/-N)", seekStr,
                            parsed.error().message);
            return 1;
        }
        double target = *parsed;
        // parseSeek trims whitespace, so detect +/- on the trimmed form too:
        // " +5" is a +5 delta, not absolute 5.
        std::string_view sv = seekStr;
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
            sv.remove_prefix(1);
        bool isRelative = !sv.empty() && (sv.front() == '+' || sv.front() == '-');
        return detail::render(app_.seek(target, isRelative), seekJson);
    }
    if (statusCmd->parsed()) {
        // NOTE: the watch loop stays in the shell on purpose: carriage-return
        // timestamps and ANSI output are terminal rendering, not reusable
        // behavior. Phase 1 will replace it with a status-stream API.
        if (statusWatch) {
            if (statusInterval <= 0) {
                kanariv::println(std::cerr, "status: --interval must be positive (got {})",
                                statusInterval);
                return 1;
            }
            using namespace std::chrono;
            OutputFormatter fmt{jsonFlag};
            while (true) {
                kanariv::client::Client client{config_.dbPath, config_.socketPath};
                // waitForStatus blocks until the status changes or the
                // interval elapses, avoiding wasteful polling.
                auto res =
                    client.waitForStatus(milliseconds{statusInterval});
                if (!res) {
                    kanariv::ipc::Result errRes{res.error()};
                    if (jsonFlag) {
                        kanariv::println(std::cerr, "{}", kanariv::ipc::toJson(errRes).dump());
                    } else {
                        fmt.print(errRes, std::cerr);
                    }
                    return 1;
                }
                if (!jsonFlag) {
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
                    // kanariv::print(std::cout, output);
                    if (auto* st = std::get_if<kanariv::ipc::Status>(&*res)) {
                        kanariv::print(std::cout, "\r\33[2K[{}] {}", buf, detail::statusLine(*st));
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
                if (std::holds_alternative<kanariv::utils::Error>(*res))
                    return 1;
            }
        }
        return detail::render(app_.status(), jsonFlag);
    }
    if (volumeCmd->parsed()) {
        auto pv = detail::parseVolume(volumeArg);
        if (!pv) {
            kanariv::println(std::cerr, "volume: '{}': {} (expected 0-100, +N/-N, mute, or unmute)",
                            volumeArg, pv.error().message);
            return 1;
        }
        return detail::render(app_.setVolume(*pv), volumeJson);
    }
    if (queueCmd->parsed()) {
        if (qList->parsed() || qQueuesAlias->parsed())
            return detail::render(app_.queueList(), qList->parsed() ? qJson : qQueuesJson);
        if (qTracks->parsed()) {
            auto qr = app_.queueTracks(qTracksOrder);
            if (!qr)
                return detail::renderError(qr.error());
            if (qTracksJson)
                return detail::renderJson(qr->result);
            OutputFormatter fmt{false, detail::useColor()};
            fmt.setHighlightTrackId(qr->highlightId);
            fmt.print(qr->result, std::cout);
            return 0;
        }
        if (qSwitch->parsed())
            return detail::render(app_.queueSwitch(qSwitchId), qSwitchJson);
        if (qCreate->parsed())
            return detail::render(app_.queueCreate(qCreateName), qCreateJson);
        if (qDelete->parsed())
            return detail::render(app_.queueDelete(qDeleteQid), qDeleteJson);
        if (qAdd->parsed())
            return detail::renderBatch(app_.queueAdd(qAddPaths, qAddId, qAddSearch, qAddPlaylist,
                                                     qAddReplace, qAddRecursive),
                                       qAddJson);
        if (qRemove->parsed())
            return detail::renderBatch(
                app_.queueRemove(qRemoveId, qRemovePos, qRemovePaths, qRemoveRecursive),
                qRemoveJson);
        if (qMove->parsed())
            return detail::render(app_.queueMove(qFrom, qTo), qMoveJson);
        if (qClear->parsed())
            return detail::render(app_.queueClear(), qClearJson);
        if (qShuffle->parsed())
            return detail::render(app_.queueShuffle(qShuffleArg), qShuffleJson);
        if (qRepeat->parsed())
            return detail::render(app_.queueRepeat(qRepeatArg), qRepeatJson);
        std::cout << queueCmd->help() << "\n";
        return 0;
    }
    if (plCmd->parsed()) {
        if (plList->parsed())
            return detail::render(app_.playlistList(), plJson);
        if (plTracks->parsed())
            return detail::render(app_.playlistTracks(plTracksPid), plTracksJson);
        if (plCreate->parsed())
            return detail::render(app_.playlistCreate(plCreateName, plCreateSmart), plCreateJson);
        if (plAdd->parsed())
            return detail::renderBatch(
                app_.playlistAdd(plAddPid, plAddIds, plAddPaths, plAddRecursive), plAddJson);
        if (plLoad->parsed())
            return detail::render(app_.playlistLoad(plLoadPid, plLoadPlay, plLoadReplace),
                                  plLoadJson);
        if (plSave->parsed()) {
            std::optional<std::int64_t> qid;
            if (plSave->get_option("--queue")->count() > 0)
                qid = plSaveQid;
            return detail::render(app_.playlistSave(plSaveName, qid), plSaveJson);
        }
        if (plDelete->parsed())
            return detail::render(app_.playlistDelete(plDeletePid), plDeleteJson);
        if (plRename->parsed())
            return detail::render(app_.playlistRename(plRenamePid, plRenameName), plRenameJson);
        if (plExport->parsed())
            return detail::renderBatch(
                app_.playlistExport(plExportPid, plExportPath, plExportFormat), false);
        if (plImport->parsed()) {
            std::optional<std::string> name;
            if (!plImportName.empty())
                name = plImportName;
            return detail::render(app_.playlistImport(plImportPath, name), plImportJson);
        }
        std::cout << plCmd->help() << "\n";
        return 0;
    }
    if (libCmd->parsed()) {
        if (libScan->parsed()) {
            std::optional<std::string> p;
            if (!libScanPath.empty())
                p = libScanPath;
            return detail::render(app_.libraryScan(p, libScanFullHash), libScanJson);
        }
        if (libSearch->parsed()) {
            auto sr = app_.librarySearch(libSearchQuery, libSearchLimit);
            if (!sr)
                return detail::renderError(sr.error());
            if (!sr->result)
                return 0;
            if (libSearchJson)
                return detail::renderJson(*sr->result);
            OutputFormatter fmt{false, detail::useColor()};
            fmt.setHighlightNeedle(libSearchQuery);
            fmt.print(*sr->result, std::cout);
            return 0;
        }
        if (libStats->parsed()) {
            if (libStatsJson && (!libStatsQueues.empty() || !libStatsPlaylists.empty())) {
                kanariv::println(std::cerr, "library stats: --json takes no --queue or --playlist");
                return 1;
            }
            return detail::renderBatch(
                app_.libraryStats(libStatsTopTracks >= 0
                                      ? std::optional<int>{libStatsTopTracks}
                                      : std::nullopt,
                                  libStatsTopAlbums > 0
                                      ? std::optional<int>{libStatsTopAlbums}
                                  : std::nullopt,
                                  libStatsTopGenres > 0
                                      ? std::optional<int>{libStatsTopGenres}
                                  : std::nullopt,
                                  libStatsTopArtists > 0
                                      ? std::optional<int>{libStatsTopArtists}
                                  : std::nullopt,
                                  libStatsQueues, libStatsPlaylists),
                libStatsJson);
        }
        if (libList->parsed()) {
            std::optional<std::int32_t> ratingOpt;
            if (libListRating > 0)
                ratingOpt = libListRating;
            return detail::render(app_.libraryList(libListQuery, libListLimit, libListOffset,
                                                   libListSort, libListOrder,
                                                   libListArtist, libListAlbum, libListGenre,
                                                   ratingOpt),
                                  libListJson, libListColumns);
        }
        if (libFav->parsed()) {
            return detail::render(app_.libraryFavorites(libFavMinRating, libFavLimit),
                                  libFavJson);
        }
        if (libMaint->parsed()) {
            return detail::render(app_.libraryMaintenance(libMaintAction, libMaintDryRun,
                                                          libMaintLibraryId),
                                  libMaintJson);
        }
        if (libAdd->parsed())
            return detail::render(app_.libraryAdd(libAddPath, libAddRecursive), libAddJson);
        if (libRemove->parsed())
            return detail::render(app_.libraryRemove(libRemoveQuery), libRemoveJson);
        std::cout << libCmd->help() << "\n";
        return 0;
    }
    if (tagCmd->parsed()) {
        if (tagEdit->parsed())
            return detail::render(app_.tagEdit(tagEditId, tagEditField, tagEditValue), tagEditJson);
        if (tagGet->parsed()) {
            if (!tagGetField.empty() && !kanariv::app::isTagField(tagGetField)) {
                kanariv::println(std::cerr,
                                "tag get: unknown field '{}' (expected one of "
                                "title|artist|album|album_artist|genre|year|"
                                "track_number|disc_number|cover)",
                                tagGetField);
                return 1;
            }
            if (tagGetField.empty())
                return detail::render(app_.tagGet(tagGetId), tagGetJson);
            auto tv = app_.tagValue(tagGetId, tagGetField);
            if (!tv)
                return detail::renderError(tv.error());
            if (!tagGetOut.empty() && tagGetField == "cover") {
                // --out only applies to cover art: decode base64 and write to file.
                auto decoded = kanariv::utils::Base64::decode(tv->value);
                if (!decoded)
                    return detail::renderError(decoded.error());
                std::ofstream ofs(tagGetOut, std::ios::binary);
                if (!ofs)
                    return detail::renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                 "tag get: cannot open output '" +
                                                     tagGetOut + "'"));
                if (decoded->size() > static_cast<std::size_t>(
                                         std::numeric_limits<std::streamsize>::max()))
                    return detail::renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                 "tag get: cover art is too large to write"));
                ofs.write(reinterpret_cast<const char*>(decoded->data()),
                          static_cast<std::streamsize>(decoded->size()));
                ofs.flush();
                if (!ofs)
                    return detail::renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                                 "tag get: write failed for '" +
                                                     tagGetOut + "'"));
                kanariv::println(std::cout, "Wrote cover art ({} bytes) to {}",
                                decoded->size(), tagGetOut);
                return 0;
            }
            if (tagGetJson)
                kanariv::println("{}", tv->json);
            else
                kanariv::println("{}", tv->value);
            return 0;
        }
        if (tagBatch->parsed()) {
            std::ifstream ifs(tagBatchFile, std::ios::binary);
            if (!ifs) {
                kanariv::println(std::cerr, "tag batch: cannot open file '{}'", tagBatchFile);
                return 1;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            std::string content = ss.str();

            std::vector<kanariv::ipc::TagBatchEditEntry> entries;
            // Detect JSON by leading '[' or '{'; otherwise parse line-oriented
            // "id|field|value" or "path|field|value" records.
            std::size_t first = content.find_first_not_of(" \t\r\n");
            bool isJson = first != std::string::npos &&
                          (content[first] == '[' || content[first] == '{');
            if (isJson) {
                auto j = kanariv::utils::Json::tryParse(content);
                if (!j)
                    return detail::renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                 "tag batch: invalid JSON: " + j.error().message));
                const kanariv::utils::Json& arr = *j;
                if (!arr.isArray())
                    return detail::renderError(
                        kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                 "tag batch: JSON must be an array of entries"));
                for (std::size_t i = 0, n = arr.size(); i < n; ++i) {
                    auto jeExp = arr.at(i);
                    if (!jeExp)
                        return detail::renderError(jeExp.error());
                    const kanariv::utils::Json& je = *jeExp;
                    kanariv::ipc::TagBatchEditEntry e{};
                    if (je.contains("id") && je["id"].isNumber())
                        e.id = je["id"].get<int64_t>();
                    if (je.contains("path") && je["path"].isString())
                        e.path = je["path"].get<std::string>();
                    if (je.contains("field") && je["field"].isString())
                        e.field = je["field"].get<std::string>();
                    if (je.contains("value") && je["value"].isString())
                        e.value = je["value"].get<std::string>();
                    entries.push_back(std::move(e));
                }
            } else {
                std::istringstream lines(content);
                std::string line;
                while (std::getline(lines, line)) {
                    // Strip trailing \r
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty() || line[0] == '#')
                        continue;
                    std::vector<std::string> parts;
                    std::size_t start = 0;
                    std::size_t pos = line.find('|');
                    while (pos != std::string::npos) {
                        parts.push_back(line.substr(start, pos - start));
                        start = pos + 1;
                        pos = line.find('|', start);
                    }
                    parts.push_back(line.substr(start));
                    if (parts.size() != 3) {
                        kanariv::println(std::cerr,
                                        "tag batch: malformed line (expected id|field|value): {}",
                                        line);
                        return 1;
                    }
                    kanariv::ipc::TagBatchEditEntry e{};
                    // First part: numeric id or path
                    bool isId = true;
                    for (char c : parts[0]) {
                        if (c < '0' || c > '9') {
                            isId = false;
                            break;
                        }
                    }
                    if (isId && !parts[0].empty())
                        e.id = std::stoll(parts[0]);
                    else
                        e.path = parts[0];
                    e.field = parts[1];
                    e.value = parts[2];
                    entries.push_back(std::move(e));
                }
            }

            if (entries.empty()) {
                kanariv::println(std::cerr, "tag batch: no entries found in '{}'", tagBatchFile);
                return 1;
            }

            auto res = app_.tagBatchEdit(std::move(entries), tagBatchDryRun);
            if (!res)
                return detail::renderError(res.error());
            if (auto* outcome = &*res) {
                if (!outcome->result) {
                    kanariv::println(std::cerr, "tag batch: no result from daemon");
                    return 1;
                }
                auto& ipc_res = *outcome->result;
                if (auto* br = std::get_if<kanariv::ipc::TagBatchEditResult>(&ipc_res)) {
                    if (tagBatchJson) {
                        kanariv::println("{}", kanariv::ipc::toJson(ipc_res).dump());
                        return 0;
                    }
                    kanariv::println(std::cout, "Tag batch: {} succeeded, {} failed",
                                     br->succeeded, br->failed);
                    for (const auto& er : br->results) {
                        std::string idStr = er.id.has_value()
                                                ? std::to_string(*er.id)
                                                : (er.path.has_value() ? *er.path : "?");
                        kanariv::println(std::cout, "  [{}] {}={} {}", idStr, er.field,
                                         er.ok ? "OK" : "FAIL",
                                         er.ok ? "" : er.error);
                    }
                    return br->failed > 0 ? 1 : 0;
                }
                kanariv::println(std::cerr, "tag batch: unexpected result type");
                return 1;
            }
            return detail::render(std::move(res), tagBatchJson);
        }
        std::cout << tagCmd->help() << "\n";
        return 0;
    }
    if (trackCmd->parsed()) {
        if (trRate->parsed())
            return detail::render(app_.trackRate(trRateId, trRateRating), trRateJson);
        std::cout << trackCmd->help() << "\n";
        return 0;
    }
    if (historyCmd->parsed()) {
        if (historyList->parsed())
            return detail::render(app_.historyList(historyLimit), historyJson);
        if (historyClear->parsed())
            return detail::render(app_.historyClear(), historyClearJson);
        std::cout << historyCmd->help() << "\n";
        return 0;
    }
    if (bookmarkCmd->parsed()) {
        if (bmAdd->parsed()) {
            std::optional<double> pos;
            if (!bmAddTime.empty()) {
                auto parsed = detail::parseTime(bmAddTime);
                if (!parsed) {
                    kanariv::println(std::cerr,
                                    "bookmark add: '{}': {} (expected mm:ss, hh:mm:ss, or seconds)",
                                    bmAddTime, parsed.error().message);
                    return 1;
                }
                pos = *parsed;
            }
            std::optional<std::int64_t> track;
            if (bmAddTrack > 0)
                track = bmAddTrack;
            return detail::render(app_.bookmarkAdd(pos, bmAddNote, track), bmAddJson);
        }
        if (bmList->parsed()) {
            std::optional<std::int64_t> track;
            if (bmListTrack > 0)
                track = bmListTrack;
            return detail::render(app_.bookmarkList(track), bmListJson);
        }
        if (bmRemove->parsed())
            return detail::render(app_.bookmarkRemove(bmRemoveId), bmRemoveJson);
        if (bmGoto->parsed())
            return detail::render(app_.bookmarkGoto(bmGotoId), bmGotoJson);
        std::cout << bookmarkCmd->help() << "\n";
        return 0;
    }
    if (lyricsCmd->parsed()) {
        if (lyricsSet->parsed()) {
            std::string lrc_text = lyricsSetText;
            if (!lyricsSetFile.empty()) {
                std::ifstream ifs(lyricsSetFile, std::ios::binary);
                if (!ifs) {
                    kanariv::println(std::cerr, "lyrics set: cannot open file '{}'", lyricsSetFile);
                    return 1;
                }
                std::stringstream ss;
                ss << ifs.rdbuf();
                lrc_text = ss.str();
                // .lrc files are inherently time-coded
                lyricsSetSynced = true;
            } else if (lrc_text.empty()) {
                kanariv::println(std::cerr,
                                "lyrics set: provide TEXT or --file <path>");
                return 1;
            }
            return detail::render(app_.lyricsSet(std::move(lrc_text), lyricsSetSynced,
                                                 lyricsSetSource,
                                                 lyricsSetTrack.empty()
                                                     ? std::nullopt
                                                     : std::optional<std::string>{lyricsSetTrack}),
                                  lyricsSetJson);
        }
        if (lyricsGet->parsed()) {
            auto res = app_.lyricsGet(
                          lyricsGetTrack.empty()
                              ? std::nullopt
                              : std::optional<std::string>{lyricsGetTrack});
            if (!res) {
                return detail::renderError(res.error());
            }
            auto* outcome = &*res;
            if (!outcome->result) {
                kanariv::println(std::cerr, "lyrics get: no result from daemon");
                return 1;
            }
            auto& ipc_res = *outcome->result;
            if (lyricsGetLive) {
                if (auto* ly = std::get_if<kanariv::ipc::Lyrics>(&ipc_res)) {
                    return detail::renderLyricsLive(*ly, config_.dbPath, config_.socketPath);
                }
                kanariv::println(std::cerr, "lyrics get --live: no lyrics data");
                return 1;
            }
            if (auto* ly = std::get_if<kanariv::ipc::Lyrics>(&ipc_res)) {
                if (!lyricsGetOut.empty()) {
                    std::ofstream ofs(lyricsGetOut, std::ios::binary | std::ios::trunc);
                    if (!ofs) {
                        kanariv::println(std::cerr, "lyrics get: cannot write to file '{}'",
                                         lyricsGetOut);
                        return 1;
                    }
                    ofs << ly->lrc_text;
                    ofs.flush();
                    kanariv::println(std::cout, "Wrote lyrics ({} bytes) to {}",
                                     ly->lrc_text.size(), lyricsGetOut);
                    return 0;
                }
                if (lyricsGetJson) {
                    kanariv::println("{}", kanariv::ipc::toJson(ipc_res).dump());
                    return 0;
                }
                std::string line;
                if (!ly->lrc_text.empty())
                    line = std::format("Lyrics for track {} ({}):\n{}", ly->track_id,
                                       ly->is_synced ? "synced" : "plain", ly->lrc_text);
                else
                    line = std::format("No lyrics text for track {}", ly->track_id);
                kanariv::println(std::cout, "{}", line);
                return 0;
            }
            return detail::render(std::move(res), lyricsGetJson);
        }
        if (lyricsRemove->parsed()) {
            return detail::render(app_.lyricsRemove(
                                      lyricsRemoveTrack.empty()
                                          ? std::nullopt
                                          : std::optional<std::string>{lyricsRemoveTrack}),
                                  lyricsRemoveJson);
        }
        std::cout << lyricsCmd->help() << "\n";
        return 0;
    }
    if (previewCmd->parsed()) {
        // Transient-focus duck: pause a playing daemon while auditioning,
        // resume afterwards. Best-effort and silent: daemon-less runs and
        // mid-preview failures just preview. Only resumes if still paused
        // (the user may have driven the daemon from elsewhere meanwhile).
        auto daemonState = [this]() -> std::optional<kanariv::types::PlaybackState> {
            kanariv::client::Client probe{config_.dbPath, config_.socketPath};
            auto pres = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}},
                                   std::chrono::milliseconds{2000});
            if (!pres)
                return std::nullopt;
            if (auto* st = std::get_if<kanariv::ipc::Status>(&*pres))
                return st->state;
            return std::nullopt;
        };
        bool ducked = false;
        if (!previewKeepPlaying) {
            auto state = daemonState();
            if (state == kanariv::types::PlaybackState::Playing)
                ducked = app_.pause().has_value();
        }
        auto rc = detail::render(app_.previewFile(previewFile), false);
        if (ducked && daemonState() == kanariv::types::PlaybackState::Paused)
            (void)app_.resume();
        return rc;
    }
    if (cfgCmd->parsed()) {
        if (cfgGet->parsed())
            return detail::render(app_.configGet(cfgGetKey), cfgGetJson);
        if (cfgSet->parsed())
            return detail::render(app_.configSet(cfgSetKey, cfgSetVal), false);
        if (cfgList->parsed())
            return detail::render(app_.configList(), cfgListJson);
        if (cfgExport->parsed())
            return detail::render(app_.configExport(cfgExportPath), cfgExportJson);
        if (cfgImport->parsed())
            return detail::render(app_.configImport(cfgImportPath), cfgImportJson);
        if (cfgReset->parsed()) {
            std::optional<std::string> k;
            if (cfgReset->count("key") > 0 && !cfgResetKey.empty())
                k = cfgResetKey;
            else if (cfgReset->count("key") > 0 && cfgResetKey.empty()) {
                // explicit empty string passed -> treat as error
                kanariv::println(std::cerr, "config reset: empty key (omit --key to reset all)");
                return 1;
            }
            return detail::render(app_.configReset(k), cfgResetJson);
        }
        std::cout << cfgCmd->help() << "\n";
        return 0;
    }
    if (deviceCmd->parsed()) {
        if (devList->parsed())
            return detail::render(app_.deviceList(), devJson);
        if (devSet->parsed())
            return detail::render(app_.deviceSet(devSetId), devSetJson);
        if (devTest->parsed()) {
            std::optional<std::string> id;
            if (!devTestId.empty())
                id = devTestId;
            return detail::render(app_.deviceTest(id, devTestTone), devTestJson);
        }
        std::cout << deviceCmd->help() << "\n";
        return 0;
    }
    if (infoCmd->parsed()) {
        return detail::render(app_.info(), infoJson);
    }
    std::cout << cli_->help() << "\n";
    return 0;
}

} // namespace kanariv::app::cli
