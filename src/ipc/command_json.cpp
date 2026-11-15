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

namespace kanariv::ipc {

// Defined after the serializer; the schema table below needs it by address.
template <CommandAlternative T>
std::expected<T, kanariv::utils::Error> parseCommandPayload(const Json& j);

// Dependent-false so a missing branch fails to compile only when instantiated.
// Kept out of the anonymous namespace: nothing odr-uses it when coverage is
// complete, and an internal-linkage template would be flagged unused.
template <typename>
inline constexpr bool kAlwaysFalse = false;

namespace {

/// Compile-time index of `T` in `Variant`; equals the variant size when the
/// type is absent, so the schema assertion below reports the missing row.
template <typename T, typename Variant, std::size_t I = 0>
constexpr std::size_t variantIndexOf() {
    if constexpr (I == std::variant_size_v<Variant>)
        return I;
    else if constexpr (std::is_same_v<std::variant_alternative_t<I, Variant>, T>)
        return I;
    else
        return variantIndexOf<T, Variant, I + 1>();
}

struct CommandSchema {
    std::string_view name;
    std::size_t index;
    std::expected<Command, kanariv::utils::Error> (*parse)(const Json&);
};

template <CommandAlternative T>
std::expected<Command, kanariv::utils::Error> parseCommandSchema(const Json& j) {
    auto parsed = parseCommandPayload<T>(j);
    if (!parsed)
        return std::unexpected{std::move(parsed.error())};
    return Command{std::move(*parsed)};
}

template <CommandAlternative T>
constexpr CommandSchema commandSchema(std::string_view name) {
    return CommandSchema{name, variantIndexOf<T, Command>(), &parseCommandSchema<T>};
}

/// One row per Command alternative, in variant order. The assertions below turn
/// a missing row, a reordered row or a duplicate wire name into a compile
/// error instead of a runtime "unknown Command type".
constexpr std::array kCommandSchema{
    commandSchema<Play>("Play"),
    commandSchema<Pause>("Pause"),
    commandSchema<Resume>("Resume"),
    commandSchema<Restart>("Restart"),
    commandSchema<Stop>("Stop"),
    commandSchema<Next>("Next"),
    commandSchema<Prev>("Prev"),
    commandSchema<Seek>("Seek"),
    commandSchema<SeekBy>("SeekBy"),
    commandSchema<StatusReq>("StatusReq"),
    commandSchema<VolumeSet>("VolumeSet"),
    commandSchema<QueueList>("QueueList"),
    commandSchema<QueueQueues>("QueueQueues"),
    commandSchema<QueueCreate>("QueueCreate"),
    commandSchema<QueueDelete>("QueueDelete"),
    commandSchema<PlayFiles>("PlayFiles"),
    commandSchema<QueueSwitch>("QueueSwitch"),
    commandSchema<QueueAdd>("QueueAdd"),
    commandSchema<QueueRemove>("QueueRemove"),
    commandSchema<QueueMove>("QueueMove"),
    commandSchema<QueueClear>("QueueClear"),
    commandSchema<QueueShuffle>("QueueShuffle"),
    commandSchema<QueueRepeat>("QueueRepeat"),
    commandSchema<PlaylistList>("PlaylistList"),
    commandSchema<PlaylistCreate>("PlaylistCreate"),
    commandSchema<PlaylistAdd>("PlaylistAdd"),
    commandSchema<PlaylistTracks>("PlaylistTracks"),
    commandSchema<PlaylistLoad>("PlaylistLoad"),
    commandSchema<PlaylistSave>("PlaylistSave"),
    commandSchema<PlaylistDelete>("PlaylistDelete"),
    commandSchema<PlaylistRename>("PlaylistRename"),
    commandSchema<PlaylistExport>("PlaylistExport"),
    commandSchema<PlaylistImport>("PlaylistImport"),
    commandSchema<LibraryScan>("LibraryScan"),
    commandSchema<LibrarySearch>("LibrarySearch"),
    commandSchema<LibraryStats>("LibraryStats"),
    commandSchema<LibraryAdd>("LibraryAdd"),
    commandSchema<LibraryRemove>("LibraryRemove"),
    commandSchema<LibraryList>("LibraryList"),
    commandSchema<TagEdit>("TagEdit"),
    commandSchema<TagGet>("TagGet"),
    commandSchema<ConfigGet>("ConfigGet"),
    commandSchema<ConfigSet>("ConfigSet"),
    commandSchema<ConfigList>("ConfigList"),
    commandSchema<ConfigExport>("ConfigExport"),
    commandSchema<ConfigImport>("ConfigImport"),
    commandSchema<ConfigReset>("ConfigReset"),
    commandSchema<HistoryList>("HistoryList"),
    commandSchema<HistoryClear>("HistoryClear"),
    commandSchema<BookmarkList>("BookmarkList"),
    commandSchema<BookmarkAdd>("BookmarkAdd"),
    commandSchema<BookmarkRemove>("BookmarkRemove"),
    commandSchema<BookmarkGoto>("BookmarkGoto"),
    commandSchema<LyricsGet>("LyricsGet"),
    commandSchema<LyricsSet>("LyricsSet"),
    commandSchema<LyricsRemove>("LyricsRemove"),
    commandSchema<Shutdown>("Shutdown"),
    commandSchema<Preview>("Preview"),
    commandSchema<DeviceList>("DeviceList"),
    commandSchema<DeviceSet>("DeviceSet"),
    commandSchema<DeviceTest>("DeviceTest"),
    commandSchema<Info>("Info"),
    commandSchema<TrackRate>("TrackRate"),
    commandSchema<LibraryMaintenance>("LibraryMaintenance"),
    commandSchema<StatusWait>("StatusWait"),
    commandSchema<TagBatchEdit>("TagBatchEdit"),
};

static_assert(kCommandSchema.size() == std::variant_size_v<Command>,
              "every Command alternative needs a kCommandSchema row");

template <std::size_t... I>
constexpr bool schemaRowsMatchVariant(std::index_sequence<I...>) {
    return ((kCommandSchema[I].index == I) && ...);
}
static_assert(schemaRowsMatchVariant(std::make_index_sequence<kCommandSchema.size()>{}),
              "kCommandSchema rows must sit at their alternative's index");

constexpr bool schemaNamesUnique() {
    for (std::size_t i = 0; i < kCommandSchema.size(); ++i) {
        if (kCommandSchema[i].name.empty())
            return false;
        for (std::size_t k = i + 1; k < kCommandSchema.size(); ++k)
            if (kCommandSchema[i].name == kCommandSchema[k].name)
                return false;
    }
    return true;
}
static_assert(schemaNamesUnique(), "command wire names must be unique and non-empty");

/// Pre-registry name still accepted from old clients: same payload as
/// LibraryStats, but an absent `top_tracks` defaults to 10.
inline constexpr std::string_view kLibraryStatsDetailedAlias = "LibraryStatsDetailed";

} // namespace

Json toJson(const Command& cmd) {
    Json j;
    // Wire names have exactly one source (kCommandSchema); the visitor below
    // only fills payload fields.
    j["type"] = kCommandSchema[cmd.index()].name;
    std::visit(
        [&j](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Play>) {
            } else if constexpr (std::is_same_v<T, Pause>) {
            } else if constexpr (std::is_same_v<T, Resume>) {
            } else if constexpr (std::is_same_v<T, Restart>) {
            } else if constexpr (std::is_same_v<T, Stop>) {
            } else if constexpr (std::is_same_v<T, Next>) {
            } else if constexpr (std::is_same_v<T, Prev>) {
            } else if constexpr (std::is_same_v<T, Seek>) {
                j["seconds"] = v.seconds;
            } else if constexpr (std::is_same_v<T, SeekBy>) {
                j["delta"] = v.delta;
            } else if constexpr (std::is_same_v<T, StatusReq>) {
            } else if constexpr (std::is_same_v<T, StatusWait>) {
                j["timeout_ms"] = v.timeout_ms;
            } else if constexpr (std::is_same_v<T, VolumeSet>) {
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
                j["order"] = v.order;
            } else if constexpr (std::is_same_v<T, QueueQueues>) {
            } else if constexpr (std::is_same_v<T, QueueCreate>) {
                j["name"] = v.name;
            } else if constexpr (std::is_same_v<T, QueueDelete>) {
                j["qid"] = v.qid;
            } else if constexpr (std::is_same_v<T, PlayFiles>) {
                j["paths"] = Json::array();
                for (const auto& p : v.paths) {
                    Json e;
                    e = p;
                    j["paths"].push_back(e);
                }
                j["save"] = v.save;
            } else if constexpr (std::is_same_v<T, QueueSwitch>) {
                j["qid"] = v.qid;
            } else if constexpr (std::is_same_v<T, QueueAdd>) {
                j["query"] = v.query;
                j["search"] = v.search;
            } else if constexpr (std::is_same_v<T, QueueRemove>) {
                j["idOrIndex"] = v.idOrIndex;
            } else if constexpr (std::is_same_v<T, QueueMove>) {
                j["from"] = v.from;
                j["to"] = v.to;
            } else if constexpr (std::is_same_v<T, QueueClear>) {
            } else if constexpr (std::is_same_v<T, QueueShuffle>) {
                if (v.on.has_value())
                    j["on"] = *v.on;
                else
                    j["on"] = nullptr;
            } else if constexpr (std::is_same_v<T, QueueRepeat>) {
                if (v.mode.has_value())
                    j["mode"] = std::format("{}", *v.mode);
                else
                    j["mode"] = nullptr;
            } else if constexpr (std::is_same_v<T, PlaylistList>) {
            } else if constexpr (std::is_same_v<T, PlaylistTracks>) {
                j["pid"] = v.pid;
            } else if constexpr (std::is_same_v<T, PlaylistCreate>) {
                j["name"] = v.name;
                j["smart_query"] = v.smart_query;
            } else if constexpr (std::is_same_v<T, PlaylistAdd>) {
                j["pid"] = v.pid;
                j["track_id"] = v.track_id;
            } else if constexpr (std::is_same_v<T, PlaylistLoad>) {
                j["pid"] = v.pid;
                j["play"] = v.play;
                j["replace"] = v.replace;
            } else if constexpr (std::is_same_v<T, PlaylistSave>) {
                j["name"] = v.name;
                if (v.queue_id.has_value())
                    j["queue_id"] = *v.queue_id;
                else
                    j["queue_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
                j["pid"] = v.pid;
            } else if constexpr (std::is_same_v<T, PlaylistRename>) {
                j["pid"] = v.pid;
                j["newName"] = v.newName;
            } else if constexpr (std::is_same_v<T, PlaylistExport>) {
                j["pid"] = v.pid;
                j["path"] = v.path;
                j["format"] = v.format;
            } else if constexpr (std::is_same_v<T, PlaylistImport>) {
                j["path"] = v.path;
                if (v.name.has_value())
                    j["name"] = *v.name;
                else
                    j["name"] = nullptr;
            } else if constexpr (std::is_same_v<T, LibraryScan>) {
                if (v.path.has_value())
                    j["path"] = *v.path;
                else
                    j["path"] = nullptr;
                j["full_hash"] = v.full_hash;
            } else if constexpr (std::is_same_v<T, LibrarySearch>) {
                j["query"] = v.query;
                j["limit"] = v.limit;
            } else if constexpr (std::is_same_v<T, LibraryStats>) {
                if (v.top_tracks)
                    j["top_tracks"] = *v.top_tracks;
                if (v.top_albums)
                    j["top_albums"] = *v.top_albums;
                if (v.top_genres)
                    j["top_genres"] = *v.top_genres;
                if (v.top_artists)
                    j["top_artists"] = *v.top_artists;
            } else if constexpr (std::is_same_v<T, LibraryAdd>) {
                j["path"] = v.path;
                j["recursive"] = v.recursive;
            } else if constexpr (std::is_same_v<T, LibraryRemove>) {
                j["query"] = v.query;
            } else if constexpr (std::is_same_v<T, LibraryList>) {
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
                j["action"] = v.action;
                j["dry_run"] = v.dry_run;
                j["library_id"] = v.library_id;
            } else if constexpr (std::is_same_v<T, TagEdit>) {
                j["id"] = v.id;
                j["field"] = v.field;
                j["value"] = v.value;
            } else if constexpr (std::is_same_v<T, TagGet>) {
                if (v.id.has_value())
                    j["id"] = *v.id;
                if (v.path.has_value())
                    j["path"] = *v.path;
                j["cover"] = v.cover;
            } else if constexpr (std::is_same_v<T, TagBatchEdit>) {
                j["dry_run"] = v.dry_run;
                j["entries"] = Json::array();
                for (const auto& e : v.entries) {
                    Json je = Json::object();
                    if (e.id.has_value())
                        je["id"] = *e.id;
                    if (e.path.has_value())
                        je["path"] = *e.path;
                    je["field"] = e.field;
                    je["value"] = e.value;
                    j["entries"].push_back(std::move(je));
                }
            } else if constexpr (std::is_same_v<T, ConfigGet>) {
                j["key"] = v.key;
            } else if constexpr (std::is_same_v<T, ConfigSet>) {
                j["key"] = v.key;
                j["value"] = v.value;
            } else if constexpr (std::is_same_v<T, ConfigList>) {
            } else if constexpr (std::is_same_v<T, ConfigExport>) {
                j["path"] = v.path;
            } else if constexpr (std::is_same_v<T, ConfigImport>) {
                j["path"] = v.path;
            } else if constexpr (std::is_same_v<T, ConfigReset>) {
                if (v.key.has_value())
                    j["key"] = *v.key;
                else
                    j["key"] = nullptr;
            } else if constexpr (std::is_same_v<T, HistoryList>) {
                if (v.limit.has_value())
                    j["limit"] = *v.limit;
                else
                    j["limit"] = nullptr;
            } else if constexpr (std::is_same_v<T, HistoryClear>) {
            } else if constexpr (std::is_same_v<T, BookmarkList>) {
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, BookmarkAdd>) {
                j["position_ms"] = v.position_ms;
                j["note"] = v.note;
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, BookmarkRemove>) {
                j["id"] = v.id;
            } else if constexpr (std::is_same_v<T, BookmarkGoto>) {
                j["id"] = v.id;
            } else if constexpr (std::is_same_v<T, LyricsGet>) {
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, LyricsSet>) {
                j["lrc_text"] = v.lrc_text;
                j["is_synced"] = v.is_synced;
                j["source"] = v.source;
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, LyricsRemove>) {
                if (v.track_id.has_value())
                    j["track_id"] = *v.track_id;
                else
                    j["track_id"] = nullptr;
            } else if constexpr (std::is_same_v<T, Shutdown>) {
            } else if constexpr (std::is_same_v<T, Preview>) {
                j["file"] = v.file;
            } else if constexpr (std::is_same_v<T, DeviceList>) {
            } else if constexpr (std::is_same_v<T, DeviceSet>) {
                j["id"] = v.id;
            } else if constexpr (std::is_same_v<T, DeviceTest>) {
                if (v.id.has_value())
                    j["id"] = *v.id;
                else
                    j["id"] = nullptr;
                j["tone"] = v.tone;
            } else if constexpr (std::is_same_v<T, Info>) {
            } else if constexpr (std::is_same_v<T, TrackRate>) {
                j["track_id"] = v.track_id;
                j["rating"] = v.rating;
            } else {
                static_assert(kAlwaysFalse<T>, "Command alternative has no JSON body");
            }
        },
        cmd);
    return j;
}

template <CommandAlternative T>
std::expected<T, kanariv::utils::Error> parseCommandPayload(const Json& j) {
    try {
        if constexpr (std::is_same_v<T, Play>)
            return Play{};
        else if constexpr (std::is_same_v<T, Pause>)
            return Pause{};
        else if constexpr (std::is_same_v<T, Resume>)
            return Resume{};
        else if constexpr (std::is_same_v<T, Restart>)
            return Restart{};
        else if constexpr (std::is_same_v<T, Stop>)
            return Stop{};
        else if constexpr (std::is_same_v<T, Next>)
            return Next{};
        else if constexpr (std::is_same_v<T, Prev>)
            return Prev{};
        else if constexpr (std::is_same_v<T, Seek>) {
            double sec = 0;
            if (j.contains("seconds") && j["seconds"].isNumber())
                sec = j["seconds"].get<double>();
            return Seek{sec};
        } else if constexpr (std::is_same_v<T, SeekBy>) {
            double delta = 0;
            if (j.contains("delta") && j["delta"].isNumber())
                delta = j["delta"].get<double>();
            return SeekBy{delta};
        } else if constexpr (std::is_same_v<T, StatusReq>)
            return StatusReq{};
        else if constexpr (std::is_same_v<T, StatusWait>) {
            StatusWait sw{};
            if (j.contains("timeout_ms") && j["timeout_ms"].isNumber())
                sw.timeout_ms = j["timeout_ms"].get<int>();
            return std::move(sw);
        } else if constexpr (std::is_same_v<T, VolumeSet>) {
            VolumeSet v{};
            if (j.contains("level") && !j["level"].isNull() && j["level"].isNumber())
                v.level = j["level"].get<float>();
            if (j.contains("mute") && !j["mute"].isNull() && j["mute"].isBoolean())
                v.mute = j["mute"].get<bool>();
            if (j.contains("deltaPct") && !j["deltaPct"].isNull() && j["deltaPct"].isNumber())
                v.deltaPct = j["deltaPct"].get<int>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, QueueList>) {
            QueueList q{};
            if (j.contains("order") && j["order"].isString())
                q.order = j["order"].get<std::string>();
            return std::move(q);
        } else if constexpr (std::is_same_v<T, QueueQueues>)
            return QueueQueues{};
        else if constexpr (std::is_same_v<T, QueueCreate>) {
            QueueCreate q{};
            if (j.contains("name") && j["name"].isString())
                q.name = j["name"].get<std::string>();
            return std::move(q);
        } else if constexpr (std::is_same_v<T, QueueDelete>) {
            // Never fall back to queue 1 on a missing field: a malformed frame
            // must not delete a queue the client did not name.
            if (!j.contains("qid") || !j["qid"].isNumber()) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "QueueDelete requires numeric qid")};
            }
            return QueueDelete{j["qid"].get<int64_t>()};
        } else if constexpr (std::is_same_v<T, PlayFiles>) {
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
            return std::move(p);
        } else if constexpr (std::is_same_v<T, QueueSwitch>) {
            // Same fail-closed rule as QueueDelete: `qid` is mandatory.
            if (!j.contains("qid") || !j["qid"].isNumber()) {
                return std::unexpected{kanariv::utils::makeError(
                    kanariv::utils::StatusCode::InvalidArg, "QueueSwitch requires numeric qid")};
            }
            return QueueSwitch{j["qid"].get<int64_t>()};
        } else if constexpr (std::is_same_v<T, QueueAdd>) {
            std::string q;
            bool search = false;
            if (j.contains("query") && j["query"].isString())
                q = j["query"].get<std::string>();
            if (j.contains("search") && j["search"].isBoolean())
                search = j["search"].get<bool>();
            return QueueAdd{std::move(q), search};
        } else if constexpr (std::is_same_v<T, QueueRemove>) {
            std::string id;
            if (j.contains("idOrIndex") && j["idOrIndex"].isString())
                id = j["idOrIndex"].get<std::string>();
            return QueueRemove{std::move(id)};
        } else if constexpr (std::is_same_v<T, QueueMove>) {
            std::size_t from = 0, to = 0;
            if (j.contains("from") && j["from"].isNumber())
                from = j["from"].get<std::size_t>();
            if (j.contains("to") && j["to"].isNumber())
                to = j["to"].get<std::size_t>();
            return QueueMove{from, to};
        } else if constexpr (std::is_same_v<T, QueueClear>)
            return QueueClear{};
        else if constexpr (std::is_same_v<T, QueueShuffle>) {
            QueueShuffle v{};
            if (j.contains("on") && !j["on"].isNull() && j["on"].isBoolean())
                v.on = j["on"].get<bool>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, QueueRepeat>) {
            QueueRepeat v{};
            if (j.contains("mode") && !j["mode"].isNull() && j["mode"].isString()) {
                auto m = kanariv::types::parseRepeatMode(j["mode"].get<std::string>());
                if (!m)
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg, "unknown RepeatMode")};
                v.mode = *m;
            }
            return std::move(v);
        } else if constexpr (std::is_same_v<T, PlaylistList>)
            return PlaylistList{};
        else if constexpr (std::is_same_v<T, PlaylistTracks>) {
            int64_t pid = 0;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            return PlaylistTracks{pid};
        } else if constexpr (std::is_same_v<T, PlaylistCreate>) {
            PlaylistCreate pc;
            if (j.contains("name") && j["name"].isString())
                pc.name = j["name"].get<std::string>();
            if (j.contains("smart_query") && j["smart_query"].isString())
                pc.smart_query = j["smart_query"].get<std::string>();
            return std::move(pc);
        } else if constexpr (std::is_same_v<T, PlaylistAdd>) {
            int64_t pid = 0;
            int64_t tid = 0;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("track_id") && j["track_id"].isNumber())
                tid = j["track_id"].get<int64_t>();
            return PlaylistAdd{pid, tid};
        } else if constexpr (std::is_same_v<T, PlaylistLoad>) {
            int64_t pid = 0;
            bool play = false;
            bool replace = false;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("play") && j["play"].isBoolean())
                play = j["play"].get<bool>();
            if (j.contains("replace") && j["replace"].isBoolean())
                replace = j["replace"].get<bool>();
            return PlaylistLoad{pid, play, replace};
        } else if constexpr (std::is_same_v<T, PlaylistSave>) {
            std::string name;
            std::optional<int64_t> qid;
            if (j.contains("name") && j["name"].isString())
                name = j["name"].get<std::string>();
            if (j.contains("queue_id") && !j["queue_id"].isNull() && j["queue_id"].isNumber())
                qid = j["queue_id"].get<int64_t>();
            return PlaylistSave{std::move(name), qid};
        } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
            int64_t pid = 0;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            return PlaylistDelete{pid};
        } else if constexpr (std::is_same_v<T, PlaylistRename>) {
            int64_t pid = 0;
            std::string newName;
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("newName") && j["newName"].isString())
                newName = j["newName"].get<std::string>();
            return PlaylistRename{pid, std::move(newName)};
        } else if constexpr (std::is_same_v<T, PlaylistExport>) {
            int64_t pid = 0;
            std::string path;
            std::string format = "m3u";
            if (j.contains("pid") && j["pid"].isNumber())
                pid = j["pid"].get<int64_t>();
            if (j.contains("path") && j["path"].isString())
                path = j["path"].get<std::string>();
            if (j.contains("format") && j["format"].isString())
                format = j["format"].get<std::string>();
            return PlaylistExport{pid, std::move(path), std::move(format)};
        } else if constexpr (std::is_same_v<T, PlaylistImport>) {
            std::string path;
            std::optional<std::string> name;
            if (j.contains("path") && j["path"].isString())
                path = j["path"].get<std::string>();
            if (j.contains("name") && !j["name"].isNull() && j["name"].isString())
                name = j["name"].get<std::string>();
            return PlaylistImport{std::move(path), name};
        } else if constexpr (std::is_same_v<T, LibraryScan>) {
            LibraryScan v{};
            if (j.contains("path") && !j["path"].isNull() && j["path"].isString())
                v.path = j["path"].get<std::string>();
            if (j.contains("full_hash") && j["full_hash"].isBoolean())
                v.full_hash = j["full_hash"].get<bool>();
            else if (j.contains("mode") && j["mode"].isString())
                v.full_hash = (j["mode"].get<std::string>() == "full");
            return std::move(v);
        } else if constexpr (std::is_same_v<T, LibrarySearch>) {
            std::string q;
            int lim = 50;
            if (j.contains("query") && j["query"].isString())
                q = j["query"].get<std::string>();
            if (j.contains("limit") && j["limit"].isNumber())
                lim = j["limit"].get<int>();
            return LibrarySearch{std::move(q), lim};
        } else if constexpr (std::is_same_v<T, LibraryStats>) {
            LibraryStats v{};
            if (j.contains("top_tracks") && j["top_tracks"].isNumber())
                v.top_tracks = j["top_tracks"].get<int>();
            if (j.contains("top_albums") && j["top_albums"].isNumber())
                v.top_albums = j["top_albums"].get<int>();
            if (j.contains("top_genres") && j["top_genres"].isNumber())
                v.top_genres = j["top_genres"].get<int>();
            if (j.contains("top_artists") && j["top_artists"].isNumber())
                v.top_artists = j["top_artists"].get<int>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, LibraryAdd>) {
            std::string p;
            bool rec = false;
            if (j.contains("path") && j["path"].isString())
                p = j["path"].get<std::string>();
            if (j.contains("recursive") && j["recursive"].isBoolean())
                rec = j["recursive"].get<bool>();
            return LibraryAdd{std::move(p), rec};
        } else if constexpr (std::is_same_v<T, LibraryRemove>) {
            std::string q;
            if (j.contains("query") && j["query"].isString())
                q = j["query"].get<std::string>();
            else if (j.contains("id") && j["id"].isString())
                q = j["id"].get<std::string>();
            else if (j.contains("id") && j["id"].isNumber())
                q = std::to_string(j["id"].get<int64_t>());
            return LibraryRemove{std::move(q)};
        } else if constexpr (std::is_same_v<T, LibraryList>) {
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
            return std::move(v);
        } else if constexpr (std::is_same_v<T, LibraryMaintenance>) {
            LibraryMaintenance v{};
            if (j.contains("action") && j["action"].isString())
                v.action = j["action"].get<std::string>();
            if (j.contains("dry_run") && j["dry_run"].isBoolean())
                v.dry_run = j["dry_run"].get<bool>();
            if (j.contains("library_id") && j["library_id"].isNumber())
                v.library_id = j["library_id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, TagEdit>) {
            int64_t id = 0;
            std::string field, value;
            if (j.contains("id") && j["id"].isNumber())
                id = j["id"].get<int64_t>();
            if (j.contains("field") && j["field"].isString())
                field = j["field"].get<std::string>();
            if (j.contains("value") && j["value"].isString())
                value = j["value"].get<std::string>();
            return TagEdit{id, std::move(field), std::move(value)};
        } else if constexpr (std::is_same_v<T, TagGet>) {
            TagGet tg{};
            if (j.contains("id") && j["id"].isNumber())
                tg.id = j["id"].get<int64_t>();
            if (j.contains("path") && j["path"].isString())
                tg.path = j["path"].get<std::string>();
            if (j.contains("cover") && j["cover"].isBoolean())
                tg.cover = j["cover"].get<bool>();
            return std::move(tg);
        } else if constexpr (std::is_same_v<T, TagBatchEdit>) {
            TagBatchEdit tbe{};
            if (j.contains("dry_run") && j["dry_run"].isBoolean())
                tbe.dry_run = j["dry_run"].get<bool>();
            if (j.contains("entries") && j["entries"].isArray()) {
                const Json arr = j["entries"];
                for (std::size_t ei = 0, en = arr.size(); ei < en; ++ei) {
                    auto jeExp = arr.at(ei);
                    if (!jeExp)
                        return std::unexpected{jeExp.error()};
                    const Json je = std::move(*jeExp);
                    TagBatchEditEntry e{};
                    if (je.contains("id") && je["id"].isNumber())
                        e.id = je["id"].get<int64_t>();
                    if (je.contains("path") && je["path"].isString())
                        e.path = je["path"].get<std::string>();
                    if (je.contains("field") && je["field"].isString())
                        e.field = je["field"].get<std::string>();
                    if (je.contains("value") && je["value"].isString())
                        e.value = je["value"].get<std::string>();
                    tbe.entries.push_back(std::move(e));
                }
            }
            return std::move(tbe);
        } else if constexpr (std::is_same_v<T, ConfigGet>) {
            std::string k;
            if (j.contains("key") && j["key"].isString())
                k = j["key"].get<std::string>();
            return ConfigGet{std::move(k)};
        } else if constexpr (std::is_same_v<T, ConfigSet>) {
            std::string k, v;
            if (j.contains("key") && j["key"].isString())
                k = j["key"].get<std::string>();
            if (j.contains("value") && j["value"].isString())
                v = j["value"].get<std::string>();
            return ConfigSet{std::move(k), std::move(v)};
        } else if constexpr (std::is_same_v<T, ConfigList>)
            return ConfigList{};
        else if constexpr (std::is_same_v<T, ConfigExport>) {
            std::string p;
            if (j.contains("path") && j["path"].isString())
                p = j["path"].get<std::string>();
            return ConfigExport{std::move(p)};
        } else if constexpr (std::is_same_v<T, ConfigImport>) {
            std::string p;
            if (j.contains("path") && j["path"].isString())
                p = j["path"].get<std::string>();
            return ConfigImport{std::move(p)};
        } else if constexpr (std::is_same_v<T, ConfigReset>) {
            std::optional<std::string> k;
            if (j.contains("key") && !j["key"].isNull() && j["key"].isString())
                k = j["key"].get<std::string>();
            return ConfigReset{std::move(k)};
        } else if constexpr (std::is_same_v<T, HistoryList>) {
            HistoryList v{};
            if (j.contains("limit") && !j["limit"].isNull() && j["limit"].isNumber())
                v.limit = j["limit"].get<int>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, HistoryClear>) {
            return HistoryClear{};
        } else if constexpr (std::is_same_v<T, BookmarkList>) {
            BookmarkList v{};
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, BookmarkAdd>) {
            BookmarkAdd v{};
            if (j.contains("position_ms") && j["position_ms"].isNumber())
                v.position_ms = j["position_ms"].get<int64_t>();
            if (j.contains("note") && j["note"].isString())
                v.note = j["note"].get<std::string>();
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, BookmarkRemove>) {
            BookmarkRemove v{};
            if (j.contains("id") && j["id"].isNumber())
                v.id = j["id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, BookmarkGoto>) {
            BookmarkGoto v{};
            if (j.contains("id") && j["id"].isNumber())
                v.id = j["id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, LyricsGet>) {
            LyricsGet v{};
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, LyricsSet>) {
            LyricsSet v{};
            if (j.contains("lrc_text") && j["lrc_text"].isString())
                v.lrc_text = j["lrc_text"].get<std::string>();
            if (j.contains("is_synced") && j["is_synced"].isBoolean())
                v.is_synced = j["is_synced"].get<bool>();
            if (j.contains("source") && j["source"].isString())
                v.source = j["source"].get<std::string>();
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, LyricsRemove>) {
            LyricsRemove v{};
            if (j.contains("track_id") && !j["track_id"].isNull() && j["track_id"].isNumber())
                v.track_id = j["track_id"].get<int64_t>();
            return std::move(v);
        } else if constexpr (std::is_same_v<T, Shutdown>)
            return Shutdown{};
        else if constexpr (std::is_same_v<T, Preview>) {
            std::string f;
            if (j.contains("file") && j["file"].isString())
                f = j["file"].get<std::string>();
            return Preview{std::move(f)};
        } else if constexpr (std::is_same_v<T, DeviceList>)
            return DeviceList{};
        else if constexpr (std::is_same_v<T, DeviceSet>) {
            std::string id;
            if (j.contains("id") && j["id"].isString())
                id = j["id"].get<std::string>();
            return DeviceSet{std::move(id)};
        } else if constexpr (std::is_same_v<T, DeviceTest>) {
            std::optional<std::string> id;
            if (j.contains("id") && !j["id"].isNull() && j["id"].isString())
                id = j["id"].get<std::string>();
            DeviceTest dt{std::move(id)};
            if (j.contains("tone") && j["tone"].isBoolean())
                dt.tone = j["tone"].get<bool>();
            return std::move(dt);
        } else if constexpr (std::is_same_v<T, Info>)
            return Info{};
        else if constexpr (std::is_same_v<T, TrackRate>) {
            TrackRate tr{};
            if (j.contains("track_id") && j["track_id"].isNumber())
                tr.track_id = j["track_id"].get<int64_t>();
            if (j.contains("rating") && j["rating"].isNumber())
                tr.rating = j["rating"].get<int32_t>();
            return std::move(tr);
        } else {
            static_assert(kAlwaysFalse<T>, "Command alternative has no JSON parser");
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, "no parser")};
        }
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::expected<Command, kanariv::utils::Error> commandFromJson(const Json& j) {
    try {
        if (!j.contains("type") || !j["type"].isString()) {
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg, "missing type")};
        }
        const std::string t = j["type"].get<std::string>();
        if (t == kLibraryStatsDetailedAlias) {
            auto detailed = parseCommandPayload<LibraryStats>(j);
            if (!detailed)
                return std::unexpected{std::move(detailed.error())};
            if (!(j.contains("top_tracks") && j["top_tracks"].isNumber()))
                detailed->top_tracks = 10;
            return Command{std::move(*detailed)};
        }
        for (const auto& row : kCommandSchema)
            if (row.name == t)
                return row.parse(j);
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "unknown Command type: " + t)};
    } catch (const std::exception& e) {
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::Corrupt, e.what())};
    }
}

std::string toJsonString(const Command& c) {
    return toJson(c).dump(2);
}
} // namespace kanariv::ipc
