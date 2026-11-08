#include <service/media_session.hpp>

#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>

#include <dbus/dbus.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <unistd.h>

namespace kanariv::media_session {

namespace {

constexpr const char* kObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kMediaPlayerInterface = "org.mpris.MediaPlayer2";
constexpr const char* kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr const char* kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char* kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";
constexpr const char* kNoTrack = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
constexpr const char* kTrackPrefix = "/org/kanariv/Track/";

/**
 * @brief RAII helper around dbus_message_iter_open_container().
 *
 * libdbus requires that every successful open_container() is paired with
 * exactly one close_container() or abandon_container() *with the same parent
 * and child iterators*. Mixing those up trips libdbus' internal consistency
 * checks and aborts the process with:
 *
 *   dbus message iterator has already been closed, or is uninitialized or
 *   corrupt
 *
 * Keeping the parent pointer in the guard makes that mistake impossible and
 * ensures partially built containers are abandoned on every early return.
 */
class OpenContainer {
  public:
    OpenContainer(DBusMessageIter& parent, int type, const char* signature) : parent_(&parent) {
        open_ = dbus_message_iter_open_container(parent_, type, signature, &iter_);
    }

    ~OpenContainer() {
        if (open_)
            dbus_message_iter_abandon_container_if_open(parent_, &iter_);
    }

    OpenContainer(const OpenContainer&) = delete;
    OpenContainer& operator=(const OpenContainer&) = delete;

    [[nodiscard]] bool opened() const noexcept {
        return open_;
    }

    DBusMessageIter& iter() noexcept {
        return iter_;
    }

    bool close() {
        if (!open_)
            return false;
        const bool ok = dbus_message_iter_close_container(parent_, &iter_);
        open_ = false;
        // close_container() can fail with out-of-memory, in which case the
        // child may still be on the parent's container stack. abandon_if_open()
        // is a no-op when the container was actually closed.
        if (!ok)
            dbus_message_iter_abandon_container_if_open(parent_, &iter_);
        return ok;
    }

  private:
    DBusMessageIter* parent_{nullptr};
    DBusMessageIter iter_{};
    bool open_{false};
};

std::string trackPath(std::int64_t trackId) {
    if (trackId <= 0)
        return kNoTrack;
    return std::string{kTrackPrefix} + std::to_string(trackId);
}

// MPRIS mpris:artUrl is a URI, not a bare path. Percent-encode everything
// except the path separators and RFC 3986 unreserved characters.
std::string fileUri(const std::string& path) {
    std::string uri = "file://";
    uri.reserve(path.size() + 16);
    for (const unsigned char c : path) {
        if (std::isalnum(c) != 0 || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            uri.push_back(static_cast<char>(c));
        } else {
            char encoded[4]{};
            std::snprintf(encoded, sizeof(encoded), "%%%02X", static_cast<unsigned int>(c));
            uri += encoded;
        }
    }
    return uri;
}

} // namespace

/**
 * @brief Linux media-session backend via MPRIS / D-Bus.
 * @ingroup kanariv_service
 *
 * Implements the MPRIS D-Bus Interface Specification v2.2:
 * - Session bus name: org.mpris.MediaPlayer2.kanariv (plus optional instance suffix)
 * - Object path: /org/mpris/MediaPlayer2
 * - Interfaces: org.mpris.MediaPlayer2, org.mpris.MediaPlayer2.Player
 * - org.freedesktop.DBus.Properties (Get, GetAll, Set)
 *
 * Runs a libdbus-1 dispatch thread that processes incoming method calls
 * (Play, Pause, Next, etc.) and property reads and forwards media-key actions
 * to MediaSession::Callbacks. State changes are published via
 * org.freedesktop.DBus.Properties.PropertiesChanged so MPRIS clients
 * (desktop media indicators, Sound Menu, playerctl, etc.) stay in sync.
 *
 * Implementation notes:
 * - dbus_threads_init_default() runs once before the connection is used.
 * - The backend owns a *private* bus connection (dbus_bus_get_private), so
 *   two services in one process cannot register the same object path on a
 *   shared libdbus connection, and closing it is well-defined.
 * - dbus_connection_set_exit_on_disconnect(FALSE) prevents a vanished
 *   session bus from terminating the daemon (or the test process).
 * - One thread performs all reading/dispatching with
 *   dbus_connection_read_write_dispatch(), the libdbus pattern for simple
 *   servers.
 * - Position is intentionally never sent in PropertiesChanged: the MPRIS
 *   specification says that signal is not emitted when Position changes.
 */
class LinuxImpl final : public Impl {
  public:
    LinuxImpl(std::string appName, std::string appIcon, bool enableNotifications,
              bool enableMediaKeys)
        : appName_(std::move(appName)) {
        (void)appIcon;
        (void)enableNotifications;
        (void)enableMediaKeys;
    }

    ~LinuxImpl() override {
        (void)deactivate();
    }

