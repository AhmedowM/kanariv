// Read-only FFI parity harness: compares `kanariv_db_ffi` against the C++
// database for search and track lookup on the same on-disk schema.
//
// This is a pilot measurement, not a production path. The daemon never links
// the Rust library; see rust/README.md.

#include <kanariv/db/database.hpp>
#include <kanariv/db_ffi.h>
#include <kanariv/types/record.hpp>
#include <kanariv/utils/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using kanariv::types::Track;
using kanariv::utils::Json;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

/// Owns a string returned across the FFI boundary.
class RustString {
  public:
    RustString() = default;
    RustString(const RustString&) = delete;
    RustString& operator=(const RustString&) = delete;
    ~RustString() { kanariv_db_ffi_string_free(value_); }
    char** out() { return &value_; }
    [[nodiscard]] const char* get() const { return value_; }
    void reset() {
        kanariv_db_ffi_string_free(value_);
        value_ = nullptr;
    }

  private:
    char* value_{nullptr};
};

std::string fingerprintHex(const Track& track) {
    std::string out;
    out.reserve(64);
    std::array<char, 3> buf{};
    for (uint8_t byte : track.fingerprint) {
        std::snprintf(buf.data(), buf.size(), "%02x", byte);
        out += buf.data();
    }
    return out;
}

std::array<uint8_t, 32> parseFingerprint(const std::string& hex) {
    std::array<uint8_t, 32> out{};
    if (hex.size() != out.size() * 2) {
        return out;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    }
    return out;
}

Track makeTrack(std::string path, std::string title, std::string artist, uint8_t seed) {
    Track track;
    track.path = std::move(path);
    track.title = std::move(title);
    track.artist = std::move(artist);
    track.album = "Album";
    track.album_artist = "Album Artist";
    track.genre = "Rock";
    track.duration = 100.0 + seed;
    track.sample_rate = 44100;
    track.channels = 2;
    track.bitrate = 320000;
    track.year = 1969;
    track.track_num = seed;
    track.disc_num = 1;
    track.rating = 4;
    for (std::size_t i = 0; i < track.fingerprint.size(); ++i) {
        track.fingerprint[i] = static_cast<uint8_t>(seed + i);
    }
    return track;
}

std::vector<Track> searchCpp(kanariv::contracts::IDatabase& db, const std::string& query) {
    auto result = db.searchTracks(query, 10);
    if (!result) {
        check(false, "C++ search failed for \"" + query + "\": " + result.error().message);
        return {};
    }
    auto tracks = std::move(*result);
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.id < b.id; });
    return tracks;
}

template <typename T>
T field(const Json& object, std::string_view key, const std::string& context) {
    auto value = object.at(key);
    if (!value) {
        check(false, context + ": missing field " + std::string(key));
        return T{};
    }
    try {
        return value->template get<T>();
    } catch (const std::exception& error) {
        check(false, context + ": bad field " + std::string(key) + ": " + error.what());
        return T{};
    }
}

std::vector<Track> parseRustTracks(const char* jsonText, const std::string& context) {
    std::vector<Track> tracks;
    if (jsonText == nullptr) {
        check(false, context + ": null JSON");
        return tracks;
    }
    auto parsed = Json::tryParse(jsonText);
    if (!parsed || !parsed->isArray()) {
        check(false, context + ": not a JSON array: " + std::string(jsonText));
        return tracks;
    }
    for (std::size_t i = 0; i < parsed->size(); ++i) {
        auto item = parsed->at(i);
        if (!item || !item->isObject()) {
            check(false, context + ": element is not an object");
            continue;
        }
        Track track;
        track.id = field<int64_t>(*item, "id", context);
        track.path = field<std::string>(*item, "path", context);
        track.title = field<std::string>(*item, "title", context);
        track.artist = field<std::string>(*item, "artist", context);
        track.album = field<std::string>(*item, "album", context);
        track.album_artist = field<std::string>(*item, "album_artist", context);
        track.genre = field<std::string>(*item, "genre", context);
        track.duration = field<double>(*item, "duration", context);
        track.library_id = field<int64_t>(*item, "library_id", context);
        track.date_added = field<int64_t>(*item, "date_added", context);
        track.play_count = field<int64_t>(*item, "play_count", context);
        track.last_played = field<int64_t>(*item, "last_played", context);
        track.sample_rate = static_cast<uint32_t>(field<int64_t>(*item, "sample_rate", context));
        track.channels = static_cast<uint32_t>(field<int64_t>(*item, "channels", context));
        track.bitrate = static_cast<int32_t>(field<int64_t>(*item, "bitrate", context));
        track.year = static_cast<int32_t>(field<int64_t>(*item, "year", context));
        track.track_num = static_cast<int32_t>(field<int64_t>(*item, "track_num", context));
        track.disc_num = static_cast<int32_t>(field<int64_t>(*item, "disc_num", context));
        track.rating = static_cast<int32_t>(field<int64_t>(*item, "rating", context));
        track.deleted_at = field<int64_t>(*item, "deleted_at", context);
        track.fingerprint = parseFingerprint(field<std::string>(*item, "fingerprint", context));
        tracks.push_back(std::move(track));
    }
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.id < b.id; });
    return tracks;
}

