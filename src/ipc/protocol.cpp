#include <kanariv/ipc/command.hpp>
#include <kanariv/ipc/protocol.hpp>
#include <kanariv/ipc/result.hpp>
#include <kanariv/types/format.hpp>
#include <kanariv/types/parse.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/result.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <ipc/protocol_internal.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::ipc::detail {

std::string resultCodeToString(kanariv::utils::StatusCode r) {
    return std::string(kanariv::utils::toString(r));
}

std::expected<kanariv::utils::StatusCode, kanariv::utils::Error>
resultCodeFromString(std::string_view sv) {
    using R = kanariv::utils::StatusCode;
    if (sv == "Ok")
        return R::Ok;
    if (sv == "InvalidArg")
        return R::InvalidArg;
    if (sv == "NotFound")
        return R::NotFound;
    if (sv == "Unsupported")
        return R::Unsupported;
    if (sv == "Io")
        return R::Io;
    if (sv == "Device")
        return R::Device;
    if (sv == "State")
        return R::State;
    if (sv == "NoMem")
        return R::NoMem;
    if (sv == "Internal")
        return R::Internal;
    if (sv == "AlreadyExists")
        return R::AlreadyExists;
    if (sv == "Busy")
        return R::Busy;
    if (sv == "Corrupt")
        return R::Corrupt;
    if (sv == "NoSpace")
        return R::NoSpace;
    return std::unexpected{kanariv::utils::makeError(R::InvalidArg, "unknown Result code")};
}

// Track conversion lives in kanariv::utils (utils/record_json.hpp) -- single
// definition shared by db export/import and the IPC codec.
// (The former ipc::detail subset is deleted; wire readers tolerate the
// fuller db field set: all reads are contains-guarded.)

Json playlistToJson(const kanariv::types::Playlist& p) {
    Json j;
    j["id"] = p.id;
    j["name"] = p.name;
    j["type"] = p.type;
    j["smart_query"] = p.smart_query;
    j["created"] = p.created;
    j["modified"] = p.modified;
    j["library_id"] = p.library_id;
    return j;
}

