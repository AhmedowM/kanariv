#import <service/media_session.hpp>

#import <MediaPlayer/MPNowPlayingInfoCenter.h>
#import <MediaPlayer/MPRemoteCommandCenter.h>
#import <MediaPlayer/MPRemoteCommand.h>
#import <MediaPlayer/MPMediaItem.h>

#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace kanariv::media_session {

/**
 * @brief macOS media-session backend via MediaPlayer framework.
 * @ingroup kanariv_service
 *
 * Uses:
 * - MPNowPlayingInfoCenter for now-playing metadata / playback state
 *   (lock screen, Control Center, Touch Bar). On macOS the playback state is
 *   conveyed via MPNowPlayingInfoPropertyPlaybackRate (1.0 = playing,
 *   0.0 = paused/stopped), not a separate state key.
 * - MPRemoteCommandCenter for media-key events (play/pause/next/prev/stop).
 *
 * Compiled only on __APPLE__ with Objective-C++ (.mm).
 */
class MacImpl final : public Impl {
  public:
    MacImpl(std::string appName, std::string appIcon,
            bool enableNotifications, bool enableMediaKeys)
        : appName_(std::move(appName)),
          appIcon_(std::move(appIcon)),
          enableNotifications_(enableNotifications),
          enableMediaKeys_(enableMediaKeys) {}

    ~MacImpl() override { (void)deactivate(); }