    kanariv::utils::Expected<void> activate() override {
        std::lock_guard lock(mtx_);
        if (activeState_)
            return {};

        // libdbus is only thread-safe once this has been called. It must run
        // before any connection is shared between the dispatch thread and the
        // service thread that publishes PropertiesChanged.
        static std::once_flag threadsOnce;
        std::call_once(threadsOnce, [] { (void)dbus_threads_init_default(); });

        DBusError error;
        dbus_error_init(&error);
        DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        if (!conn) {
            std::string msg = "failed to connect to session bus: ";
            if (error.message)
                msg += error.message;
            dbus_error_free(&error);
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
        }
        dbus_connection_set_exit_on_disconnect(conn, FALSE);

        // Prefer the canonical well-known name. If another player instance
        // already owns it, fall back to the instance name recommended by the
        // MPRIS specification instead of stealing the name from it.
        const char* primaryName = "org.mpris.MediaPlayer2.kanariv";
        std::string requestedName;
        int reply = dbus_bus_request_name(conn, primaryName, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
        if (reply == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            requestedName = primaryName;
        } else {
            if (dbus_error_is_set(&error)) {
                if (logger_)
                    logger_->debug("MPRIS: primary name unavailable: {}", error.message);
                dbus_error_free(&error);
                dbus_error_init(&error);
            }
            const std::string instanceName =
                std::string{primaryName} + ".instance" + std::to_string(::getpid());
            reply = dbus_bus_request_name(conn, instanceName.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                          &error);
            if (reply == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
                requestedName = instanceName;
        }

        if (requestedName.empty()) {
            std::string msg = "failed to request an MPRIS bus name";
            if (error.message)
                msg += ": " + std::string{error.message};
            dbus_error_free(&error);
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            return std::unexpected{
                kanariv::utils::makeError(kanariv::utils::StatusCode::Internal, msg)};
        }
        dbus_error_free(&error);

        static const DBusObjectPathVTable vtable = {
            .unregister_function = nullptr,
            .message_function = [](DBusConnection* connection, DBusMessage* message,
                                   void* userData) -> DBusHandlerResult {
                return static_cast<LinuxImpl*>(userData)->dispatchMethod(connection, message);
            },
            .dbus_internal_pad1 = nullptr,
            .dbus_internal_pad2 = nullptr,
            .dbus_internal_pad3 = nullptr,
            .dbus_internal_pad4 = nullptr,
        };
        if (!dbus_connection_register_object_path(conn, kObjectPath, &vtable, this)) {
            dbus_error_init(&error);
            dbus_bus_release_name(conn, requestedName.c_str(), &error);
            dbus_error_free(&error);
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Internal, "failed to register D-Bus object path")};
        }

        conn_ = conn;
        busName_ = requestedName;
        stopRequested_.store(false, std::memory_order_release);
        hasPublished_ = false;
        dispatchThread_ = std::thread([this, conn] { dispatchLoop(conn); });
        activeState_ = true;

        if (logger_)
            logger_->debug("MPRIS D-Bus backend activated as {}", busName_);
        return {};
    }

    kanariv::utils::Expected<void> deactivate() override {
        {
            std::lock_guard lock(mtx_);
            if (!activeState_)
                return {};
            activeState_ = false;
            stopRequested_.store(true, std::memory_order_release);
        }

        // Wake the dispatch loop; read_write_dispatch() uses a short timeout
        // so it observes stopRequested_ promptly. Join before touching the
        // connection so no handler can be running while it is closed.
        if (dispatchThread_.joinable())
            dispatchThread_.join();

        std::lock_guard lock(mtx_);
        // Embedded artwork is materialized into a temp file so MPRIS clients
        // can fetch it over file://; remove it with the backend.
        removeArtworkFileLocked();
        if (!conn_)
            return {};

        DBusConnection* conn = conn_;
        const std::string busName = busName_;
        conn_ = nullptr;
        busName_.clear();

        dbus_connection_unregister_object_path(conn, kObjectPath);
        DBusError error;
        dbus_error_init(&error);
        if (!busName.empty())
            dbus_bus_release_name(conn, busName.c_str(), &error);
        dbus_error_free(&error);
        dbus_connection_flush(conn);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return {};
    }

    kanariv::utils::Expected<void> setMetadata(const Metadata& metadata) override {
        std::lock_guard lock(mtx_);
        metadata_ = metadata;
        // Publishing is deferred to setPlaybackState(), which the service
        // always calls immediately after setMetadata() in syncMediaSession().
        // publishChangedLocked() diffs the complete snapshot, so metadata is
        // included in that same signal.
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackState(kanariv::types::PlaybackState state,
                                                    double position, double duration) override {
        std::lock_guard lock(mtx_);
        state_ = state;
        position_ = position;
        duration_ = duration;
        positionStamp_ = std::chrono::steady_clock::now();
        publishChangedLocked();
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackMode(bool shuffle,
                                                   kanariv::types::RepeatMode repeat) override {
        std::lock_guard lock(mtx_);
        shuffle_ = shuffle;
        repeat_ = repeat;
        publishChangedLocked();
        return {};
    }

    void setLogger(kanariv::utils::Logger* logger) noexcept override {
        logger_ = logger;
    }

    kanariv::utils::Expected<void> setVolume(float volume, bool muted) override {
        std::lock_guard lock(mtx_);
        volume_ = muted ? 0.0f : volume;
        publishChangedLocked();
        return {};
    }

    bool active() const noexcept override {
        std::lock_guard lock(mtx_);
        return activeState_;
    }

  private:
    struct StateSnapshot {
        kanariv::types::PlaybackState state{kanariv::types::PlaybackState::Stopped};
        double position{0.0};
        double duration{0.0};
        Metadata metadata{};
        bool shuffle{false};
        kanariv::types::RepeatMode repeat{kanariv::types::RepeatMode::Off};
        float volume{1.0f};
    };

    struct PlayerProperties {
        bool playbackStatus{false};
        bool loopStatus{false};
        bool rate{false};
        bool shuffle{false};
        bool metadata{false};
        bool volume{false};
        bool position{false};
        bool minimumRate{false};
        bool maximumRate{false};
        bool canGoNext{false};
        bool canGoPrevious{false};
        bool canPlay{false};
        bool canPause{false};
        bool canSeek{false};
        bool canControl{false};
    };

    static const char* playbackStatusString(kanariv::types::PlaybackState state) {
        switch (state) {
        case kanariv::types::PlaybackState::Playing:
            return "Playing";
        case kanariv::types::PlaybackState::Paused:
            return "Paused";
        default:
            return "Stopped";
        }
    }

    static const char* loopStatusString(kanariv::types::RepeatMode repeat) {
        switch (repeat) {
        case kanariv::types::RepeatMode::One:
            return "Track";
        case kanariv::types::RepeatMode::All:
            return "Playlist";
        default:
            return "None";
        }
    }

    static bool sameMetadata(const Metadata& left, const Metadata& right) {
        return left.title == right.title && left.artist == right.artist &&
               left.album == right.album && left.artworkPath == right.artworkPath &&
               left.artworkBase64 == right.artworkBase64 && left.artworkMime == right.artworkMime &&
               left.trackId == right.trackId;
    }

    // The daemon only pushes a position when playback state changes, so
    // MPRIS Get/GetAll("Position") has to extrapolate while playing.
    double effectivePositionLocked() const {
        double position = position_;
        if (state_ == kanariv::types::PlaybackState::Playing) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - positionStamp_).count();
            position += elapsed;
            if (duration_ > 0.0 && position > duration_)
                position = duration_;
        }
        return position > 0.0 ? position : 0.0;
    }

