/**
 * @file detail.cpp
 * @brief App-internal reporting helpers (NOT public API).
 * @details Builds the human-readable text for CLI reports directly from
 * IPC results. The CLI renderer (cli/src/shell/output_formatter.cpp) uses the
 * same spelling for every variant so app and CLI output stay identical.
 */

#include <algorithm>
#include <app/detail.hpp>
#include <app/format_internal.hpp>
#include <cstdint>
#include <format>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/print.hpp>
#include <set>
#include <sstream>
#include <string>
#include <variant>

namespace kanariv::app::detail {

namespace {

constexpr int secsPerHour = 3600;
constexpr int secsPerMinute = 60;
constexpr int defaultMaxLen = 40;
constexpr int ellipsisLen = 3;
constexpr int volPctScale = 100;
constexpr int msToSecDivisor = 1000;

auto formatTime(double secs) -> std::string {
    secs = std::max<double>(secs, 0);
    int total = static_cast<int>(secs);
    int h = total / secsPerHour;
    int m = (total % secsPerHour) / secsPerMinute;
    int s = total % secsPerMinute;
    if (h > 0) {
        return std::format("{:02}:{:02}:{:02}", h, m, s);
    }
    return std::format("{:02}:{:02}", m, s);
}

auto truncateField(const std::string& s, std::size_t maxLen = defaultMaxLen) -> std::string {
    if (s.empty()) {
        return "---";
    }
    if (s.size() <= maxLen) {
        return s;
    }
    if (maxLen <= ellipsisLen) {
        return s.substr(0, maxLen);
    }
    return s.substr(0, maxLen - ellipsisLen) + "...";
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
    Overloaded(Ts... ts) : Ts(std::move(ts))... {}
};

} // namespace

void emitLine(std::string& blob, const std::string& line) {
    if (!blob.empty()) {
        blob += '\n';
    }
    blob += line;
}

void appendBlock(std::string& blob, const std::string& text) {
    std::size_t start = 0;
    bool first = blob.empty();
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        bool last = (end == text.size());
        if (last && line.empty()) {
            break;
        }
        if (!first) {
            blob += '\n';
        }
        blob += line;
        first = false;
        start = end + 1;
    }
}

