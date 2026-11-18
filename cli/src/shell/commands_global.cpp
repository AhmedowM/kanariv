/**
 * @file commands_global.cpp
 * @brief Root options, lifecycle commands and the declaration entry point.
 */

#include <filesystem>
#include <iostream>
#include <kanariv/app/paths.hpp>
#include <kanariv/utils/print.hpp>
#include <string>
#include <system_error>
#include <vector>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declareGlobal(CLI::App& cli, Options& o) {
    cli.add_option("--config", o.global.configPath, "Config file");
    cli.add_option("--log-level", o.global.logLevel, "trace|debug|info|warn|error");
    cli.add_option("--device", o.global.device, "Audio output device");
    cli.add_option("path", o.global.paths, "Audio file, folder, or glob (direct play)");
    cli.add_flag("--save", o.global.save, "Keep the direct-play queue (default: temporary)");
    cli.add_flag("--daemon", o.global.daemon, "Internal daemon flag")->group("");
    cli.add_flag("--foreground", o.global.foreground, "")->group("");
}

void declareLifecycle(CLI::App& cli, Options& o) {
    o.lifecycle.start = cli.add_subcommand("start", "Start daemon");
    o.lifecycle.start->add_flag("--foreground", o.lifecycle.startForeground, "Run in foreground");
    o.lifecycle.shutdown = cli.add_subcommand("shutdown", "Stop daemon");
}

void declareAll(CLI::App& cli, Options& o) {
    declareGlobal(cli, o);
    declareLifecycle(cli, o);
    declarePlayback(cli, o);
    declareQueue(cli, o);
    declarePlaylist(cli, o);
    declareLibrary(cli, o);
    declareTags(cli, o);
    declareHistory(cli, o);
    declareLyrics(cli, o);
    declarePreview(cli, o);
    declareConfig(cli, o);
    // Last on purpose: the help listing follows declaration order.
    o.lifecycle.version = cli.add_subcommand("version", "Show version and build details");
    addJsonFlag(o.lifecycle.version, o.lifecycle.versionJson);
}

bool isMutating(const Options& o) {
    const auto& q = o.queue;
    const auto& pl = o.playlist;
    const auto& lib = o.library;
    const auto& cfg = o.config;
    const auto& bm = o.history;
    return o.global.daemon || o.lifecycle.start->parsed() || !o.global.paths.empty() ||
           o.global.save ||
           (cfg.config->parsed() &&
            (cfg.set->parsed() || cfg.importFile->parsed() || cfg.reset->parsed())) ||
           (lib.library->parsed() &&
            (lib.scan->parsed() || lib.analyze->parsed() || lib.add->parsed() ||
             lib.remove->parsed() || lib.maintenance->parsed())) ||
           (o.tag.tag->parsed() && (o.tag.edit->parsed() || o.tag.batch->parsed())) ||
           (o.tag.track->parsed() && o.tag.rate->parsed()) ||
           (q.queue->parsed() &&
            (q.add->parsed() || q.remove->parsed() || q.clear->parsed() || q.create->parsed() ||
             q.deleteQueue->parsed() || q.move->parsed() || q.shuffle->parsed() ||
             q.repeat->parsed() || q.switchTo->parsed())) ||
           (pl.playlist->parsed() &&
            (pl.create->parsed() || pl.add->parsed() || pl.load->parsed() || pl.remove->parsed() ||
             pl.rename->parsed() || pl.importFile->parsed())) ||
           (bm.bookmark->parsed() &&
            (bm.add->parsed() || bm.remove->parsed() || bm.gotoBookmark->parsed())) ||
           (o.lyrics.lyrics->parsed() && (o.lyrics.set->parsed() || o.lyrics.remove->parsed())) ||
           (bm.history->parsed() && bm.clear->parsed()) ||
           (cfg.device->parsed() && cfg.devSet->parsed());
}

std::optional<int> runDirectPlay(App& app, const CLI::App& cli, const Options& o) {
    if (o.global.paths.empty() && !o.global.save)
        return std::nullopt;
    bool anySub = false;
    for (auto* sc : cli.get_subcommands()) {
        if (sc->parsed()) {
            anySub = true;
            break;
        }
    }
    if (anySub) {
        kanariv::println(std::cerr, "paths and --save take no subcommand");
        return exitUsage;
    }
    if (o.global.paths.empty()) {
        kanariv::println(std::cerr, "need a PATH (audio file, folder, or glob)");
        return exitUsage;
    }
    std::vector<std::string> files;
    std::vector<std::string> unmatched;
    for (auto& tok : o.global.paths)
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
    return render(app.playFiles(files, o.global.save), false);
}

std::optional<int> runLifecycle(App& app, std::stop_token stop, const Options& o) {
    const auto& lc = o.lifecycle;
    if (lc.start->parsed())
        return render(app.startDaemon(lc.startForeground || o.global.foreground, stop), false);
    if (lc.shutdown->parsed())
        return render(app.shutdownDaemon(), false);
    return std::nullopt;
}

} // namespace kanariv::app::cli::detail
