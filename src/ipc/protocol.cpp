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
#include <kanariv/types/parse.hpp>
#include <kanariv/types/record.hpp>
#include <kanariv/types/wire.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/result.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kanariv::ipc::detail {

auto resultCodeToString(kanariv::utils::StatusCode r) -> std::string {
    return std::string(kanariv::utils::toString(r));
}

auto resultCodeFromString(std::string_view sv)
    -> std::expected<kanariv::utils::StatusCode, kanariv::utils::Error> {
    using R = kanariv::utils::StatusCode;
    if (sv == "Ok") {
        return R::Ok;
    }
    if (sv == "InvalidArg") {
        return R::InvalidArg;
    }
    if (sv == "NotFound") {
        return R::NotFound;
    }
    if (sv == "Unsupported") {
        return R::Unsupported;
    }
    if (sv == "Io") {
        return R::Io;
    }
    if (sv == "Device") {
        return R::Device;
    }
    if (sv == "State") {
        return R::State;
    }
    if (sv == "NoMem") {
        return R::NoMem;
    }
    if (sv == "Internal") {
        return R::Internal;
    }
    if (sv == "AlreadyExists") {
        return R::AlreadyExists;
    }
    if (sv == "Busy") {
        return R::Busy;
    }
    if (sv == "Corrupt") {
        return R::Corrupt;
    }
    if (sv == "NoSpace") {
        return R::NoSpace;
    }
    return std::unexpected{kanariv::utils::makeError(R::InvalidArg, "unknown Result code")};
}

// Track conversion lives in kanariv::utils (utils/record_json.hpp) -- single
// definition shared by db export/import and the IPC codec.
// (The former ipc::detail subset is deleted; wire readers tolerate the
// fuller db field set: all reads are contains-guarded.)

