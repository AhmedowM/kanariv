#import <service/media_session.hpp>

#import <MediaPlayer/MPNowPlayingInfoCenter.h>
#import <MediaPlayer/MPRemoteCommandCenter.h>
#import <MediaPlayer/MPRemoteCommand.h>
#import <MediaPlayer/MPMediaItem.h>
#import <objc/runtime.h>

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
 *   (lock screen, Control Center, Touch Bar)
 * - MPRemoteCommandCenter for media-key events (play/pause/next/prev/seek)
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

        // Register command handlers. Each block captures `this` via a
        // __block variable so it can read callbacks_ from the C++ object.
        __block MacImpl* self = this;

        commandCenter.playCommand.enabled = YES;
        [commandCenter.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull event) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onPlay)
                    cb.onPlay();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.pauseCommand.enabled = YES;
        [commandCenter.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull event) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onPause)
                    cb.onPause();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.togglePlayPauseCommand.enabled = YES;
        [commandCenter.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull event) {
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
            MPRemoteCommandEvent* _Nonnull event) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onNext)
                    cb.onNext();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.previousTrackCommand.enabled = YES;
        [commandCenter.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull event) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onPrev)
                    cb.onPrev();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        commandCenter.stopCommand.enabled = YES;
        [commandCenter.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
            MPRemoteCommandEvent* _Nonnull event) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onStop)
                    cb.onStop();
            }
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        // Change playback position (skip forward/backward by interval).
        commandCenter.changePlaybackPositionCommand.enabled = YES;
        [commandCenter.changePlaybackPositionCommand
            addTargetWithHandler:^MPRemoteCommandHandlerStatus(
                MPRemoteCommandEvent* _Nonnull event) {
            if (self) {
                auto cb = self->callbacks();
                if (cb.onSeek) {
                    // MPChangePlaybackPositionCommandEvent carries the
                    // requested offset in seconds.
                    NSTimeInterval interval =
                        [(MPChangePlaybackPositionCommandEvent*)event positionTime];
                    cb.onSeek(interval);
                }
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
        commandCenter.changePlaybackPositionCommand.enabled = NO;

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
        publishNowPlayingInfo();
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

        // Playback state
        NSNumber* playbackState;
        switch (state_) {
            case kanariv::types::PlaybackState::Playing:
                playbackState = @(MPNowPlayingPlaybackStatePlaying);
                break;
            case kanariv::types::PlaybackState::Paused:
                playbackState = @(MPNowPlayingPlaybackStatePaused);
                break;
            default:
                playbackState = @(MPNowPlayingPlaybackStateStopped);
                break;
        }
        info[MPNowPlayingInfoPropertyPlaybackState] = playbackState;

        // Position / duration (seconds)
        if (duration_ > 0.0) {
            info[MPMediaItemPropertyPlaybackDuration] = @(duration_);
        }
        info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position_);

        // Volume
        info[MPNowPlayingInfoPropertyVolume] = @(volume_);

        // Shuffle / repeat
        info[MPNowPlayingInfoPropertyShuffleEnabled] = @(shuffle_);
        switch (repeat_) {
            case kanariv::types::RepeatMode::One:
                info[MPNowPlayingInfoPropertyRepeatMode] =
                    @(MPNowPlayingRepeatModeOne);
                break;
            case kanariv::types::RepeatMode::All:
                info[MPNowPlayingInfoPropertyRepeatMode] =
                    @(MPNowPlayingRepeatModeAll);
                break;
            default:
                info[MPNowPlayingInfoPropertyRepeatMode] =
                    @(MPNowPlayingRepeatModeDefault);
                break;
        }

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
