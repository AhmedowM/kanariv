/**
 * @file commands_queue.cpp
 * @brief Queue inspection and mutation commands.
 */

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <kanariv/utils/print.hpp>
#include <string>
#include <vector>

#include "cli_helpers.hpp"
#include "commands.hpp"
#include "render.hpp"

namespace kanariv::app::cli::detail {

void declareQueue(CLI::App& cli, Options& o) {
    auto& q = o.queue;
    q.queue = cli.add_subcommand("queue", "Queue operations");
    q.list = q.queue->add_subcommand("list", "List all queues");
    addJsonFlag(q.list, q.listJson);
    q.queuesAlias = q.queue->add_subcommand("queues", "List all queues (alias)")->group("");
    addJsonFlag(q.queuesAlias, q.queuesAliasJson);
    q.tracks = q.queue->add_subcommand("tracks", "List tracks in active queue");
    q.tracks->add_option("--order", q.tracksOrder, "Track order: added|playback")
        ->check(CLI::IsMember({"added", "playback"}));
    addJsonFlag(q.tracks, q.tracksJson);
    q.switchTo = q.queue->add_subcommand("switch", "Switch active queue");
    q.switchTo->add_option("qid", q.switchId, "Queue id")->required();
    addJsonFlag(q.switchTo, q.switchJson);
    q.add = q.queue->add_subcommand("add", "Add to queue");
    q.add->add_option("path", q.addPaths, "File, folder, or glob (repeatable)");
    q.add->add_option("--id", q.addId, "Library track id");
    q.add->add_flag("--search", q.addSearch, "Treat path as FTS query (single)");
    q.add->add_flag("--recursive", q.addRecursive, "Recurse into folders");
    q.add->add_option("--playlist", q.addPlaylist, "Playlist id to append");
    q.add->add_flag("--replace", q.addReplace, "Clear queue first (with --playlist)");
    addJsonFlag(q.add, q.addJson);
    q.remove = q.queue->add_subcommand("remove", "Remove from queue");
    q.remove->add_option("path", q.removePaths, "File, folder, or glob (repeatable)");
    q.remove->add_option("--id", q.removeId, "Library track id");
    q.remove->add_option("--pos", q.removePos, "Queue position");
    q.remove->add_flag("--recursive", q.removeRecursive, "Recurse into folders");
    addJsonFlag(q.remove, q.removeJson);
    q.create = q.queue->add_subcommand("create", "Create a new queue");
    q.create->add_option("name", q.createName, "Queue name")->required();
    addJsonFlag(q.create, q.createJson);
    q.deleteQueue = q.queue->add_subcommand("delete", "Delete a queue");
    q.deleteQueue->add_option("qid", q.deleteQid, "Queue id")->required();
    addJsonFlag(q.deleteQueue, q.deleteJson);
    q.move = q.queue->add_subcommand("move", "Move within queue");
    q.move->add_option("from", q.from, "from index")->required();
    q.move->add_option("to", q.to, "to index")->required();
    addJsonFlag(q.move, q.moveJson);
    q.clear = q.queue->add_subcommand("clear", "Clear queue");
    addJsonFlag(q.clear, q.clearJson);
    q.shuffle = q.queue->add_subcommand("shuffle", "Set shuffle");
    q.shuffle->add_option("mode", q.shuffleArg, "on|off")->check(CLI::IsMember({"on", "off"}));
    addJsonFlag(q.shuffle, q.shuffleJson);
    q.repeat = q.queue->add_subcommand("repeat", "Set repeat");
    q.repeat->add_option("mode", q.repeatArg, "off|one|all")
        ->check(CLI::IsMember({"off", "one", "all"}));
    addJsonFlag(q.repeat, q.repeatJson);
}

std::optional<int> runQueue(App& app, const Options& o) {
    const auto& q = o.queue;
    if (!q.queue->parsed())
        return std::nullopt;
    if (q.list->parsed() || q.queuesAlias->parsed())
        return render(app.queueList(), q.list->parsed() ? q.listJson : q.queuesAliasJson);
    if (q.tracks->parsed()) {
        auto qr = app.queueTracks(q.tracksOrder);
        if (!qr)
            return renderError(qr.error());
        if (q.tracksJson) {
            renderJson(qr->result);
            return exitOk;
        }
        kanariv::client::OutputFormatter fmt{false, useColor()};
        fmt.setHighlightTrackId(qr->highlightId);
        fmt.print(qr->result, std::cout);
        return 0;
    }
    if (q.switchTo->parsed())
        return render(app.queueSwitch(q.switchId), q.switchJson);
    if (q.create->parsed())
        return render(app.queueCreate(q.createName), q.createJson);
    if (q.deleteQueue->parsed())
        return render(app.queueDelete(q.deleteQid), q.deleteJson);
    if (q.add->parsed())
        return renderBatch(app.queueAdd(q.addPaths, q.addId, q.addSearch, q.addPlaylist,
                                        q.addReplace, q.addRecursive),
                           q.addJson);
    if (q.remove->parsed())
        return renderBatch(
            app.queueRemove(q.removeId, q.removePos, q.removePaths, q.removeRecursive),
            q.removeJson);
    if (q.move->parsed())
        return render(app.queueMove(q.from, q.to), q.moveJson);
    if (q.clear->parsed())
        return render(app.queueClear(), q.clearJson);
    if (q.shuffle->parsed())
        return render(app.queueShuffle(q.shuffleArg), q.shuffleJson);
    if (q.repeat->parsed())
        return render(app.queueRepeat(q.repeatArg), q.repeatJson);
    std::cout << q.queue->help() << "\n";
    return 0;
}

} // namespace kanariv::app::cli::detail
