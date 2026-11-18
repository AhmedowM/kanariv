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

// Defined after the serializer; the schema table below needs it by address.
template <CommandAlternative T>
std::expected<T, kanariv::utils::Error> parseCommandPayload(const Json& j);

// Dependent-false so a missing branch fails to compile only when instantiated.
// Kept out of the anonymous namespace: nothing odr-uses it when coverage is
// complete, and an internal-linkage template would be flagged unused.
template <typename>
inline constexpr bool alwaysFalse = false;

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
    TransportPolicy policy;
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
constexpr CommandSchema commandSchema(std::string_view name,
                                      TransportPolicy policy = TransportPolicy::Interactive) {
    return CommandSchema{name, variantIndexOf<T, Command>(), policy, &parseCommandSchema<T>};
}

/// One row per Command alternative, in variant order. The assertions below turn
/// a missing row, a reordered row or a duplicate wire name into a compile
/// error instead of a runtime "unknown Command type".
constexpr std::array commandSchemas{
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
    commandSchema<QueueAddBatch>("QueueAddBatch", TransportPolicy::Batch),
    commandSchema<QueueRemove>("QueueRemove"),
    commandSchema<QueueRemoveBatch>("QueueRemoveBatch", TransportPolicy::Batch),
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
    commandSchema<PlaylistExport>("PlaylistExport", TransportPolicy::Batch),
    commandSchema<PlaylistImport>("PlaylistImport", TransportPolicy::Batch),
    commandSchema<LibraryScan>("LibraryScan", TransportPolicy::LongRunning),
    commandSchema<LibraryAnalyze>("LibraryAnalyze", TransportPolicy::LongRunning),
    commandSchema<LibrarySearch>("LibrarySearch"),
    commandSchema<LibraryStats>("LibraryStats"),
    commandSchema<LibraryAdd>("LibraryAdd", TransportPolicy::Batch),
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
    commandSchema<LibraryMaintenance>("LibraryMaintenance", TransportPolicy::Batch),
    commandSchema<StatusWait>("StatusWait"),
    commandSchema<TagBatchEdit>("TagBatchEdit", TransportPolicy::Batch),
};

static_assert(commandSchemas.size() == std::variant_size_v<Command>,
              "every Command alternative needs a commandSchemas row");

template <std::size_t... I>
constexpr bool schemaRowsMatchVariant(std::index_sequence<I...>) {
    return ((commandSchemas[I].index == I) && ...);
}
static_assert(schemaRowsMatchVariant(std::make_index_sequence<commandSchemas.size()>{}),
              "commandSchemas rows must sit at their alternative's index");

constexpr bool schemaNamesUnique() {
    for (std::size_t i = 0; i < commandSchemas.size(); ++i) {
        if (commandSchemas[i].name.empty())
            return false;
        for (std::size_t k = i + 1; k < commandSchemas.size(); ++k)
            if (commandSchemas[i].name == commandSchemas[k].name)
                return false;
    }
    return true;
}
static_assert(schemaNamesUnique(), "command wire names must be unique and non-empty");

/// Pre-registry name still accepted from old clients: same payload as
/// LibraryStats, but an absent `top_tracks` defaults to 10.
inline constexpr std::string_view libraryStatsDetailedAlias = "LibraryStatsDetailed";

} // namespace

Json toJson(const Command& cmd) {
    Json j;
    // Wire names have exactly one source (commandSchemas); the visitor below
    // only fills payload fields.
    j["type"] = commandSchemas[cmd.index()].name;
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
            } else if constexpr (std::is_same_v<T, QueueAddBatch>) {
                j["queries"] = Json::array();
                for (const auto& q : v.queries) {
                    Json e;
                    e = q;
                    j["queries"].push_back(e);
                }
                j["search"] = v.search;
            } else if constexpr (std::is_same_v<T, QueueRemove>) {
                j["idOrIndex"] = v.idOrIndex;
            } else if constexpr (std::is_same_v<T, QueueRemoveBatch>) {
                j["selectors"] = Json::array();
                for (const auto& s : v.selectors) {
                    Json e;
                    e = s;
                    j["selectors"].push_back(e);
                }
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
            } else if constexpr (std::is_same_v<T, LibraryAnalyze>) {
                j["reanalyze"] = v.reanalyze;
                if (v.library_id.has_value())
                    j["library_id"] = *v.library_id;
                else
                    j["library_id"] = nullptr;
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
                static_assert(alwaysFalse<T>, "Command alternative has no JSON body");
            }
        },
        cmd);
    return j;
}

