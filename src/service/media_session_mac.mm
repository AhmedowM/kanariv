#import <service/media_session.hpp>

#import <AppKit/AppKit.h>
// MediaPlayer on macOS uses the same headers as iOS but folds every remote
// command event class into MPRemoteCommandEvent.h and MPMediaItemArtwork into
// MPMediaItem.h (there are no per-event headers on macOS).
#import <MediaPlayer/MPNowPlayingInfoCenter.h>
#import <MediaPlayer/MPRemoteCommandCenter.h>
#import <MediaPlayer/MPRemoteCommand.h>
#import <MediaPlayer/MPRemoteCommandEvent.h>
#import <MediaPlayer/MPMediaItem.h>

#include <kanariv/utils/base64.hpp>
#include <kanariv/utils/error.hpp>
#include <kanariv/utils/log.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kanariv::media_session {

/**
 * @brief macOS media-session backend via MediaPlayer framework.
 * @ingroup kanariv_service
 *
 * Uses:
 * - MPNowPlayingInfoCenter for now-playing metadata / playback state
 *   (lock screen, Control Center, Touch Bar). On macOS the playback state is
 *   published both via MPNowPlayingInfoPropertyPlaybackRate (1.0 = playing,
 *   0.0 = paused/stopped) and MPNowPlayingInfoCenter.playbackState, which is
 *   the documented macOS-only way to tell the system whether the app is
 *   playing. Artwork is supplied through MPMediaItemArtwork's
 *   initWithBoundsSize:requestHandler: initializer (macOS 10.12.2+) with an
 *   NSImage, so embedded/base64 cover art works on macOS as well.
 * - MPRemoteCommandCenter for media-key events (play/pause/next/prev/stop),
 *   absolute and relative seeking, and repeat/shuffle mode requests.
 *   Gated by enableMediaKeys_.
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

        if (enableMediaKeys_) {
            installCommandHandlers();
            active_ = true;
            publishPlaybackModesLocked();
        } else {
            active_ = true;
        }

        if (logger_)
            logger_->debug("macOS media-session backend activated");
        return {};
    }

    kanariv::utils::Expected<void> deactivate() override {
        std::vector<CommandBinding> bindings;
        {
            std::lock_guard lock(mtx_);
            if (!active_)
                return {};
            bindings.swap(commandBindings_);
            active_ = false;
        }

        // Remove handlers outside the lock: command removal must never run
        // while a callback that re-enters this object could be blocked.
        for (auto& [command, token] : bindings) {
            command.enabled = NO;
            [command removeTarget:token];
            [token release];
        }

        @autoreleasepool {
            if (enableNotifications_) {
                MPNowPlayingInfoCenter* center =
                    [MPNowPlayingInfoCenter defaultCenter];
                center.playbackState = MPNowPlayingPlaybackStateStopped;
                [center setNowPlayingInfo:@{}];
            }
        }

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
        publishPlaybackModesLocked();
        publishNowPlayingInfo();
        return {};
    }

    void setLogger(kanariv::utils::Logger* logger) noexcept override {
        logger_ = logger;
    }

    kanariv::utils::Expected<void> setVolume(float volume, bool muted) override {
        // macOS exposes no per-application volume Property in
        // MPNowPlayingInfoCenter, so volume has to be applied by the audio
        // engine; there is nothing to publish here.
        (void)volume;
        (void)muted;
        return {};
    }

    bool active() const noexcept override {
        std::lock_guard lock(mtx_);
        return active_;
    }

  private:
    using CommandBinding = std::pair<MPRemoteCommand*, id>;

    // Copies the callback list so it can be invoked without holding mtx_.
    // Invoking a callback while holding the mutex would deadlock, because the
    // callback synchronously calls back into setPlaybackState()/setPlaybackMode().
    Callbacks copyCallbacks() const {
        std::lock_guard lock(mtx_);
        return callbacks_;
    }

    void bindCommand(MPRemoteCommand* command,
                     MPRemoteCommandHandlerStatus (^handler)(MPRemoteCommandEvent*)) {
        id token = [command addTargetWithHandler:handler];
        if (!token)
            return;
        // MRR: the returned token must outlive the autorelease pool so that
        // deactivate() can pass it to removeTarget:. The command itself is a
        // shared singleton and is owned by MPRemoteCommandCenter.
        [token retain];
        commandBindings_.emplace_back(command, token);
    }

    void installCommandHandlers() {
        MPRemoteCommandCenter* commandCenter =
            [MPRemoteCommandCenter sharedCommandCenter];
        MacImpl* self = this;

        commandCenter.playCommand.enabled = YES;
        bindCommand(commandCenter.playCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull) {
                        auto cb = self->copyCallbacks();
                        if (cb.onPlay)
                            cb.onPlay();
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.pauseCommand.enabled = YES;
        bindCommand(commandCenter.pauseCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull) {
                        auto cb = self->copyCallbacks();
                        if (cb.onPause)
                            cb.onPause();
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.togglePlayPauseCommand.enabled = YES;
        bindCommand(commandCenter.togglePlayPauseCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull) {
                        bool playing = false;
                        {
                            std::lock_guard lock(self->mtx_);
                            playing =
                                self->state_ == kanariv::types::PlaybackState::Playing;
                        }
                        auto cb = self->copyCallbacks();
                        if (playing) {
                            if (cb.onPause)
                                cb.onPause();
                        } else if (cb.onPlay) {
                            cb.onPlay();
                        }
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.nextTrackCommand.enabled = YES;
        bindCommand(commandCenter.nextTrackCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull) {
                        auto cb = self->copyCallbacks();
                        if (cb.onNext)
                            cb.onNext();
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.previousTrackCommand.enabled = YES;
        bindCommand(commandCenter.previousTrackCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull) {
                        auto cb = self->copyCallbacks();
                        if (cb.onPrev)
                            cb.onPrev();
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.stopCommand.enabled = YES;
        bindCommand(commandCenter.stopCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull) {
                        auto cb = self->copyCallbacks();
                        if (cb.onStop)
                            cb.onStop();
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        // Absolute seek (scrubbing the Now Playing timeline).
        commandCenter.changePlaybackPositionCommand.enabled = YES;
        bindCommand(commandCenter.changePlaybackPositionCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull event) {
                        auto* positionEvent =
                            (MPChangePlaybackPositionCommandEvent*)event;
                        auto cb = self->copyCallbacks();
                        if (cb.onSeekTo)
                            cb.onSeekTo(positionEvent.positionTime);
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        // Relative seek (skip forward/backward buttons, headset controls).
        commandCenter.skipForwardCommand.enabled = YES;
        commandCenter.skipForwardCommand.preferredIntervals = @[ @15.0 ];
        bindCommand(commandCenter.skipForwardCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull event) {
                        auto* skipEvent = (MPSkipIntervalCommandEvent*)event;
                        auto cb = self->copyCallbacks();
                        if (cb.onSeek)
                            cb.onSeek(skipEvent.interval);
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.skipBackwardCommand.enabled = YES;
        commandCenter.skipBackwardCommand.preferredIntervals = @[ @15.0 ];
        bindCommand(commandCenter.skipBackwardCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull event) {
                        auto* skipEvent = (MPSkipIntervalCommandEvent*)event;
                        auto cb = self->copyCallbacks();
                        if (cb.onSeek)
                            cb.onSeek(-skipEvent.interval);
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        // Repeat / shuffle requests from Control Center or media keys.
        commandCenter.changeRepeatModeCommand.enabled = YES;
        bindCommand(commandCenter.changeRepeatModeCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull event) {
                        auto* repeatEvent =
                            (MPChangeRepeatModeCommandEvent*)event;
                        kanariv::types::RepeatMode repeat =
                            kanariv::types::RepeatMode::Off;
                        switch (repeatEvent.repeatType) {
                            case MPRepeatTypeTrack:
                                repeat = kanariv::types::RepeatMode::One;
                                break;
                            case MPRepeatTypeAll:
                                repeat = kanariv::types::RepeatMode::All;
                                break;
                            default:
                                repeat = kanariv::types::RepeatMode::Off;
                                break;
                        }
                        auto cb = self->copyCallbacks();
                        if (cb.onRepeat)
                            cb.onRepeat(repeat);
                        return MPRemoteCommandHandlerStatusSuccess;
                    });

        commandCenter.changeShuffleModeCommand.enabled = YES;
        bindCommand(commandCenter.changeShuffleModeCommand,
                    ^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* _Nonnull event) {
                        auto* shuffleEvent =
                            (MPChangeShuffleModeCommandEvent*)event;
                        const bool shuffle =
                            shuffleEvent.shuffleType != MPShuffleTypeOff;
                        auto cb = self->copyCallbacks();
                        if (cb.onShuffle)
                            cb.onShuffle(shuffle);
                        return MPRemoteCommandHandlerStatusSuccess;
                    });
    }

    void publishPlaybackModesLocked() {
        if (!active_ || !enableMediaKeys_)
            return;

        MPRemoteCommandCenter* commandCenter =
            [MPRemoteCommandCenter sharedCommandCenter];

        MPRepeatType repeatType = MPRepeatTypeOff;
        switch (repeat_) {
            case kanariv::types::RepeatMode::One:
                repeatType = MPRepeatTypeTrack;
                break;
            case kanariv::types::RepeatMode::All:
                repeatType = MPRepeatTypeAll;
                break;
            default:
                repeatType = MPRepeatTypeOff;
                break;
        }
        commandCenter.changeRepeatModeCommand.currentRepeatType = repeatType;
        commandCenter.changeShuffleModeCommand.currentShuffleType =
            shuffle_ ? MPShuffleTypeItems : MPShuffleTypeOff;
    }

    void publishNowPlayingInfo() {
        if (!enableNotifications_)
            return;

        @autoreleasepool {
            MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];

            NSMutableDictionary<NSString*, id>* info =
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

            double rate = 0.0;
            switch (state_) {
                case kanariv::types::PlaybackState::Playing:
                    rate = 1.0;
                    center.playbackState = MPNowPlayingPlaybackStatePlaying;
                    break;
                case kanariv::types::PlaybackState::Paused:
                    rate = 0.0;
                    center.playbackState = MPNowPlayingPlaybackStatePaused;
                    break;
                default:
                    rate = 0.0;
                    center.playbackState = MPNowPlayingPlaybackStateStopped;
                    break;
            }
            info[MPNowPlayingInfoPropertyPlaybackRate] = @(rate);

            // Position / duration (seconds). The system extrapolates the
            // elapsed time from the playback rate between updates.
            if (duration_ > 0.0)
                info[MPMediaItemPropertyPlaybackDuration] = @(duration_);
            info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position_);

            // Artwork: prefer embedded bytes, fall back to a local file. Both
            // are supported on macOS via MPMediaItemArtwork's blocks-based
            // initializer, which takes an NSImage.
            NSData* artworkData = nil;
            if (metadata_.artworkBase64 && !metadata_.artworkBase64->empty()) {
                auto decoded =
                    kanariv::utils::Base64::decode(*metadata_.artworkBase64);
                if (decoded && !decoded->empty()) {
                    artworkData = [NSData dataWithBytes:decoded->data()
                                                length:decoded->size()];
                }
            } else if (metadata_.artworkPath && !metadata_.artworkPath->empty()) {
                NSString* path = [NSString stringWithUTF8String:
                                               metadata_.artworkPath->c_str()];
                artworkData = [NSData dataWithContentsOfFile:path];
            }

            if (artworkData) {
                NSImage* image = [[NSImage alloc] initWithData:artworkData];
                if (image) {
                    MPMediaItemArtwork* artwork = [[MPMediaItemArtwork alloc]
                        initWithBoundsSize:image.size
                            requestHandler:^NSImage*(CGSize) {
                                return image;
                            }];
                    if (artwork) {
                        info[MPMediaItemPropertyArtwork] = artwork;
                        [artwork release];
                    }
                    [image release];
                }
            }

            [center setNowPlayingInfo:info];

            if (logger_)
                logger_->debug("macOS now-playing info updated");
        }
    }

    [[maybe_unused]] std::string appName_;
    [[maybe_unused]] std::string appIcon_;
    bool enableNotifications_{true};
    bool enableMediaKeys_{true};

    mutable std::mutex mtx_;
    bool active_{false};
    std::vector<CommandBinding> commandBindings_;

    kanariv::types::PlaybackState state_{
        kanariv::types::PlaybackState::Stopped};
    double position_{0.0};
    double duration_{0.0};
    Metadata metadata_{};
    bool shuffle_{false};
    kanariv::types::RepeatMode repeat_{kanariv::types::RepeatMode::Off};
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
