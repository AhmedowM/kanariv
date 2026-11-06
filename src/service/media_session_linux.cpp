#include <service/media_session.hpp>

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>

#include <dbus/dbus.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kanariv::media_session {

/**
 * @brief Linux media-session backend via MPRIS / D-Bus.
 * @ingroup kanariv_service
 *
 * Implements the MPRIS D-Bus Interface Specification v2.2:
 * - Session bus name: org.mpris.MediaPlayer2.kanariv (plus optional instance suffix)
 * - Object path: /org/mpris/MediaPlayer2
 * - Interfaces: org.mpris.MediaPlayer2, org.mpris.MediaPlayer2.Player
 *
 * Runs a libdbus-1 dispatch thread that processes incoming method calls
 * (Play, Pause, Next, etc.) and forwards them to MediaSession::Callbacks.
 * Publish state changes via org.freedesktop.DBus.Properties.PropertiesChanged
 * signals so MPRIS clients (desktop media indicators, Sound Menu, etc.) stay
 * in sync with kanariv's playback state.
 */
class LinuxImpl final : public Impl {
  public:
    LinuxImpl(std::string appName, std::string appIcon,
              bool enableNotifications, bool enableMediaKeys)
        : appName_(std::move(appName)),
          appIcon_(std::move(appIcon)),
          enableNotifications_(enableNotifications),
          enableMediaKeys_(enableMediaKeys) {}

    ~LinuxImpl() override { (void)deactivate(); }

