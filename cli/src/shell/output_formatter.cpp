#include "output_formatter.hpp"

#include <caudio/ipc/protocol.hpp>
#include <caudio/ipc/result.hpp>
#include <caudio/types/format.hpp>
#include <caudio/types/record.hpp>
#include <caudio/utils/error.hpp>
#include <caudio/utils/json.hpp>
#include <caudio/utils/print.hpp>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace caudio::app::cli {

std::string OutputFormatter::formatTime(double secs) {
    if (secs < 0)
        secs = 0;
    int total = static_cast<int>(secs);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    if (h > 0)
        return std::format("{:02}:{:02}:{:02}", h, m, s);
    return std::format("{:02}:{:02}", m, s);
}

std::string OutputFormatter::truncateField(const std::string& s, std::size_t maxLen) {
    if (s.empty())
        return "---";
    if (s.size() <= maxLen)
        return s;
    if (maxLen <= 3)
        return s.substr(0, maxLen);
    return s.substr(0, maxLen - 3) + "...";
}

void OutputFormatter::setColumns(std::string_view columns) {
    columns_.clear();
    if (columns.empty())
        return;
    std::size_t start = 0;
    while (start <= columns.size()) {
        std::size_t comma = columns.find(',', start);
        if (comma == std::string::npos)
            comma = columns.size();
        std::string col(columns.substr(start, comma - start));
        while (!col.empty() && (col.front() == ' ' || col.front() == '\t'))
            col.erase(0, 1);
        while (!col.empty() && (col.back() == ' ' || col.back() == '\t'))
            col.pop_back();
        if (!col.empty())
            columns_.insert(std::move(col));
        start = comma + 1;
    }
}

void OutputFormatter::printTrackDetail(std::ostream& os, const caudio::types::Track& t) {
    caudio::println(os, "Track [{}]", t.id);
    caudio::println(os, "  Path:         {}", t.path);
    caudio::println(os, "  Title:        {}", t.title.empty() ? "---" : t.title);
    caudio::println(os, "  Artist:       {}", t.artist.empty() ? "---" : t.artist);
    caudio::println(os, "  Album:        {}", t.album.empty() ? "---" : t.album);
    caudio::println(os, "  Album Artist: {}", t.album_artist.empty() ? "---" : t.album_artist);
    caudio::println(os, "  Genre:        {}", t.genre.empty() ? "---" : t.genre);
    caudio::println(os, "  Year:         {}", t.year == 0 ? "---" : std::to_string(t.year));
    caudio::println(os, "  Track:        {}",
                    t.track_num == 0 ? "---" : std::to_string(t.track_num));
    caudio::println(os, "  Disc:         {}", t.disc_num == 0 ? "---" : std::to_string(t.disc_num));
    caudio::println(os, "  Duration:     {}", formatTime(t.duration));
    caudio::println(os, "  Sample Rate:  {} Hz", t.sample_rate);
    caudio::println(os, "  Channels:     {}", t.channels);
    caudio::println(os, "  Bitrate:      {}",
                    t.bitrate == 0 ? "---" : std::format("{} kbps", t.bitrate / 1000));
}

