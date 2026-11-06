#import <service/media_session.hpp>

#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>
#include <memory>
#include <string>

namespace kanariv::media_session {

/**
 * @brief macOS media-session backend (stub).
 * @ingroup kanariv_service
 *
 * Real implementation would use:
 * - MPNowPlayingInfoCenter for now-playing metadata / playback state
 * - MPRemoteCommandCenter for media-key events (play/pause/next/prev/seek)
 * - Compiled only under Apple Clang (Objective-C++).
 *
 * Compiled only on __APPLE__.
 */
class MacImpl : public Impl {
  public:
    kanariv::utils::Expected<void> activate() override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "macOS media-session backend not yet implemented")};
    }
    kanariv::utils::Expected<void> deactivate() override { return {}; }
    kanariv::utils::Expected<void> setMetadata(const Metadata&) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "macOS media-session backend not yet implemented")};
    }
    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState, double, double) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "macOS media-session backend not yet implemented")};
    }
    kanariv::utils::Expected<void> setVolume(float, bool) override {
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::Unsupported,
            "macOS media-session backend not yet implemented")};
    }
    bool active() const noexcept override { return false; }
};

MediaSession::ExpectedSession createMacOS(std::string appName,
                                           std::string appIcon,
                                           bool enableNotifications,
                                           bool enableMediaKeys) {
    (void)appName;
    (void)appIcon;
    (void)enableNotifications;
    (void)enableMediaKeys;
    return MediaSession::wrap(std::make_unique<MacImpl>());
}

} // namespace kanariv::media_session
