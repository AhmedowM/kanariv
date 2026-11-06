#include <service/media_session.hpp>

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <string>

namespace kanariv::media_session {

/**
 * @brief Linux media-session backend via MPRIS / D-Bus (stub).
 * @ingroup kanariv_service
 *
 * Real implementation would:
 * - Connect to the session D-Bus (sd-bus or libdbus-1)
 * - Request the well-known name `org.mpris.MediaPlayer2.kanariv`
 * - Export object at `/org/mpris/MediaPlayer2`
 * - Implement `org.mpris.MediaPlayer2.Player`:
 *     Methods: Play, Pause, PlayPause, Next, Previous, Stop, Seek,
 *              SetPosition, OpenUri
 *     Properties: PlaybackStatus, Metadata, Rate, Volume, Position
 * - Implement `org.mpris.MediaPlayer2`: Raise, Quit
 * - Forward method calls to MediaSession::Callbacks -> engine commands
 *
 * Compiled only on __linux__.
 */
class LinuxImpl : public Impl {
  public:
    kanariv::utils::Expected<void> activate() override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "Linux MPRIS media-session backend not yet implemented")};
    }
    kanariv::utils::Expected<void> deactivate() override { return {}; }
    kanariv::utils::Expected<void> setMetadata(const Metadata&) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "Linux MPRIS media-session backend not yet implemented")};
    }
    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState, double, double) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "Linux MPRIS media-session backend not yet implemented")};
    }
    kanariv::utils::Expected<void> setVolume(float, bool) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "Linux MPRIS media-session backend not yet implemented")};
    }
    bool active() const noexcept override { return false; }
};

MediaSession::ExpectedSession createLinux(std::string appName,
                                           std::string appIcon,
                                           bool enableNotifications,
                                           bool enableMediaKeys) {
    (void)appName;
    (void)appIcon;
    (void)enableNotifications;
    (void)enableMediaKeys;
    return MediaSession::wrap(std::make_unique<LinuxImpl>());
}

} // namespace kanariv::media_session
