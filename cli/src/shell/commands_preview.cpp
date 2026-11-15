/**
 * @file commands_preview.cpp
 * @brief Local audition of one file, optionally ducking a playing daemon.
 */

#include <chrono>
#include <iostream>
#include <kanariv/client/client.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/types/wire.hpp>
#include <optional>
#include <string>

#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declarePreview(CLI::App& cli, Options& o) {
    auto& p = o.preview;
    p.preview = cli.add_subcommand("preview", "Preview file (ephemeral)");
    p.preview->add_option("file", p.file, "File path")->required();
    p.preview->add_flag("--keep-playing", p.keepPlaying,
                        "Do not pause a playing daemon while previewing");
    p.preview->group("");
}

std::optional<int> runPreview(App& app, const config::Config& cfg, std::stop_token stop,
                              const Options& o) {
    const auto& p = o.preview;
    if (!p.preview->parsed())
        return std::nullopt;
    // Transient-focus duck: pause a playing daemon while auditioning,
    // resume afterwards. Best-effort and silent: daemon-less runs and
    // mid-preview failures just preview. Only resumes if still paused
    // (the user may have driven the daemon from elsewhere meanwhile).
    auto daemonState = [&cfg]() -> std::optional<kanariv::types::PlaybackState> {
        kanariv::client::Client probe{cfg.dbPath, cfg.socketPath};
        auto pres = probe.send(kanariv::ipc::Command{kanariv::ipc::StatusReq{}},
                               std::chrono::milliseconds{2000});
        if (!pres)
            return std::nullopt;
        if (auto* st = std::get_if<kanariv::ipc::Status>(&*pres))
            return st->state;
        return std::nullopt;
    };
    bool ducked = false;
    if (!p.keepPlaying) {
        auto state = daemonState();
        if (state == kanariv::types::PlaybackState::Playing)
            ducked = app.pause().has_value();
    }
    auto rc = render(app.previewFile(p.file, stop), false);
    if (ducked && daemonState() == kanariv::types::PlaybackState::Paused)
        (void)app.resume();
    return rc;
}

} // namespace kanariv::app::cli::detail