    StateSnapshot snapshotLocked() {
        materializeArtworkLocked();
        return StateSnapshot{.state = state_,
                             .position = effectivePositionLocked(),
                             .duration = duration_,
                             .metadata = metadata_,
                             .shuffle = shuffle_,
                             .repeat = repeat_,
                             .volume = volume_};
    }

    static std::string artworkExtension(const std::string& mime) {
        if (mime == "image/jpeg" || mime == "image/jpg")
            return ".jpg";
        if (mime == "image/png")
            return ".png";
        if (mime == "image/webp")
            return ".webp";
        if (mime == "image/gif")
            return ".gif";
        if (mime == "image/bmp")
            return ".bmp";
        return ".img";
    }

    // MPRIS mpris:artUrl is a URI, and the common clients only handle
    // file:// (or http://) URIs. Embedded cover art therefore has to be
    // written to a temporary file before it can be advertised. The file is
    // keyed by the artwork bytes so repeated syncs do not rewrite it.
    void materializeArtworkLocked() {
        if (metadata_.artworkPath && !metadata_.artworkPath->empty()) {
            // A real on-disk cover wins over any embedded copy. The one
            // exception is a previously materialized temp file that has been
            // removed (e.g. across deactivate/reactivate): regenerate it from
            // the embedded bytes instead of publishing a dead URI.
            const bool staleTemp =
                !artworkFilePath_.empty() &&
                artworkFilePath_.string() == *metadata_.artworkPath &&
                !std::filesystem::exists(artworkFilePath_) &&
                metadata_.artworkBase64 && !metadata_.artworkBase64->empty();
            if (!staleTemp) {
                removeArtworkFileLocked();
                return;
            }
        }

        const auto& base64 = metadata_.artworkBase64;
        if (!base64 || base64->empty()) {
            removeArtworkFileLocked();
            return;
        }

        if (*base64 == artworkKey_ && !artworkFilePath_.empty() &&
            std::filesystem::exists(artworkFilePath_)) {
            metadata_.artworkPath = artworkFilePath_.string();
            return;
        }

        const auto decoded = kanariv::utils::Base64::decode(*base64);
        if (!decoded || decoded->empty()) {
            removeArtworkFileLocked();
            return;
        }

        std::error_code error;
        const auto directory = std::filesystem::temp_directory_path(error);
        if (error) {
            removeArtworkFileLocked();
            return;
        }

        const std::string name =
            "kanariv-mpris-" + std::to_string(std::hash<std::string>{}(*base64)) +
            artworkExtension(metadata_.artworkMime);
        const auto path = directory / name;

        // Drop the previous temp file only when the new artwork lives at a
        // different path; removing the path we are about to (re)write would
        // delete the fresh file when the cached copy had vanished. Clearing
        // artworkPath too means a failed write publishes no artwork at all
        // instead of a URI pointing at a file we just deleted.
        if (!artworkFilePath_.empty() && artworkFilePath_ != path) {
            std::error_code removeError;
            std::filesystem::remove(artworkFilePath_, removeError);
        }
        artworkFilePath_.clear();
        artworkKey_.clear();
        metadata_.artworkPath.reset();

        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
                return;
            output.write(reinterpret_cast<const char*>(decoded->data()),
                         static_cast<std::streamsize>(decoded->size()));
            if (!output)
                return;
        }

        artworkKey_ = *base64;
        artworkFilePath_ = path;
        metadata_.artworkPath = artworkFilePath_.string();
    }

    void removeArtworkFileLocked() {
        if (artworkFilePath_.empty())
            return;
        std::error_code error;
        std::filesystem::remove(artworkFilePath_, error);
        // The temp file may still be the published mpris:artUrl; drop that
        // reference so clients are never handed a URI to a removed file.
        if (metadata_.artworkPath && *metadata_.artworkPath == artworkFilePath_.string())
            metadata_.artworkPath.reset();
        artworkFilePath_.clear();
        artworkKey_.clear();
    }

    // Copies the callback list so it can be invoked without holding mtx_.
    // Invoking a callback while holding the mutex would deadlock, because the
    // callback synchronously calls back into setPlaybackState()/setPlaybackMode().
    Callbacks copyCallbacks() const {
        std::lock_guard lock(mtx_);
        return callbacks_;
    }

    // ----- container/value helpers -----

    static bool appendVariantValue(DBusMessageIter& parent, const char* signature, int type,
                                   const void* value) {
        OpenContainer variant(parent, DBUS_TYPE_VARIANT, signature);
        if (!variant.opened())
            return false;
        if (!dbus_message_iter_append_basic(&variant.iter(), type, value))
            return false;
        return variant.close();
    }

    static bool appendVariantString(DBusMessageIter& parent, const char* value) {
        return appendVariantValue(parent, "s", DBUS_TYPE_STRING, &value);
    }

    static bool appendVariantObjectPath(DBusMessageIter& parent, const char* value) {
        return appendVariantValue(parent, "o", DBUS_TYPE_OBJECT_PATH, &value);
    }

    static bool appendVariantBool(DBusMessageIter& parent, bool value) {
        const dbus_bool_t dbusValue = value ? TRUE : FALSE;
        return appendVariantValue(parent, "b", DBUS_TYPE_BOOLEAN, &dbusValue);
    }

    static bool appendVariantDouble(DBusMessageIter& parent, double value) {
        return appendVariantValue(parent, "d", DBUS_TYPE_DOUBLE, &value);
    }

    static bool appendVariantInt64(DBusMessageIter& parent, dbus_int64_t value) {
        return appendVariantValue(parent, "x", DBUS_TYPE_INT64, &value);
    }

    template <typename AppendValue>
    static bool appendDictProperty(DBusMessageIter& dict, const char* name,
                                   AppendValue&& appendValue) {
        OpenContainer entry(dict, DBUS_TYPE_DICT_ENTRY, nullptr);
        if (!entry.opened())
            return false;
        const char* key = name;
        if (!dbus_message_iter_append_basic(&entry.iter(), DBUS_TYPE_STRING, &key))
            return false;
        if (!appendValue(entry.iter()))
            return false;
        return entry.close();
    }

