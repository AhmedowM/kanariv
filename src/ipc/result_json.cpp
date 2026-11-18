#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <ipc/protocol_internal.hpp>
#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/status_format.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::ipc {

Json toJson(const Result& r) {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Status>) {
                Json j;
                j["type"] = "Status";
                j["state"] = std::format("{}", v.state);
                j["state_value"] = std::to_underlying(v.state);
                j["pos"] = v.pos;
                j["dur"] = v.dur;
                j["vol"] = v.vol;
                j["muted"] = v.muted;
                j["shuffle"] = v.shuffle;
                j["repeat"] = std::format("{}", v.repeat);
                j["repeat_value"] = std::to_underlying(v.repeat);
                j["track_id"] = v.track_id;
                j["title"] = v.title;
                j["artist"] = v.artist;
                j["path"] = v.path;
                j["q_size"] = v.q_size;
                j["q_idx"] = v.q_idx;
                j["version"] = v.version;
                if (!v.transition.empty())
                    j["transition"] = v.transition;
                return j;
            } else if constexpr (std::is_same_v<T, QueueTracks>) {
                Json j;
                j["type"] = "QueueTracks";
                j["tracks"] = Json::array();
                for (const auto& t : v.tracks)
                    j["tracks"].push_back(kanariv::utils::trackToJson(t));
                return j;
            } else if constexpr (std::is_same_v<T, QueueAddReport>) {
                Json j;
                j["type"] = "QueueAddReport";
                j["items"] = Json::array();
                for (const auto& it : v.items) {
                    Json e = Json::object();
                    e["query"] = it.query;
                    if (it.track.id != 0)
                        e["track"] = kanariv::utils::trackToJson(it.track);
                    e["status"] = it.status;
                    if (!it.detail.empty())
                        e["detail"] = it.detail;
                    j["items"].push_back(e);
                }
                return j;
            } else if constexpr (std::is_same_v<T, QueueRemoveReport>) {
                Json j;
                j["type"] = "QueueRemoveReport";
                j["items"] = Json::array();
                for (const auto& it : v.items) {
                    Json e = Json::object();
                    e["selector"] = it.selector;
                    if (it.track.id != 0)
                        e["track"] = kanariv::utils::trackToJson(it.track);
                    e["status"] = it.status;
                    if (!it.detail.empty())
                        e["detail"] = it.detail;
                    j["items"].push_back(e);
                }
                return j;
            } else if constexpr (std::is_same_v<T, Queues>) {
                Json j;
                j["type"] = "Queues";
                j["queues"] = Json::array();
                for (const auto& q : v.entries) {
                    Json e = Json::object();
                    e["id"] = q.id;
                    e["name"] = q.name;
                    e["tracks"] = q.tracks;
                    e["active"] = q.active;
                    e["temp"] = q.temp;
                    j["queues"].push_back(e);
                }
                return j;
            } else if constexpr (std::is_same_v<T, QueueCreated>) {
                Json j;
                j["type"] = "QueueCreated";
                j["id"] = v.id;
                j["name"] = v.name;
                return j;
            } else if constexpr (std::is_same_v<T, PlaylistLoaded>) {
                Json j;
                j["type"] = "PlaylistLoaded";
                j["queue_id"] = v.queue_id;
                j["status"] = toJson(Result{v.status});
                return j;
            } else if constexpr (std::is_same_v<T, VolumeInfo>) {
                Json j;
                j["type"] = "VolumeInfo";
                j["vol"] = v.vol;
                j["muted"] = v.muted;
                return j;
            } else if constexpr (std::is_same_v<T, kanariv::types::LibraryStats>) {
                Json j;
                j["type"] = "LibraryStats";
                j["tracks"] = v.tracks;
                j["queues"] = v.queues;
                j["playlists"] = v.playlists;
                j["total_duration_ms"] = v.total_duration_ms;
                j["total_play_time_ms"] = v.total_play_time_ms;
                if (v.top_tracks) {
                    j["top_tracks"] = Json::array();
                    for (const auto& t : *v.top_tracks)
                        j["top_tracks"].push_back(kanariv::utils::trackToJson(t));
                }
                if (v.top_albums) {
                    j["top_albums"] = Json::array();
                    for (const auto& e : *v.top_albums) {
                        Json je;
                        je["name"] = e.name;
                        je["track_count"] = static_cast<int64_t>(e.track_count);
                        je["total_plays"] = e.total_plays;
                        j["top_albums"].push_back(je);
                    }
                }
                if (v.top_genres) {
                    j["top_genres"] = Json::array();
                    for (const auto& e : *v.top_genres) {
                        Json je;
                        je["name"] = e.name;
                        je["track_count"] = static_cast<int64_t>(e.track_count);
                        je["total_plays"] = e.total_plays;
                        j["top_genres"].push_back(je);
                    }
                }
                if (v.top_artists) {
                    j["top_artists"] = Json::array();
                    for (const auto& e : *v.top_artists) {
                        Json je;
                        je["name"] = e.name;
                        je["track_count"] = static_cast<int64_t>(e.track_count);
                        je["total_plays"] = e.total_plays;
                        j["top_artists"].push_back(je);
                    }
                }
                return j;
            } else if constexpr (std::is_same_v<T, Tracks>) {
                Json j;
                j["type"] = "Tracks";
                j["tracks"] = Json::array();
                for (const auto& t : v.tracks)
                    j["tracks"].push_back(kanariv::utils::trackToJson(t));
                return j;
            } else if constexpr (std::is_same_v<T, SearchResults>) {
                Json j;
                j["type"] = "SearchResults";
                j["query"] = v.query;
                j["tracks"] = Json::array();
                for (const auto& t : v.tracks)
                    j["tracks"].push_back(kanariv::utils::trackToJson(t));
                return j;
            } else if constexpr (std::is_same_v<T, ScanReport>) {
                Json j;
                j["type"] = "ScanReport";
                j["added"] = v.added;
                return j;
            } else if constexpr (std::is_same_v<T, AnalyzeReport>) {
                Json j;
                j["type"] = "AnalyzeReport";
                j["tracks"] = v.tracks;
                j["analyzed"] = v.analyzed;
                j["failed"] = v.failed;
                j["skipped"] = v.skipped;
                return j;
            } else if constexpr (std::is_same_v<T, Playlists>) {
                Json j;
                j["type"] = "Playlists";
                j["playlists"] = Json::array();
                for (const auto& p : v.playlists)
                    j["playlists"].push_back(detail::playlistToJson(p));
                return j;
            } else if constexpr (std::is_same_v<T, PlaylistData>) {
                Json j;
                j["type"] = "PlaylistData";
                j["tracks"] = Json::array();
                for (const auto& t : v.tracks)
                    j["tracks"].push_back(kanariv::utils::trackToJson(t));
                j["format"] = v.format;
                return j;
            } else if constexpr (std::is_same_v<T, PlaylistCreated>) {
                Json j;
                j["type"] = "PlaylistCreated";
                j["id"] = v.id;
                j["name"] = v.name;
                return j;
            } else if constexpr (std::is_same_v<T, PlaylistImportReport>) {
                Json j;
                j["type"] = "PlaylistImportReport";
                j["pid"] = v.pid;
                j["name"] = v.name;
                j["tracks"] = Json::array();
                for (const auto& t : v.tracks)
                    j["tracks"].push_back(kanariv::utils::trackToJson(t));
                j["matched"] = v.matched;
                j["skipped"] = v.skipped;
                j["duplicates"] = v.duplicates;
                return j;
            } else if constexpr (std::is_same_v<T, LibraryMaintenanceReport>) {
                Json j;
                j["type"] = "LibraryMaintenanceReport";
                j["tracks_checked"] = v.tracks_checked;
                j["missing_tracks"] = Json::array();
                for (const auto& t : v.missing_tracks)
                    j["missing_tracks"].push_back(kanariv::utils::trackToJson(t));
                j["duplicate_groups"] = v.duplicate_groups;
                j["duplicate_tracks"] = v.duplicate_tracks;
                return j;
            } else if constexpr (std::is_same_v<T, ConfigValue>) {
                Json j;
                j["type"] = "ConfigValue";
                j["key"] = v.key;
                j["value"] = v.value;
                return j;
            } else if constexpr (std::is_same_v<T, ConfigValues>) {
                Json j;
                j["type"] = "ConfigValues";
                j["values"] = Json::array();
                for (const auto& cv : v.values) {
                    Json kv = Json::object();
                    kv["key"] = cv.key;
                    kv["value"] = cv.value;
                    j["values"].push_back(kv);
                }
                return j;
            } else if constexpr (std::is_same_v<T, SingleTrack>) {
                Json j;
                j["type"] = "SingleTrack";
                j["track"] = kanariv::utils::trackToJson(v.track);
                if (!v.cover_mime.empty())
                    j["cover_mime"] = v.cover_mime;
                if (!v.cover_data.empty())
                    j["cover_data"] = v.cover_data;
                return j;
            } else if constexpr (std::is_same_v<T, TrackInfo>) {
                Json j;
                j["type"] = "TrackInfo";
                j["track"] = kanariv::utils::trackToJson(v.track);
                j["play_count"] = v.play_count;
                j["last_played"] = v.last_played;
                return j;
            } else if constexpr (std::is_same_v<T, types::HistoryEntry>) {
                Json j;
                j["type"] = "HistoryEntry";
                j["id"] = v.id;
                j["track_id"] = v.track_id;
                j["started_at"] = v.started_at;
                j["completed_at"] = v.completed_at;
                j["position_ms"] = v.position_ms;
                j["completion_pct"] = v.completion_pct;
                j["queue_id"] = v.queue_id;
                j["title"] = v.title;
                j["artist"] = v.artist;
                j["path"] = v.path;
                j["duration"] = v.duration;
                return j;
            } else if constexpr (std::is_same_v<T, History>) {
                Json j;
                j["type"] = "History";
                j["entries"] = Json::array();
                for (const auto& e : v.entries) {
                    Json ej;
                    ej["id"] = e.id;
                    ej["track_id"] = e.track_id;
                    ej["started_at"] = e.started_at;
                    ej["completed_at"] = e.completed_at;
                    ej["position_ms"] = e.position_ms;
                    ej["completion_pct"] = e.completion_pct;
                    ej["queue_id"] = e.queue_id;
                    ej["title"] = e.title;
                    ej["artist"] = e.artist;
                    ej["path"] = e.path;
                    ej["duration"] = e.duration;
                    j["entries"].push_back(ej);
                }
                return j;
            } else if constexpr (std::is_same_v<T, Bookmarks>) {
                Json j;
                j["type"] = "Bookmarks";
                j["entries"] = Json::array();
                for (const auto& e : v.entries) {
                    Json ej;
                    ej["id"] = e.id;
                    ej["track_id"] = e.track_id;
                    ej["position_ms"] = e.position_ms;
                    ej["note"] = e.note;
                    ej["created"] = e.created;
                    j["entries"].push_back(ej);
                }
                return j;
            } else if constexpr (std::is_same_v<T, Lyrics>) {
                Json j;
                j["type"] = "Lyrics";
                j["id"] = v.id;
                j["track_id"] = v.track_id;
                j["lrc_text"] = v.lrc_text;
                j["is_synced"] = v.is_synced;
                j["source"] = v.source;
                j["fetched_at"] = v.fetched_at;
                return j;
            } else if constexpr (std::is_same_v<T, TagBatchEditResult>) {
                Json j;
                j["type"] = "TagBatchEditResult";
                j["succeeded"] = v.succeeded;
                j["failed"] = v.failed;
                j["results"] = Json::array();
                for (const auto& r : v.results) {
                    Json er = Json::object();
                    if (r.id.has_value())
                        er["id"] = *r.id;
                    if (r.path.has_value())
                        er["path"] = *r.path;
                    er["field"] = r.field;
                    er["ok"] = r.ok;
                    er["error"] = r.error;
                    j["results"].push_back(std::move(er));
                }
                return j;
            } else if constexpr (std::is_same_v<T, Devices>) {
                Json j;
                j["type"] = "Devices";
                j["devices"] = Json::array();
                for (const auto& d : v.devices) {
                    Json dj;
                    dj["id"] = d.id;
                    dj["name"] = d.name;
                    dj["isDefault"] = d.isDefault;
                    j["devices"].push_back(dj);
                }
                return j;
            } else if constexpr (std::is_same_v<T, Empty>) {
                Json j;
                j["type"] = "Empty";
                return j;
            } else {
                Json j;
                j["type"] = "Unknown";
                return j;
            }
        },
        r);
}