std::expected<kanariv::types::Playlist, kanariv::utils::Error> playlistFromJson(const Json& j) {
    try {
        kanariv::types::Playlist p{};
        if (j.contains("id") && j["id"].isNumber())
            p.id = j["id"].get<int64_t>();
        if (j.contains("name") && j["name"].isString())
            p.name = j["name"].get<std::string>();
        if (j.contains("type") && j["type"].isNumber())
            p.type = j["type"].get<int32_t>();
        if (j.contains("smart_query") && j["smart_query"].isString())
            p.smart_query = j["smart_query"].get<std::string>();
        if (j.contains("created") && j["created"].isNumber())
            p.created = j["created"].get<int64_t>();
        if (j.contains("modified") && j["modified"].isNumber())
            p.modified = j["modified"].get<int64_t>();
        if (j.contains("library_id") && j["library_id"].isNumber())
            p.library_id = j["library_id"].get<int64_t>();
        return p;
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

Json errorToJson(const kanariv::utils::Error& e) {
    Json j;
    j["type"] = "Error";
    j["code"] = resultCodeToString(e.code);
    j["code_value"] = std::to_underlying(e.code);
    j["message"] = e.message;
    return j;
}

std::expected<kanariv::utils::Error, kanariv::utils::Error> errorFromJson(const Json& j) {
    try {
        std::string codeStr;
        std::string msg;
        if (j.contains("code") && j["code"].isString())
            codeStr = j["code"].get<std::string>();
        else if (j.contains("code_value") && j["code_value"].isNumber()) {
            int v = j["code_value"].get<int>();
            // map via to_underlying comparison
            for (auto c :
                 {kanariv::utils::StatusCode::Ok, kanariv::utils::StatusCode::InvalidArg,
                  kanariv::utils::StatusCode::NotFound, kanariv::utils::StatusCode::Unsupported,
                  kanariv::utils::StatusCode::Io, kanariv::utils::StatusCode::Device,
                  kanariv::utils::StatusCode::State, kanariv::utils::StatusCode::NoMem,
                  kanariv::utils::StatusCode::Internal, kanariv::utils::StatusCode::AlreadyExists,
                  kanariv::utils::StatusCode::Busy, kanariv::utils::StatusCode::Corrupt,
                  kanariv::utils::StatusCode::NoSpace}) {
                if (std::to_underlying(c) == v) {
                    codeStr = std::string(kanariv::utils::toString(c));
                    break;
                }
            }
        }
        if (j.contains("message") && j["message"].isString())
            msg = j["message"].get<std::string>();
        if (codeStr.empty())
            codeStr = "Internal";
        auto rc = resultCodeFromString(codeStr);
        if (!rc)
            return std::unexpected{rc.error()};
        return kanariv::utils::Error{*rc, msg};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace kanariv::ipc::detail

namespace kanariv::ipc {

Json toJson(const Command& cmd) {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            Json j;
            if constexpr (std::is_same_v<T, Play>) {
                j["type"] = "Play";
            } else if constexpr (std::is_same_v<T, Pause>) {
                j["type"] = "Pause";
            } else if constexpr (std::is_same_v<T, Resume>) {
                j["type"] = "Resume";
            } else if constexpr (std::is_same_v<T, Restart>) {
                j["type"] = "Restart";
            } else if constexpr (std::is_same_v<T, Stop>) {
                j["type"] = "Stop";
            } else if constexpr (std::is_same_v<T, Next>) {
                j["type"] = "Next";
            } else if constexpr (std::is_same_v<T, Prev>) {
                j["type"] = "Prev";
            } else if constexpr (std::is_same_v<T, Seek>) {
                j["type"] = "Seek";
                j["seconds"] = v.seconds;
            } else if constexpr (std::is_same_v<T, StatusReq>) {
                j["type"] = "StatusReq";
            } else if constexpr (std::is_same_v<T, StatusWait>) {
                j["type"] = "StatusWait";
                j["timeout_ms"] = v.timeout_ms;
            } else if constexpr (std::is_same_v<T, VolumeSet>) {
                j["type"] = "VolumeSet";
                if (v.level.has_value())
                    j["level"] = *v.level;
                else
                    j["level"] = nullptr;
                if (v.mute.has_value())
                    j["mute"] = *v.mute;
                else
                    j["mute"] = nullptr;
                if (v.deltaPct.has_value())
                    j["deltaPct"] = *v.deltaPct;
                else
                    j["deltaPct"] = nullptr;
            } else if constexpr (std::is_same_v<T, QueueList>) {
                j["type"] = "QueueList";
                j["order"] = v.order;
            } else if constexpr (std::is_same_v<T, QueueQueues>) {
                j["type"] = "QueueQueues";
            } else if constexpr (std::is_same_v<T, QueueCreate>) {
                j["type"] = "QueueCreate";
                j["name"] = v.name;
            } else if constexpr (std::is_same_v<T, QueueDelete>) {
                j["type"] = "QueueDelete";
                j["qid"] = v.qid;
            } else if constexpr (std::is_same_v<T, PlayFiles>) {
                j["type"] = "PlayFiles";
                j["paths"] = Json::array();
                for (const auto& p : v.paths) {
                    Json e;
                    e = p;
                    j["paths"].push_back(e);
                }
                j["save"] = v.save;
            } else if constexpr (std::is_same_v<T, QueueSwitch>) {
                j["type"] = "QueueSwitch";
                j["qid"] = v.qid;
            } else if constexpr (std::is_same_v<T, QueueAdd>) {
                j["type"] = "QueueAdd";
                j["query"] = v.query;
                j["search"] = v.search;
            } else if constexpr (std::is_same_v<T, QueueRemove>) {
                j["type"] = "QueueRemove";
                j["idOrIndex"] = v.idOrIndex;
            } else if constexpr (std::is_same_v<T, QueueMove>) {
                j["type"] = "QueueMove";
                j["from"] = v.from;
                j["to"] = v.to;
            } else if constexpr (std::is_same_v<T, QueueClear>) {
                j["type"] = "QueueClear";
            } else if constexpr (std::is_same_v<T, QueueShuffle>) {
                j["type"] = "QueueShuffle";
                if (v.on.has_value())
                    j["on"] = *v.on;
                else
                    j["on"] = nullptr;
            } else if constexpr (std::is_same_v<T, QueueRepeat>) {
                j["type"] = "QueueRepeat";
                if (v.mode.has_value())
                    j["mode"] = std::format("{}", *v.mode);
                else
                    j["mode"] = nullptr;
            } else if constexpr (std::is_same_v<T, PlaylistList>) {
                j["type"] = "PlaylistList";
            } else if constexpr (std::is_same_v<T, PlaylistTracks>) {
                j["type"] = "PlaylistTracks";
                j["pid"] = v.pid;
            } else if constexpr (std::is_same_v<T, PlaylistCreate>) {
                j["type"] = "PlaylistCreate";
                j["name"] = v.name;
                j["smart_query"] = v.smart_query;
            } else if constexpr (std::is_same_v<T, PlaylistAdd>) {
                j["type"] = "PlaylistAdd";
                j["pid"] = v.pid;
                j["track_id"] = v.track_id;
            } else if constexpr (std::is_same_v<T, PlaylistLoad>) {
                j["type"] = "PlaylistLoad";
                j["pid"] = v.pid;
                j["play"] = v.play;
                j["replace"] = v.replace;
            } else if constexpr (std::is_same_v<T, PlaylistSave>) {
                j["type"] = "PlaylistSave";
                j["name"] = v.name;
                if (v.queue_id.has_value())
                    j["queue_id"] = *v.queue_id;
                else
                    j["queue_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
                j["type"] = "PlaylistDelete";
                j["pid"] = v.pid;
            } else if constexpr (std::is_same_v<T, PlaylistRename>) {
                j["type"] = "PlaylistRename";
                j["pid"] = v.pid;
                j["newName"] = v.newName;
            } else if constexpr (std::is_same_v<T, PlaylistExport>) {
                j["type"] = "PlaylistExport";
                j["pid"] = v.pid;
                j["path"] = v.path;
                j["format"] = v.format;
            } else if constexpr (std::is_same_v<T, PlaylistImport>) {
                j["type"] = "PlaylistImport";
                j["path"] = v.path;
                if (v.name.has_value())
                    j["name"] = *v.name;
                else
                    j["name"] = nullptr;
            } else if constexpr (std::is_same_v<T, LibraryScan>) {
                j["type"] = "LibraryScan";
                if (v.path.has_value())
                    j["path"] = *v.path;
                else
                    j["path"] = nullptr;
                j["full_hash"] = v.full_hash;
            } else if constexpr (std::is_same_v<T, LibrarySearch>) {
                j["type"] = "LibrarySearch";
                j["query"] = v.query;
                j["limit"] = v.limit;
            } else if constexpr (std::is_same_v<T, LibraryStats>) {
                j["type"] = "LibraryStats";
                if (v.top_tracks)
                    j["top_tracks"] = *v.top_tracks;
                if (v.top_albums)
                    j["top_albums"] = *v.top_albums;
                if (v.top_genres)
                    j["top_genres"] = *v.top_genres;
                if (v.top_artists)
                    j["top_artists"] = *v.top_artists;
            } else if constexpr (std::is_same_v<T, LibraryAdd>) {
                j["type"] = "LibraryAdd";
                j["path"] = v.path;
                j["recursive"] = v.recursive;
            } else if constexpr (std::is_same_v<T, LibraryRemove>) {
                j["type"] = "LibraryRemove";
                j["query"] = v.query;
            } else if constexpr (std::is_same_v<T, LibraryList>) {
                j["type"] = "LibraryList";
                if (v.query.has_value())
                    j["query"] = *v.query;
                else
                    j["query"] = nullptr;
                j["limit"] = v.limit;
                j["offset"] = v.offset;
                if (v.artist.has_value())
                    j["artist"] = *v.artist;
                else
                    j["artist"] = nullptr;
                if (v.album.has_value())
                    j["album"] = *v.album;
                else
                    j["album"] = nullptr;
                if (v.genre.has_value())
                    j["genre"] = *v.genre;
                else
                    j["genre"] = nullptr;
                if (v.rating.has_value())
                    j["rating"] = *v.rating;
                else
                    j["rating"] = nullptr;
            } else if constexpr (std::is_same_v<T, LibraryMaintenance>) {
                j["type"] = "LibraryMaintenance";
                j["action"] = v.action;
                j["dry_run"] = v.dry_run;
                j["library_id"] = v.library_id;
            } else if constexpr (std::is_same_v<T, TagEdit>) {
                j["type"] = "TagEdit";
                j["id"] = v.id;
                j["field"] = v.field;
                j["value"] = v.value;
            } else if constexpr (std::is_same_v<T, TagGet>) {
                j["type"] = "TagGet";
                if (v.id.has_value())
                    j["id"] = *v.id;
                if (v.path.has_value())
                    j["path"] = *v.path;
                j["cover"] = v.cover;
            } else if constexpr (std::is_same_v<T, ConfigGet>) {
                j["type"] = "ConfigGet";
                j["key"] = v.key;
            } else if constexpr (std::is_same_v<T, ConfigSet>) {
                j["type"] = "ConfigSet";
                j["key"] = v.key;
                j["value"] = v.value;
            } else if constexpr (std::is_same_v<T, ConfigList>) {
                j["type"] = "ConfigList";
            } else if constexpr (std::is_same_v<T, ConfigExport>) {
                j["type"] = "ConfigExport";
                j["path"] = v.path;
            } else if constexpr (std::is_same_v<T, ConfigImport>) {
                j["type"] = "ConfigImport";
                j["path"] = v.path;
            } else if constexpr (std::is_same_v<T, ConfigReset>) {
                j["type"] = "ConfigReset";
                if (v.key.has_value())
                    j["key"] = *v.key;
                else
                    j["key"] = nullptr;
            } else if constexpr (std::is_same_v<T, HistoryList>) {
                j["type"] = "HistoryList";
                if (v.limit.has_value())
                    j["limit"] = *v.limit;
                else
                    j["limit"] = nullptr;
            } else if constexpr (std::is_same_v<T, HistoryClear>) {
                j["type"] = "HistoryClear";
            } else if constexpr (std::is_same_v<T, BookmarkList>) {
                j["type"] = "BookmarkList";
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, BookmarkAdd>) {
                j["type"] = "BookmarkAdd";
                j["position_ms"] = v.position_ms;
                j["note"] = v.note;
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, BookmarkRemove>) {
                j["type"] = "BookmarkRemove";
                j["id"] = v.id;
            } else if constexpr (std::is_same_v<T, BookmarkGoto>) {
                j["type"] = "BookmarkGoto";
                j["id"] = v.id;
            } else if constexpr (std::is_same_v<T, Shutdown>) {
                j["type"] = "Shutdown";
            } else if constexpr (std::is_same_v<T, Preview>) {
                j["type"] = "Preview";
                j["file"] = v.file;
            } else if constexpr (std::is_same_v<T, DeviceList>) {
                j["type"] = "DeviceList";
            } else if constexpr (std::is_same_v<T, DeviceSet>) {
                j["type"] = "DeviceSet";
                j["id"] = v.id;
            } else if constexpr (std::is_same_v<T, DeviceTest>) {
                j["type"] = "DeviceTest";
                if (v.id.has_value())
                    j["id"] = *v.id;
                else
                    j["id"] = nullptr;
                j["tone"] = v.tone;
            } else if constexpr (std::is_same_v<T, Info>) {
                j["type"] = "Info";
            } else if constexpr (std::is_same_v<T, TrackRate>) {
                j["type"] = "TrackRate";
                j["track_id"] = v.track_id;
                j["rating"] = v.rating;
            }
            return j;
        },
        cmd);
}

std::expected<Command, kanariv::utils::Error> commandFromJson(const Json& j) {
    try {
        if (!j.contains("type") || !j["type"].isString()) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing type")};
        }
        std::string t = j["type"].get<std::string>();
        if (t == "Play")
            return Command{Play{}};
        if (t == "Pause")
            return Command{Pause{}};
        if (t == "Resume")
            return Command{Resume{}};
        if (t == "Restart")
            return Command{Restart{}};
        if (t == "Stop")
            return Command{Stop{}};
        if (t == "Next")
            return Command{Next{}};
        if (t == "Prev")
            return Command{Prev{}};
        if (t == "Seek") {
            double sec = 0;
            if (j.contains("seconds") && j["seconds"].isNumber())
                sec = j["seconds"].get<double>();
            return Command{Seek{sec}};
        }
        if (t == "StatusReq")
            return Command{StatusReq{}};
        if (t == "StatusWait") {
            StatusWait sw{};
            if (j.contains("timeout_ms") && j["timeout_ms"].isNumber())
                sw.timeout_ms = j["timeout_ms"].get<int>();
            return Command{std::move(sw)};
        }
        if (t == "VolumeSet") {
            VolumeSet v{};
            if (j.contains("level") && !j["level"].isNull() && j["level"].isNumber())
                v.level = j["level"].get<float>();
            if (j.contains("mute") && !j["mute"].isNull() && j["mute"].isBoolean())
                v.mute = j["mute"].get<bool>();
            if (j.contains("deltaPct") && !j["deltaPct"].isNull() && j["deltaPct"].isNumber())
                v.deltaPct = j["deltaPct"].get<int>();
            return Command{std::move(v)};
        }
        if (t == "QueueList") {
            QueueList q{};
            if (j.contains("order") && j["order"].isString())
                q.order = j["order"].get<std::string>();
            return Command{std::move(q)};
        }
        if (t == "QueueQueues")
            return Command{QueueQueues{}};
        if (t == "QueueCreate") {
            QueueCreate q{};
            if (j.contains("name") && j["name"].isString())
                q.name = j["name"].get<std::string>();
            return Command{std::move(q)};
        }
        if (t == "QueueDelete") {
            int64_t qid = 1;
            if (j.contains("qid") && j["qid"].isNumber())
                qid = j["qid"].get<int64_t>();
            return Command{QueueDelete{qid}};
        }
        if (t == "PlayFiles") {
            PlayFiles p{};
            if (j.contains("paths") && j["paths"].isArray()) {
                const Json arr = j["paths"];
                for (std::size_t i = 0, n = arr.size(); i < n; ++i) {
                    auto je = arr.at(i);
                    if (!je)
                        return std::unexpected{je.error()};
                    if (je->isString())
                        p.paths.push_back(je->get<std::string>());
                }
            }
            if (j.contains("save") && j["save"].isBoolean())
                p.save = j["save"].get<bool>();
            return Command{std::move(p)};
        }
        if (t == "QueueSwitch") {
            int64_t qid = 1;
            if (j.contains("qid") && j["qid"].isNumber())
                qid = j["qid"].get<int64_t>();
            return Command{QueueSwitch{qid}};
        }
        if (t == "QueueAdd") {
            std::string q;
            bool search = false;
            if (j.contains("query") && j["query"].isString())
                q = j["query"].get<std::string>();
            if (j.contains("search") && j["search"].isBoolean())
                search = j["search"].get<bool>();
            return Command{QueueAdd{std::move(q), search}};
        }
        if (t == "QueueRemove") {
            std::string id;
            if (j.contains("idOrIndex") && j["idOrIndex"].isString())
                id = j["idOrIndex"].get<std::string>();
            return Command{QueueRemove{std::move(id)}};
        }
        if (t == "QueueMove") {
            std::size_t from = 0, to = 0;
            if (j.contains("from") && j["from"].isNumber())
                from = j["from"].get<std::size_t>();
            if (j.contains("to") && j["to"].isNumber())
                to = j["to"].get<std::size_t>();
            return Command{QueueMove{from, to}};
        }
        if (t == "QueueClear")
            return Command{QueueClear{}};
        if (t == "QueueShuffle") {
            QueueShuffle v{};
            if (j.contains("on") && !j["on"].isNull() && j["on"].isBoolean())
                v.on = j["on"].get<bool>();
            return Command{std::move(v)};
        }
        if (t == "QueueRepeat") {
            QueueRepeat v{};
            if (j.contains("mode") && !j["mode"].isNull() && j["mode"].isString()) {
                auto m = kanariv::types::parseRepeatMode(j["mode"].get<std::string>());
                if (!m)
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg, "unknown RepeatMode")};
                v.mode = *m;
            }
            return Command{std::move(v)};
        }
        if (t == "PlaylistList")
            return Command{PlaylistList{}};
        if (t == "PlaylistTracks") {
            int64_t pid = 0;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            return Command{PlaylistTracks{pid}};
        }
        if (t == "PlaylistCreate") {
            PlaylistCreate pc;
            if (j.contains("name") && j["name"].isString())
                pc.name = j["name"].get<std::string>();
            if (j.contains("smart_query") && j["smart_query"].isString())
                pc.smart_query = j["smart_query"].get<std::string>();
            return Command{std::move(pc)};
        }
        if (t == "PlaylistAdd") {
            int64_t pid = 0;
            int64_t tid = 0;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("track_id") && j["track_id"].isNumber())
                tid = j["track_id"].get<int64_t>();
            return Command{PlaylistAdd{pid, tid}};
        }
        if (t == "PlaylistLoad") {
            int64_t pid = 0;
            bool play = false;
            bool replace = false;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("play") && j["play"].isBoolean())
                play = j["play"].get<bool>();
            if (j.contains("replace") && j["replace"].isBoolean())
                replace = j["replace"].get<bool>();
            return Command{PlaylistLoad{pid, play, replace}};
        }
        if (t == "PlaylistSave") {
            std::string name;
            std::optional<int64_t> qid;
            if (j.contains("name") && j["name"].isString())
                name = j["name"].get<std::string>();
            if (j.contains("queue_id") && !j["queue_id"].isNull() && j["queue_id"].isNumber())
                qid = j["queue_id"].get<int64_t>();
            return Command{PlaylistSave{std::move(name), qid}};
        }
        if (t == "PlaylistDelete") {
            int64_t pid = 0;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            return Command{PlaylistDelete{pid}};
        }
        if (t == "PlaylistRename") {
            int64_t pid = 0;
            std::string newName;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("newName") && j["newName"].isString())
                newName = j["newName"].get<std::string>();
            return Command{PlaylistRename{pid, std::move(newName)}};
        }
        if (t == "PlaylistExport") {
            int64_t pid = 0;
            std::string path;
            std::string format = "m3u";
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("path") && j["path"].isString())
                path = j["path"].get<std::string>();
            if (j.contains("format") && j["format"].isString())
                format = j["format"].get<std::string>();
            return Command{PlaylistExport{pid, std::move(path), std::move(format)}};
        }
        if (t == "PlaylistImport") {
            std::string path;
            std::optional<std::string> name;
            if (j.contains("path") && j["path"].isString())
                path = j["path"].get<std::string>();
            if (j.contains("name") && !j["name"].isNull() && j["name"].isString())
                name = j["name"].get<std::string>();
            return Command{PlaylistImport{std::move(path), name}};
        }
        if (t == "LibraryScan") {
            LibraryScan v{};
            if (j.contains("path") && !j["path"].isNull() && j["path"].isString())
                v.path = j["path"].get<std::string>();
            if (j.contains("full_hash") && j["full_hash"].isBoolean())
                v.full_hash = j["full_hash"].get<bool>();
            else if (j.contains("mode") && j["mode"].isString())
                v.full_hash = (j["mode"].get<std::string>() == "full");
            return Command{std::move(v)};
        }
        if (t == "LibrarySearch") {
            std::string q;
            int lim = 50;
            if (j.contains("query") && j["query"].isString())
                q = j["query"].get<std::string>();
            if (j.contains("limit") && j["limit"].isNumber())
                lim = j["limit"].get<int>();
            return Command{LibrarySearch{std::move(q), lim}};
        }
        if (t == "LibraryStats" || t == "LibraryStatsDetailed") {
            LibraryStats v{};
            if (j.contains("top_tracks") && j["top_tracks"].isNumber())
                v.top_tracks = j["top_tracks"].get<int>();
            else if (t == "LibraryStatsDetailed")
                v.top_tracks = 10;
            if (j.contains("top_albums") && j["top_albums"].isNumber())
                v.top_albums = j["top_albums"].get<int>();
            if (j.contains("top_genres") && j["top_genres"].isNumber())
                v.top_genres = j["top_genres"].get<int>();
            if (j.contains("top_artists") && j["top_artists"].isNumber())
                v.top_artists = j["top_artists"].get<int>();
            return Command{std::move(v)};
        }
        if (t == "LibraryAdd") {
            std::string p;
            bool rec = false;
            if (j.contains("path") && j["path"].isString())
                p = j["path"].get<std::string>();
            if (j.contains("recursive") && j["recursive"].isBoolean())
                rec = j["recursive"].get<bool>();
            return Command{LibraryAdd{std::move(p), rec}};
        }
        if (t == "LibraryRemove") {
            std::string q;
            if (j.contains("query") && j["query"].isString())
                q = j["query"].get<std::string>();
            else if (j.contains("id") && j["id"].isString())
                q = j["id"].get<std::string>();
            else if (j.contains("id") && j["id"].isNumber())
                q = std::to_string(j["id"].get<int64_t>());
            return Command{LibraryRemove{std::move(q)}};
        }
        if (t == "LibraryList") {
            LibraryList v{};
            if (j.contains("query") && !j["query"].isNull() && j["query"].isString())
                v.query = j["query"].get<std::string>();
            if (j.contains("limit") && j["limit"].isNumber())
                v.limit = j["limit"].get<int>();
            if (j.contains("offset") && j["offset"].isNumber())
                v.offset = j["offset"].get<int>();
            if (j.contains("artist") && !j["artist"].isNull() && j["artist"].isString())
                v.artist = j["artist"].get<std::string>();
            if (j.contains("album") && !j["album"].isNull() && j["album"].isString())
                v.album = j["album"].get<std::string>();
            if (j.contains("genre") && !j["genre"].isNull() && j["genre"].isString())
                v.genre = j["genre"].get<std::string>();
            if (j.contains("rating") && !j["rating"].isNull() && j["rating"].isNumber())
                v.rating = j["rating"].get<int32_t>();
            return Command{std::move(v)};
        }
        if (t == "LibraryMaintenance") {
            LibraryMaintenance v{};
            if (j.contains("action") && j["action"].isString())
                v.action = j["action"].get<std::string>();
            if (j.contains("dry_run") && j["dry_run"].isBoolean())
                v.dry_run = j["dry_run"].get<bool>();
            if (j.contains("library_id") && j["library_id"].isNumber())
                v.library_id = j["library_id"].get<int64_t>();
            return Command{std::move(v)};
        }
        if (t == "TagEdit") {
            int64_t id = 0;
            std::string field, value;
            if (j.contains("id") && j["id"].isNumber())
                id = j["id"].get<int64_t>();
            if (j.contains("field") && j["field"].isString())
                field = j["field"].get<std::string>();
            if (j.contains("value") && j["value"].isString())
                value = j["value"].get<std::string>();
            return Command{TagEdit{id, std::move(field), std::move(value)}};
        }
        if (t == "TagGet") {
            TagGet tg{};
            if (j.contains("id") && j["id"].isNumber())
                tg.id = j["id"].get<int64_t>();
            if (j.contains("path") && j["path"].isString())
                tg.path = j["path"].get<std::string>();
            if (j.contains("cover") && j["cover"].isBoolean())
                tg.cover = j["cover"].get<bool>();
            return Command{std::move(tg)};
        }
        if (t == "ConfigGet") {
            std::string k;
            if (j.contains("key") && j["key"].isString())
                k = j["key"].get<std::string>();
            return Command{ConfigGet{std::move(k)}};
        }
        if (t == "ConfigSet") {
            std::string k, v;
            if (j.contains("key") && j["key"].isString())
                k = j["key"].get<std::string>();
            if (j.contains("value") && j["value"].isString())
                v = j["value"].get<std::string>();
            return Command{ConfigSet{std::move(k), std::move(v)}};
        }
        if (t == "ConfigList")
            return Command{ConfigList{}};
        if (t == "ConfigExport") {
            std::string p;
            if (j.contains("path") && j["path"].isString())
                p = j["path"].get<std::string>();
            return Command{ConfigExport{std::move(p)}};
        }
        if (t == "ConfigImport") {
            std::string p;
            if (j.contains("path") && j["path"].isString())
                p = j["path"].get<std::string>();
            return Command{ConfigImport{std::move(p)}};
        }
        if (t == "ConfigReset") {
            std::optional<std::string> k;
            if (j.contains("key") && !j["key"].isNull() && j["key"].isString())
                k = j["key"].get<std::string>();
            return Command{ConfigReset{std::move(k)}};
        }
        if (t == "HistoryList") {
            HistoryList v{};
            if (j.contains("limit") && !j["limit"].isNull() && j["limit"].isNumber())
                v.limit = j["limit"].get<int>();
            return Command{std::move(v)};
        }
        if (t == "HistoryClear") {
            return Command{HistoryClear{}};
        }
        if (t == "BookmarkList") {
            BookmarkList v{};
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return Command{std::move(v)};
        }
        if (t == "BookmarkAdd") {
            BookmarkAdd v{};
            if (j.contains("position_ms") && j["position_ms"].isNumber())
                v.position_ms = j["position_ms"].get<int64_t>();
            if (j.contains("note") && j["note"].isString())
                v.note = j["note"].get<std::string>();
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return Command{std::move(v)};
        }
        if (t == "BookmarkRemove") {
            BookmarkRemove v{};
            if (j.contains("id") && j["id"].isNumber())
                v.id = j["id"].get<int64_t>();
            return Command{std::move(v)};
        }
        if (t == "BookmarkGoto") {
            BookmarkGoto v{};
            if (j.contains("id") && j["id"].isNumber())
                v.id = j["id"].get<int64_t>();
            return Command{std::move(v)};
        }
        if (t == "Shutdown")
            return Command{Shutdown{}};
        if (t == "Preview") {
            std::string f;
            if (j.contains("file") && j["file"].isString())
                f = j["file"].get<std::string>();
            return Command{Preview{std::move(f)}};
        }
        if (t == "DeviceList")
            return Command{DeviceList{}};
        if (t == "DeviceSet") {
            std::string id;
            if (j.contains("id") && j["id"].isString())
                id = j["id"].get<std::string>();
            return Command{DeviceSet{std::move(id)}};
        }
        if (t == "DeviceTest") {
            std::optional<std::string> id;
            if (j.contains("id") && !j["id"].isNull() && j["id"].isString())
                id = j["id"].get<std::string>();
            DeviceTest dt{std::move(id)};
            if (j.contains("tone") && j["tone"].isBoolean())
                dt.tone = j["tone"].get<bool>();
            return Command{std::move(dt)};
        }
        if (t == "Info")
            return Command{Info{}};
        if (t == "TrackRate") {
            TrackRate tr{};
            if (j.contains("track_id") && j["track_id"].isNumber())
                tr.track_id = j["track_id"].get<int64_t>();
            if (j.contains("rating") && j["rating"].isNumber())
                tr.rating = j["rating"].get<int32_t>();
            return Command{std::move(tr)};
        }
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "unknown Command type: " + t)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

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
                return j;
            } else if constexpr (std::is_same_v<T, QueueTracks>) {
                Json j;
                j["type"] = "QueueTracks";
                j["tracks"] = Json::array();
                for (const auto& t : v.tracks)
                    j["tracks"].push_back(kanariv::utils::trackToJson(t));
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
            } else if constexpr (std::is_same_v<T, kanariv::utils::Error>) {
                return detail::errorToJson(v);
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
        if (t == "Error") {
            auto e = detail::errorFromJson(j);
            if (!e)
                return std::unexpected{e.error()};
            return Result{*e};
        }
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                        "unknown Result type: " + t)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::string serializeRequest(const IpcRequest& req) {
    Json j;
    j["id"] = req.id;
    j["cmd"] = toJson(req.cmd);
    return j.dump();
}