    kanariv::utils::Expected<void> activate() override {
        std::lock_guard lock(mtx_);
        if (active_)
            return {};

        // Connect to the session bus.
        DBusError error;
        dbus_error_init(&error);
        conn_ = dbus_bus_get(DBUS_BUS_SESSION, &error);
        if (!conn_) {
            std::string msg = "failed to connect to session bus: ";
            if (error.message)
                msg += error.message;
            dbus_error_free(&error);
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Internal, msg)};
        }

        // Request the well-known MPRIS name.
        const char* name = "org.mpris.MediaPlayer2.kanariv";
        int req = dbus_bus_request_name(conn_, name, DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
        if (req != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            std::string msg = "failed to request bus name ";
            msg += name;
            if (error.message)
                msg += ": " + std::string(error.message);
            dbus_error_free(&error);
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Internal, msg)};
        }

        // Register the object path.
        static const DBusObjectPathVTable vtable = {
            nullptr,                                      // unregister_function
            nullptr,                                      // get_all_properties
            &LinuxImpl::handle_method_call,               // message_function
            nullptr,                                      // dispose_function
            nullptr, nullptr, nullptr, nullptr,           // dbus_internal_pad1-4
        };
        if (!dbus_connection_register_object_path(conn_, "/org/mpris/MediaPlayer2",
                                                   &vtable, this)) {
            return std::unexpected{kanariv::utils::makeError(
                kanariv::utils::StatusCode::Internal,
                "failed to register D-Bus object path")};
        }

        stopRequested_.store(false, std::memory_order_release);
        dispatchThread_ = std::thread([this] { dispatchLoop(); });
        active_ = true;

        if (logger_)
            logger_->debug("MPRIS D-Bus backend activated on session bus");
        return {};
    }

    kanariv::utils::Expected<void> deactivate() override {
        {
            std::lock_guard lock(mtx_);
            if (!active_)
                return {};

            stopRequested_.store(true, std::memory_order_release);
        }

        if (dispatchThread_.joinable()) {
            // Wake the dispatch loop so it can observe stopRequested_.
            if (conn_)
                dbus_connection_flush(conn_);
            dispatchThread_.join();
        }

        if (conn_) {
            dbus_connection_unregister_object_path(conn_, "/org/mpris/MediaPlayer2");
            dbus_bus_release_name(conn_, "org.mpris.MediaPlayer2.kanariv", &error);
            dbus_connection_close(conn_);
            dbus_connection_unref(conn_);
            conn_ = nullptr;
        }

        std::lock_guard lock(mtx_);
        active_ = false;
        return {};
    }

    kanariv::utils::Expected<void> setMetadata(const Metadata& metadata) override {
        std::lock_guard lock(mtx_);
        metadata_ = metadata;
        // PublishChanged is deferred to setPlaybackState / setPlaybackMode /
        // setVolume which are always called together from syncMediaSession().
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState state, double position, double duration) override {
        std::lock_guard lock(mtx_);
        state_ = state;
        position_ = position;
        duration_ = duration;
        publishChanged();
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackMode(
        bool shuffle, kanariv::types::RepeatMode repeat) override {
        std::lock_guard lock(mtx_);
        shuffle_ = shuffle;
        repeat_ = repeat;
        publishChanged();
        return {};
    }

    void setLogger(kanariv::utils::Logger* logger) noexcept override {
        logger_ = logger;
    }

    kanariv::utils::Expected<void> setVolume(float volume, bool muted) override {
        std::lock_guard lock(mtx_);
        volume_ = muted ? 0.0f : volume;
        publishChanged();
        return {};
    }

    bool active() const noexcept override {
        std::lock_guard lock(mtx_);
        return active_;
    }

  private:
    // ----- MPRIS property value helpers -----

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

    // Publish all current state as PropertiesChanged on both MPRIS interfaces.
    void publishChanged() {
        if (!conn_)
            return;

        DBusMessage* sig = dbus_message_new_signal(
            "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties", "PropertiesChanged");
        if (!sig)
            return;

        // Build: interface_name (s), changed_properties (a{sv}), invalidated (as)
        DBusMessageIter iter;
        dbus_message_iter_init_append(sig, &iter);

        // interface_name: "org.mpris.MediaPlayer2.Player"
        if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
                                            &playerInterface_))
            goto fail;

        // changed_properties: a{sv}
        DBusMessageIter dict;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                              "{sv}", &dict))
            goto fail;

        // PlaybackStatus
        appendProperty(dict, "PlaybackStatus", playbackStatusString(state_));

        // LoopStatus
        appendProperty(dict, "LoopStatus", loopStatusString(repeat_));

        // Rate
        {
            double rate = 1.0;
            appendProperty(dict, "Rate", rate);
        }

        // Shuffle
        {
            dbus_bool_t shuffle = shuffle_ ? TRUE : FALSE;
            appendProperty(dict, "Shuffle", shuffle);
        }

        // Volume
        {
            double volume = static_cast<double>(volume_);
            appendProperty(dict, "Volume", volume);
        }

        // Metadata (a{sv})
        appendMetadataProperty(dict);

        // Position (int64 microseconds)
        {
            dbus_int64_t positionUs =
                static_cast<dbus_int64_t>(position_ * 1'000'000.0);
            appendProperty(dict, "Position", positionUs);
        }

        if (!dbus_message_iter_close_container(&iter, &dict))
            goto fail;

        // invalidated: as (empty array)
        DBusMessageIter invalidated;
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s",
                                              &invalidated))
            goto fail;
        if (!dbus_message_iter_close_container(&iter, &invalidated))
            goto fail;

        if (!dbus_connection_send(conn_, sig, nullptr))
            goto fail;
        dbus_connection_flush(conn_);

        dbus_message_unref(sig);
        return;

    fail:
        dbus_message_unref(sig);
        if (logger_)
            logger_->warn("MPRIS: failed to emit PropertiesChanged");
    }

    template <typename T>
    bool appendProperty(DBusMessageIter& dict, const char* name, T value) {
        DBusMessageIter entry;
        if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY,
                                              nullptr, &entry))
            return false;
        if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name))
            return false;
        DBusMessageIter variant;
        if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                              dbusSignature<T>(), &variant))
            return false;
        if (!dbus_message_iter_append_basic(&variant, dbusType<T>(), &value))
            return false;
        if (!dbus_message_iter_close_container(&variant, &entry))
            return false;
        if (!dbus_message_iter_close_container(&dict, &entry))
            return false;
        return true;
    }

    // Metadata property: build an a{sv} variant with mpris:trackid, xesam:title,
    // xesam:artist, xesam:album, mpris:artUrl, xesam:trackNumber.
    void appendMetadataProperty(DBusMessageIter& dict) {
        DBusMessageIter entry;
        if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY,
                                              nullptr, &entry))
            return;
        if (!dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &metadataKey_))
            return;
        DBusMessageIter variant;
        if (!dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                              "a{sv}", &variant))
            return;
        DBusMessageIter mdDict;
        if (!dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
                                              "{sv}", &mdDict))
            return;

        // mpris:trackid (object_path) - required
        {
            std::string trackId = "/org/mpris/MediaPlayer2/Track/" +
                                  std::to_string(metadata_.trackId);
            const char* trackIdPath = trackId.c_str();
            DBusMessageIter e2;
            if (dbus_message_iter_open_container(&mdDict, DBUS_TYPE_DICT_ENTRY,
                                                  nullptr, &e2)) {
                const char* k = "mpris:trackid";
                if (dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k)) {
                    DBusMessageIter v2;
                    if (dbus_message_iter_open_container(
                            &v2, DBUS_TYPE_VARIANT, "o", &v2)) {
                        if (dbus_message_iter_append_basic(
                                &v2, DBUS_TYPE_OBJECT_PATH, &trackIdPath)) {
                            dbus_message_iter_close_container(&v2, &e2);
                            dbus_message_iter_close_container(&mdDict, &e2);
                        } else {
                            dbus_message_iter_abandon_container(&v2, &e2);
                            dbus_message_iter_abandon_container(&mdDict, &e2);
                        }
                    } else {
                        dbus_message_iter_abandon_container(&e2, &mdDict);
                        dbus_message_iter_abandon_container(&mdDict, &variant);
                    }
                } else {
                    dbus_message_iter_abandon_container(&e2, &mdDict);
                    dbus_message_iter_abandon_container(&mdDict, &variant);
                }
            }
        }

        // xesam:title
        if (!metadata_.title.empty()) {
            DBusMessageIter e2;
            if (dbus_message_iter_open_container(&mdDict, DBUS_TYPE_DICT_ENTRY,
                                                  nullptr, &e2)) {
                const char* k = "xesam:title";
                dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k);
                DBusMessageIter v2;
                dbus_message_iter_open_container(&v2, DBUS_TYPE_VARIANT, "s", &v2);
                dbus_message_iter_append_basic(&v2, DBUS_TYPE_STRING,
                                               metadata_.title.c_str());
                dbus_message_iter_close_container(&v2, &e2);
                dbus_message_iter_close_container(&mdDict, &e2);
            }
        }

        // xesam:artist (as - array of strings)
        if (!metadata_.artist.empty()) {
            DBusMessageIter e2;
            if (dbus_message_iter_open_container(&mdDict, DBUS_TYPE_DICT_ENTRY,
                                                  nullptr, &e2)) {
                const char* k = "xesam:artist";
                dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k);
                DBusMessageIter v2;
                dbus_message_iter_open_container(&v2, DBUS_TYPE_VARIANT, "as", &v2);
                DBusMessageIter arr;
                dbus_message_iter_open_container(&arr, DBUS_TYPE_ARRAY, "s", &arr);
                const char* artist = metadata_.artist.c_str();
                dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &artist);
                dbus_message_iter_close_container(&arr, &v2);
                dbus_message_iter_close_container(&v2, &e2);
                dbus_message_iter_close_container(&mdDict, &e2);
            }
        }

        // xesam:album
        if (metadata_.album) {
            DBusMessageIter e2;
            if (dbus_message_iter_open_container(&mdDict, DBUS_TYPE_DICT_ENTRY,
                                                  nullptr, &e2)) {
                const char* k = "xesam:album";
                dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k);
                DBusMessageIter v2;
                dbus_message_iter_open_container(&v2, DBUS_TYPE_VARIANT, "s", &v2);
                dbus_message_iter_append_basic(&v2, DBUS_TYPE_STRING,
                                               metadata_.album->c_str());
                dbus_message_iter_close_container(&v2, &e2);
                dbus_message_iter_close_container(&mdDict, &e2);
            }
        }

        // mpris:artUrl (file:// URL to artwork)
        if (metadata_.artworkPath && !metadata_.artworkPath->empty()) {
            DBusMessageIter e2;
            if (dbus_message_iter_open_container(&mdDict, DBUS_TYPE_DICT_ENTRY,
                                                  nullptr, &e2)) {
                const char* k = "mpris:artUrl";
                dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k);
                DBusMessageIter v2;
                dbus_message_iter_open_container(&v2, DBUS_TYPE_VARIANT, "s", &v2);
                std::string artUrl = "file://" + *metadata_.artworkPath;
                dbus_message_iter_append_basic(&v2, DBUS_TYPE_STRING,
                                               artUrl.c_str());
                dbus_message_iter_close_container(&v2, &e2);
                dbus_message_iter_close_container(&mdDict, &e2);
            }
        }

        // xesam:trackNumber
        if (metadata_.trackId > 0) {
            DBusMessageIter e2;
            if (dbus_message_iter_open_container(&mdDict, DBUS_TYPE_DICT_ENTRY,
                                                  nullptr, &e2)) {
                const char* k = "xesam:trackNumber";
                dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k);
                DBusMessageIter v2;
                dbus_message_iter_open_container(&v2, DBUS_TYPE_VARIANT, "u", &v2);
                dbus_uint32_t trackNum = static_cast<dbus_uint32_t>(metadata_.trackId);
                dbus_message_iter_append_basic(&v2, DBUS_TYPE_UINT32, &trackNum);
                dbus_message_iter_close_container(&v2, &e2);
                dbus_message_iter_close_container(&mdDict, &e2);
            }
        }

        dbus_message_iter_close_container(&mdDict, &variant);
        dbus_message_iter_close_container(&variant, &entry);
        dbus_message_iter_close_container(&dict, &entry);
    }

    // ----- D-Bus method dispatch -----

    static DBusHandlerResult handle_method_call(DBusConnection* connection,
                                                 DBusMessage* message,
                                                 void* user_data) {
        auto* self = static_cast<LinuxImpl*>(user_data);
        return self->dispatchMethod(connection, message);
    }

    DBusHandlerResult dispatchMethod(DBusConnection* connection,
                                      DBusMessage* message) {
        const char* iface = dbus_message_get_interface(message);
        const char* method = dbus_message_get_member(message);

        if (!iface || !method)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        Callbacks cb;
        {
            std::lock_guard lock(mtx_);
            cb = callbacks_;
        }

        if (std::strcmp(iface, "org.mpris.MediaPlayer2.Player") == 0) {
            if (std::strcmp(method, "Play") == 0) {
                if (cb.onPlay) cb.onPlay();
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "Pause") == 0) {
                if (cb.onPause) cb.onPause();
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "PlayPause") == 0) {
                std::lock_guard lock(mtx_);
                bool playing = state_ == kanariv::types::PlaybackState::Playing;
                if (playing) {
                    if (cb.onPause) cb.onPause();
                } else if (cb.onPlay) {
                    cb.onPlay();
                }
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "Next") == 0) {
                if (cb.onNext) cb.onNext();
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "Previous") == 0) {
                if (cb.onPrev) cb.onPrev();
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "Stop") == 0) {
                if (cb.onStop) cb.onStop();
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "Seek") == 0) {
                DBusMessageIter iter;
                dbus_message_iter_init(message, &iter);
                dbus_int64_t offsetUs = 0;
                if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT64)
                    dbus_message_iter_get_basic(&iter, &offsetUs);
                double offsetSec = static_cast<double>(offsetUs) / 1'000'000.0;
                if (cb.onSeek) cb.onSeek(offsetSec);
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "SetPosition") == 0) {
                // (o: TrackId, x: Position microseconds)
                DBusMessageIter iter;
                dbus_message_iter_init(message, &iter);
                if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
                    const char* trackIdPath = nullptr;
                    dbus_message_iter_get_basic(&iter, &trackIdPath);
                    dbus_message_iter_next(&iter);
                    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT64) {
                        dbus_int64_t positionUs = 0;
                        dbus_message_iter_get_basic(&iter, &positionUs);
                        double positionSec =
                            static_cast<double>(positionUs) / 1'000'000.0;
                        if (cb.onSeekTo) cb.onSeekTo(positionSec);
                    }
                }
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "OpenUri") == 0) {
                // Not supported: kanariv manages its own library.
                return sendError(connection, message, "org.freedesktop.DBus.Error.NotSupported",
                                 "OpenUri not supported");
            }
        }

        if (std::strcmp(iface, "org.mpris.MediaPlayer2") == 0) {
            if (std::strcmp(method, "Raise") == 0) {
                return sendReply(connection, message);
            }
            if (std::strcmp(method, "Quit") == 0) {
                if (cb.onStop) cb.onStop();
                return sendReply(connection, message);
            }
        }

        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    static DBusHandlerResult sendReply(DBusConnection* connection,
                                        DBusMessage* message) {
        DBusMessage* reply = dbus_message_new_method_return(message);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        bool ok = dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return ok ? DBUS_HANDLER_RESULT_HANDLED
                  : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    static DBusHandlerResult sendError(DBusConnection* connection,
                                        DBusMessage* message,
                                        const char* errorName,
                                        const char* errorMsg) {
        DBusMessage* err = dbus_message_new_error(message, errorName, errorMsg);
        if (!err)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;
        bool ok = dbus_connection_send(connection, err, nullptr);
        dbus_message_unref(err);
        return ok ? DBUS_HANDLER_RESULT_HANDLED
                  : DBUS_HANDLER_RESULT_NEED_MEMORY;
    }

    void dispatchLoop() {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (!conn_)
                break;
            // Non-blocking dispatch with a short timeout so we can check the
            // stop flag regularly.
            dbus_connection_read_write(conn_, 100);
            DBusMessage* msg = nullptr;
            while ((msg = dbus_connection_pop_message(conn_)) != nullptr) {
                if (!dbus_connection_dispatch(conn_)) {
                    // No handler registered for this message; ignore.
                }
                dbus_message_unref(msg);
            }
        }
    }

    // ----- D-Bus type signature helpers (primary templates) -----
    template <typename T>
    static const char* dbusSignature() { return "?"; }

    template <typename T>
    static int dbusType() { return DBUS_TYPE_INVALID; }

    // ----- members -----

    std::string appName_;
    std::string appIcon_;
    bool enableNotifications_{true};
    bool enableMediaKeys_{true};

    mutable std::mutex mtx_;
    bool active_{false};
    DBusConnection* conn_{nullptr};
    std::thread dispatchThread_;
    std::atomic<bool> stopRequested_{false};

    kanariv::types::PlaybackState state_{
        kanariv::types::PlaybackState::Stopped};
    double position_{0.0};
    double duration_{0.0};
    Metadata metadata_{};
    bool shuffle_{false};
    kanariv::types::RepeatMode repeat_{kanariv::types::RepeatMode::Off};
    float volume_{1.0f};
    kanariv::utils::Logger* logger_{nullptr};

    inline static constexpr const char* playerInterface_ =
        "org.mpris.MediaPlayer2.Player";
    inline static constexpr const char* metadataKey_ = "Metadata";
};