std::expected<Result, kanariv::utils::Error> resultFromJson(const Json& j) {
    try {
        if (!j.contains("type") || !j["type"].isString()) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "missing Result type")};
        }
        std::string t = j["type"].get<std::string>();
        if (t == "Status") {
            Status s{};
            if (j.contains("state") && j["state"].isString()) {
                auto ps = kanariv::types::parsePlaybackState(j["state"].get<std::string>());
                if (!ps)
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg, "unknown PlaybackState")};
                s.state = *ps;
            } else if (j.contains("state_value") && j["state_value"].isNumber()) {
                int v = j["state_value"].get<int>();
                s.state = static_cast<kanariv::types::PlaybackState>(v);
            }
            if (j.contains("pos") && j["pos"].isNumber())
                s.pos = j["pos"].get<double>();
            if (j.contains("dur") && j["dur"].isNumber())
                s.dur = j["dur"].get<double>();
            if (j.contains("vol") && j["vol"].isNumber())
                s.vol = j["vol"].get<float>();
            if (j.contains("muted") && j["muted"].isBoolean())
                s.muted = j["muted"].get<bool>();
            if (j.contains("shuffle") && j["shuffle"].isBoolean())
                s.shuffle = j["shuffle"].get<bool>();
            if (j.contains("repeat") && j["repeat"].isString()) {
                auto rm = kanariv::types::parseRepeatMode(j["repeat"].get<std::string>());
                if (!rm)
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg, "unknown RepeatMode")};
                s.repeat = *rm;
            } else if (j.contains("repeat_value") && j["repeat_value"].isNumber()) {
                int v = j["repeat_value"].get<int>();
                s.repeat = static_cast<kanariv::types::RepeatMode>(v);
            }
            if (j.contains("track_id") && j["track_id"].isNumber())
                s.track_id = j["track_id"].get<int64_t>();
            if (j.contains("title") && j["title"].isString())
                s.title = j["title"].get<std::string>();
            if (j.contains("artist") && j["artist"].isString())
                s.artist = j["artist"].get<std::string>();
            if (j.contains("path") && j["path"].isString())
                s.path = j["path"].get<std::string>();
            if (j.contains("q_size") && j["q_size"].isNumber())
                s.q_size = j["q_size"].get<std::size_t>();
            if (j.contains("q_idx") && j["q_idx"].isNumber())
                s.q_idx = j["q_idx"].get<std::size_t>();
            if (j.contains("version") && j["version"].isString())
                s.version = j["version"].get<std::string>();
            if (j.contains("transition") && j["transition"].isString())
                s.transition = j["transition"].get<std::string>();
            return Result{std::move(s)};
        }
        if (t == "QueueTracks") {
            QueueTracks qt{};
            if (j.contains("tracks") && j["tracks"].isArray()) {
                const Json tracks = j["tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto jt = tracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    qt.tracks.push_back(std::move(*tr));
                }
            }
            return Result{std::move(qt)};
        }
        if (t == "QueueAddReport" || t == "QueueRemoveReport") {
            const bool adding = t == "QueueAddReport";
            QueueAddReport addReport{};
            QueueRemoveReport removeReport{};
            if (j.contains("items") && j["items"].isArray()) {
                const Json items = j["items"];
                for (std::size_t ii = 0, in = items.size(); ii < in; ++ii) {
                    auto je = items.at(ii);
                    if (!je)
                        return std::unexpected{je.error()};
                    const Json& item = *je;
                    kanariv::types::TrackRow track{};
                    if (item.contains("track")) {
                        auto tr = kanariv::utils::trackFromJson(item["track"]);
                        if (!tr)
                            return std::unexpected{tr.error()};
                        track = std::move(*tr);
                    }
                    std::string detail;
                    if (auto d = item.at("detail"); d && d->isString())
                        detail = d->get<std::string>();
                    if (adding) {
                        QueueAddItem it{};
                        if (auto q = item.at("query"); q && q->isString())
                            it.query = q->get<std::string>();
                        if (auto s = item.at("status"); s && s->isString())
                            it.status = s->get<std::string>();
                        it.track = std::move(track);
                        it.detail = std::move(detail);
                        addReport.items.push_back(std::move(it));
                    } else {
                        QueueRemoveItem it{};
                        if (auto s = item.at("selector"); s && s->isString())
                            it.selector = s->get<std::string>();
                        if (auto st = item.at("status"); st && st->isString())
                            it.status = st->get<std::string>();
                        it.track = std::move(track);
                        it.detail = std::move(detail);
                        removeReport.items.push_back(std::move(it));
                    }
                }
            }
            if (adding)
                return Result{std::move(addReport)};
            return Result{std::move(removeReport)};
        }
        if (t == "Queues") {
            Queues qs{};
            if (j.contains("queues") && j["queues"].isArray()) {
                const Json arr = j["queues"];
                for (std::size_t qi = 0, qn = arr.size(); qi < qn; ++qi) {
                    auto je = arr.at(qi);
                    if (!je)
                        return std::unexpected{je.error()};
                    QueueEntry e{};
                    if (auto idExp = je->at("id"); idExp && idExp->isNumber())
                        e.id = idExp->get<int64_t>();
                    if (auto nameExp = je->at("name"); nameExp && nameExp->isString())
                        e.name = nameExp->get<std::string>();
                    if (auto trExp = je->at("tracks"); trExp && trExp->isNumber())
                        e.tracks = trExp->get<std::size_t>();
                    if (auto acExp = je->at("active"); acExp && acExp->isBoolean())
                        e.active = acExp->get<bool>();
                    if (auto tmExp = je->at("temp"); tmExp && tmExp->isBoolean())
                        e.temp = tmExp->get<bool>();
                    qs.entries.push_back(std::move(e));
                }
            }
            return Result{std::move(qs)};
        }
        if (t == "QueueCreated") {
            QueueCreated q{};
            if (j.contains("id") && j["id"].isNumber())
                q.id = j["id"].get<int64_t>();
            if (j.contains("name") && j["name"].isString())
                q.name = j["name"].get<std::string>();
            return Result{std::move(q)};
        }
        if (t == "PlaylistLoaded") {
            PlaylistLoaded p{};
            if (j.contains("queue_id") && j["queue_id"].isNumber())
                p.queue_id = j["queue_id"].get<int64_t>();
            if (j.contains("status")) {
                auto sub = resultFromJson(j["status"]);
                if (!sub)
                    return std::unexpected{sub.error()};
                if (auto* st = std::get_if<Status>(&*sub))
                    p.status = std::move(*st);
                else
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::Corrupt, "PlaylistLoaded status not a Status")};
            }
            return Result{std::move(p)};
        }
        if (t == "VolumeInfo") {
            VolumeInfo vi{};
            if (j.contains("vol") && j["vol"].isNumber())
                vi.vol = j["vol"].get<float>();
            if (j.contains("muted") && j["muted"].isBoolean())
                vi.muted = j["muted"].get<bool>();
            return Result{std::move(vi)};
        }
        if (t == "LibraryStats" || t == "LibraryStatsDetailed") {
            kanariv::types::LibraryStats ls{};
            if (j.contains("tracks") && j["tracks"].isNumber())
                ls.tracks = j["tracks"].get<std::size_t>();
            if (j.contains("queues") && j["queues"].isNumber())
                ls.queues = j["queues"].get<std::size_t>();
            if (j.contains("playlists") && j["playlists"].isNumber())
                ls.playlists = j["playlists"].get<std::size_t>();
            if (j.contains("total_duration_ms") && j["total_duration_ms"].isNumber())
                ls.total_duration_ms = j["total_duration_ms"].get<int64_t>();
            if (j.contains("total_play_time_ms") && j["total_play_time_ms"].isNumber())
                ls.total_play_time_ms = j["total_play_time_ms"].get<int64_t>();
            if (j.contains("top_tracks") && j["top_tracks"].isArray()) {
                ls.top_tracks.emplace();
                const Json topTracks = j["top_tracks"];
                for (std::size_t ti = 0, tn = topTracks.size(); ti < tn; ++ti) {
                    auto jt = topTracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    ls.top_tracks->push_back(std::move(*tr));
                }
            } else if (t == "LibraryStatsDetailed") {
                ls.top_tracks.emplace();
            }
            if (j.contains("top_albums") && j["top_albums"].isArray()) {
                ls.top_albums.emplace();
                const Json topAlbums = j["top_albums"];
                for (std::size_t ti = 0, tn = topAlbums.size(); ti < tn; ++ti) {
                    auto jt = topAlbums.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    const Json& jo = *jt;
                    kanariv::types::TopEntity e;
                    if (jo.contains("name") && jo["name"].isString())
                        e.name = jo["name"].get<std::string>();
                    if (jo.contains("track_count") && jo["track_count"].isNumber())
                        e.track_count = static_cast<std::size_t>(jo["track_count"].get<int64_t>());
                    if (jo.contains("total_plays") && jo["total_plays"].isNumber())
                        e.total_plays = jo["total_plays"].get<int64_t>();
                    ls.top_albums->push_back(std::move(e));
                }
            }
            if (j.contains("top_genres") && j["top_genres"].isArray()) {
                ls.top_genres.emplace();
                const Json topGenres = j["top_genres"];
                for (std::size_t ti = 0, tn = topGenres.size(); ti < tn; ++ti) {
                    auto jt = topGenres.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    const Json& jo = *jt;
                    kanariv::types::TopEntity e;
                    if (jo.contains("name") && jo["name"].isString())
                        e.name = jo["name"].get<std::string>();
                    if (jo.contains("track_count") && jo["track_count"].isNumber())
                        e.track_count = static_cast<std::size_t>(jo["track_count"].get<int64_t>());
                    if (jo.contains("total_plays") && jo["total_plays"].isNumber())
                        e.total_plays = jo["total_plays"].get<int64_t>();
                    ls.top_genres->push_back(std::move(e));
                }
            }
            if (j.contains("top_artists") && j["top_artists"].isArray()) {
                ls.top_artists.emplace();
                const Json topArtists = j["top_artists"];
                for (std::size_t ti = 0, tn = topArtists.size(); ti < tn; ++ti) {
                    auto jt = topArtists.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    const Json& jo = *jt;
                    kanariv::types::TopEntity e;
                    if (jo.contains("name") && jo["name"].isString())
                        e.name = jo["name"].get<std::string>();
                    if (jo.contains("track_count") && jo["track_count"].isNumber())
                        e.track_count = static_cast<std::size_t>(jo["track_count"].get<int64_t>());
                    if (jo.contains("total_plays") && jo["total_plays"].isNumber())
                        e.total_plays = jo["total_plays"].get<int64_t>();
                    ls.top_artists->push_back(std::move(e));
                }
            }
            return Result{std::move(ls)};
        }
        if (t == "Tracks") {
            Tracks trs{};
            if (j.contains("tracks") && j["tracks"].isArray()) {
                const Json tracks = j["tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto jt = tracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    trs.tracks.push_back(std::move(*tr));
                }
            }
            return Result{std::move(trs)};
        }
        if (t == "SearchResults") {
            SearchResults sr{};
            if (j.contains("query") && j["query"].isString())
                sr.query = j["query"].get<std::string>();
            if (j.contains("tracks") && j["tracks"].isArray()) {
                const Json tracks = j["tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto jt = tracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    sr.tracks.push_back(std::move(*tr));
                }
            }
            return Result{std::move(sr)};
        }
        if (t == "ScanReport") {
            ScanReport sr{};
            if (j.contains("added") && j["added"].isNumber())
                sr.added = j["added"].get<std::size_t>();
            return Result{std::move(sr)};
        }
        if (t == "AnalyzeReport") {
            AnalyzeReport ar{};
            if (j.contains("tracks") && j["tracks"].isNumber())
                ar.tracks = j["tracks"].get<int64_t>();
            if (j.contains("analyzed") && j["analyzed"].isNumber())
                ar.analyzed = j["analyzed"].get<int64_t>();
            if (j.contains("failed") && j["failed"].isNumber())
                ar.failed = j["failed"].get<int64_t>();
            if (j.contains("skipped") && j["skipped"].isNumber())
                ar.skipped = j["skipped"].get<int64_t>();
            return Result{std::move(ar)};
        }
        if (t == "Playlists") {
            Playlists pl{};
            if (j.contains("playlists") && j["playlists"].isArray()) {
                const Json playlists = j["playlists"];
                for (std::size_t pi = 0, pn = playlists.size(); pi < pn; ++pi) {
                    auto jp = playlists.at(pi);
                    if (!jp)
                        return std::unexpected{jp.error()};
                    auto pr = detail::playlistFromJson(*jp);
                    if (!pr)
                        return std::unexpected{pr.error()};
                    pl.playlists.push_back(std::move(*pr));
                }
            }
            return Result{std::move(pl)};
        }
        if (t == "PlaylistData") {
            PlaylistData pd{};
            if (j.contains("tracks") && j["tracks"].isArray()) {
                const Json tracks = j["tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto jt = tracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    pd.tracks.push_back(std::move(*tr));
                }
            }
            if (j.contains("format") && j["format"].isString())
                pd.format = j["format"].get<std::string>();
            return Result{std::move(pd)};
        }
        if (t == "PlaylistCreated") {
            PlaylistCreated p{};
            if (j.contains("id") && j["id"].isNumber())
                p.id = j["id"].get<int64_t>();
            if (j.contains("name") && j["name"].isString())
                p.name = j["name"].get<std::string>();
            return Result{std::move(p)};
        }
        if (t == "PlaylistImportReport") {
            PlaylistImportReport r{};
            if (j.contains("pid") && j["pid"].isNumber())
                r.pid = j["pid"].get<int64_t>();
            if (j.contains("name") && j["name"].isString())
                r.name = j["name"].get<std::string>();
            if (j.contains("tracks") && j["tracks"].isArray()) {
                const Json tracks = j["tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto jt = tracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    r.tracks.push_back(std::move(*tr));
                }
            }
            if (j.contains("matched") && j["matched"].isNumber())
                r.matched = j["matched"].get<std::size_t>();
            if (j.contains("skipped") && j["skipped"].isNumber())
                r.skipped = j["skipped"].get<std::size_t>();
            if (j.contains("duplicates") && j["duplicates"].isNumber())
                r.duplicates = j["duplicates"].get<std::size_t>();
            return Result{std::move(r)};
        }
        if (t == "LibraryMaintenanceReport") {
            LibraryMaintenanceReport r{};
            if (j.contains("tracks_checked") && j["tracks_checked"].isNumber())
                r.tracks_checked = j["tracks_checked"].get<std::size_t>();
            if (j.contains("missing_tracks") && j["missing_tracks"].isArray()) {
                const Json tracks = j["missing_tracks"];
                for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
                    auto jt = tracks.at(ti);
                    if (!jt)
                        return std::unexpected{jt.error()};
                    auto tr = kanariv::utils::trackFromJson(*jt);
                    if (!tr)
                        return std::unexpected{tr.error()};
                    r.missing_tracks.push_back(std::move(*tr));
                }
            }
            if (j.contains("duplicate_groups") && j["duplicate_groups"].isNumber())
                r.duplicate_groups = j["duplicate_groups"].get<std::size_t>();
            if (j.contains("duplicate_tracks") && j["duplicate_tracks"].isNumber())
                r.duplicate_tracks = j["duplicate_tracks"].get<std::size_t>();
            return Result{std::move(r)};
        }
        if (t == "ConfigValue") {
            ConfigValue cv{};
            if (j.contains("key") && j["key"].isString())
                cv.key = j["key"].get<std::string>();
            if (j.contains("value") && j["value"].isString())
                cv.value = j["value"].get<std::string>();
            return Result{std::move(cv)};
        }
        if (t == "ConfigValues") {
            ConfigValues cvs{};
            if (j.contains("values") && j["values"].isArray()) {
                const Json values = j["values"];
                for (std::size_t vi = 0, vn = values.size(); vi < vn; ++vi) {
                    auto jvExp = values.at(vi);
                    if (!jvExp)
                        return std::unexpected{jvExp.error()};
                    const Json jv = std::move(*jvExp);
                    ConfigValue cv{};
                    if (jv.contains("key") && jv["key"].isString())
                        cv.key = jv["key"].get<std::string>();
                    if (jv.contains("value") && jv["value"].isString())
                        cv.value = jv["value"].get<std::string>();
                    cvs.values.push_back(std::move(cv));
                }
            }
            return Result{std::move(cvs)};
        }
        if (t == "SingleTrack") {
            SingleTrack st{};
            if (j.contains("track")) {
                auto tr = kanariv::utils::trackFromJson(j["track"]);
                if (!tr)
                    return std::unexpected{tr.error()};
                st.track = std::move(*tr);
            } else if (j.contains("id")) {
                // legacy: track fields directly in object
                auto tr = kanariv::utils::trackFromJson(j);
                if (!tr)
                    return std::unexpected{tr.error()};
                st.track = std::move(*tr);
            }
            if (j.contains("cover_mime") && j["cover_mime"].isString())
                st.cover_mime = j["cover_mime"].get<std::string>();
            if (j.contains("cover_data") && j["cover_data"].isString())
                st.cover_data = j["cover_data"].get<std::string>();
            return Result{std::move(st)};
        }
        if (t == "TrackInfo") {
            TrackInfo ti{};
            if (j.contains("track")) {
                auto tr = kanariv::utils::trackFromJson(j["track"]);
                if (!tr)
                    return std::unexpected{tr.error()};
                ti.track = std::move(*tr);
            }
            if (j.contains("play_count") && j["play_count"].isNumber())
                ti.play_count = j["play_count"].get<int64_t>();
            if (j.contains("last_played") && j["last_played"].isNumber())
                ti.last_played = j["last_played"].get<int64_t>();
            return Result{std::move(ti)};
        }
        if (t == "History") {
            History h{};
            if (j.contains("entries") && j["entries"].isArray()) {
                const Json entries = j["entries"];
                for (std::size_t ei = 0, en = entries.size(); ei < en; ++ei) {
                    auto jeExp = entries.at(ei);
                    if (!jeExp)
                        return std::unexpected{jeExp.error()};
                    const Json je = std::move(*jeExp);
                    types::HistoryEntry e{};
                    if (je.contains("id") && je["id"].isNumber())
                        e.id = je["id"].get<int64_t>();
                    if (je.contains("track_id") && je["track_id"].isNumber())
                        e.track_id = je["track_id"].get<int64_t>();
                    if (je.contains("started_at") && je["started_at"].isNumber())
                        e.started_at = je["started_at"].get<int64_t>();
                    if (je.contains("completed_at") && je["completed_at"].isNumber())
                        e.completed_at = je["completed_at"].get<int64_t>();
                    if (je.contains("position_ms") && je["position_ms"].isNumber())
                        e.position_ms = je["position_ms"].get<int64_t>();
                    if (je.contains("completion_pct") && je["completion_pct"].isNumber())
                        e.completion_pct = je["completion_pct"].get<double>();
                    if (je.contains("queue_id") && je["queue_id"].isNumber())
                        e.queue_id = je["queue_id"].get<int64_t>();
                    if (je.contains("title") && je["title"].isString())
                        e.title = je["title"].get<std::string>();
                    if (je.contains("artist") && je["artist"].isString())
                        e.artist = je["artist"].get<std::string>();
                    if (je.contains("path") && je["path"].isString())
                        e.path = je["path"].get<std::string>();
                    if (je.contains("duration") && je["duration"].isNumber())
                        e.duration = je["duration"].get<double>();
                    h.entries.push_back(std::move(e));
                }
            }
            return Result{std::move(h)};
        }
        if (t == "Bookmarks") {
            Bookmarks b{};
            if (j.contains("entries") && j["entries"].isArray()) {
                const Json entries = j["entries"];
                for (std::size_t bi = 0, bn = entries.size(); bi < bn; ++bi) {
                    auto jbExp = entries.at(bi);
                    if (!jbExp)
                        return std::unexpected{jbExp.error()};
                    const Json jb = std::move(*jbExp);
                    types::Bookmark e{};
                    if (jb.contains("id") && jb["id"].isNumber())
                        e.id = jb["id"].get<int64_t>();
                    if (jb.contains("track_id") && jb["track_id"].isNumber())
                        e.track_id = jb["track_id"].get<int64_t>();
                    if (jb.contains("position_ms") && jb["position_ms"].isNumber())
                        e.position_ms = jb["position_ms"].get<int64_t>();
                    if (jb.contains("note") && jb["note"].isString())
                        e.note = jb["note"].get<std::string>();
                    if (jb.contains("created") && jb["created"].isNumber())
                        e.created = jb["created"].get<int64_t>();
                    b.entries.push_back(std::move(e));
                }
            }
            return Result{std::move(b)};
        }
        if (t == "Lyrics") {
            Lyrics l{};
            if (j.contains("id") && j["id"].isNumber())
                l.id = j["id"].get<int64_t>();
            if (j.contains("track_id") && j["track_id"].isNumber())
                l.track_id = j["track_id"].get<int64_t>();
            if (j.contains("lrc_text") && j["lrc_text"].isString())
                l.lrc_text = j["lrc_text"].get<std::string>();
            if (j.contains("is_synced") && j["is_synced"].isBoolean())
                l.is_synced = j["is_synced"].get<bool>();
            if (j.contains("source") && j["source"].isString())
                l.source = j["source"].get<std::string>();
            if (j.contains("fetched_at") && j["fetched_at"].isNumber())
                l.fetched_at = j["fetched_at"].get<int64_t>();
            return Result{std::move(l)};
        }
        if (t == "TagBatchEditResult") {
            TagBatchEditResult r{};
            if (j.contains("succeeded") && j["succeeded"].isNumber())
                r.succeeded = j["succeeded"].get<int64_t>();
            if (j.contains("failed") && j["failed"].isNumber())
                r.failed = j["failed"].get<int64_t>();
            if (j.contains("results") && j["results"].isArray()) {
                const Json arr = j["results"];
                for (std::size_t ri = 0, rn = arr.size(); ri < rn; ++ri) {
                    auto jreExp = arr.at(ri);
                    if (!jreExp)
                        return std::unexpected{jreExp.error()};
                    const Json jre = std::move(*jreExp);
                    TagBatchEditEntryResult er{};
                    if (jre.contains("id") && jre["id"].isNumber())
                        er.id = jre["id"].get<int64_t>();
                    if (jre.contains("path") && jre["path"].isString())
                        er.path = jre["path"].get<std::string>();
                    if (jre.contains("field") && jre["field"].isString())
                        er.field = jre["field"].get<std::string>();
                    if (jre.contains("ok") && jre["ok"].isBoolean())
                        er.ok = jre["ok"].get<bool>();
                    if (jre.contains("error") && jre["error"].isString())
                        er.error = jre["error"].get<std::string>();
                    r.results.push_back(std::move(er));
                }
            }
            return Result{std::move(r)};
        }
        if (t == "Devices") {
            Devices d{};
            if (j.contains("devices") && j["devices"].isArray()) {
                const Json devices = j["devices"];
                for (std::size_t di_ = 0, dn = devices.size(); di_ < dn; ++di_) {
                    auto jdExp = devices.at(di_);
                    if (!jdExp)
                        return std::unexpected{jdExp.error()};
                    const Json jd = std::move(*jdExp);
                    types::DeviceInfo di{};
                    if (jd.contains("id") && jd["id"].isString())
                        di.id = jd["id"].get<std::string>();
                    if (jd.contains("name") && jd["name"].isString())
                        di.name = jd["name"].get<std::string>();
                    if (jd.contains("isDefault") && jd["isDefault"].isBoolean())
                        di.isDefault = jd["isDefault"].get<bool>();
                    d.devices.push_back(std::move(di));
                }
            }
            return Result{std::move(d)};
        }
        if (t == "Empty") {
            return Result{Empty{}};
        }
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "unknown Result type: " + t)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::string toJsonString(const Result& r) {
    return toJson(r).dump(2);
}
} // namespace kanariv::ipc
