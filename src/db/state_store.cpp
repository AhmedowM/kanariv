#include <cstddef>
#include <cstdio>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <kanariv/db/json_state_store.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/json.hpp>
#include <kanariv/utils/log.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <db/database_access.hpp>

namespace kanariv::db {

namespace {

using kanariv::utils::Json;

/// Bumped when a field changes meaning; files with another version are ignored.
constexpr int kStateVersion = 1;

std::filesystem::path withSuffix(const std::filesystem::path& base, std::string_view suffix) {
    std::filesystem::path out = base;
    out += suffix;
    return out;
}

std::string_view repeatName(kanariv::types::RepeatMode mode) noexcept {
    switch (mode) {
    case kanariv::types::RepeatMode::One:
        return "one";
    case kanariv::types::RepeatMode::All:
        return "all";
    case kanariv::types::RepeatMode::Off:
        break;
    }
    return "off";
}

kanariv::types::RepeatMode repeatFromName(std::string_view name) noexcept {
    if (name == "one")
        return kanariv::types::RepeatMode::One;
    if (name == "all")
        return kanariv::types::RepeatMode::All;
    return kanariv::types::RepeatMode::Off;
}

Json toJson(const kanariv::types::EngineState& state) {
    Json doc = Json::object();
    doc["version"] = kStateVersion;
    doc["queue_name"] = state.queue_name.empty() ? std::string{"default"} : state.queue_name;
    doc["cursor"] = state.cursor_pos;
    doc["current_path"] = state.current_path;
    if (state.current_track_id)
        doc["current_track_id"] = *state.current_track_id;
    doc["shuffle"] = state.shuffle_enabled;
    doc["repeat"] = repeatName(state.repeat_mode);
    doc["shuffle_perm"] = Json::array();
    for (const int64_t position : state.shuffle_perm) {
        Json value;
        value = position;
        doc["shuffle_perm"].push_back(value);
    }
    doc["volume"] = static_cast<double>(state.volume);
    doc["pre_mute_volume"] = static_cast<double>(state.pre_mute_volume);
    doc["position_ms"] = state.position_ms;
    return doc;
}

/// Parses a sidecar document; nullopt means "not this format / unusable".
std::optional<kanariv::types::EngineState> parseState(std::string_view text) {
    auto parsed = Json::tryParse(text);
    if (!parsed || !parsed->isObject())
        return std::nullopt;
    const Json& doc = *parsed;
    try {
        if (!doc.contains("version") || doc["version"].get<int>() != kStateVersion)
            return std::nullopt;
        kanariv::types::EngineState state;
        if (doc.contains("queue_name") && doc["queue_name"].isString())
            state.queue_name = doc["queue_name"].get<std::string>();
        if (state.queue_name.empty())
            state.queue_name = "default";
        if (doc.contains("cursor") && doc["cursor"].isNumber()) {
            state.cursor_pos = doc["cursor"].get<int64_t>();
            if (state.cursor_pos < 0)
                state.cursor_pos = 0;
        }
        if (doc.contains("current_path") && doc["current_path"].isString())
            state.current_path = doc["current_path"].get<std::string>();
        if (doc.contains("current_track_id") && doc["current_track_id"].isNumber())
            state.current_track_id = doc["current_track_id"].get<int64_t>();
        if (doc.contains("shuffle")) {
            if (!doc["shuffle"].isBoolean())
                return std::nullopt;
            state.shuffle_enabled = doc["shuffle"].get<bool>();
        }
        if (doc.contains("repeat") && doc["repeat"].isString())
            state.repeat_mode = repeatFromName(doc["repeat"].get<std::string>());
        if (doc.contains("shuffle_perm")) {
            if (!doc["shuffle_perm"].isArray())
                return std::nullopt;
            const std::size_t count = doc["shuffle_perm"].size();
            state.shuffle_perm.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const Json entry = doc["shuffle_perm"][i];
                if (!entry.isInteger())
                    return std::nullopt;
                state.shuffle_perm.push_back(entry.get<int64_t>());
            }
        }
        if (doc.contains("volume") && doc["volume"].isNumber()) {
            state.volume = doc["volume"].get<float>();
            if (state.volume < 0.0f)
                state.volume = 0.0f;
            if (state.volume > 1.0f)
                state.volume = 1.0f;
        }
        if (doc.contains("pre_mute_volume") && doc["pre_mute_volume"].isNumber()) {
            const float preMute = doc["pre_mute_volume"].get<float>();
            if (preMute > 0.0f && preMute <= 1.0f)
                state.pre_mute_volume = preMute;
        }
        if (doc.contains("position_ms") && doc["position_ms"].isNumber()) {
            state.position_ms = doc["position_ms"].get<int64_t>();
            if (state.position_ms < 0)
                state.position_ms = 0;
        }
        return state;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

kanariv::utils::Error ioError(std::string_view what, const std::filesystem::path& path) {
    return kanariv::utils::makeError(kanariv::utils::StatusCode::Io,
                                     std::string(what) + " '" + path.generic_string() + "'");
}

/// Writes `payload` to `target` via a flushed temp file and a rename.
std::expected<void, kanariv::utils::Error> writeAtomic(const std::filesystem::path& target,
                                                       std::string_view payload) {
    namespace fs = std::filesystem;
    const fs::path tmp = withSuffix(target, ".tmp");
#ifdef _WIN32
    std::FILE* file = _wfopen(tmp.c_str(), L"wb");
#else
    std::FILE* file = std::fopen(tmp.c_str(), "wb");
#endif
    if (!file)
        return std::unexpected(ioError("cannot write engine state", tmp));
    bool ok = std::fwrite(payload.data(), 1, payload.size(), file) == payload.size();
    ok = std::fflush(file) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(file)) == 0 && ok;
#else
    ok = ::fsync(::fileno(file)) == 0 && ok;
#endif
    if (std::fclose(file) != 0)
        ok = false;
    if (!ok) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return std::unexpected(ioError("cannot write engine state", tmp));
    }
    // Keep exactly one previous generation for corrupt-file recovery. Rotation
    // is best effort: an unavailable backup must not block the live write.
    std::error_code ec;
    if (fs::exists(target, ec)) {
        std::error_code rotateEc;
        fs::rename(target, withSuffix(target, ".prev"), rotateEc);
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return std::unexpected(ioError("cannot replace engine state", target));
    }
    return {};
}

} // namespace

