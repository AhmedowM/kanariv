#include <array>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/math.hpp>
#include <kanariv/utils/record_json.hpp>
#include <kanariv/utils/result.hpp>
#include <cstdint>
#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace kanariv::utils {

namespace {

std::array<uint8_t, 32> fallbackFingerprint(std::string_view path) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (char c : path) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ULL;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 32; i++) {
        out[i] = static_cast<uint8_t>(h >> ((i % 8) * 8));
        h = h * 6364136223846793005ULL + 1;
    }
    return out;
}

} // namespace

Json trackToJson(const kanariv::types::Track& t) {
    Json j;
    j["id"] = t.id;
    j["size"] = t.size;
    j["mtime"] = t.mtime;
    j["duration"] = t.duration;
    j["sample_rate"] = t.sample_rate;
    j["channels"] = t.channels;
    j["bitrate"] = t.bitrate;
    j["year"] = t.year;
    j["track_num"] = t.track_num;
    j["disc_num"] = t.disc_num;
    j["rating"] = t.rating;
    j["play_count"] = t.play_count;
    j["last_played"] = t.last_played;
    j["date_added"] = t.date_added;
    j["last_scanned"] = t.last_scanned;
    j["dirty"] = t.dirty;
    j["library_id"] = t.library_id;
    j["deleted_at"] = t.deleted_at;
    j["fingerprint"] = toHex(t.fingerprint);
    j["path"] = t.path;
    {
        const auto source = kanariv::types::normalizedSource(t);
        Json s;
        s["kind"] = std::string(kanariv::types::sourceKindName(source.kind));
        s["key"] = source.key;
        s["display_name"] = source.display_name;
        s["available"] = source.available;
        j["source"] = std::move(s);
    }
    j["title"] = t.title;
    j["artist"] = t.artist;
    j["album"] = t.album;
    j["album_artist"] = t.album_artist;
    j["genre"] = t.genre;
    j["cover_art_path"] = t.cover_art_path;
    return j;
}

// Forward declarations for helpers defined below.
static void parseSourceFromJson(const Json& j, kanariv::types::Track& t);
static void getStrHelper(const Json& j, const char* k, std::string& out);
static void readFingerprint(const Json& j, const std::string& path,
                            std::array<std::uint8_t, 32>& fp);

std::expected<kanariv::types::Track, Error> trackFromJson(const Json& j) {
    try {
        kanariv::types::Track t;
        auto getI64 = [&](const char* k, int64_t& out, int64_t def = 0) {
            if (j.contains(k) && !j[k].isNull()) {
                if (j[k].isNumber())
                    out = j[k].get<int64_t>();
                else
                    out = def;
            } else
                out = def;
        };
        auto getInt = [&](const char* k, int& out, int def = 0) {
            int64_t v = def;
            getI64(k, v, def);
            out = static_cast<int>(v);
        };
        auto getDbl = [&](const char* k, double& out, double def = 0) {
            if (j.contains(k) && !j[k].isNull() && j[k].isNumber())
                out = j[k].get<double>();
            else
                out = def;
        };
        auto getStr = [&](const char* k, std::string& out) {
            if (j.contains(k) && !j[k].isNull() && j[k].isString())
                out = j[k].get<std::string>();
            else
                out.clear();
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
        if (t.library_id == 0)
            t.library_id = 1;
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
    if (!j.contains("source") || !j["source"].isObject())
        return;
    const Json s = j["source"];
    std::string kindName = "path";
    if (s.contains("kind") && s["kind"].isString())
        kindName = s["kind"].get<std::string>();
    auto parsedKind = kanariv::types::sourceKindFromName(kindName);
    if (!parsedKind) {
        throw std::runtime_error("invalid source kind: " + kindName);
    }
    t.source.kind = *parsedKind;
    if (s.contains("key") && s["key"].isString())
        t.source.key = s["key"].get<std::string>();
    else
        t.source.key.clear();
    if (s.contains("display_name") && s["display_name"].isString())
        t.source.display_name = s["display_name"].get<std::string>();
    else
        t.source.display_name.clear();
    if (s.contains("available") && s["available"].isBoolean())
        t.source.available = s["available"].get<bool>();
    else
        t.source.available = true;
    if (t.source.kind == kanariv::types::SourceKind::path && t.source.key.empty())
        t.source.key = t.path;
}

// Reads the fingerprint from JSON, falling back to a path-derived fingerprint
// when the JSON value is absent, empty, or undecodable.
static void readFingerprint(const Json& j, const std::string& path,
                            std::array<std::uint8_t, 32>& fp) {
    std::string fpHex;
    getStrHelper(j, "fingerprint", fpHex);
    if (!fpHex.empty()) {
        if (fromHex(fpHex, fp))
            return;
    }
    fp = fallbackFingerprint(path);
}

// Helper used by readFingerprint — mirrors getStr lambda but as a free function.
static void getStrHelper(const Json& j, const char* k, std::string& out) {
    if (j.contains(k) && !j[k].isNull() && j[k].isString())
        out = j[k].get<std::string>();
    else
        out.clear();
}

} // namespace kanariv::utils
