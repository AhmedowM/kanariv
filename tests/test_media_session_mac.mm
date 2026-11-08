// Behavioural test for the macOS media-session backend.
//
// Unlike the CI smoke step (which only checks the frameworks are linked and
// the daemon stays alive), this drives the public MediaSession API the daemon
// uses and reads the state back from the real MediaPlayer objects:
// MPNowPlayingInfoCenter (metadata, playback rate/state) and
// MPRemoteCommandCenter (enabled commands, repeat/shuffle mapping).
//
// MediaPlayer keeps nowPlayingInfo/playbackState as per-process properties, so
// the assertions hold even on a headless runner without a logged-in GUI
// session. The test restores whatever info/state the process had before it
// ran. Apple-only: the backend is compiled only on macOS.

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

#include <kanariv/types/engine.hpp>
#include <service/media_session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

namespace {

// Builds a tiny valid PNG so the artwork path (NSImage via AppKit ->
// MPMediaItemArtwork) is exercised without a fixture file.
NSData* makePng() {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:2
                      pixelsHigh:2
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:8
                    bitsPerPixel:32];
    if (!rep)
        return nil;
    std::memset([rep bitmapData], 0x40, 8 * 2);
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                    properties:@{}];
    [rep release];
    return png;
}

} // namespace

TEST_CASE("macOS media session publishes now-playing info and modes",
          "[service][media-session][macos]") {
    @autoreleasepool {
        MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
        NSDictionary<NSString*, id>* previousInfo =
            [[center nowPlayingInfo] copy];
        const MPNowPlayingPlaybackState previousState = center.playbackState;

        auto created = kanariv::media_session::MediaSession::create(
            "kanariv-test", "", /*enableNotifications=*/true,
            /*enableMediaKeys=*/true);
        REQUIRE(created.has_value());
        kanariv::media_session::MediaSession& session = **created;

        REQUIRE(session.activate().has_value());
        CHECK(session.active());

        MPRemoteCommandCenter* commands = [MPRemoteCommandCenter sharedCommandCenter];
        CHECK(commands.playCommand.isEnabled);
        CHECK(commands.pauseCommand.isEnabled);
        CHECK(commands.stopCommand.isEnabled);
        CHECK(commands.nextTrackCommand.isEnabled);
        CHECK(commands.previousTrackCommand.isEnabled);
        CHECK(commands.changeRepeatModeCommand.isEnabled);
        CHECK(commands.changeShuffleModeCommand.isEnabled);

        kanariv::media_session::Metadata meta;
        meta.title = "Now playing title";
        meta.artist = "Now playing artist";
        meta.album = std::string{"Now playing album"};
        meta.trackId = 42;

        REQUIRE(session.setMetadata(meta).has_value());
        REQUIRE(session.setPlaybackState(
                    kanariv::types::PlaybackState::Playing, 12.5, 240.0)
                    .has_value());

        NSDictionary<NSString*, id>* info = [center nowPlayingInfo];
        REQUIRE(info != nil);
        CHECK([(NSString*)info[MPMediaItemPropertyTitle]
            isEqualToString:@"Now playing title"]);
        CHECK([(NSString*)info[MPMediaItemPropertyArtist]
            isEqualToString:@"Now playing artist"]);
        CHECK([(NSString*)info[MPMediaItemPropertyAlbumTitle]
            isEqualToString:@"Now playing album"]);
        CHECK([(NSNumber*)info[MPNowPlayingInfoPropertyPlaybackRate]
                  doubleValue] == 1.0);
        CHECK([(NSNumber*)info[MPMediaItemPropertyPlaybackDuration]
                  doubleValue] == 240.0);
        CHECK([(NSNumber*)info[MPNowPlayingInfoPropertyElapsedPlaybackTime]
                  doubleValue] == 12.5);
        CHECK(center.playbackState == MPNowPlayingPlaybackStatePlaying);

        // Repeat/shuffle are published as the current command state.
        REQUIRE(session.setPlaybackMode(true, kanariv::types::RepeatMode::One)
                    .has_value());
        CHECK(commands.changeRepeatModeCommand.currentRepeatType ==
              MPRepeatTypeOne);
        CHECK(commands.changeShuffleModeCommand.currentShuffleType ==
              MPShuffleTypeItems);

        REQUIRE(session.setPlaybackMode(false, kanariv::types::RepeatMode::All)
                    .has_value());
        CHECK(commands.changeRepeatModeCommand.currentRepeatType ==
              MPRepeatTypeAll);
        CHECK(commands.changeShuffleModeCommand.currentShuffleType ==
              MPShuffleTypeOff);

        // Artwork: base64 PNG -> NSImage -> MPMediaItemArtwork.
        NSData* png = makePng();
        REQUIRE(png != nil);
        NSString* pngBase64 = [png base64EncodedStringWithOptions:0];
        REQUIRE(pngBase64 != nil);
        meta.artworkBase64 = std::string([pngBase64 UTF8String]);
        meta.artworkMime = "image/png";
        REQUIRE(session.setMetadata(meta).has_value());
        CHECK([center nowPlayingInfo][MPMediaItemPropertyArtwork] != nil);

        // Paused/stopped flip the rate and the system playback state.
        REQUIRE(session.setPlaybackState(
                    kanariv::types::PlaybackState::Paused, 5.0, 240.0)
                    .has_value());
        CHECK(center.playbackState == MPNowPlayingPlaybackStatePaused);
        CHECK([(NSNumber*)[center nowPlayingInfo]
                         [MPNowPlayingInfoPropertyPlaybackRate] doubleValue] ==
              0.0);

        REQUIRE(session.setPlaybackState(
                    kanariv::types::PlaybackState::Stopped, 0.0, 240.0)
                    .has_value());
        CHECK(center.playbackState == MPNowPlayingPlaybackStateStopped);

        // Deactivation clears now-playing info and unhooks media keys.
        REQUIRE(session.deactivate().has_value());
        CHECK_FALSE(session.active());
        NSDictionary<NSString*, id>* cleared = [center nowPlayingInfo];
        CHECK(cleared == nil || cleared.count == 0);
        CHECK_FALSE(commands.playCommand.isEnabled);
        CHECK_FALSE(commands.changeRepeatModeCommand.isEnabled);

        if (previousInfo)
            [center setNowPlayingInfo:previousInfo];
        center.playbackState = previousState;
        [previousInfo release];
    }
}