void renderInto(std::string& blob, const kanariv::ipc::Result& res) {
    std::ostringstream os;
    std::visit(
        Overloaded{
            [&os](const kanariv::ipc::Status& v) -> void {
                std::string stateStr(kanariv::types::toString(v.state));
                std::string posStr = formatTime(v.pos);
                std::string durStr = formatTime(v.dur);
                int volPct = static_cast<int>(v.vol * volPctScale);
                kanariv::println(os, "State: {}", stateStr);
                kanariv::println(os, "Pos: {} / {}", posStr, durStr);
                if (v.muted) {
                    kanariv::println(os, "Volume: muted");
                } else {
                    kanariv::println(os, "Volume: {}%", volPct);
                }
                kanariv::println(os, "Shuffle: {} Repeat: {}", v.shuffle ? "on" : "off",
                                 kanariv::types::toString(v.repeat));
                if (!v.artist.empty() && !v.title.empty()) {
                    kanariv::println(os, "Track: {} - {} [id: {}]", truncateField(v.artist, defaultMaxLen),
                                     truncateField(v.title, defaultMaxLen), v.track_id);
                } else if (!v.artist.empty() || !v.title.empty()) {
                    kanariv::println(os, "Track: {} [id: {}]",
                                     truncateField(v.artist + v.title, defaultMaxLen), v.track_id);
                } else if (v.track_id != 0) {
                    kanariv::println(os, "Track: [id: {}]", v.track_id);
                }
                if (!v.path.empty()) {
                    kanariv::println(os, "Path: {}", v.path);
                }
                if (v.track_id != 0 && v.q_size > 0) {
                    kanariv::println(os, "Queue: track {} of {}", v.q_idx + 1, v.q_size);
                } else {
                    kanariv::println(os, "Queue: {} tracks", v.q_size);
                }
                if (!v.version.empty()) {
                    kanariv::println(os, "Version: {}", v.version);
                }
            },
            [&os](const kanariv::ipc::QueueTracks& v) -> void {
                std::span<const kanariv::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                kanariv::println(os, "Queue ({} tracks):", tracksSpan.size());
                kanariv::println(os, "{:>3} {:>6}  {:<40} {:<40} {:>8}", "#", "ID", "Artist",
                                 "Title", "Dur");
                for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                    const auto& t = tracksSpan[i]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    kanariv::println(os, "{:3} {:6}  {:<40} {:<40} {:>8}", i, t.id,
                                     truncateField(t.artist, defaultMaxLen), truncateField(t.title, defaultMaxLen),
                                     formatTime(t.duration));
                }
            },
            [&os](const kanariv::ipc::Queues& v) -> void {
                kanariv::println(os, "Queues ({}):", v.entries.size());
                kanariv::println(os, "{:>3} {:>6}  {:<40} {:>8} {:>6} {:>4}", "#", "ID", "Name",
                                 "Tracks", "Active", "Temp");
                for (std::size_t i = 0; i < v.entries.size(); ++i) {
                    const auto& q = v.entries[i]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    kanariv::println(os, "{:3} {:6}  {:<40} {:>8} {:>6} {:>4}", i, q.id,
                                     truncateField(q.name.empty() ? "(unnamed)" : q.name, defaultMaxLen),
                                     q.tracks, q.active ? "*" : "", q.temp ? "yes" : "");
                }
            },
            [&os](const kanariv::ipc::PlaylistData& v) -> void {
                std::span<const kanariv::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                kanariv::println(os, "Playlist ({} tracks):", tracksSpan.size());
                kanariv::println(os, "{:>3} {:>6}  {:<40} {:<40} {:>8}", "#", "ID", "Artist",
                                 "Title", "Dur");
                for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                    const auto& t = tracksSpan[i]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    kanariv::println(os, "{:3} {:6}  {:<40} {:<40} {:>8}", i, t.id,
                                     truncateField(t.artist, defaultMaxLen), truncateField(t.title, defaultMaxLen),
                                     formatTime(t.duration));
                }
            },
            [&os](const kanariv::types::LibraryStats& v) -> void {
                if (!v.top_tracks) {
                    kanariv::println(os, "Tracks: {} Queues: {} Playlists: {}", v.tracks, v.queues,
                                     v.playlists);
                    return;
                }
                kanariv::println(os, "Library Stats (Detailed):");
                kanariv::println(os, "  Tracks:      {}", v.tracks);
                kanariv::println(os, "  Queues:      {}", v.queues);
                kanariv::println(os, "  Playlists:   {}", v.playlists);
                double totalDurSec = v.total_duration_ms / msToSecDivisor;
                double totalPlaySec = v.total_play_time_ms / msToSecDivisor;
                kanariv::println(os, "  Total Duration:  {}", formatTime(totalDurSec));
                kanariv::println(os, "  Total Play Time: {}", formatTime(totalPlaySec));
                if (!v.top_tracks->empty()) {
                    kanariv::println(os, "\n  Most Played Tracks:");
                    kanariv::println(os, "  {:>3} {:>6}  {:<40} {:<40} {:>8} {:>10}", "#", "ID",
                                     "Artist", "Title", "Dur", "Plays");
                    for (std::size_t i = 0; i < v.top_tracks->size(); ++i) {
                        const auto& t = (*v.top_tracks)[i]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        kanariv::println(os, "  {:3} {:6}  {:<40} {:<40} {:>8} {:>10}", i + 1, t.id,
                                         truncateField(t.artist, defaultMaxLen), truncateField(t.title, defaultMaxLen),
                                         formatTime(t.duration), t.play_count);
                    }
                }
            },
            [&os](const kanariv::utils::Error& v) -> void {
                kanariv::println(os, "Error: {} {}", kanariv::utils::toString(v.code), v.message);
            },
            [&os, &res](const auto&) -> auto {
                kanariv::println(os, "{}", kanariv::ipc::toJson(res).dump(2));
            }},
        res);
    appendBlock(blob, os.str());
}

auto printAddedText(const kanariv::ipc::Result& res, std::set<int64_t>& seen, std::string& out,
                    std::string& err) -> int {
    const auto* qt = std::get_if<kanariv::ipc::QueueTracks>(&res);
    if (qt == nullptr) {
        renderInto(out, res);
        return 0;
    }
    int added = 0;
    for (const auto& t : qt->tracks) {
        std::string label = kanariv::app::addedLabel(t);
        if (seen.contains(t.id)) {
            emitLine(err, std::format("already in queue: {}", label));
        } else {
            emitLine(out, std::format("Added {}", label));
            seen.insert(t.id);
            ++added;
        }
    }
    return added;
}

void countLineText(int added, std::string& out) {
    if (added == 1) {
        emitLine(out, "1 track added");
    } else {
        emitLine(out, std::format("{} tracks added", added));
    }
}

void renderErrorInto(std::string& err, const kanariv::utils::Error& e) {
    std::ostringstream os;
    kanariv::println(os, "Error: {} {}", kanariv::utils::toString(e.code), e.message);
    appendBlock(err, os.str());
    if (e.code == kanariv::utils::StatusCode::State && e.message == "daemon not running") {
        emitLine(err, "hint: run `kanariv start` to start the daemon");
    }
}

} // namespace kanariv::app::detail
