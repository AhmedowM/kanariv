#include <array>
#include <cstdint>
#include <exception>
#include <expected>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/result.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace kanariv::utils {

namespace {

constexpr std::size_t fingerprintSize = 32;
constexpr uint64_t fnvOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t fnvPrime = 1099511628211ULL;
constexpr uint64_t lcgMultiplier = 6364136223846793005ULL;
constexpr uint64_t lcgIncrement = 1;
constexpr int bitsPerByte = 8;

auto fallbackFingerprint(std::string_view path) noexcept -> std::array<uint8_t, fingerprintSize> {
    uint64_t h = fnvOffsetBasis;
    for (char c : path) {
        h ^= static_cast<uint8_t>(c);
        h *= fnvPrime;
    }
    std::array<uint8_t, fingerprintSize> out{};
    for (int i = 0; i < static_cast<int>(fingerprintSize); i++) {
        out.at(i) = static_cast<uint8_t>(h >> ((i % bitsPerByte) * bitsPerByte));
        h = h * lcgMultiplier + lcgIncrement;
    }
    return out;
}

} // namespace

auto trackToJson(const kanariv::types::Track& t) -> Json {
    Json j;
    j["id"] = t.id;       // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["size"] = t.size;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["mtime"] = t.mtime; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["duration"] =
        t.duration; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["sample_rate"] =
        t.sample_rate; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["channels"] =
        t.channels; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["bitrate"] =
        t.bitrate;      // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["year"] = t.year; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["track_num"] =
        t.track_num; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["disc_num"] =
        t.disc_num;         // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["rating"] = t.rating; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["play_count"] =
        t.play_count; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["last_played"] =
        t.last_played; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["date_added"] =
        t.date_added; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["last_scanned"] =
        t.last_scanned;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["dirty"] = t.dirty; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["library_id"] =
        t.library_id; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["deleted_at"] =
        t.deleted_at; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["fingerprint"] = toHex(
        t.fingerprint); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["path"] = t.path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    {
        const auto source = kanariv::types::normalizedSource(t);
        Json s;
        s["kind"] = kanariv::types::sourceKindName(
            source.kind); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s["key"] =
            source.key; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s["display_name"] =
            source
                .display_name; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        s["available"] =
            source
                .available; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        j["source"] = s;    // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    j["title"] = t.title;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["artist"] = t.artist; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["album"] = t.album;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["album_artist"] =
        t.album_artist;   // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["genre"] = t.genre; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    j["cover_art_path"] =
        t.cover_art_path; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return j;
}

// Forward declarations for helpers defined below.
static void parseSourceFromJson(const Json& j, kanariv::types::Track& t);
static void getStrHelper(const Json& j, const char* k, std::string& out);
static void readFingerprint(const Json& j, const std::string& path,
                            std::array<std::uint8_t, fingerprintSize>& fp);

auto trackFromJson(const Json& j) -> std::expected<kanariv::types::Track, Error> {
    try {
        kanariv::types::Track t;
        auto getI64 = [&](const char* k, int64_t& out, int64_t def = 0) -> void {
            if (j.contains(k) &&
                !j[k].isNull()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                if (j[k].isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                    out = j[k].get<
                        int64_t>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                } else {
                    out = def;
                }
            } else {
                out = def;
            }
        };
        auto getInt = [&](const char* k, int& out, int def = 0) -> void {
            int64_t v = def;
            getI64(k, v, def);
            out = static_cast<int>(v);
        };
        auto getDbl = [&](const char* k, double& out, double def = 0) -> void {
            if (j.contains(k) && !j[k].isNull() &&
                j[k].isNumber()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                out = j[k].get<
                    double>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else {
                out = def;
            }
        };
        auto getStr = [&](const char* k, std::string& out) -> void {
            if (j.contains(k) && !j[k].isNull() &&
                j[k].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                out = j[k].get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            } else {
                out.clear();
            }
        };
        getI64("id", t.id, 0);
        getI64("size", t.size, 0);
        getI64("mtime", t.mtime, 0);
        getDbl("duration", t.duration, 0);
        {
            int64_t v;
            getI64("sample_rate", v, 0);
            t.sample_rate = static_cast<uint32_t>(v);
        }
        {
            int64_t v;
            getI64("channels", v, 0);
            t.channels = static_cast<uint32_t>(v);
        }
        {
            int64_t v;
            getI64("bitrate", v, 0);
            t.bitrate = static_cast<int32_t>(v);
        }
        getInt("year", t.year, 0);
        getInt("track_num", t.track_num, 0);
        getInt("disc_num", t.disc_num, 0);
        getInt("rating", t.rating, 0);
        getI64("play_count", t.play_count, 0);
        getI64("last_played", t.last_played, 0);
        getI64("date_added", t.date_added, 0);
        getI64("last_scanned", t.last_scanned, 0);
        {
            int dirtyTmp = 0;
            getInt("dirty", dirtyTmp, 0);
            t.dirty = dirtyTmp != 0;
        }
        getI64("library_id", t.library_id, 1);
        if (t.library_id == 0) {
            t.library_id = 1;
        }
        getI64("deleted_at", t.deleted_at, 0);
        getStr("path", t.path);
        t.source.kind = kanariv::types::SourceKind::path;
        t.source.key = t.path;
        t.source.display_name = t.path;
        t.source.available = true;
        parseSourceFromJson(j, t);
        getStr("title", t.title);
        getStr("artist", t.artist);
        getStr("album", t.album);
        getStr("album_artist", t.album_artist);
        getStr("genre", t.genre);
        getStr("cover_art_path", t.cover_art_path);
        readFingerprint(j, t.path, t.fingerprint);
        return t;
    } catch (const std::exception& e) {
        return std::unexpected{makeError(StatusCode::Corrupt, e.what())};
    }
}

// Parses the optional "source" object from JSON into the track's source fields.
// Falls back to path-based defaults when source is absent or invalid.
static void parseSourceFromJson(const Json& j, kanariv::types::Track& t) {
    if (!j.contains("source") ||
        !j["source"]
             .isObject()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        return;
    }
    const Json s =
        j["source"]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::string kindName = "path";
    if (s.contains("kind") &&
        s["kind"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        kindName =
            s["kind"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }
    auto parsedKind = kanariv::types::sourceKindFromName(kindName);
    if (!parsedKind) {
        throw std::runtime_error("invalid source kind: " + kindName);
    }
    t.source.kind = *parsedKind;
    if (s.contains("key") &&
        s["key"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        t.source.key =
            s["key"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else {
        t.source.key.clear();
    }
    if (s.contains("display_name") &&
        s["display_name"]
            .isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        t.source.display_name =
            s["display_name"]
                .get<
                    std::
                        string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else {
        t.source.display_name.clear();
    }
    if (s.contains("available") &&
        s["available"]
            .isBoolean()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        t.source.available =
            s["available"]
                .get<
                    bool>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else {
        t.source.available = true;
    }
    if (t.source.kind == kanariv::types::SourceKind::path && t.source.key.empty()) {
        t.source.key = t.path;
    }
}

// Reads the fingerprint from JSON, falling back to a path-derived fingerprint
// when the JSON value is absent, empty, or undecodable.
static void readFingerprint(const Json& j, const std::string& path,
                            std::array<std::uint8_t, fingerprintSize>& fp) {
    std::string fpHex;
    getStrHelper(j, "fingerprint", fpHex);
    if (!fpHex.empty()) {
        if (fromHex(fpHex, fp)) {
            return;
        }
    }
    fp = fallbackFingerprint(path);
}

// Helper used by readFingerprint — mirrors getStr lambda but as a free function.
static void getStrHelper(const Json& j, const char* k, std::string& out) {
    if (j.contains(k) && !j[k].isNull() &&
        j[k].isString()) { // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        out = j[k].get<
            std::string>(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    } else {
        out.clear();
    }
}

} // namespace kanariv::utils