void compareTrack(const Track& cpp, const Track& rust, const std::string& context) {
    check(cpp.id == rust.id, context + ": id " + std::to_string(cpp.id) + " != " +
                                std::to_string(rust.id));
    check(cpp.path == rust.path, context + ": path");
    check(cpp.title == rust.title, context + ": title");
    check(cpp.artist == rust.artist, context + ": artist");
    check(cpp.album == rust.album, context + ": album");
    check(cpp.album_artist == rust.album_artist, context + ": album_artist");
    check(cpp.genre == rust.genre, context + ": genre");
    check(std::fabs(cpp.duration - rust.duration) < 1e-9, context + ": duration");
    check(cpp.library_id == rust.library_id, context + ": library_id");
    check(cpp.date_added == rust.date_added, context + ": date_added");
    check(cpp.play_count == rust.play_count, context + ": play_count");
    check(cpp.last_played == rust.last_played, context + ": last_played");
    check(cpp.sample_rate == rust.sample_rate, context + ": sample_rate");
    check(cpp.channels == rust.channels, context + ": channels");
    check(cpp.bitrate == rust.bitrate, context + ": bitrate");
    check(cpp.year == rust.year, context + ": year");
    check(cpp.track_num == rust.track_num, context + ": track_num");
    check(cpp.disc_num == rust.disc_num, context + ": disc_num");
    check(cpp.rating == rust.rating, context + ": rating");
    check(cpp.deleted_at == rust.deleted_at, context + ": deleted_at");
    check(fingerprintHex(cpp) == fingerprintHex(rust), context + ": fingerprint");
}

void compareSearch(const std::vector<Track>& cpp, const std::vector<Track>& rust,
                   const std::string& query) {
    const std::string context = "search \"" + query + "\"";
    check(cpp.size() == rust.size(), context + ": result count " + std::to_string(cpp.size()) +
                                         " != " + std::to_string(rust.size()));
    const std::size_t common = std::min(cpp.size(), rust.size());
    for (std::size_t i = 0; i < common; ++i) {
        compareTrack(cpp[i], rust[i], context + " row " + std::to_string(i));
    }
}

