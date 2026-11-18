/**
 * @file detail.cpp
 * @brief App-internal reporting helpers (NOT public API).
 * @details Builds the human-readable text for CLI reports directly from
 * IPC results. Result text itself is `kanariv::client::formatResult` /
 * `formatError`, the same renderer the CLI uses, so both paths stay
 * byte-identical; these helpers only join lines into a blob.
 */

#include <algorithm>
#include <app/detail.hpp>
#include <app/format_internal.hpp>
#include <cstdint>
#include <format>
#include <kanariv/client/output_formatter.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/utils/error.hpp>
#include <string>

namespace kanariv::app::detail {

std::chrono::milliseconds readinessAttemptTimeout(std::chrono::milliseconds remaining) {
    if (remaining <= std::chrono::milliseconds::zero())
        return std::chrono::milliseconds::zero();
    return std::min(daemonReadyAttempt, remaining);
}

kanariv::utils::Error daemonDownError() {
    return kanariv::utils::makeError(
        kanariv::utils::StatusCode::State,
        "daemon not running\nhint: run `kanariv start` to start the daemon");
}

void emitLine(std::string& blob, const std::string& line) {
    if (!blob.empty())
        blob += '\n';
    blob += line;
}

void appendBlock(std::string& blob, const std::string& text) {
    std::size_t start = 0;
    bool first = blob.empty();
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        bool last = (end == text.size());
        if (last && line.empty())
            break;
        if (!first)
            blob += '\n';
        blob += line;
        first = false;
        start = end + 1;
    }
}

int printAddReportText(const kanariv::ipc::Result& res, std::string& out, std::string& err) {
    auto* rep = std::get_if<kanariv::ipc::QueueAddReport>(&res);
    if (!rep) {
        appendBlock(out, kanariv::client::formatResult(res));
        return 0;
    }
    int added = 0;
    for (const auto& item : rep->items) {
        if (item.status == "added") {
            emitLine(out, std::format("Added {}", kanariv::app::addedLabel(item.track)));
            ++added;
        } else if (item.status == "duplicate") {
            emitLine(err,
                     std::format("already in queue: {}", kanariv::app::addedLabel(item.track)));
        } else {
            emitLine(err, std::format("queue add: {}: {}", item.query, item.detail));
        }
    }
    return added;
}

int printRemoveReportText(const kanariv::ipc::Result& res, std::string& out, std::string& err) {
    auto* rep = std::get_if<kanariv::ipc::QueueRemoveReport>(&res);
    if (!rep) {
        appendBlock(out, kanariv::client::formatResult(res));
        return 0;
    }
    int removed = 0;
    for (const auto& item : rep->items) {
        if (item.status == "removed") {
            emitLine(out,
                     std::format("Removed from queue {}", kanariv::app::addedLabel(item.track)));
            ++removed;
        } else if (item.status == "missing") {
            emitLine(err, std::format("queue remove: not in queue: {}", item.selector));
        } else {
            emitLine(err, std::format("queue remove: {}: {}", item.selector, item.detail));
        }
    }
    return removed;
}

void countLineText(int added, std::string& out) {
    if (added == 1)
        emitLine(out, "1 track added");
    else
        emitLine(out, std::format("{} tracks added", added));
}

} // namespace kanariv::app::detail