JsonPlaybackStateStore::JsonPlaybackStateStore(std::filesystem::path path,
                                               kanariv::utils::Logger* logger)
    : path_(std::move(path)), logger_(logger) {}

JsonPlaybackStateStore::~JsonPlaybackStateStore() = default;

std::expected<kanariv::types::EngineState, kanariv::utils::Error> JsonPlaybackStateStore::load() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (path_.empty())
        return memory_.value_or(kanariv::types::EngineState{});
    if (const auto primary = readFile(path_)) {
        if (auto state = parseState(*primary))
            return *state;
        if (logger_)
            logger_->warn("engine state is corrupt, trying previous generation path={}",
                          path_.generic_string());
    }
    if (const auto previous = readFile(withSuffix(path_, ".prev"))) {
        if (auto state = parseState(*previous)) {
            if (logger_)
                logger_->warn("engine state recovered from previous generation path={}",
                              path_.generic_string());
            return *state;
        }
    }
    return kanariv::types::EngineState{};
}

std::expected<void, kanariv::utils::Error>
JsonPlaybackStateStore::save(const kanariv::types::EngineState& state) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (path_.empty()) {
        memory_ = state;
        return {};
    }
    const std::filesystem::path dir = path_.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return std::unexpected(ioError("cannot create engine state directory", dir));
    }
    const std::string payload = toJson(state).dump(2) + "\n";
    return writeAtomic(path_, payload);
}

bool JsonPlaybackStateStore::isOpen() const noexcept {
    return true;
}

kanariv::utils::Logger* JsonPlaybackStateStore::logger() const noexcept {
    return logger_;
}

const std::filesystem::path& JsonPlaybackStateStore::path() const noexcept {
    return path_;
}

std::shared_ptr<kanariv::contracts::IPlaybackStateStore> makePlaybackStateStore(Database& db) {
    const std::filesystem::path& dbPath = db.path();
    if (dbPath.empty())
        return std::make_shared<JsonPlaybackStateStore>(std::filesystem::path{}, db.logger());
    std::filesystem::path sidecar = dbPath;
    sidecar += ".state.json";
    auto store = std::make_shared<JsonPlaybackStateStore>(sidecar, db.logger());
    std::error_code ec;
    if (!std::filesystem::exists(sidecar, ec)) {
        if (auto legacy = DatabaseAccess::legacyEngineState(db)) {
            if (auto saved = store->save(*legacy); !saved && db.logger())
                db.logger()->warn("engine state import failed: {}", saved.error().message);
        }
    }
    return store;
}

} // namespace kanariv::db