std::int64_t callSearch(KanarivDbHandle* handle, const std::string& query, RustString& json,
                        RustString& error) {
    return kanariv_db_ffi_search_json(handle, query.c_str(), 10, json.out(), error.out());
}

} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dbPath =
        fs::temp_directory_path() / ("kanariv_ffi_" + std::to_string(stamp) + ".db");

    std::vector<std::pair<int64_t, Track>> inserted;
    std::vector<std::string> queries = {"Abbey Road", "Abb", "hello", "100%", "Abbey OR Road",
                                        "\"Abbey Road\"", "", "   ", "zzz"};
    std::vector<std::vector<Track>> cppResults;

    {
        auto opened = kanariv::db::Database::open(dbPath.string());
        if (!opened) {
            std::cerr << "setup: cannot open " << dbPath << ": " << opened.error().message << '\n';
            return 1;
        }
        auto db = std::move(*opened);
        const std::vector<Track> seeds = {
            makeTrack((dbPath.parent_path() / "abbey.mp3").string(), "Abbey Road", "Beatles", 0x10),
            makeTrack((dbPath.parent_path() / "abbeville.mp3").string(), "Abbeville", "Other", 0x40),
            makeTrack((dbPath.parent_path() / "hello.mp3").string(), "Hello World", "Artist", 0x70),
            makeTrack((dbPath.parent_path() / "percent.mp3").string(), "100% Pure", "Percent", 0xA0),
        };
        for (const Track& seed : seeds) {
            auto id = db->insertTrack(seed);
            if (!id) {
                std::cerr << "setup: insert failed: " << id.error().message << '\n';
                return 1;
            }
            Track stored = seed;
            stored.id = *id;
            inserted.emplace_back(*id, std::move(stored));
        }
        for (const std::string& query : queries) {
            cppResults.push_back(searchCpp(*db, query));
        }
    }

    KanarivDbHandle* handle = nullptr;
    {
        RustString error;
        handle = kanariv_db_ffi_open(dbPath.string().c_str(), error.out());
        if (handle == nullptr) {
            std::cerr << "setup: Rust FFI open failed: "
                      << (error.get() != nullptr ? error.get() : "(no message)") << '\n';
            return 1;
        }
    }

    for (std::size_t i = 0; i < queries.size(); ++i) {
        RustString json;
        RustString error;
        const int32_t status = static_cast<int32_t>(callSearch(handle, queries[i], json, error));
        check(status == 0, "search \"" + queries[i] + "\" status " + std::to_string(status) +
                               " (" + (error.get() != nullptr ? error.get() : "") + ")");
        if (status == 0) {
            compareSearch(cppResults[i], parseRustTracks(json.get(), "search \"" + queries[i] + "\""),
                          queries[i]);
        }
    }

    for (const auto& [id, expected] : inserted) {
        RustString json;
        RustString error;
        const int32_t status =
            kanariv_db_ffi_get_track_json(handle, id, json.out(), error.out());
        check(status == 0, "get_track " + std::to_string(id) + " status " +
                               std::to_string(status));
        if (status != 0) {
            continue;
        }
        const auto parsed = parseRustTracks(("[" + std::string(json.get()) + "]").c_str(),
                                            "get_track " + std::to_string(id));
        check(parsed.size() == 1, "get_track " + std::to_string(id) + ": one row");
        if (parsed.size() == 1) {
            compareTrack(expected, parsed.front(), "get_track " + std::to_string(id));
        }
    }

    {
        RustString json;
        RustString error;
        const int32_t status =
            kanariv_db_ffi_search_json(handle, nullptr, 10, json.out(), error.out());
        check(status == 1, "null query maps to invalid-arg");
    }
    {
        RustString json;
        RustString error;
        const int32_t status = kanariv_db_ffi_get_track_json(handle, 0, json.out(), error.out());
        check(status == 1, "id 0 maps to invalid-arg");
    }
    {
        RustString json;
        RustString error;
        const int32_t status =
            kanariv_db_ffi_get_track_json(handle, 999999, json.out(), error.out());
        check(status == 2, "missing id maps to not-found");
    }
    {
        RustString json;
        RustString error;
        const int32_t status =
            kanariv_db_ffi_search_json(nullptr, "x", 10, json.out(), error.out());
        check(status == 1, "null handle maps to invalid-arg");
    }

    // Steady-state read cost: C++ contract call vs FFI call. The FFI number
    // includes JSON serialization and allocation, so it bounds, rather than
    // isolates, the boundary cost.
    {
        auto reopened = kanariv::db::Database::open(dbPath.string());
        if (!reopened) {
            check(false, "timing: cannot reopen the C++ database");
        } else {
            constexpr int kIterations = 2000;
            using Clock = std::chrono::steady_clock;
            using Micros = std::chrono::duration<double, std::micro>;

            auto started = Clock::now();
            for (int i = 0; i < kIterations; ++i) {
                auto result = (*reopened)->searchTracks("Abb", 10);
                if (!result) {
                    check(false, "timing: C++ search failed");
                    break;
                }
            }
            const double cppMicros = Micros(Clock::now() - started).count() / kIterations;

            started = Clock::now();
            for (int i = 0; i < kIterations; ++i) {
                RustString json;
                RustString error;
                if (callSearch(handle, "Abb", json, error) != 0) {
                    check(false, "timing: FFI search failed");
                    break;
                }
            }
            const double ffiMicros = Micros(Clock::now() - started).count() / kIterations;

            std::cout << "timing: cpp=" << cppMicros << " us, ffi=" << ffiMicros
                      << " us per search (JSON included)\n";
        }
    }

    kanariv_db_ffi_close(handle);

    if (std::getenv("KANARIV_FFI_KEEP_DB") != nullptr) {
        std::cout << "kept database: " << dbPath << '\n';
    } else {
        std::error_code ignored;
        fs::remove(dbPath, ignored);
        fs::remove(dbPath.string() + "-wal", ignored);
        fs::remove(dbPath.string() + "-shm", ignored);
    }

    if (failures == 0) {
        std::cout << "rust db ffi parity: OK (" << queries.size() << " queries, " << inserted.size()
                  << " tracks)\n";
        return 0;
    }
    std::cerr << "rust db ffi parity: " << failures << " failure(s)\n";
    return 1;
}