    static bool appendStringProperty(DBusMessageIter& dict, const char* name, const char* value) {
        return appendDictProperty(dict, name, [value](DBusMessageIter& entry) {
            return appendVariantString(entry, value);
        });
    }

    static bool appendObjectPathProperty(DBusMessageIter& dict, const char* name,
                                         const char* value) {
        return appendDictProperty(dict, name, [value](DBusMessageIter& entry) {
            return appendVariantObjectPath(entry, value);
        });
    }

    static bool appendBoolProperty(DBusMessageIter& dict, const char* name, bool value) {
        return appendDictProperty(dict, name, [value](DBusMessageIter& entry) {
            return appendVariantBool(entry, value);
        });
    }

    static bool appendDoubleProperty(DBusMessageIter& dict, const char* name, double value) {
        return appendDictProperty(dict, name, [value](DBusMessageIter& entry) {
            return appendVariantDouble(entry, value);
        });
    }

    static bool appendInt64Property(DBusMessageIter& dict, const char* name, dbus_int64_t value) {
        return appendDictProperty(dict, name, [value](DBusMessageIter& entry) {
            return appendVariantInt64(entry, value);
        });
    }

    static bool appendStringArrayProperty(DBusMessageIter& dict, const char* name,
                                          const char* value) {
        return appendDictProperty(dict, name, [value](DBusMessageIter& entry) {
            OpenContainer variant(entry, DBUS_TYPE_VARIANT, "as");
            if (!variant.opened())
                return false;
            OpenContainer array(variant.iter(), DBUS_TYPE_ARRAY, "s");
            if (!array.opened())
                return false;
            if (!dbus_message_iter_append_basic(&array.iter(), DBUS_TYPE_STRING, &value))
                return false;
            if (!array.close())
                return false;
            return variant.close();
        });
    }

    static bool appendEmptyStringArrayProperty(DBusMessageIter& dict, const char* name) {
        return appendDictProperty(dict, name, [](DBusMessageIter& entry) {
            OpenContainer variant(entry, DBUS_TYPE_VARIANT, "as");
            if (!variant.opened())
                return false;
            OpenContainer array(variant.iter(), DBUS_TYPE_ARRAY, "s");
            if (!array.opened())
                return false;
            if (!array.close())
                return false;
            return variant.close();
        });
    }

    static bool appendVariantMetadata(DBusMessageIter& parent, const StateSnapshot& snapshot) {
        OpenContainer variant(parent, DBUS_TYPE_VARIANT, "a{sv}");
        if (!variant.opened())
            return false;
        OpenContainer dict(variant.iter(), DBUS_TYPE_ARRAY, "{sv}");
        if (!dict.opened())
            return false;
        if (!appendMetadataEntries(dict.iter(), snapshot))
            return false;
        if (!dict.close())
            return false;
        return variant.close();
    }

    static bool appendVariantEmptyStringArray(DBusMessageIter& parent) {
        OpenContainer variant(parent, DBUS_TYPE_VARIANT, "as");
        if (!variant.opened())
            return false;
        OpenContainer array(variant.iter(), DBUS_TYPE_ARRAY, "s");
        if (!array.opened())
            return false;
        if (!array.close())
            return false;
        return variant.close();
    }

    static bool appendMetadataEntries(DBusMessageIter& dict, const StateSnapshot& snapshot) {
        const std::string trackId = trackPath(snapshot.metadata.trackId);
        if (!appendObjectPathProperty(dict, "mpris:trackid", trackId.c_str()))
            return false;

        if (!snapshot.metadata.title.empty() &&
            !appendStringProperty(dict, "xesam:title", snapshot.metadata.title.c_str()))
            return false;

        if (!snapshot.metadata.artist.empty() &&
            !appendStringArrayProperty(dict, "xesam:artist", snapshot.metadata.artist.c_str()))
            return false;

        if (snapshot.metadata.album &&
            !appendStringProperty(dict, "xesam:album", snapshot.metadata.album->c_str()))
            return false;

        if (snapshot.metadata.artworkPath && !snapshot.metadata.artworkPath->empty()) {
            const std::string artUrl = fileUri(*snapshot.metadata.artworkPath);
            if (!appendStringProperty(dict, "mpris:artUrl", artUrl.c_str()))
                return false;
        }

        if (snapshot.duration > 0.0) {
            const dbus_int64_t lengthUs =
                static_cast<dbus_int64_t>(snapshot.duration * 1'000'000.0);
            if (!appendInt64Property(dict, "mpris:length", lengthUs))
                return false;
        }

        return true;
    }

    static bool appendMetadataProperty(DBusMessageIter& dict, const StateSnapshot& snapshot) {
        return appendDictProperty(dict, "Metadata", [&snapshot](DBusMessageIter& entry) {
            return appendVariantMetadata(entry, snapshot);
        });
    }

    static bool appendPlayerProperties(DBusMessageIter& dict, const StateSnapshot& snapshot,
                                       const PlayerProperties& include) {
        if (include.playbackStatus &&
            !appendStringProperty(dict, "PlaybackStatus", playbackStatusString(snapshot.state)))
            return false;
        if (include.loopStatus &&
            !appendStringProperty(dict, "LoopStatus", loopStatusString(snapshot.repeat)))
            return false;
        if (include.rate && !appendDoubleProperty(dict, "Rate", 1.0))
            return false;
        if (include.shuffle && !appendBoolProperty(dict, "Shuffle", snapshot.shuffle))
            return false;
        if (include.metadata && !appendMetadataProperty(dict, snapshot))
            return false;
        if (include.volume &&
            !appendDoubleProperty(dict, "Volume", static_cast<double>(snapshot.volume)))
            return false;
        if (include.position &&
            !appendInt64Property(dict, "Position",
                                 static_cast<dbus_int64_t>(snapshot.position * 1'000'000.0)))
            return false;
        if (include.minimumRate && !appendDoubleProperty(dict, "MinimumRate", 1.0))
            return false;
        if (include.maximumRate && !appendDoubleProperty(dict, "MaximumRate", 1.0))
            return false;
        if (include.canGoNext && !appendBoolProperty(dict, "CanGoNext", true))
            return false;
        if (include.canGoPrevious && !appendBoolProperty(dict, "CanGoPrevious", true))
            return false;
        if (include.canPlay && !appendBoolProperty(dict, "CanPlay", true))
            return false;
        if (include.canPause && !appendBoolProperty(dict, "CanPause", true))
            return false;
        if (include.canSeek && !appendBoolProperty(dict, "CanSeek", true))
            return false;
        if (include.canControl && !appendBoolProperty(dict, "CanControl", true))
            return false;
        return true;
    }