template <CommandAlternative T>
std::expected<T, kanariv::utils::Error> parseCommandPayload(const Json& j) {
    // Strictness policy (I10): a field the serializer always writes is
    // mandatory; a field it emits as `null` (or omits) when unset stays
    // optional; a present field of the wrong type is InvalidArg -- never a
    // silent default.
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
            Seek v{};
            if (auto r = detail::requiredNumber(j, "seconds", v.seconds); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, SeekBy>) {
            SeekBy v{};
            if (auto r = detail::requiredNumber(j, "delta", v.delta); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, StatusReq>)
            return StatusReq{};
        else if constexpr (std::is_same_v<T, StatusWait>) {
            StatusWait v{};
            if (auto r = detail::requiredNumber(j, "timeout_ms", v.timeout_ms); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, VolumeSet>) {
            VolumeSet v{};
            if (auto r = detail::optionalNumber(j, "level", v.level); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalBool(j, "mute", v.mute); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "deltaPct", v.deltaPct); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueList>) {
            QueueList v{};
            if (auto r = detail::requiredString(j, "order", v.order); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueQueues>)
            return QueueQueues{};
        else if constexpr (std::is_same_v<T, QueueCreate>) {
            QueueCreate v{};
            if (auto r = detail::requiredString(j, "name", v.name); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueDelete>) {
            // Never fall back to queue 1 on a missing field: a malformed frame
            // must not delete a queue the client did not name.
            QueueDelete v{};
            if (auto r = detail::requiredNumber(j, "qid", v.qid); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlayFiles>) {
            PlayFiles v{};
            if (auto r = detail::requiredStringArray(j, "paths", v.paths); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "save", v.save); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueSwitch>) {
            // Same fail-closed rule as QueueDelete: `qid` is mandatory.
            QueueSwitch v{};
            if (auto r = detail::requiredNumber(j, "qid", v.qid); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueAdd>) {
            QueueAdd v{};
            if (auto r = detail::requiredString(j, "query", v.query); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "search", v.search); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueAddBatch>) {
            QueueAddBatch v{};
            if (auto r = detail::requiredStringArray(j, "queries", v.queries); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "search", v.search); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueRemove>) {
            QueueRemove v{};
            if (auto r = detail::requiredString(j, "idOrIndex", v.idOrIndex); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueRemoveBatch>) {
            QueueRemoveBatch v{};
            if (auto r = detail::requiredStringArray(j, "selectors", v.selectors); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueMove>) {
            QueueMove v{};
            if (auto r = detail::requiredNumber(j, "from", v.from); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "to", v.to); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueClear>)
            return QueueClear{};
        else if constexpr (std::is_same_v<T, QueueShuffle>) {
            QueueShuffle v{};
            if (auto r = detail::optionalBool(j, "on", v.on); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, QueueRepeat>) {
            QueueRepeat v{};
            std::optional<std::string> mode;
            if (auto r = detail::optionalString(j, "mode", mode); !r)
                return std::unexpected{std::move(r.error())};
            if (mode) {
                auto parsed = kanariv::types::parseRepeatMode(*mode);
                if (!parsed)
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg, "unknown RepeatMode")};
                v.mode = *parsed;
            }
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistList>)
            return PlaylistList{};
        else if constexpr (std::is_same_v<T, PlaylistTracks>) {
            PlaylistTracks v{};
            if (auto r = detail::requiredNumber(j, "pid", v.pid); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistCreate>) {
            PlaylistCreate v{};
            if (auto r = detail::requiredString(j, "name", v.name); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "smart_query", v.smart_query); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistAdd>) {
            PlaylistAdd v{};
            if (auto r = detail::requiredNumber(j, "pid", v.pid); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistLoad>) {
            PlaylistLoad v{};
            if (auto r = detail::requiredNumber(j, "pid", v.pid); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "play", v.play); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "replace", v.replace); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistSave>) {
            PlaylistSave v{};
            if (auto r = detail::requiredString(j, "name", v.name); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "queue_id", v.queue_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
            PlaylistDelete v{};
            if (auto r = detail::requiredNumber(j, "pid", v.pid); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistRename>) {
            PlaylistRename v{};
            if (auto r = detail::requiredNumber(j, "pid", v.pid); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "newName", v.newName); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistExport>) {
            PlaylistExport v{};
            if (auto r = detail::requiredNumber(j, "pid", v.pid); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "format", v.format); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, PlaylistImport>) {
            PlaylistImport v{};
            if (auto r = detail::requiredString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalString(j, "name", v.name); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LibraryScan>) {
            LibraryScan v{};
            if (auto r = detail::optionalString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            if (j.contains("full_hash")) {
                if (auto r = detail::requiredBool(j, "full_hash", v.full_hash); !r)
                    return std::unexpected{std::move(r.error())};
            } else if (j.contains("mode")) {
                // Pre-`full_hash` wire shape, still accepted from older clients.
                std::string mode;
                if (auto r = detail::requiredString(j, "mode", mode); !r)
                    return std::unexpected{std::move(r.error())};
                v.full_hash = mode == "full";
            } else {
                return std::unexpected{detail::missingFieldError("full_hash")};
            }
            return v;
        } else if constexpr (std::is_same_v<T, LibraryAnalyze>) {
            LibraryAnalyze v{};
            if (j.contains("reanalyze")) {
                if (auto r = detail::requiredBool(j, "reanalyze", v.reanalyze); !r)
                    return std::unexpected{std::move(r.error())};
            }
            if (auto r = detail::optionalNumber(j, "library_id", v.library_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LibrarySearch>) {
            LibrarySearch v{};
            if (auto r = detail::requiredString(j, "query", v.query); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "limit", v.limit); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LibraryStats>) {
            LibraryStats v{};
            if (auto r = detail::optionalNumber(j, "top_tracks", v.top_tracks); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "top_albums", v.top_albums); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "top_genres", v.top_genres); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "top_artists", v.top_artists); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LibraryAdd>) {
            LibraryAdd v{};
            if (auto r = detail::requiredString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "recursive", v.recursive); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LibraryRemove>) {
            LibraryRemove v{};
            if (j.contains("query")) {
                if (auto r = detail::requiredString(j, "query", v.query); !r)
                    return std::unexpected{std::move(r.error())};
            } else if (j.contains("id")) {
                // Legacy `id` alias: a track id as a JSON string or integer.
                const Json id = j["id"];
                if (id.isString())
                    v.query = id.get<std::string>();
                else if (id.isInteger() || id.isUnsigned())
                    v.query = std::to_string(id.get<int64_t>());
                else
                    return std::unexpected{detail::typeMismatchError("id", "a string or integer")};
            } else {
                return std::unexpected{detail::missingFieldError("query")};
            }
            return v;
        } else if constexpr (std::is_same_v<T, LibraryList>) {
            LibraryList v{};
            if (auto r = detail::optionalString(j, "query", v.query); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "limit", v.limit); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "offset", v.offset); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalString(j, "artist", v.artist); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalString(j, "album", v.album); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalString(j, "genre", v.genre); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "rating", v.rating); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LibraryMaintenance>) {
            LibraryMaintenance v{};
            if (auto r = detail::requiredString(j, "action", v.action); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "dry_run", v.dry_run); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "library_id", v.library_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, TagEdit>) {
            TagEdit v{};
            if (auto r = detail::requiredNumber(j, "id", v.id); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "field", v.field); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "value", v.value); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, TagGet>) {
            TagGet v{};
            if (auto r = detail::optionalNumber(j, "id", v.id); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "cover", v.cover); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, TagBatchEdit>) {
            TagBatchEdit v{};
            if (auto r = detail::requiredBool(j, "dry_run", v.dry_run); !r)
                return std::unexpected{std::move(r.error())};
            if (!j.contains("entries"))
                return std::unexpected{detail::missingFieldError("entries")};
            const Json entries = j["entries"];
            if (!entries.isArray())
                return std::unexpected{detail::typeMismatchError("entries", "an array of objects")};
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto item = entries.at(i);
                if (!item)
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg,
                        std::format("field 'entries' item {} could not be read", i))};
                if (!item->isObject())
                    return std::unexpected{kanariv::utils::makeError(
                        kanariv::utils::StatusCode::InvalidArg,
                        std::format("field 'entries' item {} must be an object", i))};
                TagBatchEditEntry e{};
                if (auto r = detail::optionalNumber(*item, "id", e.id); !r)
                    return std::unexpected{std::move(r.error())};
                if (auto r = detail::optionalString(*item, "path", e.path); !r)
                    return std::unexpected{std::move(r.error())};
                if (auto r = detail::requiredString(*item, "field", e.field); !r)
                    return std::unexpected{std::move(r.error())};
                if (auto r = detail::requiredString(*item, "value", e.value); !r)
                    return std::unexpected{std::move(r.error())};
                v.entries.push_back(std::move(e));
            }
            return v;
        } else if constexpr (std::is_same_v<T, ConfigGet>) {
            ConfigGet v{};
            if (auto r = detail::requiredString(j, "key", v.key); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, ConfigSet>) {
            ConfigSet v{};
            if (auto r = detail::requiredString(j, "key", v.key); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "value", v.value); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, ConfigList>)
            return ConfigList{};
        else if constexpr (std::is_same_v<T, ConfigExport>) {
            ConfigExport v{};
            if (auto r = detail::requiredString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, ConfigImport>) {
            ConfigImport v{};
            if (auto r = detail::requiredString(j, "path", v.path); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, ConfigReset>) {
            ConfigReset v{};
            if (auto r = detail::optionalString(j, "key", v.key); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, HistoryList>) {
            HistoryList v{};
            if (auto r = detail::optionalNumber(j, "limit", v.limit); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, HistoryClear>) {
            return HistoryClear{};
        } else if constexpr (std::is_same_v<T, BookmarkList>) {
            BookmarkList v{};
            if (auto r = detail::optionalNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, BookmarkAdd>) {
            BookmarkAdd v{};
            if (auto r = detail::requiredNumber(j, "position_ms", v.position_ms); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "note", v.note); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, BookmarkRemove>) {
            BookmarkRemove v{};
            if (auto r = detail::requiredNumber(j, "id", v.id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, BookmarkGoto>) {
            BookmarkGoto v{};
            if (auto r = detail::requiredNumber(j, "id", v.id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LyricsGet>) {
            LyricsGet v{};
            if (auto r = detail::optionalNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LyricsSet>) {
            LyricsSet v{};
            if (auto r = detail::requiredString(j, "lrc_text", v.lrc_text); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "is_synced", v.is_synced); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredString(j, "source", v.source); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::optionalNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, LyricsRemove>) {
            LyricsRemove v{};
            if (auto r = detail::optionalNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, Shutdown>)
            return Shutdown{};
        else if constexpr (std::is_same_v<T, Preview>) {
            Preview v{};
            if (auto r = detail::requiredString(j, "file", v.file); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, DeviceList>)
            return DeviceList{};
        else if constexpr (std::is_same_v<T, DeviceSet>) {
            DeviceSet v{};
            if (auto r = detail::requiredString(j, "id", v.id); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, DeviceTest>) {
            DeviceTest v{};
            if (auto r = detail::optionalString(j, "id", v.id); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredBool(j, "tone", v.tone); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else if constexpr (std::is_same_v<T, Info>)
            return Info{};
        else if constexpr (std::is_same_v<T, TrackRate>) {
            TrackRate v{};
            if (auto r = detail::requiredNumber(j, "track_id", v.track_id); !r)
                return std::unexpected{std::move(r.error())};
            if (auto r = detail::requiredNumber(j, "rating", v.rating); !r)
                return std::unexpected{std::move(r.error())};
            return v;
        } else {
            static_assert(alwaysFalse<T>, "Command alternative has no JSON parser");
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
        if (t == libraryStatsDetailedAlias) {
            auto detailed = parseCommandPayload<LibraryStats>(j);
            if (!detailed)
                return std::unexpected{std::move(detailed.error())};
            if (!(j.contains("top_tracks") && j["top_tracks"].isNumber()))
                detailed->top_tracks = 10;
            return Command{std::move(*detailed)};
        }
        for (const auto& row : commandSchemas)
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

TransportPolicy transportPolicyFor(const Command& cmd) noexcept {
    return commandSchemas[cmd.index()].policy;
}
} // namespace kanariv::ipc