std::expected<IpcRequest, kanariv::utils::Error> deserializeRequest(std::string_view sv) {
    try {
        const Json j = Json::parse(sv);
        if (!j.contains("id") || !j.contains("cmd")) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing id/cmd")};
        }
        uint32_t id = j["id"].get<uint32_t>();
        auto cmd = commandFromJson(j["cmd"]);
        if (!cmd)
            return std::unexpected{cmd.error()};
        return IpcRequest{id, std::move(*cmd)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::string serializeReply(const IpcReply& rep) {
    Json j;
    j["id"] = rep.id;
    if (rep.result.has_value()) {
        j["ok"] = true;
        j["result"] = toJson(*rep.result);
    } else {
        j["ok"] = false;
        j["error"] = detail::errorToJson(rep.result.error());
    }
    return j.dump();
}

std::expected<IpcReply, kanariv::utils::Error> deserializeReply(std::string_view sv) {
    try {
        const Json j = Json::parse(sv);
        if (!j.contains("id")) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing id")};
        }
        uint32_t id = j["id"].get<uint32_t>();
        bool ok = true;
        if (j.contains("ok") && j["ok"].isBoolean())
            ok = j["ok"].get<bool>();
        else if (j.contains("error"))
            ok = false;

        if (ok) {
            if (!j.contains("result")) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "missing result")};
            }
            auto r = resultFromJson(j["result"]);
            if (!r)
                return std::unexpected{r.error()};
            return IpcReply{id, std::move(*r)};
        } else {
            Json ej;
            if (j.contains("error"))
                ej = j["error"];
            else if (j.contains("result"))
                ej = j["result"];
            else
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "missing error")};
            auto e = detail::errorFromJson(ej);
            if (!e)
                return std::unexpected{e.error()};
            return IpcReply{id, std::unexpected{*e}};
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::vector<std::byte> frame(std::string_view json) {
    std::vector<std::byte> out;
    out.reserve(4 + json.size());
    uint32_t len = static_cast<uint32_t>(json.size());
    out.push_back(static_cast<std::byte>((len >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(len & 0xFF));
    // Bulk insert (M-2): the old per-byte push_back loop defeated the
    // reserve() above with per-byte size checks on some STLs.
    const auto* bytes = reinterpret_cast<const std::byte*>(json.data());
    out.insert(out.end(), bytes, bytes + json.size());
    return out;
}

std::expected<std::string, kanariv::utils::Error> deframe(std::span<const std::byte> buf) {
    if (buf.size() < 4) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too short")};
    }
    uint32_t len = (static_cast<uint32_t>(std::to_integer<unsigned char>(buf[0])) << 24) |
                   (static_cast<uint32_t>(std::to_integer<unsigned char>(buf[1])) << 16) |
                   (static_cast<uint32_t>(std::to_integer<unsigned char>(buf[2])) << 8) |
                   static_cast<uint32_t>(std::to_integer<unsigned char>(buf[3]));
    if (buf.size() < 4 + len) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame incomplete")};
    }
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(std::to_integer<unsigned char>(buf[4 + i])));
    }
    return s;
}

std::string toJsonString(const Result& r) {
    return toJson(r).dump(2);
}

std::string toJsonString(const Command& c) {
    return toJson(c).dump(2);
}

} // namespace kanariv::ipc