    static bool appendMediaPlayerProperties(DBusMessageIter& dict, const std::string& appName) {
        if (!appendStringProperty(dict, "Identity", appName.empty() ? "kanariv" : appName.c_str()))
            return false;
        if (!appendStringProperty(dict, "DesktopEntry", "kanariv"))
            return false;
        // The daemon is not a desktop application: it cannot raise a window
        // and quitting it would only drop the MPRIS name, not stop playback.
        if (!appendBoolProperty(dict, "CanQuit", false))
            return false;
        if (!appendBoolProperty(dict, "CanRaise", false))
            return false;
        if (!appendBoolProperty(dict, "HasTrackList", false))
            return false;
        if (!appendEmptyStringArrayProperty(dict, "SupportedUriSchemes"))
            return false;
        if (!appendEmptyStringArrayProperty(dict, "SupportedMimeTypes"))
            return false;
        return true;
    }

    // Returns nullopt when the property is not part of the Player interface.
    static std::optional<bool> appendPlayerPropertyValue(DBusMessageIter& parent, const char* name,
                                                         const StateSnapshot& snapshot) {
        if (std::strcmp(name, "PlaybackStatus") == 0)
            return appendVariantString(parent, playbackStatusString(snapshot.state));
        if (std::strcmp(name, "LoopStatus") == 0)
            return appendVariantString(parent, loopStatusString(snapshot.repeat));
        if (std::strcmp(name, "Rate") == 0)
            return appendVariantDouble(parent, 1.0);
        if (std::strcmp(name, "Shuffle") == 0)
            return appendVariantBool(parent, snapshot.shuffle);
        if (std::strcmp(name, "Metadata") == 0)
            return appendVariantMetadata(parent, snapshot);
        if (std::strcmp(name, "Volume") == 0)
            return appendVariantDouble(parent, static_cast<double>(snapshot.volume));
        if (std::strcmp(name, "Position") == 0)
            return appendVariantInt64(parent,
                                      static_cast<dbus_int64_t>(snapshot.position * 1'000'000.0));
        if (std::strcmp(name, "MinimumRate") == 0 || std::strcmp(name, "MaximumRate") == 0)
            return appendVariantDouble(parent, 1.0);
        if (std::strcmp(name, "CanGoNext") == 0 || std::strcmp(name, "CanGoPrevious") == 0 ||
            std::strcmp(name, "CanPlay") == 0 || std::strcmp(name, "CanPause") == 0 ||
            std::strcmp(name, "CanSeek") == 0 || std::strcmp(name, "CanControl") == 0)
            return appendVariantBool(parent, true);
        return std::nullopt;
    }

    // Returns nullopt when the property is not part of org.mpris.MediaPlayer2.
    static std::optional<bool> appendMediaPlayerPropertyValue(DBusMessageIter& parent,
                                                              const char* name,
                                                              const std::string& appName) {
        if (std::strcmp(name, "Identity") == 0)
            return appendVariantString(parent, appName.empty() ? "kanariv" : appName.c_str());
        if (std::strcmp(name, "DesktopEntry") == 0)
            return appendVariantString(parent, "kanariv");
        if (std::strcmp(name, "CanQuit") == 0 || std::strcmp(name, "CanRaise") == 0)
            return appendVariantBool(parent, false);
        if (std::strcmp(name, "HasTrackList") == 0)
            return appendVariantBool(parent, false);
        if (std::strcmp(name, "SupportedUriSchemes") == 0 ||
            std::strcmp(name, "SupportedMimeTypes") == 0)
            return appendVariantEmptyStringArray(parent);
        return std::nullopt;
    }

    // ----- PropertiesChanged -----

    void publishChangedLocked() {
        if (!conn_)
            return;

        const StateSnapshot now = snapshotLocked();
        PlayerProperties changed;
        bool any = false;

        if (!hasPublished_) {
            // First publish after activation: seed every client with the full
            // state. Position is deliberately excluded.
            changed.playbackStatus = true;
            changed.loopStatus = true;
            changed.rate = true;
            changed.shuffle = true;
            changed.metadata = true;
            changed.volume = true;
            changed.minimumRate = true;
            changed.maximumRate = true;
            changed.canGoNext = true;
            changed.canGoPrevious = true;
            changed.canPlay = true;
            changed.canPause = true;
            changed.canSeek = true;
            changed.canControl = true;
            any = true;
        } else {
            const auto mark = [&any](bool& flag, bool value) {
                flag = value;
                any = any || value;
            };
            mark(changed.playbackStatus, now.state != lastPublished_.state);
            mark(changed.loopStatus, now.repeat != lastPublished_.repeat);
            mark(changed.shuffle, now.shuffle != lastPublished_.shuffle);
            mark(changed.volume, now.volume != lastPublished_.volume);
            mark(changed.metadata, !sameMetadata(now.metadata, lastPublished_.metadata) ||
                                       now.duration != lastPublished_.duration);
        }

        if (!any)
            return;

        DBusMessage* signal =
            dbus_message_new_signal(kObjectPath, kPropertiesInterface, "PropertiesChanged");
        if (!signal)
            return;

        bool ok = false;
        do {
            DBusMessageIter iter;
            dbus_message_iter_init_append(signal, &iter);
            const char* iface = kPlayerInterface;
            if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface))
                break;