// ----- D-Bus type signature helpers for variant encoding -----
// Explicit specializations must be at namespace scope, not inside the class.

template <>
inline const char* LinuxImpl::dbusSignature<const char*>() { return "s"; }
template <>
inline int LinuxImpl::dbusType<const char*>() { return DBUS_TYPE_STRING; }

template <>
inline const char* LinuxImpl::dbusSignature<double>() { return "d"; }
template <>
inline int LinuxImpl::dbusType<double>() { return DBUS_TYPE_DOUBLE; }

template <>
inline const char* LinuxImpl::dbusSignature<dbus_bool_t>() { return "b"; }
template <>
inline int LinuxImpl::dbusType<dbus_bool_t>() { return DBUS_TYPE_BOOLEAN; }

template <>
inline const char* LinuxImpl::dbusSignature<dbus_int64_t>() { return "x"; }
template <>
inline int LinuxImpl::dbusType<dbus_int64_t>() { return DBUS_TYPE_INT64; }

template <>
inline const char* LinuxImpl::dbusSignature<dbus_uint32_t>() { return "u"; }
template <>
inline int LinuxImpl::dbusType<dbus_uint32_t>() { return DBUS_TYPE_UINT32; }

MediaSession::ExpectedSession createLinux(std::string appName,
                                           std::string appIcon,
                                           bool enableNotifications,
                                           bool enableMediaKeys) {
    return MediaSession::wrap(std::make_unique<LinuxImpl>(
        std::move(appName), std::move(appIcon), enableNotifications,
        enableMediaKeys));
}

} // namespace kanariv::media_session