namespace {

// Wrap all case-insensitive occurrences of needle in ANSI green.
std::string highlightAll(const std::string& text, std::string_view needle, bool color) {
    if (!color || needle.empty())
        return text;
    std::string out;
    std::size_t pos = 0;
    auto lower = [](char c) { return (char)std::tolower((unsigned char)c); };
    while (pos < text.size()) {
        std::size_t hit = std::string::npos;
        for (std::size_t i = pos; i + needle.size() <= text.size(); ++i) {
            bool match = true;
            for (std::size_t k = 0; k < needle.size(); ++k) {
                if (lower(text[i + k]) != lower(needle[k])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                hit = i;
                break;
            }
        }
        if (hit == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, hit - pos);
        out += "\33[32m";
        out += text.substr(hit, needle.size());
        out += "\33[0m";
        pos = hit + needle.size();
    }
    return out;
}

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

void OutputFormatter::print(const caudio::ipc::Result& r, std::ostream& os) const {
    if (json_) {
        // Pretty-printed for single-shot human --json; watch streaming uses compact separately.
        caudio::println(os, "{}", caudio::ipc::toJson(r).dump(2));
        return;
    }

    std::visit(
        Overloaded{
            [&os](const caudio::ipc::Status& v) {
                std::string stateStr = std::format("{}", v.state);
                std::string posStr = formatTime(v.pos);
                std::string durStr = formatTime(v.dur);
                int volPct = static_cast<int>(v.vol * 100.0f);
                caudio::println(os, "State: {}", stateStr);
                caudio::println(os, "Pos: {} / {}", posStr, durStr);
                if (v.muted)
                    caudio::println(os, "Volume: muted");
                else
                    caudio::println(os, "Volume: {}%", volPct);
                caudio::println(os, "Shuffle: {} Repeat: {}", v.shuffle ? "on" : "off",
                                std::format("{}", v.repeat));
                if (!v.artist.empty() && !v.title.empty()) {
                    caudio::println(os, "Track: {} - {} [id: {}]", truncateField(v.artist, 40),
                                    truncateField(v.title, 40), v.track_id);
                } else if (!v.artist.empty() || !v.title.empty()) {
                    caudio::println(os, "Track: {} [id: {}]", truncateField(v.artist + v.title, 40),
                                    v.track_id);
                } else if (v.track_id != 0) {
                    caudio::println(os, "Track: [id: {}]", v.track_id);
                }
                if (!v.path.empty()) {
                    caudio::println(os, "Path: {}", v.path);
                }
                if (v.track_id != 0 && v.q_size > 0) {
                    caudio::println(os, "Queue: track {} of {}", v.q_idx + 1, v.q_size);
                } else {
                    caudio::println(os, "Queue: {} tracks", v.q_size);
                }
                if (!v.version.empty()) {
                    caudio::println(os, "Version: {}", v.version);
                }
            },
            [&os, this](const caudio::ipc::QueueTracks& v) {
                std::span<const caudio::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                caudio::println(os, "Queue ({} tracks):", tracksSpan.size());
                caudio::println(os, "{:>3} {:>6}  {:<40} {:<40} {:>8}", "#", "ID", "Artist",
                                "Title", "Dur");
                for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                    const auto& t = tracksSpan[i];
                    bool mark = (highlightTrackId_ != 0 && t.id == highlightTrackId_);
                    std::string line = std::format(
                        "{:3} {:6}  {:<40} {:<40} {:>8}", i, t.id, truncateField(t.artist, 40),
                        truncateField(t.title, 40), formatTime(t.duration));
                    if (mark && color_)
                        line = "\33[32m" + line + "\33[0m";
                    caudio::println(os, "{} {}", mark ? ">" : " ", line);
                }
            },
            [&os](const caudio::ipc::Queues& v) {
                caudio::println(os, "Queues ({}):", v.entries.size());
                caudio::println(os, "{:>3} {:>6}  {:<40} {:>8} {:>6} {:>4}", "#", "ID", "Name",
                                "Tracks", "Active", "Temp");
                for (std::size_t i = 0; i < v.entries.size(); ++i) {
                    const auto& q = v.entries[i];
                    caudio::println(os, "{:3} {:6}  {:<40} {:>8} {:>6} {:>4}", i, q.id,
                                    truncateField(q.name.empty() ? "(unnamed)" : q.name, 40),
                                    q.tracks, q.active ? "*" : "", q.temp ? "yes" : "");
                }
            },
            [&os](const caudio::ipc::QueueCreated& v) {
                caudio::println(os, "Created queue {} '{}'", v.id, v.name);
            },
            [&os](const caudio::ipc::PlaylistLoaded& v) {
                caudio::println(os, "Loaded playlist into queue {}", v.queue_id);
            },
            [&os](const caudio::ipc::VolumeInfo& v) {
                int pct = static_cast<int>(v.vol * 100.0f);
                if (v.muted)
                    caudio::println(os, "Volume: muted");
                else
                    caudio::println(os, "Volume: {}%", pct);
            },
            [&os](const caudio::types::LibraryStats& v) {
                if (!v.most_played) {
                    caudio::println(os, "Tracks: {} Queues: {} Playlists: {}", v.tracks, v.queues,
                                    v.playlists);
                    return;
                }
                caudio::println(os, "Library Stats (Detailed):");
                caudio::println(os, "  Tracks:      {}", v.tracks);
                caudio::println(os, "  Queues:      {}", v.queues);
                caudio::println(os, "  Playlists:   {}", v.playlists);
                double totalDurSec = v.total_duration_ms / 1000.0;
                double totalPlaySec = v.total_play_time_ms / 1000.0;
                caudio::println(os, "  Total Duration:  {}", formatTime(totalDurSec));
                caudio::println(os, "  Total Play Time: {}", formatTime(totalPlaySec));
                if (!v.most_played->empty()) {
                    caudio::println(os, "\n  Most Played Tracks:");
                    caudio::println(os, "  {:>3} {:>6}  {:<40} {:<40} {:>8} {:>10}", "#", "ID",
                                    "Artist", "Title", "Dur", "Plays");
                    for (std::size_t i = 0; i < v.most_played->size(); ++i) {
                        const auto& t = (*v.most_played)[i];
                        caudio::println(os, "  {:3} {:6}  {:<40} {:<40} {:>8} {:>10}", i + 1, t.id,
                                        truncateField(t.artist, 40), truncateField(t.title, 40),
                                        formatTime(t.duration), t.play_count);
                    }
                }
            },
            [&os, this](const caudio::ipc::Tracks& v) {
                std::span<const caudio::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                caudio::println(os, "Tracks ({}):", tracksSpan.size());
                if (columns_.empty()) {
                    caudio::println(os, "{:>3} {:>6}  {:<40} {:<40} {:>8}", "#", "ID", "Artist",
                                    "Title", "Dur");
                    for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                        const auto& t = tracksSpan[i];
                        caudio::println(os, "{:3} {:6}  {:<40} {:<40} {:>8}", i, t.id,
                                        truncateField(t.artist, 40), truncateField(t.title, 40),
                                        formatTime(t.duration));
                    }
                } else {
                    // --columns allowlist: print only selected columns.
                    bool hasId = columns_.contains("id");
                    bool hasTitle = columns_.contains("title");
                    bool hasArtist = columns_.contains("artist");
                    bool hasAlbum = columns_.contains("album");
                    bool hasGenre = columns_.contains("genre");
                    bool hasYear = columns_.contains("year");
                    bool hasRating = columns_.contains("rating");
                    bool hasDuration = columns_.contains("duration");
                    bool hasPath = columns_.contains("path");
                    // Header
                    caudio::print(os, "{:>3}", "#");
                    if (hasId) caudio::print(os, " {:>6}", "ID");
                    if (hasTitle) caudio::print(os, " {:<40}", "Title");
                    if (hasArtist) caudio::print(os, " {:<40}", "Artist");
                    if (hasAlbum) caudio::print(os, " {:<40}", "Album");
                    if (hasGenre) caudio::print(os, " {:<20}", "Genre");
                    if (hasYear) caudio::print(os, " {:>6}", "Year");
                    if (hasRating) caudio::print(os, " {:>8}", "Rating");
                    if (hasDuration) caudio::print(os, " {:>8}", "Dur");
                    if (hasPath) caudio::print(os, " {}", "Path");
                    caudio::println(os, "");
                    // Rows
                    for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                        const auto& t = tracksSpan[i];
                        caudio::print(os, "{:3}", i);
                        if (hasId) caudio::print(os, " {:6}", t.id);
                        if (hasTitle) caudio::print(os, " {:<40}", truncateField(t.title, 40));
                        if (hasArtist) caudio::print(os, " {:<40}", truncateField(t.artist, 40));
                        if (hasAlbum) caudio::print(os, " {:<40}", truncateField(t.album, 40));
                        if (hasGenre) caudio::print(os, " {:<20}", truncateField(t.genre, 20));
                        if (hasYear) caudio::print(os, " {:6}", t.year == 0 ? "---" : std::to_string(t.year));
                        if (hasRating) caudio::print(os, " {:8}", t.rating);
                        if (hasDuration) caudio::print(os, " {:>8}", formatTime(t.duration));
                        if (hasPath) caudio::print(os, " {}", t.path);
                        caudio::println(os, "");
                    }
                }
            },
            [&os, this](const caudio::ipc::SearchResults& v) {
                std::span<const caudio::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                if (tracksSpan.size() == 1)
                    caudio::println(os, "1 track found:");
                else
                    caudio::println(os, "{} tracks found:", tracksSpan.size());
                caudio::println(os, "{:>3} {:>6}  {:<30} {:<25} {:<20} {:<12} {:<25}", "#", "ID",
                                "Title", "Artist", "Album", "Genre", "File");
                for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                    const auto& t = tracksSpan[i];
                    std::string fn = std::filesystem::path(t.path).filename().generic_string();
                    caudio::println(
                        os, "{:3} {:6}  {:<30} {:<25} {:<20} {:<12} {:<25}", i, t.id,
                        highlightAll(truncateField(t.title, 30), highlightNeedle_, color_),
                        highlightAll(truncateField(t.artist, 25), highlightNeedle_, color_),
                        highlightAll(truncateField(t.album, 20), highlightNeedle_, color_),
                        highlightAll(truncateField(t.genre, 12), highlightNeedle_, color_),
                        highlightAll(truncateField(fn, 25), highlightNeedle_, color_));
                }
            },
            [&os](const caudio::ipc::ScanReport& v) {
                if (v.added == 1)
                    caudio::println(os, "1 track added");
                else
                    caudio::println(os, "{} tracks added", v.added);
            },
            [&os](const caudio::ipc::PlaylistCreated& v) {
                caudio::println(os, "Created playlist {} '{}'", v.id, v.name);
            },
            [&os](const caudio::ipc::PlaylistImportReport& v) {
                std::span<const caudio::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                caudio::println(os,
                                "Imported playlist '{}' ({} tracks, {} matched, {} skipped, "
                                "{} duplicates):",
                                v.name, tracksSpan.size(), v.matched, v.skipped, v.duplicates);
                caudio::println(os, "{:>3} {:>6}  {:<40} {:<40} {:>8}", "#", "ID", "Artist",
                                "Title", "Dur");
                for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                    const auto& t = tracksSpan[i];
                    caudio::println(os, "{:3} {:6}  {:<40} {:<40} {:>8}", i, t.id,
                                    truncateField(t.artist, 40), truncateField(t.title, 40),
                                    formatTime(t.duration));
                }
            },
            [&os](const caudio::ipc::LibraryMaintenanceReport& v) {
                caudio::println(os, "Library maintenance: {} tracks checked", v.tracks_checked);
                if (v.missing_tracks.empty() && v.duplicate_groups == 0)
                    caudio::println(os, "  No missing files or duplicates found.");
                else {
                    if (!v.missing_tracks.empty()) {
                        caudio::println(os, "  Missing files ({}):", v.missing_tracks.size());
                        caudio::println(os, "{:>3} {:>6}  {:<40} {:<40}", "#", "ID", "Artist",
                                        "Title");
                        for (std::size_t i = 0; i < v.missing_tracks.size(); ++i) {
                            const auto& t = v.missing_tracks[i];
                            caudio::println(os, "{:3} {:6}  {:<40} {:<40}", i, t.id,
                                            truncateField(t.artist, 40), truncateField(t.title, 40));
                        }
                    }
                    if (v.duplicate_groups > 0)
                        caudio::println(os, "  Duplicate groups: {} (redundant rows: {})",
                                        v.duplicate_groups, v.duplicate_tracks);
                }
            },
            [&os](const caudio::ipc::Playlists& v) {
                std::span<const caudio::types::Playlist> playlistSpan(v.playlists.data(),
                                                                      v.playlists.size());
                caudio::println(os, "Playlists ({}):", playlistSpan.size());
                caudio::println(os, "{:>3} {:>6}  {:<40}", "#", "ID", "Name");
                for (std::size_t i = 0; i < playlistSpan.size(); ++i) {
                    const auto& p = playlistSpan[i];
                    caudio::println(os, "{:3} {:6}  {:<40}", i, p.id, truncateField(p.name, 40));
                }
            },
            [&os](const caudio::ipc::ConfigValue& v) {
                caudio::println(os, "{} = {}", v.key, v.value);
            },
            [&os](const caudio::ipc::ConfigValues& v) {
                caudio::println(os, "Config ({} entries):", v.values.size());
                for (const auto& cv :
                     std::span<const caudio::ipc::ConfigValue>(v.values.data(), v.values.size())) {
                    caudio::println(os, "{} = {}", cv.key, cv.value);
                }
            },
            [&os](const caudio::ipc::PlaylistData& v) {
                std::span<const caudio::types::Track> tracksSpan(v.tracks.data(), v.tracks.size());
                caudio::println(os, "Playlist ({} tracks):", tracksSpan.size());
                caudio::println(os, "{:>3} {:>6}  {:<40} {:<40} {:>8}", "#", "ID", "Artist",
                                "Title", "Dur");
                for (std::size_t i = 0; i < tracksSpan.size(); ++i) {
                    const auto& t = tracksSpan[i];
                    caudio::println(os, "{:3} {:6}  {:<40} {:<40} {:>8}", i, t.id,
                                    truncateField(t.artist, 40), truncateField(t.title, 40),
                                    formatTime(t.duration));
                }
            },
            [&os](const caudio::ipc::SingleTrack& v) { printTrackDetail(os, v.track); },
            [&os](const caudio::ipc::TrackInfo& v) {
                const auto& t = v.track;
                printTrackDetail(os, t);
                caudio::println(os, "  Play Count:   {}", v.play_count);
                if (v.last_played > 0) {
                    std::time_t tp = static_cast<std::time_t>(v.last_played / 1000);
                    std::tm tm{};
#ifdef _WIN32
                    localtime_s(&tm, &tp);
#else
                    localtime_r(&tp, &tm);
#endif
                    char timeBuf[32];
                    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tm);
                    caudio::println(os, "  Last Played:  {}", timeBuf);
                } else {
                    caudio::println(os, "  Last Played:  (never)");
                }
            },
            [&os](const caudio::ipc::History& v) {
                std::span<const caudio::types::HistoryEntry> entriesSpan(v.entries.data(),
                                                                         v.entries.size());
                caudio::println(os, "History ({} entries):", entriesSpan.size());
                caudio::println(os, "{:>3}  {:<20}  {:<40} {:<40} {:>10} {:>8}", "#", "Date",
                                "Artist", "Title", "Pos", "Dur");
                for (std::size_t i = 0; i < entriesSpan.size(); ++i) {
                    const auto& e = entriesSpan[i];
                    // Convert started_at (milliseconds since epoch) to human readable
                    std::time_t t = static_cast<std::time_t>(e.started_at / 1000);
                    std::tm tm{};
#ifdef _WIN32
                    localtime_s(&tm, &t);
#else
                    localtime_r(&t, &tm);
#endif
                    char timeBuf[32];
                    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tm);
                    std::string posStr = formatTime(e.position_ms / 1000.0);
                    std::string durStr = formatTime(e.duration);
                    caudio::println(os, "{:3}  {:<20}  {:<40} {:<40} {:>10} {:>8}", i, timeBuf,
                                    truncateField(e.artist, 40), truncateField(e.title, 40), posStr,
                                    durStr);
                }
            },
            [&os](const caudio::ipc::Bookmarks& v) {
                caudio::println(os, "Bookmarks ({}):", v.entries.size());
                caudio::println(os, "{:>4}  {:>6}  {:>10}  {}", "ID", "Track", "Pos", "Note");
                for (const auto& e : v.entries) {
                    caudio::println(os, "{:4}  {:>6}  {:>10}  {}", e.id, e.track_id,
                                    formatTime((double)e.position_ms / 1000.0),
                                    truncateField(e.note, 40));
                }
            },
            [&os](const caudio::ipc::Devices& v) {
                std::span<const caudio::types::DeviceInfo> devicesSpan(v.devices.data(),
                                                                       v.devices.size());
                caudio::println(os, "Devices ({}):", devicesSpan.size());
                caudio::println(os, "{:>3}  {:<20}  {:<40}  {}", "#", "ID", "Name", "Default");
                for (std::size_t i = 0; i < devicesSpan.size(); ++i) {
                    const auto& d = devicesSpan[i];
                    caudio::println(os, "{:3}  {:<20}  {:<40}  {}", i, truncateField(d.id, 20),
                                    truncateField(d.name, 40), d.isDefault ? "yes" : "no");
                }
            },
            [&os](const std::monostate&) { caudio::println(os, "OK"); },
            [&os](const caudio::utils::Error& v) {
                // Error variant - print to given stream (caller may pass cerr)
                caudio::println(os, "Error: {} {}", caudio::utils::toString(v.code), v.message);
            }},
        r);
}

int OutputFormatter::printWithStatus(const caudio::ipc::Result& r, std::ostream& out,
                                     std::ostream& err) const {
    bool isError = std::holds_alternative<caudio::utils::Error>(r);
    if (isError) {
        print(r, err);
        return 1;
    }
    print(r, out);
    return 0;
}

} // namespace caudio::app::cli