            OpenContainer changedDict(iter, DBUS_TYPE_ARRAY, "{sv}");
            if (!changedDict.opened())
                break;
            if (!appendPlayerProperties(changedDict.iter(), now, changed))
                break;
            if (!changedDict.close())
                break;

            OpenContainer invalidated(iter, DBUS_TYPE_ARRAY, "s");
            if (!invalidated.opened())
                break;
            if (!invalidated.close())
                break;

            ok = dbus_connection_send(conn_, signal, nullptr) != FALSE;
        } while (false);

        dbus_message_unref(signal);

        if (ok) {
            lastPublished_ = now;
            hasPublished_ = true;
        } else if (logger_) {
            logger_->warn("MPRIS: failed to emit PropertiesChanged");
        }
    }

    // ----- D-Bus method dispatch -----

    DBusHandlerResult dispatchMethod(DBusConnection* connection, DBusMessage* message) {
        const char* iface = dbus_message_get_interface(message);
        const char* method = dbus_message_get_member(message);
        if (!iface || !method)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        if (std::strcmp(iface, kIntrospectableInterface) == 0 &&
            std::strcmp(method, "Introspect") == 0) {
            return sendStringReply(connection, message, introspectionXml());
        }

        if (std::strcmp(iface, kPropertiesInterface) == 0) {
            if (std::strcmp(method, "Get") == 0)
                return handlePropertiesGet(connection, message);
            if (std::strcmp(method, "GetAll") == 0)
                return handlePropertiesGetAll(connection, message);
            if (std::strcmp(method, "Set") == 0)
                return handlePropertiesSet(connection, message);
            return sendError(connection, message, DBUS_ERROR_UNKNOWN_METHOD,
                             "Unknown Properties method");
        }

        if (std::strcmp(iface, kPlayerInterface) == 0)
            return handlePlayerMethod(connection, message, method);
        if (std::strcmp(iface, kMediaPlayerInterface) == 0)
            return handleMediaPlayerMethod(connection, message, method);

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusHandlerResult handlePlayerMethod(DBusConnection* connection, DBusMessage* message,
                                         const char* method) {
        const Callbacks cb = copyCallbacks();

        if (std::strcmp(method, "Play") == 0) {
            if (cb.onPlay)
                cb.onPlay();
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "Pause") == 0) {
            if (cb.onPause)
                cb.onPause();
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "PlayPause") == 0) {
            // Read state under the lock, then invoke the callback without it.
            // The callback synchronously calls back into setPlaybackState(),
            // which takes the same mutex.
            bool playing = false;
            {
                std::lock_guard lock(mtx_);
                playing = state_ == kanariv::types::PlaybackState::Playing;
            }
            if (playing) {
                if (cb.onPause)
                    cb.onPause();
            } else if (cb.onPlay) {
                cb.onPlay();
            }
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "Next") == 0) {
            if (cb.onNext)
                cb.onNext();
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "Previous") == 0) {
            if (cb.onPrev)
                cb.onPrev();
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "Stop") == 0) {
            if (cb.onStop)
                cb.onStop();
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "Seek") == 0) {
            DBusMessageIter iter;
            dbus_int64_t offsetUs = 0;
            if (dbus_message_iter_init(message, &iter) &&
                dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT64) {
                dbus_message_iter_get_basic(&iter, &offsetUs);
                if (cb.onSeek) {
                    cb.onSeek(static_cast<double>(offsetUs) / 1'000'000.0);
                    // The callback synchronously seeks and syncs the engine,
                    // so position_ now holds the post-seek position. MPRIS
                    // requires a Seeked signal whenever the position changes
                    // discontinuously.
                    double position = 0.0;
                    {
                        std::lock_guard lock(mtx_);
                        position = position_;
                    }
                    emitSeeked(connection, position);
                }
            }
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "SetPosition") == 0) {
            DBusMessageIter iter;
            const char* trackIdPath = nullptr;
            dbus_int64_t positionUs = 0;
            if (dbus_message_iter_init(message, &iter) &&
                dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
                dbus_message_iter_get_basic(&iter, &trackIdPath);
                if (dbus_message_iter_next(&iter) &&
                    dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT64) {
                    dbus_message_iter_get_basic(&iter, &positionUs);
                }
            }

            std::int64_t currentTrack = 0;
            {
                std::lock_guard lock(mtx_);
                currentTrack = metadata_.trackId;
            }
            const std::string currentTrackPath = trackPath(currentTrack);
            // Ignore calls for a stale track id, as required by the MPRIS spec.
            if (trackIdPath && currentTrack > 0 && currentTrackPath == trackIdPath &&
                cb.onSeekTo) {
                const double requested =
                    static_cast<double>(positionUs > 0 ? positionUs : 0) / 1'000'000.0;
                cb.onSeekTo(requested);
                double position = requested;
                {
                    std::lock_guard lock(mtx_);
                    position = position_;
                }
                emitSeeked(connection, position);
            }
            return sendReply(connection, message);
        }
        if (std::strcmp(method, "OpenUri") == 0) {
            // kanariv manages its own library; opening arbitrary URIs is not
            // supported.
            return sendError(connection, message, DBUS_ERROR_NOT_SUPPORTED,
                             "OpenUri is not supported");
        }

        return sendError(connection, message, DBUS_ERROR_UNKNOWN_METHOD, "Unknown Player method");
    }

    DBusHandlerResult handleMediaPlayerMethod(DBusConnection* connection, DBusMessage* message,
                                              const char* method) {
        if (std::strcmp(method, "Raise") == 0)
            return sendError(connection, message, DBUS_ERROR_NOT_SUPPORTED,
                             "Raise is not supported");
        if (std::strcmp(method, "Quit") == 0)
            return sendError(connection, message, DBUS_ERROR_NOT_SUPPORTED,
                             "Quit is not supported");
        return sendError(connection, message, DBUS_ERROR_UNKNOWN_METHOD,
                         "Unknown MediaPlayer2 method");
    }

    DBusHandlerResult handlePropertiesGet(DBusConnection* connection, DBusMessage* message) {
        const char* iface = nullptr;
        const char* property = nullptr;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
                                   &property, DBUS_TYPE_INVALID)) {
            dbus_error_free(&error);
            return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                             "Expected interface name and property name");
        }
        dbus_error_free(&error);

        if (std::strcmp(iface, kPlayerInterface) != 0 &&
            std::strcmp(iface, kMediaPlayerInterface) != 0) {
            return sendError(connection, message, DBUS_ERROR_UNKNOWN_INTERFACE,
                             "Unknown interface");
        }

        StateSnapshot snapshot;
        std::string appName;
        {
            std::lock_guard lock(mtx_);
            snapshot = snapshotLocked();
            appName = appName_;
        }

        DBusMessage* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;

        bool ok = false;
        bool known = false;
        do {
            DBusMessageIter iter;
            dbus_message_iter_init_append(reply, &iter);
            if (std::strcmp(iface, kPlayerInterface) == 0) {
                const auto result = appendPlayerPropertyValue(iter, property, snapshot);
                known = result.has_value();
                ok = result.value_or(false);
            } else if (std::strcmp(iface, kMediaPlayerInterface) == 0) {
                const auto result = appendMediaPlayerPropertyValue(iter, property, appName);
                known = result.has_value();
                ok = result.value_or(false);
            }
        } while (false);

        if (!known) {
            dbus_message_unref(reply);
            return sendError(connection, message, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
        }
        if (!ok) {
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        }

        const bool sent = dbus_connection_send(connection, reply, nullptr) != FALSE;
        dbus_message_unref(reply);
        return sent ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    DBusHandlerResult handlePropertiesGetAll(DBusConnection* connection, DBusMessage* message) {
        const char* iface = nullptr;
        DBusError error;
        dbus_error_init(&error);
        if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
            dbus_error_free(&error);
            return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                             "Expected interface name");
        }
        dbus_error_free(&error);

        const bool player = std::strcmp(iface, kPlayerInterface) == 0;
        const bool mediaPlayer = std::strcmp(iface, kMediaPlayerInterface) == 0;
        if (!player && !mediaPlayer)
            return sendError(connection, message, DBUS_ERROR_UNKNOWN_INTERFACE,
                             "Unknown interface");

        StateSnapshot snapshot;
        std::string appName;
        {
            std::lock_guard lock(mtx_);
            snapshot = snapshotLocked();
            appName = appName_;
        }

        DBusMessage* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;

        bool ok = false;
        do {
            DBusMessageIter iter;
            dbus_message_iter_init_append(reply, &iter);
            OpenContainer properties(iter, DBUS_TYPE_ARRAY, "{sv}");
            if (!properties.opened())
                break;
            if (player) {
                PlayerProperties all;
                all.playbackStatus = true;
                all.loopStatus = true;
                all.rate = true;
                all.shuffle = true;
                all.metadata = true;
                all.volume = true;
                all.position = true;
                all.minimumRate = true;
                all.maximumRate = true;
                all.canGoNext = true;
                all.canGoPrevious = true;
                all.canPlay = true;
                all.canPause = true;
                all.canSeek = true;
                all.canControl = true;
                if (!appendPlayerProperties(properties.iter(), snapshot, all))
                    break;
            } else {
                if (!appendMediaPlayerProperties(properties.iter(), appName))
                    break;
            }
            if (!properties.close())
                break;
            ok = true;
        } while (false);

        if (!ok) {
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        }

        const bool sent = dbus_connection_send(connection, reply, nullptr) != FALSE;
        dbus_message_unref(reply);
        return sent ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    DBusHandlerResult handlePropertiesSet(DBusConnection* connection, DBusMessage* message) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(message, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
            return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                             "Expected interface name");
        }
        const char* iface = nullptr;
        dbus_message_iter_get_basic(&iter, &iface);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
            return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                             "Expected property name");
        }
        const char* property = nullptr;
        dbus_message_iter_get_basic(&iter, &property);

        if (!dbus_message_iter_next(&iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
            return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                             "Expected property value");
        }
        DBusMessageIter variant;
        dbus_message_iter_recurse(&iter, &variant);

        if (std::strcmp(iface, kPlayerInterface) != 0) {
            if (std::strcmp(iface, kMediaPlayerInterface) == 0)
                return sendError(connection, message, DBUS_ERROR_PROPERTY_READ_ONLY,
                                 "Property is read-only");
            return sendError(connection, message, DBUS_ERROR_UNKNOWN_INTERFACE,
                             "Unknown interface");
        }

        if (std::strcmp(property, "LoopStatus") == 0) {
            if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRING) {
                return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                                 "LoopStatus expects a string");
            }
            const char* value = nullptr;
            dbus_message_iter_get_basic(&variant, &value);
            kanariv::types::RepeatMode repeat = kanariv::types::RepeatMode::Off;
            if (std::strcmp(value, "None") == 0) {
                repeat = kanariv::types::RepeatMode::Off;
            } else if (std::strcmp(value, "Track") == 0) {
                repeat = kanariv::types::RepeatMode::One;
            } else if (std::strcmp(value, "Playlist") == 0) {
                repeat = kanariv::types::RepeatMode::All;
            } else {
                return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                                 "Invalid LoopStatus value");
            }

            const Callbacks cb = copyCallbacks();
            if (cb.onRepeat) {
                cb.onRepeat(repeat);
            } else {
                std::lock_guard lock(mtx_);
                repeat_ = repeat;
                publishChangedLocked();
            }
            return sendReply(connection, message);
        }

        if (std::strcmp(property, "Shuffle") == 0) {
            if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BOOLEAN) {
                return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                                 "Shuffle expects a boolean");
            }
            dbus_bool_t value = FALSE;
            dbus_message_iter_get_basic(&variant, &value);
            const bool shuffle = value != FALSE;

            const Callbacks cb = copyCallbacks();
            if (cb.onShuffle) {
                cb.onShuffle(shuffle);
            } else {
                std::lock_guard lock(mtx_);
                shuffle_ = shuffle;
                publishChangedLocked();
            }
            return sendReply(connection, message);
        }

        if (std::strcmp(property, "Volume") == 0) {
            if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_DOUBLE) {
                return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                                 "Volume expects a double");
            }
            double value = 0.0;
            dbus_message_iter_get_basic(&variant, &value);
            if (!std::isfinite(value)) {
                return sendError(connection, message, DBUS_ERROR_INVALID_ARGS,
                                 "Invalid Volume value");
            }
            if (value < 0.0)
                value = 0.0;
            if (value > 1.0)
                value = 1.0;
            const float volume = static_cast<float>(value);

            const Callbacks cb = copyCallbacks();
            if (cb.onVolume) {
                cb.onVolume(volume, volume == 0.0f);
            } else {
                std::lock_guard lock(mtx_);
                volume_ = volume;
                publishChangedLocked();
            }
            return sendReply(connection, message);
        }

        if (std::strcmp(property, "Rate") == 0 ||
            std::strcmp(property, "PlaybackStatus") == 0 ||
            std::strcmp(property, "Metadata") == 0 || std::strcmp(property, "Position") == 0 ||
            std::strcmp(property, "MinimumRate") == 0 ||
            std::strcmp(property, "MaximumRate") == 0 || std::strcmp(property, "CanGoNext") == 0 ||
            std::strcmp(property, "CanGoPrevious") == 0 || std::strcmp(property, "CanPlay") == 0 ||
            std::strcmp(property, "CanPause") == 0 || std::strcmp(property, "CanSeek") == 0 ||
            std::strcmp(property, "CanControl") == 0) {
            return sendError(connection, message, DBUS_ERROR_PROPERTY_READ_ONLY,
                             "Property is read-only");
        }

        return sendError(connection, message, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
    }

    static void emitSeeked(DBusConnection* connection, double positionSeconds) {
        DBusMessage* signal =
            dbus_message_new_signal(kObjectPath, kPlayerInterface, "Seeked");
        if (!signal)
            return;
        dbus_int64_t positionUs = static_cast<dbus_int64_t>(
            (positionSeconds > 0.0 ? positionSeconds : 0.0) * 1'000'000.0);
        const bool ok = dbus_message_append_args(
            signal, DBUS_TYPE_INT64, &positionUs, DBUS_TYPE_INVALID);
        if (ok)
            dbus_connection_send(connection, signal, nullptr);
        dbus_message_unref(signal);
    }

    static DBusHandlerResult sendReply(DBusConnection* connection, DBusMessage* message) {
        DBusMessage* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        const bool ok = dbus_connection_send(connection, reply, nullptr) != FALSE;
        dbus_message_unref(reply);
        return ok ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    static DBusHandlerResult sendError(DBusConnection* connection, DBusMessage* message,
                                       const char* errorName, const char* errorMessage) {
        DBusMessage* error = dbus_message_new_error(message, errorName, errorMessage);
        if (!error)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        const bool ok = dbus_connection_send(connection, error, nullptr) != FALSE;
        dbus_message_unref(error);
        return ok ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    static DBusHandlerResult sendStringReply(DBusConnection* connection, DBusMessage* message,
                                             const char* value) {
        DBusMessage* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID)) {
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        }
        const bool ok = dbus_connection_send(connection, reply, nullptr) != FALSE;
        dbus_message_unref(reply);
        return ok ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    void dispatchLoop(DBusConnection* connection) {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            // read_write_dispatch() is the documented single-thread server
            // loop: it reads, writes queued replies/signals, and dispatches
            // method calls to the object path vtable. The short timeout lets
            // deactivate() join promptly.
            if (!dbus_connection_read_write_dispatch(connection, 100)) {
                if (logger_)
                    logger_->debug("MPRIS: D-Bus connection closed");
                break;
            }
        }
    }

    static const char* introspectionXml() {
        return R"xml(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect">
      <arg name="xml_data" type="s" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get">
      <arg name="interface_name" type="s" direction="in"/>
      <arg name="property_name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="GetAll">
      <arg name="interface_name" type="s" direction="in"/>
      <arg name="properties" type="a{sv}" direction="out"/>
    </method>
    <method name="Set">
      <arg name="interface_name" type="s" direction="in"/>
      <arg name="property_name" type="s" direction="in"/>
      <arg name="value" type="v" direction="in"/>
    </method>
    <signal name="PropertiesChanged">
      <arg name="interface_name" type="s"/>
      <arg name="changed_properties" type="a{sv}"/>
      <arg name="invalidated_properties" type="as"/>
    </signal>
  </interface>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Play"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Stop"/>
    <method name="Seek">
      <arg name="offset" type="x" direction="in"/>
    </method>
    <method name="SetPosition">
      <arg name="trackid" type="o" direction="in"/>
      <arg name="position" type="x" direction="in"/>
    </method>
    <method name="OpenUri">
      <arg name="uri" type="s" direction="in"/>
    </method>
    <signal name="Seeked">
      <arg name="position" type="x"/>
    </signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus" type="s" access="readwrite"/>
    <property name="Rate" type="d" access="read"/>
    <property name="Shuffle" type="b" access="readwrite"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
)xml";
    }

    // ----- members -----

    std::string appName_;

    mutable std::mutex mtx_;
    bool activeState_{false};
    DBusConnection* conn_{nullptr};
    std::string busName_;
    std::thread dispatchThread_;
    std::atomic<bool> stopRequested_{false};

    kanariv::types::PlaybackState state_{kanariv::types::PlaybackState::Stopped};
    double position_{0.0};
    double duration_{0.0};
    std::chrono::steady_clock::time_point positionStamp_{};
    Metadata metadata_{};
    bool shuffle_{false};
    kanariv::types::RepeatMode repeat_{kanariv::types::RepeatMode::Off};
    float volume_{1.0f};
    kanariv::utils::Logger* logger_{nullptr};

    StateSnapshot lastPublished_{};
    bool hasPublished_{false};

    // Cached temp file for embedded (base64) cover art, published as
    // mpris:artUrl. Removed on artwork change and on deactivate().
    std::filesystem::path artworkFilePath_;
    std::string artworkKey_;
};

MediaSession::ExpectedSession createLinux(std::string appName, std::string appIcon,
                                          bool enableNotifications, bool enableMediaKeys) {
    return MediaSession::wrap(std::make_unique<LinuxImpl>(std::move(appName), std::move(appIcon),
                                                          enableNotifications, enableMediaKeys));
}

} // namespace kanariv::media_session