    kanariv::utils::Expected<void> activate() override {
        std::lock_guard lock(mtx_);
        if (active_)
            return {};

        MPRemoteCommandCenter* commandCenter =
            [MPRemoteCommandCenter sharedCommandCenter];

        __block MacImpl* self = this;

        commandCenter.playCommand.enabled = YES;
        [commandCenter.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull) {
            (void)0;
            if (self) {
                auto cb = self->callbacks();
                if (cb.onPlay)
                    cb.onPlay();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.pauseCommand.enabled = YES;
        [commandCenter.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onPause)
                    cb.onPause();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.togglePlayPauseCommand.enabled = YES;
        [commandCenter.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull) {
            if (self) {
                std::lock_guard lock(self->mtx_);
                bool playing =
                    self->state_ == kanariv::types::PlaybackState::Playing;
                auto cb = self->callbacks();
                if (playing) {
                    if (cb.onPause)
                        cb.onPause();
                } else if (cb.onPlay) {
                    cb.onPlay();
                }
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.nextTrackCommand.enabled = YES;
        [commandCenter.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onNext)
                    cb.onNext();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.previousTrackCommand.enabled = YES;
        [commandCenter.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onPrev)
                    cb.onPrev();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.stopCommand.enabled = YES;
        [commandCenter.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onStop)
                    cb.onStop();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        active_ = true;

        if (logger_)
            logger_->debug("macOS media-session backend activated");
        return {};
    }

    kanariv::utils::Expected<void> deactivate() override {
        std::lock_guard lock(mtx_);
        if (!active_)
            return {};

        MPRemoteCommandCenter* commandCenter =
            [MPRemoteCommandCenter sharedCommandCenter];

        commandCenter.playCommand.enabled = NO;
        commandCenter.pauseCommand.enabled = NO;
        commandCenter.togglePlayPauseCommand.enabled = NO;
        commandCenter.nextTrackCommand.enabled = NO;
        commandCenter.previousTrackCommand.enabled = NO;
        commandCenter.stopCommand.enabled = NO;

        // Clear now-playing info.
        [[MPNowPlayingInfoCenter defaultCenter]
            setNowPlayingInfo:@{ }];

        active_ = false;
        return {};
    }

    kanariv::utils::Expected<void> setMetadata(const Metadata& metadata) override {
        std::lock_guard lock(mtx_);
        metadata_ = metadata;
        publishNowPlayingInfo();
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackState(
        kanariv::types::PlaybackState state, double position, double duration) override {
        std::lock_guard lock(mtx_);
        state_ = state;
        position_ = position;
        duration_ = duration;
        publishNowPlayingInfo();
        return {};
    }

    kanariv::utils::Expected<void> setPlaybackMode(
        bool shuffle, kanariv::types::RepeatMode repeat) override {
        std::lock_guard lock(mtx_);
        shuffle_ = shuffle;
        repeat_ = repeat;
        publishNowPlayingInfo();
        return {};
    }

    void setLogger(kanariv::utils::Logger* logger) noexcept override {
        logger_ = logger;
    }

    kanariv::utils::Expected<void> setVolume(float volume, bool muted) override {
        std::lock_guard lock(mtx_);
        volume_ = muted ? 0.0f : volume;
        // Volume is not exposed as an MPNowPlayingInfoCenter key on macOS;
        // it is applied via the audio engine instead.
        (void)volume_;
        return {};
    }

    bool active() const noexcept override {
        std::lock_guard lock(mtx_);
        return active_;
    }

  private:
    void publishNowPlayingInfo() {
        MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];

        NSMutableDictionary<NSString *, id>* info =
            [NSMutableDictionary dictionary];

        // Title / artist / album
        if (!metadata_.title.empty()) {
            info[MPMediaItemPropertyTitle] =
                [NSString stringWithUTF8String:metadata_.title.c_str()];
        }
        if (!metadata_.artist.empty()) {
            info[MPMediaItemPropertyArtist] =
                [NSString stringWithUTF8String:metadata_.artist.c_str()];
        }
        if (metadata_.album) {
            info[MPMediaItemPropertyAlbumTitle] =
                [NSString stringWithUTF8String:metadata_.album->c_str()];
        }

        // Playback rate: 1.0 = playing, 0.0 = paused/stopped.
        // This is how macOS MPNowPlayingInfoCenter conveys playback state.
        double rate = 0.0;
        switch (state_) {
            case kanariv::types::PlaybackState::Playing:
                rate = 1.0;
                break;
            default:
                rate = 0.0;
                break;
        }
        info[MPNowPlayingInfoPropertyPlaybackRate] = @(rate);

        // Position / duration (seconds)
        if (duration_ > 0.0) {
            info[MPMediaItemPropertyPlaybackDuration] = @(duration_);
        }
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position_);

        // Artwork
        if (metadata_.artworkBase64 && !metadata_.artworkBase64->empty()) {
            auto decoded = kanariv::utils::Base64::decode(*metadata_.artworkBase64);
            if (decoded && !decoded->empty()) {
                NSData* artworkData = [NSData dataWithBytes:decoded->data()
                                                     length:decoded->size()];
                MPMediaItemArtwork* artwork =
                    [[MPMediaItemArtwork alloc] initWithData:artworkData];
                info[MPMediaItemPropertyArtwork] = artwork;
            }
        }

        [center setNowPlayingInfo:info];

        if (logger_)
            logger_->debug("macOS now-playing info updated");
    }

    std::string appName_;
    std::string appIcon_;
    bool enableNotifications_{true};
    bool enableMediaKeys_{true};

    mutable std::mutex mtx_;
    bool active_{false};

    kanariv::types::PlaybackState state_{
        kanariv::types::PlaybackState::Stopped};
    double position_{0.0};
    double duration_{0.0};
    Metadata metadata_{};
    bool shuffle_{false};
    kanariv::types::RepeatMode repeat_{kanariv::types::RepeatMode::Off};
    float volume_{1.0f};
    kanariv::utils::Logger* logger_{nullptr};
};

MediaSession::ExpectedSession createMacOS(std::string appName,
                                           std::string appIcon,
                                           bool enableNotifications,
                                           bool enableMediaKeys) {
    return MediaSession::wrap(std::make_unique<MacImpl>(
        std::move(appName), std::move(appIcon), enableNotifications,
        enableMediaKeys));
}

} // namespace kanariv::media_session