auto playlistToJson(const kanariv::types::Playlist& p) -> Json {
    Json j;
    j["id"] = p.id;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["name"] = p.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["type"] = p.type; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["smart_query"] =
        p.smart_query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["created"] =
        p.created; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["modified"] =
        p.modified; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["library_id"] =
        p.library_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

auto playlistFromJson(const Json& j)
    -> std::expected<kanariv::types::Playlist, kanariv::utils::Error> {
    try {
        kanariv::types::Playlist p{};
        if (j.contains("id") &&
            j["id"]
                .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.id =
                j["id"]
                    .get<
                        int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("name") &&
            j["name"]
                .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.name =
                j["name"]
                    .get<
                        std::
                            string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("type") &&
            j["type"]
                .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.type =
                j["type"]
                    .get<
                        int32_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("smart_query") &&
            j["smart_query"]
                .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.smart_query =
                j["smart_query"]
                    .get<
                        std::
                            string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("created") &&
            j["created"]
                .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.created =
                j["created"]
                    .get<
                        int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("modified") &&
            j["modified"]
                .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.modified =
                j["modified"]
                    .get<
                        int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (j.contains("library_id") &&
            j["library_id"]
                .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            p.library_id =
                j["library_id"]
                    .get<
                        int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        return p;
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

auto errorToJson(const kanariv::utils::Error& e) -> Json {
    Json j;
    j["type"] = "Error"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["code"] = resultCodeToString(
        e.code); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["code_value"] = std::to_underlying(
        e.code); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["message"] =
        e.message; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

auto errorFromJson(const Json& j) -> std::expected<kanariv::utils::Error, kanariv::utils::Error> {
    try {
        std::string codeStr;
        std::string msg;
        if (j.contains("code") &&
            j["code"]
                .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            codeStr =
                j["code"]
                    .get<
                        std::
                            string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        } else if (
            j.contains("code_value") &&
            j["code_value"]
                .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            int v =
                j["code_value"]
                    .get<
                        int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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
        if (j.contains("message") &&
            j["message"]
                .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            msg =
                j["message"]
                    .get<
                        std::
                            string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
        if (codeStr.empty()) {
            codeStr = "Internal";
        }
        auto rc = resultCodeFromString(codeStr);
        if (!rc) {
            return std::unexpected{rc.error()};
        }
        return kanariv::utils::Error{*rc, msg};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

} // namespace kanariv::ipc::detail

namespace kanariv::ipc {

constexpr int defaultLibrarySearchLimit = 50;
constexpr int defaultLibraryStatsTopTracks = 10;
constexpr int frameByteIndex3 = 3;

auto toJson(const Command& cmd) -> Json {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            Json j;
            if constexpr (std::is_same_v<T, Play>) {
                j["type"] =
                    "Play"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Pause>) {
                j["type"] =
                    "Pause"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Resume>) {
                j["type"] =
                    "Resume"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Restart>) {
                j["type"] =
                    "Restart"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Stop>) {
                j["type"] =
                    "Stop"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Next>) {
                j["type"] =
                    "Next"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Prev>) {
                j["type"] =
                    "Prev"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Seek>) {
                j["type"] =
                    "Seek"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["seconds"] =
                    v.seconds; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, StatusReq>) {
                j["type"] =
                    "StatusReq"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, StatusWait>) {
                j["type"] =
                    "StatusWait"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["timeout_ms"] =
                    v.timeout_ms; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, VolumeSet>) {
                j["type"] =
                    "VolumeSet"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.level.has_value()) {
                    j["level"] =
                        *v.level; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["level"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.mute.has_value()) {
                    j["mute"] =
                        *v.mute; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["mute"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.deltaPct.has_value()) {
                    j["deltaPct"] =
                        *v.deltaPct; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["deltaPct"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, QueueList>) {
                j["type"] =
                    "QueueList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["order"] =
                    v.order; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueQueues>) {
                j["type"] =
                    "QueueQueues"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueCreate>) {
                j["type"] =
                    "QueueCreate"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["name"] =
                    v.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueDelete>) {
                j["type"] =
                    "QueueDelete"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["qid"] =
                    v.qid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlayFiles>) {
                j["type"] =
                    "PlayFiles"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["paths"] = Json::
                    array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                for (const auto& p : v.paths) {
                    Json e;
                    e = p;
                    j["paths"].push_back(
                        e); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                j["save"] =
                    v.save; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueSwitch>) {
                j["type"] =
                    "QueueSwitch"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["qid"] =
                    v.qid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueAdd>) {
                j["type"] =
                    "QueueAdd"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["query"] =
                    v.query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["search"] =
                    v.search; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueRemove>) {
                j["type"] =
                    "QueueRemove"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["idOrIndex"] =
                    v.idOrIndex; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueMove>) {
                j["type"] =
                    "QueueMove"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["from"] =
                    v.from; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["to"] =
                    v.to; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueClear>) {
                j["type"] =
                    "QueueClear"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, QueueShuffle>) {
                j["type"] =
                    "QueueShuffle"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.on.has_value()) {
                    j["on"] =
                        *v.on; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["on"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, QueueRepeat>) {
                j["type"] =
                    "QueueRepeat"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.mode.has_value()) {
                    j["mode"] = std::format(
                        "{}",
                        *v.mode); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["mode"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, PlaylistList>) {
                j["type"] =
                    "PlaylistList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistTracks>) {
                j["type"] =
                    "PlaylistTracks"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["pid"] =
                    v.pid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistCreate>) {
                j["type"] =
                    "PlaylistCreate"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["name"] =
                    v.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["smart_query"] =
                    v.smart_query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistAdd>) {
                j["type"] =
                    "PlaylistAdd"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["pid"] =
                    v.pid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["track_id"] =
                    v.track_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistLoad>) {
                j["type"] =
                    "PlaylistLoad"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["pid"] =
                    v.pid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["play"] =
                    v.play; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["replace"] =
                    v.replace; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistSave>) {
                j["type"] =
                    "PlaylistSave"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["name"] =
                    v.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.queue_id.has_value()) {
                    j["queue_id"] =
                        *v.queue_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["queue_id"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
                j["type"] =
                    "PlaylistDelete"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["pid"] =
                    v.pid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistRename>) {
                j["type"] =
                    "PlaylistRename"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["pid"] =
                    v.pid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["newName"] =
                    v.newName; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistExport>) {
                j["type"] =
                    "PlaylistExport"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["pid"] =
                    v.pid; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["path"] =
                    v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["format"] =
                    v.format; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, PlaylistImport>) {
                j["type"] =
                    "PlaylistImport"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["path"] =
                    v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.name.has_value()) {
                    j["name"] =
                        *v.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["name"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, LibraryScan>) {
                j["type"] =
                    "LibraryScan"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.path.has_value()) {
                    j["path"] =
                        *v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["path"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                j["full_hash"] =
                    v.full_hash; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, LibrarySearch>) {
                j["type"] =
                    "LibrarySearch"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["query"] =
                    v.query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["limit"] =
                    v.limit; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, LibraryStats>) {
                j["type"] =
                    "LibraryStats"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.top_tracks) {
                    j["top_tracks"] =
                        *v.top_tracks; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.top_albums) {
                    j["top_albums"] =
                        *v.top_albums; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.top_genres) {
                    j["top_genres"] =
                        *v.top_genres; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.top_artists) {
                    j["top_artists"] =
                        *v.top_artists; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, LibraryAdd>) {
                j["type"] =
                    "LibraryAdd"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["path"] =
                    v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["recursive"] =
                    v.recursive; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, LibraryRemove>) {
                j["type"] =
                    "LibraryRemove"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["query"] =
                    v.query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, LibraryList>) {
                j["type"] =
                    "LibraryList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.query.has_value()) {
                    j["query"] =
                        *v.query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["query"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                j["limit"] =
                    v.limit; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["offset"] =
                    v.offset; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.artist.has_value()) {
                    j["artist"] =
                        *v.artist; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["artist"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.album.has_value()) {
                    j["album"] =
                        *v.album; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["album"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.genre.has_value()) {
                    j["genre"] =
                        *v.genre; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["genre"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.rating.has_value()) {
                    j["rating"] =
                        *v.rating; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["rating"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, LibraryMaintenance>) {
                j["type"] =
                    "LibraryMaintenance"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["action"] =
                    v.action; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["dry_run"] =
                    v.dry_run; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["library_id"] =
                    v.library_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, TagEdit>) {
                j["type"] =
                    "TagEdit"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["id"] =
                    v.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["field"] =
                    v.field; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["value"] =
                    v.value; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, TagGet>) {
                j["type"] =
                    "TagGet"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.id.has_value()) {
                    j["id"] =
                        *v.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                if (v.path.has_value()) {
                    j["path"] =
                        *v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                j["cover"] =
                    v.cover; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, ConfigGet>) {
                j["type"] =
                    "ConfigGet"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["key"] =
                    v.key; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, ConfigSet>) {
                j["type"] =
                    "ConfigSet"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["key"] =
                    v.key; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["value"] =
                    v.value; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, ConfigList>) {
                j["type"] =
                    "ConfigList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, ConfigExport>) {
                j["type"] =
                    "ConfigExport"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["path"] =
                    v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, ConfigImport>) {
                j["type"] =
                    "ConfigImport"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["path"] =
                    v.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, ConfigReset>) {
                j["type"] =
                    "ConfigReset"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.key.has_value()) {
                    j["key"] =
                        *v.key; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["key"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, HistoryList>) {
                j["type"] =
                    "HistoryList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.limit.has_value()) {
                    j["limit"] =
                        *v.limit; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["limit"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, HistoryClear>) {
                j["type"] =
                    "HistoryClear"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, BookmarkList>) {
                j["type"] =
                    "BookmarkList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.track_id.has_value()) {
                    j["track_id"] =
                        *v.track_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["track_id"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, BookmarkAdd>) {
                j["type"] =
                    "BookmarkAdd"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["position_ms"] =
                    v.position_ms; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["note"] =
                    v.note; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.track_id.has_value()) {
                    j["track_id"] =
                        *v.track_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["track_id"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
            } else if constexpr (std::is_same_v<T, BookmarkRemove>) {
                j["type"] =
                    "BookmarkRemove"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["id"] =
                    v.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, BookmarkGoto>) {
                j["type"] =
                    "BookmarkGoto"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["id"] =
                    v.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Shutdown>) {
                j["type"] =
                    "Shutdown"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Preview>) {
                j["type"] =
                    "Preview"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["file"] =
                    v.file; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, DeviceList>) {
                j["type"] =
                    "DeviceList"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, DeviceSet>) {
                j["type"] =
                    "DeviceSet"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["id"] =
                    v.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, DeviceTest>) {
                j["type"] =
                    "DeviceTest"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (v.id.has_value()) {
                    j["id"] =
                        *v.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    j["id"] =
                        nullptr; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                }
                j["tone"] =
                    v.tone; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, Info>) {
                j["type"] =
                    "Info"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if constexpr (std::is_same_v<T, TrackRate>) {
                j["type"] =
                    "TrackRate"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["track_id"] =
                    v.track_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                j["rating"] =
                    v.rating; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            return j;
        },
        cmd);
}

// Forward declarations for commandFromJson helpers.
static auto parseCommandPlay(const Json& j) -> std::expected<Play, kanariv::utils::Error>;
static auto parseCommandPause(const Json& j) -> std::expected<Pause, kanariv::utils::Error>;
static auto parseCommandResume(const Json& j) -> std::expected<Resume, kanariv::utils::Error>;
static auto parseCommandRestart(const Json& j) -> std::expected<Restart, kanariv::utils::Error>;
static auto parseCommandStop(const Json& j) -> std::expected<Stop, kanariv::utils::Error>;
static auto parseCommandNext(const Json& j) -> std::expected<Next, kanariv::utils::Error>;
static auto parseCommandPrev(const Json& j) -> std::expected<Prev, kanariv::utils::Error>;
static auto parseCommandSeek(const Json& j) -> std::expected<Seek, kanariv::utils::Error>;
static auto parseCommandStatusReq(const Json& j) -> std::expected<StatusReq, kanariv::utils::Error>;
static auto parseCommandStatusWait(const Json& j)
    -> std::expected<StatusWait, kanariv::utils::Error>;
static auto parseCommandVolumeSet(const Json& j) -> std::expected<VolumeSet, kanariv::utils::Error>;
static auto parseCommandQueueList(const Json& j) -> std::expected<QueueList, kanariv::utils::Error>;
static auto parseCommandQueueQueues(const Json& j)
    -> std::expected<QueueQueues, kanariv::utils::Error>;
static auto parseCommandQueueCreate(const Json& j)
    -> std::expected<QueueCreate, kanariv::utils::Error>;
static auto parseCommandQueueDelete(const Json& j)
    -> std::expected<QueueDelete, kanariv::utils::Error>;
static auto parseCommandPlayFiles(const Json& j) -> std::expected<PlayFiles, kanariv::utils::Error>;
static auto parseCommandQueueSwitch(const Json& j)
    -> std::expected<QueueSwitch, kanariv::utils::Error>;
static auto parseCommandQueueAdd(const Json& j) -> std::expected<QueueAdd, kanariv::utils::Error>;
static auto parseCommandQueueRemove(const Json& j)
    -> std::expected<QueueRemove, kanariv::utils::Error>;
static auto parseCommandQueueMove(const Json& j) -> std::expected<QueueMove, kanariv::utils::Error>;
static auto parseCommandQueueClear(const Json& j)
    -> std::expected<QueueClear, kanariv::utils::Error>;
static auto parseCommandQueueShuffle(const Json& j)
    -> std::expected<QueueShuffle, kanariv::utils::Error>;
static auto parseCommandQueueRepeat(const Json& j)
    -> std::expected<QueueRepeat, kanariv::utils::Error>;
static auto parseCommandPlaylistList(const Json& j)
    -> std::expected<PlaylistList, kanariv::utils::Error>;
static auto parseCommandPlaylistTracks(const Json& j)
    -> std::expected<PlaylistTracks, kanariv::utils::Error>;
static auto parseCommandPlaylistCreate(const Json& j)
    -> std::expected<PlaylistCreate, kanariv::utils::Error>;
static auto parseCommandPlaylistAdd(const Json& j)
    -> std::expected<PlaylistAdd, kanariv::utils::Error>;
static auto parseCommandPlaylistLoad(const Json& j)
    -> std::expected<PlaylistLoad, kanariv::utils::Error>;
static auto parseCommandPlaylistSave(const Json& j)
    -> std::expected<PlaylistSave, kanariv::utils::Error>;
static auto parseCommandPlaylistDelete(const Json& j)
    -> std::expected<PlaylistDelete, kanariv::utils::Error>;
static auto parseCommandPlaylistRename(const Json& j)
    -> std::expected<PlaylistRename, kanariv::utils::Error>;
static auto parseCommandPlaylistExport(const Json& j)
    -> std::expected<PlaylistExport, kanariv::utils::Error>;
static auto parseCommandPlaylistImport(const Json& j)
    -> std::expected<PlaylistImport, kanariv::utils::Error>;
static auto parseCommandLibraryScan(const Json& j)
    -> std::expected<LibraryScan, kanariv::utils::Error>;
static auto parseCommandLibrarySearch(const Json& j)
    -> std::expected<LibrarySearch, kanariv::utils::Error>;
static auto parseCommandLibraryStats(const Json& j)
    -> std::expected<LibraryStats, kanariv::utils::Error>;
static auto parseCommandLibraryAdd(const Json& j)
    -> std::expected<LibraryAdd, kanariv::utils::Error>;
static auto parseCommandLibraryRemove(const Json& j)
    -> std::expected<LibraryRemove, kanariv::utils::Error>;
static auto parseCommandLibraryList(const Json& j)
    -> std::expected<LibraryList, kanariv::utils::Error>;
static auto parseCommandLibraryMaintenance(const Json& j)
    -> std::expected<LibraryMaintenance, kanariv::utils::Error>;
static auto parseCommandTagEdit(const Json& j) -> std::expected<TagEdit, kanariv::utils::Error>;
static auto parseCommandTagGet(const Json& j) -> std::expected<TagGet, kanariv::utils::Error>;
static auto parseCommandConfigGet(const Json& j) -> std::expected<ConfigGet, kanariv::utils::Error>;
static auto parseCommandConfigSet(const Json& j) -> std::expected<ConfigSet, kanariv::utils::Error>;
static auto parseCommandConfigList(const Json& j)
    -> std::expected<ConfigList, kanariv::utils::Error>;
static auto parseCommandConfigExport(const Json& j)
    -> std::expected<ConfigExport, kanariv::utils::Error>;
static auto parseCommandConfigImport(const Json& j)
    -> std::expected<ConfigImport, kanariv::utils::Error>;
static auto parseCommandConfigReset(const Json& j)
    -> std::expected<ConfigReset, kanariv::utils::Error>;
static auto parseCommandHistoryList(const Json& j)
    -> std::expected<HistoryList, kanariv::utils::Error>;
static auto parseCommandHistoryClear(const Json& j)
    -> std::expected<HistoryClear, kanariv::utils::Error>;
static auto parseCommandBookmarkList(const Json& j)
    -> std::expected<BookmarkList, kanariv::utils::Error>;
static auto parseCommandBookmarkAdd(const Json& j)
    -> std::expected<BookmarkAdd, kanariv::utils::Error>;
static auto parseCommandBookmarkRemove(const Json& j)
    -> std::expected<BookmarkRemove, kanariv::utils::Error>;
static auto parseCommandBookmarkGoto(const Json& j)
    -> std::expected<BookmarkGoto, kanariv::utils::Error>;
static auto parseCommandShutdown(const Json& j) -> std::expected<Shutdown, kanariv::utils::Error>;
static auto parseCommandPreview(const Json& j) -> std::expected<Preview, kanariv::utils::Error>;
static auto parseCommandDeviceList(const Json& j)
    -> std::expected<DeviceList, kanariv::utils::Error>;
static auto parseCommandDeviceSet(const Json& j) -> std::expected<DeviceSet, kanariv::utils::Error>;
static auto parseCommandDeviceTest(const Json& j)
    -> std::expected<DeviceTest, kanariv::utils::Error>;
static auto parseCommandInfo(const Json& j) -> std::expected<Info, kanariv::utils::Error>;
static auto parseCommandTrackRate(const Json& j) -> std::expected<TrackRate, kanariv::utils::Error>;

auto commandFromJson(const Json& j) -> std::expected<Command, kanariv::utils::Error> {
    try {
        if (!j.contains("type") ||
            !j["type"]
                 .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing type")};
        }
        auto t =
            j["type"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (t == "Play") {
            return parseCommandPlay(j);
        }
        if (t == "Pause") {
            return parseCommandPause(j);
        }
        if (t == "Resume") {
            return parseCommandResume(j);
        }
        if (t == "Restart") {
            return parseCommandRestart(j);
        }
        if (t == "Stop") {
            return parseCommandStop(j);
        }
        if (t == "Next") {
            return parseCommandNext(j);
        }
        if (t == "Prev") {
            return parseCommandPrev(j);
        }
        if (t == "Seek") {
            return parseCommandSeek(j);
        }
        if (t == "StatusReq") {
            return parseCommandStatusReq(j);
        }
        if (t == "StatusWait") {
            return parseCommandStatusWait(j);
        }
        if (t == "VolumeSet") {
            return parseCommandVolumeSet(j);
        }
        if (t == "QueueList") {
            return parseCommandQueueList(j);
        }
        if (t == "QueueQueues") {
            return parseCommandQueueQueues(j);
        }
        if (t == "QueueCreate") {
            return parseCommandQueueCreate(j);
        }
        if (t == "QueueDelete") {
            return parseCommandQueueDelete(j);
        }
        if (t == "PlayFiles") {
            return parseCommandPlayFiles(j);
        }
        if (t == "QueueSwitch") {
            return parseCommandQueueSwitch(j);
        }
        if (t == "QueueAdd") {
            return parseCommandQueueAdd(j);
        }
        if (t == "QueueRemove") {
            return parseCommandQueueRemove(j);
        }
        if (t == "QueueMove") {
            return parseCommandQueueMove(j);
        }
        if (t == "QueueClear") {
            return parseCommandQueueClear(j);
        }
        if (t == "QueueShuffle") {
            return parseCommandQueueShuffle(j);
        }
        if (t == "QueueRepeat") {
            return parseCommandQueueRepeat(j);
        }
        if (t == "PlaylistList") {
            return parseCommandPlaylistList(j);
        }
        if (t == "PlaylistTracks") {
            return parseCommandPlaylistTracks(j);
        }
        if (t == "PlaylistCreate") {
            return parseCommandPlaylistCreate(j);
        }
        if (t == "PlaylistAdd") {
            return parseCommandPlaylistAdd(j);
        }
        if (t == "PlaylistLoad") {
            return parseCommandPlaylistLoad(j);
        }
        if (t == "PlaylistSave") {
            return parseCommandPlaylistSave(j);
        }
        if (t == "PlaylistDelete") {
            return parseCommandPlaylistDelete(j);
        }
        if (t == "PlaylistRename") {
            return parseCommandPlaylistRename(j);
        }
        if (t == "PlaylistExport") {
            return parseCommandPlaylistExport(j);
        }
        if (t == "PlaylistImport") {
            return parseCommandPlaylistImport(j);
        }
        if (t == "LibraryScan") {
            return parseCommandLibraryScan(j);
        }
        if (t == "LibrarySearch") {
            return parseCommandLibrarySearch(j);
        }
        if (t == "LibraryStats" || t == "LibraryStatsDetailed") {
            return parseCommandLibraryStats(j);
        }
        if (t == "LibraryAdd") {
            return parseCommandLibraryAdd(j);
        }
        if (t == "LibraryRemove") {
            return parseCommandLibraryRemove(j);
        }
        if (t == "LibraryList") {
            return parseCommandLibraryList(j);
        }
        if (t == "LibraryMaintenance") {
            return parseCommandLibraryMaintenance(j);
        }
        if (t == "TagEdit") {
            return parseCommandTagEdit(j);
        }
        if (t == "TagGet") {
            return parseCommandTagGet(j);
        }
        if (t == "ConfigGet") {
            return parseCommandConfigGet(j);
        }
        if (t == "ConfigSet") {
            return parseCommandConfigSet(j);
        }
        if (t == "ConfigList") {
            return parseCommandConfigList(j);
        }
        if (t == "ConfigExport") {
            return parseCommandConfigExport(j);
        }
        if (t == "ConfigImport") {
            return parseCommandConfigImport(j);
        }
        if (t == "ConfigReset") {
            return parseCommandConfigReset(j);
        }
        if (t == "HistoryList") {
            return parseCommandHistoryList(j);
        }
        if (t == "HistoryClear") {
            return parseCommandHistoryClear(j);
        }
        if (t == "BookmarkList") {
            return parseCommandBookmarkList(j);
        }
        if (t == "BookmarkAdd") {
            return parseCommandBookmarkAdd(j);
        }
        if (t == "BookmarkRemove") {
            return parseCommandBookmarkRemove(j);
        }
        if (t == "BookmarkGoto") {
            return parseCommandBookmarkGoto(j);
        }
        if (t == "Shutdown") {
            return parseCommandShutdown(j);
        }
        if (t == "Preview") {
            return parseCommandPreview(j);
        }
        if (t == "DeviceList") {
            return parseCommandDeviceList(j);
        }
        if (t == "DeviceSet") {
            return parseCommandDeviceSet(j);
        }
        if (t == "DeviceTest") {
            return parseCommandDeviceTest(j);
        }
        if (t == "Info") {
            return parseCommandInfo(j);
        }
        if (t == "TrackRate") {
            return parseCommandTrackRate(j);
        }
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "unknown Command type: " + t)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

// Command parsing helper implementations.

static auto parseCommandPlay(const Json& /*j*/) -> std::expected<Play, kanariv::utils::Error> {
    return Play{};
}

static auto parseCommandPause(const Json& /*j*/) -> std::expected<Pause, kanariv::utils::Error> {
    return Pause{};
}

static auto parseCommandResume(const Json& /*j*/) -> std::expected<Resume, kanariv::utils::Error> {
    return Resume{};
}

static auto parseCommandRestart(const Json& /*j*/)
    -> std::expected<Restart, kanariv::utils::Error> {
    return Restart{};
}

static auto parseCommandStop(const Json& /*j*/) -> std::expected<Stop, kanariv::utils::Error> {
    return Stop{};
}

static auto parseCommandNext(const Json& /*j*/) -> std::expected<Next, kanariv::utils::Error> {
    return Next{};
}

static auto parseCommandPrev(const Json& /*j*/) -> std::expected<Prev, kanariv::utils::Error> {
    return Prev{};
}

static auto parseCommandSeek(const Json& j) -> std::expected<Seek, kanariv::utils::Error> {
    double sec = 0;
    if (j.contains("seconds") &&
        j["seconds"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        sec =
            j["seconds"]
                .get<
                    double>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return Seek{sec};
}

static auto parseCommandStatusReq(const Json& /*j*/)
    -> std::expected<StatusReq, kanariv::utils::Error> {
    return StatusReq{};
}

static auto parseCommandStatusWait(const Json& j)
    -> std::expected<StatusWait, kanariv::utils::Error> {
    StatusWait sw{};
    if (j.contains("timeout_ms") &&
        j["timeout_ms"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        sw.timeout_ms =
            j["timeout_ms"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return sw;
}

static auto parseCommandVolumeSet(const Json& j)
    -> std::expected<VolumeSet, kanariv::utils::Error> {
    VolumeSet v{};
    if (j.contains("level") && !j["level"].isNull() &&
        j["level"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.level =
            j["level"]
                .get<
                    float>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("mute") && !j["mute"].isNull() &&
        j["mute"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.mute =
            j["mute"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("deltaPct") && !j["deltaPct"].isNull() &&
        j["deltaPct"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.deltaPct =
            j["deltaPct"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandQueueList(const Json& j)
    -> std::expected<QueueList, kanariv::utils::Error> {
    QueueList q{};
    if (j.contains("order") &&
        j["order"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q.order =
            j["order"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return q;
}

static auto parseCommandQueueQueues(const Json& /*j*/)
    -> std::expected<QueueQueues, kanariv::utils::Error> {
    return QueueQueues{};
}

static auto parseCommandQueueCreate(const Json& j)
    -> std::expected<QueueCreate, kanariv::utils::Error> {
    QueueCreate q{};
    if (j.contains("name") &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q.name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return q;
}

static auto parseCommandQueueDelete(const Json& j)
    -> std::expected<QueueDelete, kanariv::utils::Error> {
    int64_t qid = 1;
    if (j.contains("qid") &&
        j["qid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        qid =
            j["qid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return QueueDelete{qid};
}

static auto parseCommandPlayFiles(const Json& j)
    -> std::expected<PlayFiles, kanariv::utils::Error> {
    PlayFiles p{};
    if (j.contains("paths") &&
        j["paths"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json arr =
            j["paths"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t i = 0, n = arr.size(); i < n; ++i) {
            auto je = arr.at(i);
            if (!je) {
                return std::unexpected{je.error()};
            }
            if (je->isString()) {
                p.paths.push_back(je->get<std::string>());
            }
        }
    }
    if (j.contains("save") &&
        j["save"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p.save =
            j["save"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return p;
}

static auto parseCommandQueueSwitch(const Json& j)
    -> std::expected<QueueSwitch, kanariv::utils::Error> {
    int64_t qid = 1;
    if (j.contains("qid") &&
        j["qid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        qid =
            j["qid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return QueueSwitch{qid};
}

static auto parseCommandQueueAdd(const Json& j) -> std::expected<QueueAdd, kanariv::utils::Error> {
    std::string q;
    bool search = false;
    if (j.contains("query") &&
        j["query"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q = j["query"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("search") &&
        j["search"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        search =
            j["search"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return QueueAdd{.query = std::move(q), .search = search};
}

static auto parseCommandQueueRemove(const Json& j)
    -> std::expected<QueueRemove, kanariv::utils::Error> {
    std::string id;
    if (j.contains("idOrIndex") &&
        j["idOrIndex"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        id =
            j["idOrIndex"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return QueueRemove{std::move(id)};
}

static auto parseCommandQueueMove(const Json& j)
    -> std::expected<QueueMove, kanariv::utils::Error> {
    std::size_t from = 0, to = 0;
    if (j.contains("from") &&
        j["from"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        from =
            j["from"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("to") &&
        j["to"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        to =
            j["to"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return QueueMove{.from = from, .to = to};
}

static auto parseCommandQueueClear(const Json& /*j*/)
    -> std::expected<QueueClear, kanariv::utils::Error> {
    return QueueClear{};
}

static auto parseCommandQueueShuffle(const Json& j)
    -> std::expected<QueueShuffle, kanariv::utils::Error> {
    QueueShuffle v{};
    if (j.contains("on") && !j["on"].isNull() &&
        j["on"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.on =
            j["on"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandQueueRepeat(const Json& j)
    -> std::expected<QueueRepeat, kanariv::utils::Error> {
    QueueRepeat v{};
    if (j.contains("mode") && !j["mode"].isNull() &&
        j["mode"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        auto m = kanariv::types::parseRepeatMode(
            j["mode"]
                .get<
                    std::
                        string>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!m) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "unknown RepeatMode")};
        }
        v.mode = *m;
    }
    return v;
}

static auto parseCommandPlaylistList(const Json& /*j*/)
    -> std::expected<PlaylistList, kanariv::utils::Error> {
    return PlaylistList{};
}

static auto parseCommandPlaylistTracks(const Json& j)
    -> std::expected<PlaylistTracks, kanariv::utils::Error> {
    int64_t pid = 0;
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistTracks{pid};
}

static auto parseCommandPlaylistCreate(const Json& j)
    -> std::expected<PlaylistCreate, kanariv::utils::Error> {
    PlaylistCreate pc;
    if (j.contains("name") &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pc.name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("smart_query") &&
        j["smart_query"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pc.smart_query =
            j["smart_query"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return pc;
}

static auto parseCommandPlaylistAdd(const Json& j)
    -> std::expected<PlaylistAdd, kanariv::utils::Error> {
    int64_t pid = 0;
    int64_t tid = 0;
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("track_id") &&
        j["track_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        tid =
            j["track_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistAdd{.pid = pid, .track_id = tid};
}

static auto parseCommandPlaylistLoad(const Json& j)
    -> std::expected<PlaylistLoad, kanariv::utils::Error> {
    int64_t pid = 0;
    bool play = false;
    bool replace = false;
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("play") &&
        j["play"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        play =
            j["play"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("replace") &&
        j["replace"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        replace =
            j["replace"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistLoad{.pid = pid, .play = play, .replace = replace};
}

static auto parseCommandPlaylistSave(const Json& j)
    -> std::expected<PlaylistSave, kanariv::utils::Error> {
    std::string name;
    std::optional<int64_t> qid;
    if (j.contains("name") &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("queue_id") && !j["queue_id"].isNull() &&
        j["queue_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        qid =
            j["queue_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistSave{.name = std::move(name), .queue_id = qid};
}

static auto parseCommandPlaylistDelete(const Json& j)
    -> std::expected<PlaylistDelete, kanariv::utils::Error> {
    int64_t pid = 0;
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistDelete{pid};
}

static auto parseCommandPlaylistRename(const Json& j)
    -> std::expected<PlaylistRename, kanariv::utils::Error> {
    int64_t pid = 0;
    std::string newName;
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("newName") &&
        j["newName"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        newName =
            j["newName"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistRename{.pid = pid, .newName = std::move(newName)};
}

static auto parseCommandPlaylistExport(const Json& j)
    -> std::expected<PlaylistExport, kanariv::utils::Error> {
    int64_t pid = 0;
    std::string path;
    std::string format = "m3u";
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        path =
            j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("format") &&
        j["format"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        format =
            j["format"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistExport{.pid = pid, .path = std::move(path), .format = std::move(format)};
}

static auto parseCommandPlaylistImport(const Json& j)
    -> std::expected<PlaylistImport, kanariv::utils::Error> {
    std::string path;
    std::optional<std::string> name;
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        path =
            j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("name") && !j["name"].isNull() &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return PlaylistImport{.path = std::move(path), .name = name};
}

static auto parseCommandLibraryScan(const Json& j)
    -> std::expected<LibraryScan, kanariv::utils::Error> {
    LibraryScan v{};
    if (j.contains("path") && !j["path"].isNull() &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.path =
            j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("full_hash") &&
        j["full_hash"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.full_hash =
            j["full_hash"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else if (
        j.contains("mode") &&
        j["mode"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.full_hash =
            (j["mode"].get<std::string>() ==
             "full"); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandLibrarySearch(const Json& j)
    -> std::expected<LibrarySearch, kanariv::utils::Error> {
    std::string q;
    int lim = defaultLibrarySearchLimit;
    if (j.contains("query") &&
        j["query"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q = j["query"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("limit") &&
        j["limit"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        lim =
            j["limit"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return LibrarySearch{.query = std::move(q), .limit = lim};
}

static auto parseCommandLibraryStats(const Json& j)
    -> std::expected<LibraryStats, kanariv::utils::Error> {
    LibraryStats v{};
    const std::string t =
        j.contains("type") && j["type"].isString()
            ? j["type"].get<std::string>()
            : std::
                  string{}; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (j.contains("top_tracks") &&
        j["top_tracks"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.top_tracks =
            j["top_tracks"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else if (t == "LibraryStatsDetailed") {
        v.top_tracks = defaultLibraryStatsTopTracks;
    }
    if (j.contains("top_albums") &&
        j["top_albums"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.top_albums =
            j["top_albums"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("top_genres") &&
        j["top_genres"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.top_genres =
            j["top_genres"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("top_artists") &&
        j["top_artists"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.top_artists =
            j["top_artists"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandLibraryAdd(const Json& j)
    -> std::expected<LibraryAdd, kanariv::utils::Error> {
    std::string p;
    bool rec = false;
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p = j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("recursive") &&
        j["recursive"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        rec =
            j["recursive"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return LibraryAdd{.path = std::move(p), .recursive = rec};
}

static auto parseCommandLibraryRemove(const Json& j)
    -> std::expected<LibraryRemove, kanariv::utils::Error> {
    std::string q;
    if (j.contains("query") &&
        j["query"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q = j["query"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else if (
        j.contains("id") &&
        j["id"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q = j["id"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else if (
        j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q = std::to_string(
            j["id"]
                .get<
                    int64_t>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return LibraryRemove{std::move(q)};
}

static auto parseCommandLibraryList(const Json& j)
    -> std::expected<LibraryList, kanariv::utils::Error> {
    LibraryList v{};
    if (j.contains("query") && !j["query"].isNull() &&
        j["query"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.query =
            j["query"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("limit") &&
        j["limit"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.limit =
            j["limit"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("offset") &&
        j["offset"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.offset =
            j["offset"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("artist") && !j["artist"].isNull() &&
        j["artist"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.artist =
            j["artist"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("album") && !j["album"].isNull() &&
        j["album"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.album =
            j["album"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("genre") && !j["genre"].isNull() &&
        j["genre"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.genre =
            j["genre"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("rating") && !j["rating"].isNull() &&
        j["rating"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.rating =
            j["rating"]
                .get<
                    int32_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandLibraryMaintenance(const Json& j)
    -> std::expected<LibraryMaintenance, kanariv::utils::Error> {
    LibraryMaintenance v{};
    if (j.contains("action") &&
        j["action"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.action =
            j["action"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("dry_run") &&
        j["dry_run"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.dry_run =
            j["dry_run"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("library_id") &&
        j["library_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.library_id =
            j["library_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandTagEdit(const Json& j) -> std::expected<TagEdit, kanariv::utils::Error> {
    int64_t id = 0;
    std::string field, value;
    if (j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        id =
            j["id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("field") &&
        j["field"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        field =
            j["field"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("value") &&
        j["value"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        value =
            j["value"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return TagEdit{.id = id, .field = std::move(field), .value = std::move(value)};
}

static auto parseCommandTagGet(const Json& j) -> std::expected<TagGet, kanariv::utils::Error> {
    TagGet tg{};
    if (j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        tg.id =
            j["id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        tg.path =
            j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("cover") &&
        j["cover"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        tg.cover =
            j["cover"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return tg;
}

static auto parseCommandConfigGet(const Json& j)
    -> std::expected<ConfigGet, kanariv::utils::Error> {
    std::string k;
    if (j.contains("key") &&
        j["key"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        k = j["key"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return ConfigGet{std::move(k)};
}

static auto parseCommandConfigSet(const Json& j)
    -> std::expected<ConfigSet, kanariv::utils::Error> {
    std::string k, v;
    if (j.contains("key") &&
        j["key"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        k = j["key"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("value") &&
        j["value"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v = j["value"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return ConfigSet{.key = std::move(k), .value = std::move(v)};
}

static auto parseCommandConfigList(const Json& /*j*/)
    -> std::expected<ConfigList, kanariv::utils::Error> {
    return ConfigList{};
}

static auto parseCommandConfigExport(const Json& j)
    -> std::expected<ConfigExport, kanariv::utils::Error> {
    std::string p;
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p = j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return ConfigExport{std::move(p)};
}

static auto parseCommandConfigImport(const Json& j)
    -> std::expected<ConfigImport, kanariv::utils::Error> {
    std::string p;
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p = j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return ConfigImport{std::move(p)};
}

static auto parseCommandConfigReset(const Json& j)
    -> std::expected<ConfigReset, kanariv::utils::Error> {
    std::optional<std::string> k;
    if (j.contains("key") && !j["key"].isNull() &&
        j["key"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        k = j["key"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return ConfigReset{std::move(k)};
}

static auto parseCommandHistoryList(const Json& j)
    -> std::expected<HistoryList, kanariv::utils::Error> {
    HistoryList v{};
    if (j.contains("limit") && !j["limit"].isNull() &&
        j["limit"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.limit =
            j["limit"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandHistoryClear(const Json& /*j*/)
    -> std::expected<HistoryClear, kanariv::utils::Error> {
    return HistoryClear{};
}

static auto parseCommandBookmarkList(const Json& j)
    -> std::expected<BookmarkList, kanariv::utils::Error> {
    BookmarkList v{};
    if (j.contains("track_id") && !j["track_id"].isNull() &&
        j["track_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.track_id =
            j["track_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandBookmarkAdd(const Json& j)
    -> std::expected<BookmarkAdd, kanariv::utils::Error> {
    BookmarkAdd v{};
    if (j.contains("position_ms") &&
        j["position_ms"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.position_ms =
            j["position_ms"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("note") &&
        j["note"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.note =
            j["note"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("track_id") && !j["track_id"].isNull() &&
        j["track_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.track_id =
            j["track_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandBookmarkRemove(const Json& j)
    -> std::expected<BookmarkRemove, kanariv::utils::Error> {
    BookmarkRemove v{};
    if (j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.id =
            j["id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandBookmarkGoto(const Json& j)
    -> std::expected<BookmarkGoto, kanariv::utils::Error> {
    BookmarkGoto v{};
    if (j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v.id =
            j["id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return v;
}

static auto parseCommandShutdown(const Json& /*j*/)
    -> std::expected<Shutdown, kanariv::utils::Error> {
    return Shutdown{};
}

static auto parseCommandPreview(const Json& j) -> std::expected<Preview, kanariv::utils::Error> {
    std::string f;
    if (j.contains("file") &&
        j["file"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        f = j["file"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return Preview{std::move(f)};
}

static auto parseCommandDeviceList(const Json& /*j*/)
    -> std::expected<DeviceList, kanariv::utils::Error> {
    return DeviceList{};
}

static auto parseCommandDeviceSet(const Json& j)
    -> std::expected<DeviceSet, kanariv::utils::Error> {
    std::string id;
    if (j.contains("id") &&
        j["id"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        id =
            j["id"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return DeviceSet{std::move(id)};
}

static auto parseCommandDeviceTest(const Json& j)
    -> std::expected<DeviceTest, kanariv::utils::Error> {
    std::optional<std::string> id;
    if (j.contains("id") && !j["id"].isNull() &&
        j["id"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        id =
            j["id"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    DeviceTest dt{.id = std::move(id)};
    if (j.contains("tone") &&
        j["tone"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        dt.tone =
            j["tone"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return dt;
}

static auto parseCommandInfo(const Json& /*j*/) -> std::expected<Info, kanariv::utils::Error> {
    return Info{};
}

static auto parseCommandTrackRate(const Json& j)
    -> std::expected<TrackRate, kanariv::utils::Error> {
    TrackRate tr{};
    if (j.contains("track_id") &&
        j["track_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        tr.track_id =
            j["track_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("rating") &&
        j["rating"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        tr.rating =
            j["rating"]
                .get<
                    int32_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return tr;
}

// Forward declarations for toJson helpers.
static auto toJsonStatus(const Status& v) -> Json;
static auto toJsonQueueTracks(const QueueTracks& v) -> Json;
static auto toJsonQueues(const Queues& v) -> Json;
static auto toJsonQueueCreated(const QueueCreated& v) -> Json;
static auto toJsonPlaylistLoaded(const PlaylistLoaded& v) -> Json;
static auto toJsonVolumeInfo(const VolumeInfo& v) -> Json;
static auto toJsonLibraryStats(const kanariv::types::LibraryStats& v) -> Json;
static auto toJsonTracks(const Tracks& v) -> Json;
static auto toJsonSearchResults(const SearchResults& v) -> Json;
static auto toJsonScanReport(const ScanReport& v) -> Json;
static auto toJsonPlaylists(const Playlists& v) -> Json;
static auto toJsonPlaylistData(const PlaylistData& v) -> Json;
static auto toJsonPlaylistCreated(const PlaylistCreated& v) -> Json;
static auto toJsonPlaylistImportReport(const PlaylistImportReport& v) -> Json;
static auto toJsonLibraryMaintenanceReport(const LibraryMaintenanceReport& v) -> Json;
static auto toJsonConfigValue(const ConfigValue& v) -> Json;
static auto toJsonConfigValues(const ConfigValues& v) -> Json;
static auto toJsonSingleTrack(const SingleTrack& v) -> Json;
static auto toJsonTrackInfo(const TrackInfo& v) -> Json;
static auto toJsonHistory(const History& v) -> Json;
static auto toJsonBookmarks(const Bookmarks& v) -> Json;
static auto toJsonDevices(const Devices& v) -> Json;
static auto toJsonEmpty(const Empty& v) -> Json;

auto toJson(const Result& r) -> Json {
    return std::visit(
        [](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Status>) {
                return toJsonStatus(v);
            }
            if constexpr (std::is_same_v<T, QueueTracks>) {
                return toJsonQueueTracks(v);
            }
            if constexpr (std::is_same_v<T, Queues>) {
                return toJsonQueues(v);
            }
            if constexpr (std::is_same_v<T, QueueCreated>) {
                return toJsonQueueCreated(v);
            }
            if constexpr (std::is_same_v<T, PlaylistLoaded>) {
                return toJsonPlaylistLoaded(v);
            }
            if constexpr (std::is_same_v<T, VolumeInfo>) {
                return toJsonVolumeInfo(v);
            }
            if constexpr (std::is_same_v<T, kanariv::types::LibraryStats>) {
                return toJsonLibraryStats(v);
            }
            if constexpr (std::is_same_v<T, Tracks>) {
                return toJsonTracks(v);
            }
            if constexpr (std::is_same_v<T, SearchResults>) {
                return toJsonSearchResults(v);
            }
            if constexpr (std::is_same_v<T, ScanReport>) {
                return toJsonScanReport(v);
            }
            if constexpr (std::is_same_v<T, Playlists>) {
                return toJsonPlaylists(v);
            }
            if constexpr (std::is_same_v<T, PlaylistData>) {
                return toJsonPlaylistData(v);
            }
            if constexpr (std::is_same_v<T, PlaylistCreated>) {
                return toJsonPlaylistCreated(v);
            }
            if constexpr (std::is_same_v<T, PlaylistImportReport>) {
                return toJsonPlaylistImportReport(v);
            }
            if constexpr (std::is_same_v<T, LibraryMaintenanceReport>) {
                return toJsonLibraryMaintenanceReport(v);
            }
            if constexpr (std::is_same_v<T, ConfigValue>) {
                return toJsonConfigValue(v);
            }
            if constexpr (std::is_same_v<T, ConfigValues>) {
                return toJsonConfigValues(v);
            }
            if constexpr (std::is_same_v<T, SingleTrack>) {
                return toJsonSingleTrack(v);
            }
            if constexpr (std::is_same_v<T, TrackInfo>) {
                return toJsonTrackInfo(v);
            }
            if constexpr (std::is_same_v<T, History>) {
                return toJsonHistory(v);
            }
            if constexpr (std::is_same_v<T, Bookmarks>) {
                return toJsonBookmarks(v);
            }
            if constexpr (std::is_same_v<T, Devices>) {
                return toJsonDevices(v);
            }
            if constexpr (std::is_same_v<T, Empty>) {
                return toJsonEmpty(v);
            }
            if constexpr (std::is_same_v<T, kanariv::utils::Error>) {
                return detail::errorToJson(v);
            }
            Json j;
            j["type"] =
                "Unknown"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            return j;
        },
        r);
}

// toJson helper implementations.

static auto toJsonStatus(const Status& v) -> Json {
    Json j;
    j["type"] = "Status"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["state"] = std::format(
        "{}", v.state); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["state_value"] = std::to_underlying(
        v.state);         // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["pos"] = v.pos;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["dur"] = v.dur;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["vol"] = v.vol;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["muted"] = v.muted; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["shuffle"] =
        v.shuffle; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["repeat"] = std::format(
        "{}", v.repeat); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["repeat_value"] = std::to_underlying(
        v.repeat); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["track_id"] =
        v.track_id;         // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["title"] = v.title;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["artist"] = v.artist; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["path"] = v.path;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["q_size"] = v.q_size; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["q_idx"] = v.q_idx;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["version"] =
        v.version; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonQueueTracks(const QueueTracks& v) -> Json {
    Json j;
    j["type"] =
        "QueueTracks"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : v.tracks) {
        j["tracks"].push_back(kanariv::utils::trackToJson(
            t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonQueues(const Queues& v) -> Json {
    Json j;
    j["type"] = "Queues"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["queues"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& q : v.entries) {
        Json e = Json::object();
        e["id"] = q.id;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        e["name"] = q.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        e["tracks"] =
            q.tracks; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        e["active"] =
            q.active;       // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        e["temp"] = q.temp; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["queues"].push_back(
            e); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonQueueCreated(const QueueCreated& v) -> Json {
    Json j;
    j["type"] =
        "QueueCreated"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["id"] = v.id;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["name"] = v.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonPlaylistLoaded(const PlaylistLoaded& v) -> Json {
    Json j;
    j["type"] =
        "PlaylistLoaded"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["queue_id"] =
        v.queue_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["status"] = toJson(
        Result{v.status}); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonVolumeInfo(const VolumeInfo& v) -> Json {
    Json j;
    j["type"] =
        "VolumeInfo";     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["vol"] = v.vol;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["muted"] = v.muted; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonLibraryStats(const kanariv::types::LibraryStats& v) -> Json {
    Json j;
    j["type"] =
        "LibraryStats";     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] = v.tracks; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["queues"] = v.queues; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["playlists"] =
        v.playlists; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["total_duration_ms"] =
        v.total_duration_ms; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["total_play_time_ms"] =
        v.total_play_time_ms; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (v.top_tracks) {
        j["top_tracks"] =
            Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const auto& t : *v.top_tracks) {
            j["top_tracks"].push_back(kanariv::utils::trackToJson(
                t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
    }
    if (v.top_albums) {
        j["top_albums"] =
            Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const auto& e : *v.top_albums) {
            Json je;
            je["name"] =
                e.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            je["track_count"] = static_cast<int64_t>(
                e.track_count); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            je["total_plays"] =
                e.total_plays; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            j["top_albums"].push_back(
                je); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
    }
    if (v.top_genres) {
        j["top_genres"] =
            Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const auto& e : *v.top_genres) {
            Json je;
            je["name"] =
                e.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            je["track_count"] = static_cast<int64_t>(
                e.track_count); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            je["total_plays"] =
                e.total_plays; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            j["top_genres"].push_back(
                je); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
    }
    if (v.top_artists) {
        j["top_artists"] =
            Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const auto& e : *v.top_artists) {
            Json je;
            je["name"] =
                e.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            je["track_count"] = static_cast<int64_t>(
                e.track_count); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            je["total_plays"] =
                e.total_plays; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            j["top_artists"].push_back(
                je); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        }
    }
    return j;
}

static auto toJsonTracks(const Tracks& v) -> Json {
    Json j;
    j["type"] = "Tracks"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : v.tracks) {
        j["tracks"].push_back(kanariv::utils::trackToJson(
            t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonSearchResults(const SearchResults& v) -> Json {
    Json j;
    j["type"] =
        "SearchResults";  // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["query"] = v.query; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : v.tracks) {
        j["tracks"].push_back(kanariv::utils::trackToJson(
            t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonScanReport(const ScanReport& v) -> Json {
    Json j;
    j["type"] =
        "ScanReport";     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["added"] = v.added; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonPlaylists(const Playlists& v) -> Json {
    Json j;
    j["type"] =
        "Playlists"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["playlists"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& p : v.playlists) {
        j["playlists"].push_back(detail::playlistToJson(
            p)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonPlaylistData(const PlaylistData& v) -> Json {
    Json j;
    j["type"] =
        "PlaylistData"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : v.tracks) {
        j["tracks"].push_back(kanariv::utils::trackToJson(
            t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    j["format"] = v.format; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonPlaylistCreated(const PlaylistCreated& v) -> Json {
    Json j;
    j["type"] =
        "PlaylistCreated"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["id"] = v.id;        // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["name"] = v.name;    // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonPlaylistImportReport(const PlaylistImportReport& v) -> Json {
    Json j;
    j["type"] =
        "PlaylistImportReport"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["pid"] = v.pid;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["name"] = v.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : v.tracks) {
        j["tracks"].push_back(kanariv::utils::trackToJson(
            t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    j["matched"] =
        v.matched; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["skipped"] =
        v.skipped; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["duplicates"] =
        v.duplicates; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonLibraryMaintenanceReport(const LibraryMaintenanceReport& v) -> Json {
    Json j;
    j["type"] =
        "LibraryMaintenanceReport"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["tracks_checked"] =
        v.tracks_checked; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["missing_tracks"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& t : v.missing_tracks) {
        j["missing_tracks"].push_back(kanariv::utils::trackToJson(
            t)); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    j["duplicate_groups"] =
        v.duplicate_groups; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["duplicate_tracks"] =
        v.duplicate_tracks; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonConfigValue(const ConfigValue& v) -> Json {
    Json j;
    j["type"] =
        "ConfigValue";    // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["key"] = v.key;     // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["value"] = v.value; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonConfigValues(const ConfigValues& v) -> Json {
    Json j;
    j["type"] =
        "ConfigValues"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["values"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& cv : v.values) {
        Json kv = Json::object();
        kv["key"] = cv.key; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        kv["value"] =
            cv.value; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["values"].push_back(
            kv); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonSingleTrack(const SingleTrack& v) -> Json {
    Json j;
    j["type"] =
        "SingleTrack"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["track"] = kanariv::utils::trackToJson(
        v.track); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (!v.cover_mime.empty()) {
        j["cover_mime"] =
            v.cover_mime; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (!v.cover_data.empty()) {
        j["cover_data"] =
            v.cover_data; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonTrackInfo(const TrackInfo& v) -> Json {
    Json j;
    j["type"] =
        "TrackInfo"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["track"] = kanariv::utils::trackToJson(
        v.track); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["play_count"] =
        v.play_count; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["last_played"] =
        v.last_played; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

static auto toJsonHistory(const History& v) -> Json {
    Json j;
    j["type"] = "History"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["entries"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& e : v.entries) {
        Json ej;
        ej["id"] = e.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["track_id"] =
            e.track_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["started_at"] =
            e.started_at; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["completed_at"] =
            e.completed_at; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["position_ms"] =
            e.position_ms; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["completion_pct"] =
            e.completion_pct; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["queue_id"] =
            e.queue_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["title"] =
            e.title; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["artist"] =
            e.artist; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["path"] =
            e.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["duration"] =
            e.duration; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["entries"].push_back(
            ej); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonBookmarks(const Bookmarks& v) -> Json {
    Json j;
    j["type"] =
        "Bookmarks"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["entries"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& e : v.entries) {
        Json ej;
        ej["id"] = e.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["track_id"] =
            e.track_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["position_ms"] =
            e.position_ms; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["note"] =
            e.note; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ej["created"] =
            e.created; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["entries"].push_back(
            ej); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonDevices(const Devices& v) -> Json {
    Json j;
    j["type"] = "Devices"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["devices"] =
        Json::array(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (const auto& d : v.devices) {
        Json dj;
        dj["id"] = d.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        dj["name"] =
            d.name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        dj["isDefault"] =
            d.isDefault; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["devices"].push_back(
            dj); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j;
}

static auto toJsonEmpty(const Empty& /*v*/) -> Json {
    Json j;
    j["type"] = "Empty"; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

// Forward declarations for resultFromJson helpers.
static auto parseResultStatus(const Json& j) -> std::expected<Status, kanariv::utils::Error>;
static auto parseResultQueueTracks(const Json& j)
    -> std::expected<QueueTracks, kanariv::utils::Error>;
static auto parseResultQueues(const Json& j) -> std::expected<Queues, kanariv::utils::Error>;
static auto parseResultQueueCreated(const Json& j)
    -> std::expected<QueueCreated, kanariv::utils::Error>;
static auto parseResultPlaylistLoaded(const Json& j)
    -> std::expected<PlaylistLoaded, kanariv::utils::Error>;
static auto parseResultVolumeInfo(const Json& j)
    -> std::expected<VolumeInfo, kanariv::utils::Error>;
static auto parseResultLibraryStats(const Json& j)
    -> std::expected<kanariv::types::LibraryStats, kanariv::utils::Error>;
static auto parseResultTracks(const Json& j) -> std::expected<Tracks, kanariv::utils::Error>;
static auto parseResultSearchResults(const Json& j)
    -> std::expected<SearchResults, kanariv::utils::Error>;
static auto parseResultScanReport(const Json& j)
    -> std::expected<ScanReport, kanariv::utils::Error>;
static auto parseResultPlaylists(const Json& j) -> std::expected<Playlists, kanariv::utils::Error>;
static auto parseResultPlaylistData(const Json& j)
    -> std::expected<PlaylistData, kanariv::utils::Error>;
static auto parseResultPlaylistCreated(const Json& j)
    -> std::expected<PlaylistCreated, kanariv::utils::Error>;
static auto parseResultPlaylistImportReport(const Json& j)
    -> std::expected<PlaylistImportReport, kanariv::utils::Error>;
static auto parseResultLibraryMaintenanceReport(const Json& j)
    -> std::expected<LibraryMaintenanceReport, kanariv::utils::Error>;
static auto parseResultConfigValue(const Json& j)
    -> std::expected<ConfigValue, kanariv::utils::Error>;
static auto parseResultConfigValues(const Json& j)
    -> std::expected<ConfigValues, kanariv::utils::Error>;
static auto parseResultSingleTrack(const Json& j)
    -> std::expected<SingleTrack, kanariv::utils::Error>;
static auto parseResultTrackInfo(const Json& j) -> std::expected<TrackInfo, kanariv::utils::Error>;
static auto parseResultHistory(const Json& j) -> std::expected<History, kanariv::utils::Error>;
static auto parseResultBookmarks(const Json& j) -> std::expected<Bookmarks, kanariv::utils::Error>;
static auto parseResultDevices(const Json& j) -> std::expected<Devices, kanariv::utils::Error>;

auto resultFromJson(const Json& j) -> std::expected<Result, kanariv::utils::Error> {
    try {
        if (!j.contains("type") ||
            !j["type"]
                 .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "missing Result type")};
        }
        auto t =
            j["type"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (t == "Status") {
            return parseResultStatus(j);
        }
        if (t == "QueueTracks") {
            return parseResultQueueTracks(j);
        }
        if (t == "Queues") {
            return parseResultQueues(j);
        }
        if (t == "QueueCreated") {
            return parseResultQueueCreated(j);
        }
        if (t == "PlaylistLoaded") {
            return parseResultPlaylistLoaded(j);
        }
        if (t == "VolumeInfo") {
            return parseResultVolumeInfo(j);
        }
        if (t == "LibraryStats" || t == "LibraryStatsDetailed") {
            return parseResultLibraryStats(j);
        }
        if (t == "Tracks") {
            return parseResultTracks(j);
        }
        if (t == "SearchResults") {
            return parseResultSearchResults(j);
        }
        if (t == "ScanReport") {
            return parseResultScanReport(j);
        }
        if (t == "Playlists") {
            return parseResultPlaylists(j);
        }
        if (t == "PlaylistData") {
            return parseResultPlaylistData(j);
        }
        if (t == "PlaylistCreated") {
            return parseResultPlaylistCreated(j);
        }
        if (t == "PlaylistImportReport") {
            return parseResultPlaylistImportReport(j);
        }
        if (t == "LibraryMaintenanceReport") {
            return parseResultLibraryMaintenanceReport(j);
        }
        if (t == "ConfigValue") {
            return parseResultConfigValue(j);
        }
        if (t == "ConfigValues") {
            return parseResultConfigValues(j);
        }
        if (t == "SingleTrack") {
            return parseResultSingleTrack(j);
        }
        if (t == "TrackInfo") {
            return parseResultTrackInfo(j);
        }
        if (t == "History") {
            return parseResultHistory(j);
        }
        if (t == "Bookmarks") {
            return parseResultBookmarks(j);
        }
        if (t == "Devices") {
            return parseResultDevices(j);
        }
        if (t == "Empty") {
            return Result{Empty{}};
        }
        if (t == "Error") {
            auto e = detail::errorFromJson(j);
            if (!e) {
                return std::unexpected{e.error()};
            }
            return Result{*e};
        }
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "unknown Result type: " + t)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

auto serializeRequest(const IpcRequest& req) -> std::string {
    Json j;
    j["id"] = req.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["cmd"] =
        toJson(req.cmd); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j.dump();
}

auto deserializeRequest(std::string_view sv) -> std::expected<IpcRequest, kanariv::utils::Error> {
    try {
        const Json j = Json::parse(sv);
        if (!j.contains("id") || !j.contains("cmd")) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "missing id/cmd")};
        }
        auto id =
            j["id"]
                .get<
                    uint32_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        auto cmd = commandFromJson(
            j["cmd"]); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!cmd) {
            return std::unexpected{cmd.error()};
        }
        return IpcRequest{.id = id, .cmd = std::move(*cmd)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

auto serializeReply(const IpcReply& rep) -> std::string {
    Json j;
    j["id"] = rep.id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (rep.result.has_value()) {
        j["ok"] = true; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["result"] = toJson(
            *rep.result); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else {
        j["ok"] = false; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["error"] = detail::errorToJson(
            rep.result
                .error()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return j.dump();
}

auto deserializeReply(std::string_view sv) -> std::expected<IpcReply, kanariv::utils::Error> {
    try {
        const Json j = Json::parse(sv);
        if (!j.contains("id")) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing id")};
        }
        auto id =
            j["id"]
                .get<
                    uint32_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        bool ok = true;
        if (j.contains("ok") &&
            j["ok"]
                .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            ok =
                j["ok"]
                    .get<
                        bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        } else if (j.contains("error")) {
            ok = false;
        }

        if (ok) {
            if (!j.contains("result")) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "missing result")};
            }
            auto r = resultFromJson(
                j["result"]); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (!r) {
                return std::unexpected{r.error()};
            }
            return IpcReply{.id = id, .result = std::move(*r)};
        } else {
            Json ej;
            if (j.contains("error")) {
                ej = j
                    ["error"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else if (j.contains("result")) {
                ej = j
                    ["result"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "missing error")};
            }
            auto e = detail::errorFromJson(ej);
            if (!e) {
                return std::unexpected{e.error()};
            }
            return IpcReply{.id = id, .result = std::unexpected{*e}};
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

// Result parsing helper implementations.

static auto parseResultStatus(const Json& j) -> std::expected<Status, kanariv::utils::Error> {
    Status s{};
    if (j.contains("state") &&
        j["state"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        auto ps = kanariv::types::parsePlaybackState(
            j["state"]
                .get<
                    std::
                        string>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!ps) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "unknown PlaybackState")};
        }
        s.state = *ps;
    } else if (
        j.contains("state_value") &&
        j["state_value"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        int v =
            j["state_value"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.state = static_cast<kanariv::types::PlaybackState>(v);
    }
    if (j.contains("pos") &&
        j["pos"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.pos =
            j["pos"]
                .get<
                    double>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("dur") &&
        j["dur"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.dur =
            j["dur"]
                .get<
                    double>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("vol") &&
        j["vol"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.vol =
            j["vol"]
                .get<
                    float>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("muted") &&
        j["muted"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.muted =
            j["muted"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("shuffle") &&
        j["shuffle"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.shuffle =
            j["shuffle"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("repeat") &&
        j["repeat"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        auto rm = kanariv::types::parseRepeatMode(
            j["repeat"]
                .get<
                    std::
                        string>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!rm) {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                             "unknown RepeatMode")};
        }
        s.repeat = *rm;
    } else if (
        j.contains("repeat_value") &&
        j["repeat_value"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        int v =
            j["repeat_value"]
                .get<
                    int>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.repeat = static_cast<kanariv::types::RepeatMode>(v);
    }
    if (j.contains("track_id") &&
        j["track_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.track_id =
            j["track_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("title") &&
        j["title"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.title =
            j["title"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("artist") &&
        j["artist"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.artist =
            j["artist"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("path") &&
        j["path"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.path =
            j["path"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("q_size") &&
        j["q_size"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.q_size =
            j["q_size"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("q_idx") &&
        j["q_idx"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.q_idx =
            j["q_idx"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("version") &&
        j["version"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s.version =
            j["version"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return s;
}

static auto parseResultQueueTracks(const Json& j)
    -> std::expected<QueueTracks, kanariv::utils::Error> {
    QueueTracks qt{};
    if (j.contains("tracks") &&
        j["tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json tracks =
            j["tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
            auto jt = tracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            qt.tracks.push_back(std::move(*tr));
        }
    }
    return qt;
}

static auto parseResultQueues(const Json& j) -> std::expected<Queues, kanariv::utils::Error> {
    Queues qs{};
    if (j.contains("queues") &&
        j["queues"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json arr =
            j["queues"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t qi = 0, qn = arr.size(); qi < qn; ++qi) {
            auto je = arr.at(qi);
            if (!je) {
                return std::unexpected{je.error()};
            }
            QueueEntry e{};
            if (auto idExp = je->at("id"); idExp && idExp->isNumber()) {
                e.id = idExp->get<int64_t>();
            }
            if (auto nameExp = je->at("name"); nameExp && nameExp->isString()) {
                e.name = nameExp->get<std::string>();
            }
            if (auto trExp = je->at("tracks"); trExp && trExp->isNumber()) {
                e.tracks = trExp->get<std::size_t>();
            }
            if (auto acExp = je->at("active"); acExp && acExp->isBoolean()) {
                e.active = acExp->get<bool>();
            }
            if (auto tmExp = je->at("temp"); tmExp && tmExp->isBoolean()) {
                e.temp = tmExp->get<bool>();
            }
            qs.entries.push_back(std::move(e));
        }
    }
    return qs;
}

static auto parseResultQueueCreated(const Json& j)
    -> std::expected<QueueCreated, kanariv::utils::Error> {
    QueueCreated q{};
    if (j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q.id =
            j["id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("name") &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        q.name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return q;
}

static auto parseResultPlaylistLoaded(const Json& j)
    -> std::expected<PlaylistLoaded, kanariv::utils::Error> {
    PlaylistLoaded p{};
    if (j.contains("queue_id") &&
        j["queue_id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p.queue_id =
            j["queue_id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("status")) {
        auto sub = resultFromJson(
            j["status"]); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!sub) {
            return std::unexpected{sub.error()};
        }
        if (auto* st = std::get_if<Status>(&*sub)) {
            p.status = std::move(*st);
        } else {
            return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt,
                                                             "PlaylistLoaded status not a Status")};
        }
    }
    return p;
}

static auto parseResultVolumeInfo(const Json& j)
    -> std::expected<VolumeInfo, kanariv::utils::Error> {
    VolumeInfo vi{};
    if (j.contains("vol") &&
        j["vol"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        vi.vol =
            j["vol"]
                .get<
                    float>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("muted") &&
        j["muted"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        vi.muted =
            j["muted"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return vi;
}

static auto parseResultLibraryStats(const Json& j)
    -> std::expected<kanariv::types::LibraryStats, kanariv::utils::Error> {
    kanariv::types::LibraryStats ls{};
    if (j.contains("tracks") &&
        j["tracks"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.tracks =
            j["tracks"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("queues") &&
        j["queues"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.queues =
            j["queues"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("playlists") &&
        j["playlists"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.playlists =
            j["playlists"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("total_duration_ms") &&
        j["total_duration_ms"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.total_duration_ms =
            j["total_duration_ms"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("total_play_time_ms") &&
        j["total_play_time_ms"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.total_play_time_ms =
            j["total_play_time_ms"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("top_tracks") &&
        j["top_tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.top_tracks.emplace();
        const Json topTracks = j
            ["top_tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = topTracks.size(); ti < tn; ++ti) {
            auto jt = topTracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            ls.top_tracks->push_back(std::move(*tr));
        }
    }
    if (j.contains("top_albums") &&
        j["top_albums"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.top_albums.emplace();
        const Json topAlbums = j
            ["top_albums"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = topAlbums.size(); ti < tn; ++ti) {
            auto jt = topAlbums.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            const Json& jo = *jt;
            kanariv::types::TopEntity e;
            if (jo.contains("name") &&
                jo["name"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.name =
                    jo["name"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jo.contains("track_count") &&
                jo["track_count"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.track_count = static_cast<std::size_t>(
                    jo["track_count"]
                        .get<
                            int64_t>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jo.contains("total_plays") &&
                jo["total_plays"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.total_plays =
                    jo["total_plays"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            ls.top_albums->push_back(std::move(e));
        }
    }
    if (j.contains("top_genres") &&
        j["top_genres"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.top_genres.emplace();
        const Json topGenres = j
            ["top_genres"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = topGenres.size(); ti < tn; ++ti) {
            auto jt = topGenres.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            const Json& jo = *jt;
            kanariv::types::TopEntity e;
            if (jo.contains("name") &&
                jo["name"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.name =
                    jo["name"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jo.contains("track_count") &&
                jo["track_count"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.track_count = static_cast<std::size_t>(
                    jo["track_count"]
                        .get<
                            int64_t>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jo.contains("total_plays") &&
                jo["total_plays"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.total_plays =
                    jo["total_plays"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            ls.top_genres->push_back(std::move(e));
        }
    }
    if (j.contains("top_artists") &&
        j["top_artists"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ls.top_artists.emplace();
        const Json topArtists = j
            ["top_artists"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = topArtists.size(); ti < tn; ++ti) {
            auto jt = topArtists.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            const Json& jo = *jt;
            kanariv::types::TopEntity e;
            if (jo.contains("name") &&
                jo["name"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.name =
                    jo["name"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jo.contains("track_count") &&
                jo["track_count"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.track_count = static_cast<std::size_t>(
                    jo["track_count"]
                        .get<
                            int64_t>()); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jo.contains("total_plays") &&
                jo["total_plays"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.total_plays =
                    jo["total_plays"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            ls.top_artists->push_back(std::move(e));
        }
    }
    return ls;
}

static auto parseResultTracks(const Json& j) -> std::expected<Tracks, kanariv::utils::Error> {
    Tracks trs{};
    if (j.contains("tracks") &&
        j["tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json tracks =
            j["tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
            auto jt = tracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            trs.tracks.push_back(std::move(*tr));
        }
    }
    return trs;
}

static auto parseResultSearchResults(const Json& j)
    -> std::expected<SearchResults, kanariv::utils::Error> {
    SearchResults sr{};
    if (j.contains("query") &&
        j["query"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        sr.query =
            j["query"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("tracks") &&
        j["tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json tracks =
            j["tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
            auto jt = tracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            sr.tracks.push_back(std::move(*tr));
        }
    }
    return sr;
}

static auto parseResultScanReport(const Json& j)
    -> std::expected<ScanReport, kanariv::utils::Error> {
    ScanReport sr{};
    if (j.contains("added") &&
        j["added"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        sr.added =
            j["added"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return sr;
}

static auto parseResultPlaylists(const Json& j) -> std::expected<Playlists, kanariv::utils::Error> {
    Playlists pl{};
    if (j.contains("playlists") &&
        j["playlists"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json playlists =
            j["playlists"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t pi = 0, pn = playlists.size(); pi < pn; ++pi) {
            auto jp = playlists.at(pi);
            if (!jp) {
                return std::unexpected{jp.error()};
            }
            auto pr = detail::playlistFromJson(*jp);
            if (!pr) {
                return std::unexpected{pr.error()};
            }
            pl.playlists.push_back(std::move(*pr));
        }
    }
    return pl;
}

static auto parseResultPlaylistData(const Json& j)
    -> std::expected<PlaylistData, kanariv::utils::Error> {
    PlaylistData pd{};
    if (j.contains("tracks") &&
        j["tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json tracks =
            j["tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
            auto jt = tracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            pd.tracks.push_back(std::move(*tr));
        }
    }
    if (j.contains("format") &&
        j["format"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        pd.format =
            j["format"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return pd;
}

static auto parseResultPlaylistCreated(const Json& j)
    -> std::expected<PlaylistCreated, kanariv::utils::Error> {
    PlaylistCreated p{};
    if (j.contains("id") &&
        j["id"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p.id =
            j["id"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("name") &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        p.name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return p;
}

static auto parseResultPlaylistImportReport(const Json& j)
    -> std::expected<PlaylistImportReport, kanariv::utils::Error> {
    PlaylistImportReport r{};
    if (j.contains("pid") &&
        j["pid"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.pid =
            j["pid"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("name") &&
        j["name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.name =
            j["name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("tracks") &&
        j["tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json tracks =
            j["tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
            auto jt = tracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            r.tracks.push_back(std::move(*tr));
        }
    }
    if (j.contains("matched") &&
        j["matched"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.matched =
            j["matched"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("skipped") &&
        j["skipped"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.skipped =
            j["skipped"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("duplicates") &&
        j["duplicates"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.duplicates =
            j["duplicates"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return r;
}

static auto parseResultLibraryMaintenanceReport(const Json& j)
    -> std::expected<LibraryMaintenanceReport, kanariv::utils::Error> {
    LibraryMaintenanceReport r{};
    if (j.contains("tracks_checked") &&
        j["tracks_checked"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.tracks_checked =
            j["tracks_checked"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("missing_tracks") &&
        j["missing_tracks"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json tracks = j
            ["missing_tracks"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ti = 0, tn = tracks.size(); ti < tn; ++ti) {
            auto jt = tracks.at(ti);
            if (!jt) {
                return std::unexpected{jt.error()};
            }
            auto tr = kanariv::utils::trackFromJson(*jt);
            if (!tr) {
                return std::unexpected{tr.error()};
            }
            r.missing_tracks.push_back(std::move(*tr));
        }
    }
    if (j.contains("duplicate_groups") &&
        j["duplicate_groups"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.duplicate_groups =
            j["duplicate_groups"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("duplicate_tracks") &&
        j["duplicate_tracks"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        r.duplicate_tracks =
            j["duplicate_tracks"]
                .get<
                    std::
                        size_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return r;
}

static auto parseResultConfigValue(const Json& j)
    -> std::expected<ConfigValue, kanariv::utils::Error> {
    ConfigValue cv{};
    if (j.contains("key") &&
        j["key"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        cv.key =
            j["key"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("value") &&
        j["value"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        cv.value =
            j["value"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return cv;
}

static auto parseResultConfigValues(const Json& j)
    -> std::expected<ConfigValues, kanariv::utils::Error> {
    ConfigValues cvs{};
    if (j.contains("values") &&
        j["values"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json values =
            j["values"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t vi = 0, vn = values.size(); vi < vn; ++vi) {
            auto jvExp = values.at(vi);
            if (!jvExp) {
                return std::unexpected{jvExp.error()};
            }
            const Json jv = std::move(*jvExp);
            ConfigValue cv{};
            if (jv.contains("key") &&
                jv["key"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                cv.key =
                    jv["key"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jv.contains("value") &&
                jv["value"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                cv.value =
                    jv["value"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            cvs.values.push_back(std::move(cv));
        }
    }
    return cvs;
}

static auto parseResultSingleTrack(const Json& j)
    -> std::expected<SingleTrack, kanariv::utils::Error> {
    SingleTrack st{};
    if (j.contains("track")) {
        auto tr = kanariv::utils::trackFromJson(
            j["track"]); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!tr) {
            return std::unexpected{tr.error()};
        }
        st.track = std::move(*tr);
    } else if (j.contains("id")) {
        auto tr = kanariv::utils::trackFromJson(j);
        if (!tr) {
            return std::unexpected{tr.error()};
        }
        st.track = std::move(*tr);
    }
    if (j.contains("cover_mime") &&
        j["cover_mime"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        st.cover_mime =
            j["cover_mime"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("cover_data") &&
        j["cover_data"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        st.cover_data =
            j["cover_data"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return st;
}

static auto parseResultTrackInfo(const Json& j) -> std::expected<TrackInfo, kanariv::utils::Error> {
    TrackInfo ti{};
    if (j.contains("track")) {
        auto tr = kanariv::utils::trackFromJson(
            j["track"]); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (!tr) {
            return std::unexpected{tr.error()};
        }
        ti.track = std::move(*tr);
    }
    if (j.contains("play_count") &&
        j["play_count"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ti.play_count =
            j["play_count"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    if (j.contains("last_played") &&
        j["last_played"]
            .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ti.last_played =
            j["last_played"]
                .get<
                    int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return ti;
}

static auto parseResultHistory(const Json& j) -> std::expected<History, kanariv::utils::Error> {
    History h{};
    if (j.contains("entries") &&
        j["entries"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json entries =
            j["entries"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t ei = 0, en = entries.size(); ei < en; ++ei) {
            auto jeExp = entries.at(ei);
            if (!jeExp) {
                return std::unexpected{jeExp.error()};
            }
            const Json je = std::move(*jeExp);
            types::HistoryEntry e{};
            if (je.contains("id") &&
                je["id"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.id =
                    je["id"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("track_id") &&
                je["track_id"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.track_id =
                    je["track_id"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("started_at") &&
                je["started_at"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.started_at =
                    je["started_at"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("completed_at") &&
                je["completed_at"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.completed_at =
                    je["completed_at"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("position_ms") &&
                je["position_ms"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.position_ms =
                    je["position_ms"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("completion_pct") &&
                je["completion_pct"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.completion_pct =
                    je["completion_pct"]
                        .get<
                            double>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("queue_id") &&
                je["queue_id"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.queue_id =
                    je["queue_id"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("title") &&
                je["title"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.title =
                    je["title"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("artist") &&
                je["artist"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.artist =
                    je["artist"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("path") &&
                je["path"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.path =
                    je["path"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (je.contains("duration") &&
                je["duration"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.duration =
                    je["duration"]
                        .get<
                            double>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            h.entries.push_back(std::move(e));
        }
    }
    return h;
}

static auto parseResultBookmarks(const Json& j) -> std::expected<Bookmarks, kanariv::utils::Error> {
    Bookmarks b{};
    if (j.contains("entries") &&
        j["entries"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json entries =
            j["entries"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t bi = 0, bn = entries.size(); bi < bn; ++bi) {
            auto jbExp = entries.at(bi);
            if (!jbExp) {
                return std::unexpected{jbExp.error()};
            }
            const Json jb = std::move(*jbExp);
            types::Bookmark e{};
            if (jb.contains("id") &&
                jb["id"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.id =
                    jb["id"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jb.contains("track_id") &&
                jb["track_id"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.track_id =
                    jb["track_id"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jb.contains("position_ms") &&
                jb["position_ms"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.position_ms =
                    jb["position_ms"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jb.contains("note") &&
                jb["note"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.note =
                    jb["note"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jb.contains("created") &&
                jb["created"]
                    .isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                e.created =
                    jb["created"]
                        .get<
                            int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            b.entries.push_back(std::move(e));
        }
    }
    return b;
}

static auto parseResultDevices(const Json& j) -> std::expected<Devices, kanariv::utils::Error> {
    Devices d{};
    if (j.contains("devices") &&
        j["devices"]
            .isArray()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const Json devices =
            j["devices"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (std::size_t di_ = 0, dn = devices.size(); di_ < dn; ++di_) {
            auto jdExp = devices.at(di_);
            if (!jdExp) {
                return std::unexpected{jdExp.error()};
            }
            const Json jd = std::move(*jdExp);
            types::DeviceInfo di{};
            if (jd.contains("id") &&
                jd["id"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                di.id =
                    jd["id"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jd.contains("name") &&
                jd["name"]
                    .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                di.name =
                    jd["name"]
                        .get<
                            std::
                                string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            if (jd.contains("isDefault") &&
                jd["isDefault"]
                    .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                di.isDefault =
                    jd["isDefault"]
                        .get<
                            bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            }
            d.devices.push_back(std::move(di));
        }
    }
    return d;
}

constexpr uint32_t frameLenShift3 = 24;
constexpr uint32_t frameLenShift2 = 16;
constexpr uint32_t frameLenShift1 = 8;
constexpr uint32_t frameLenMask = 0xFF;

auto frame(std::string_view json) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(4 + json.size());
    auto len = static_cast<uint32_t>(json.size());
    out.push_back(static_cast<std::byte>((len >> frameLenShift3) & frameLenMask));
    out.push_back(static_cast<std::byte>((len >> frameLenShift2) & frameLenMask));
    out.push_back(static_cast<std::byte>((len >> frameLenShift1) & frameLenMask));
    out.push_back(static_cast<std::byte>(len & frameLenMask));
    const auto* bytes = reinterpret_cast<const std::byte*>(json.data());
    out.insert(out.end(), bytes, bytes + json.size());
    return out;
}

auto deframe(std::span<const std::byte> buf) -> std::expected<std::string, kanariv::utils::Error> {
    if (buf.size() < 4) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame too short")};
    }
    uint32_t len =
        (static_cast<uint32_t>(std::to_integer<unsigned char>(buf[0]))
         << frameLenShift3) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        (static_cast<uint32_t>(std::to_integer<unsigned char>(buf[1]))
         << frameLenShift2) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        (static_cast<uint32_t>(std::to_integer<unsigned char>(buf[2]))
         << frameLenShift1) | // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        static_cast<uint32_t>(std::to_integer<unsigned char>(
            buf[frameByteIndex3])); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (buf.size() < 4 + len) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "frame incomplete")};
    }
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(std::to_integer<unsigned char>(
            buf[4 + i]))); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    return s;
}

auto toJsonString(const Result& r) -> std::string {
    return toJson(r).dump(2);
}

auto toJsonString(const Command& c) -> std::string {
    return toJson(c).dump(2);
}

} // namespace kanariv::ipc
