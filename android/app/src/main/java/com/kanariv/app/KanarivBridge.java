package com.kanariv.app;

import android.media.AudioManager;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.util.Log;

/**
 * JNI bridge between the Android app and the native kanariv daemon.
 *
 * Native methods (start/stop daemon, playback commands) are implemented in
 * C++ (android/jni_bridge.cpp) and registered via JNI_OnLoad.
 *
 * Callback methods (onMediaSessionActivate, onMediaMetadata, etc.) are
 * invoked from C++ via JNI to update the Android MediaSession, notification,
 * and media-button receiver.
 */
public class KanarivBridge {
    private static final String TAG = "KanarivBridge";

    static {
        System.loadLibrary("kanariv");
    }

    // ---- Native methods (implemented in C++) ----

    private static native long nativeStartDaemon(String dbPath,
                                                 String logsPath,
                                                 String configPath);
    private static native void nativeStopDaemon();
    private static native boolean nativeIsDaemonRunning();
    private static native void nativePlay();
    private static native void nativePause();
    private static native void nativeStop();
    private static native void nativeNext();
    private static native void nativePrev();
    private static native void nativeSeek(double delta);
    private static native void nativeSeekTo(double position);
    private static native void nativeSetVolume(float volume);
    private static native void nativeSetRepeat(int repeat);
    private static native void nativeSetShuffle(boolean shuffle);
    private static native boolean nativeLibraryAdd(String path);

    // ---- Callback object reference (set by the foreground service) ----

    private static KanarivBridge sCallbackInstance;

    public static void setCallbackInstance(KanarivBridge instance) {
        sCallbackInstance = instance;
    }

    // ---- Public API used by the foreground service / activity ----

    public static long startDaemon(String dbPath, String logsPath,
                                   String configPath) {
        return nativeStartDaemon(dbPath, logsPath, configPath);
    }

    public static void stopDaemon() {
        nativeStopDaemon();
    }

    public static boolean isDaemonRunning() {
        return nativeIsDaemonRunning();
    }

    public static void play() {
        nativePlay();
    }

    public static void pause() {
        nativePause();
    }

    public static void stop() {
        nativeStop();
    }

    public static void next() {
        nativeNext();
    }

    public static void prev() {
        nativePrev();
    }

    public static void seek(double delta) {
        nativeSeek(delta);
    }

    public static void seekTo(double position) {
        nativeSeekTo(position);
    }

    public static void setVolume(float volume) {
        nativeSetVolume(volume);
    }

    public static void setRepeat(int repeat) {
        nativeSetRepeat(repeat);
    }

    public static void setShuffle(boolean shuffle) {
        nativeSetShuffle(shuffle);
    }

    public static boolean libraryAdd(String path) {
        return nativeLibraryAdd(path);
    }

    // ---- Callbacks invoked from C++ via JNI ----

    public void onMediaSessionActivate() {
        Log.d(TAG, "media session activated");
    }

    public void onMediaSessionDeactivate() {
        Log.d(TAG, "media session deactivated");
    }

    public void onMediaMetadata(String title, String artist, String album,
                                String artworkPath, String artworkMime,
                                long trackId) {
        Log.d(TAG, "metadata: " + title + " - " + artist);
    }

    public void onMediaPlaybackState(int state, double position, double duration) {
        Log.d(TAG, "playback state: " + state + " pos=" + position + " dur=" + duration);
    }

    public void onMediaPlaybackMode(boolean shuffle, int repeat) {
        Log.d(TAG, "playback mode: shuffle=" + shuffle + " repeat=" + repeat);
    }

    public void onMediaVolume(float volume, boolean muted) {
        Log.d(TAG, "volume: " + volume + " muted=" + muted);
    }
}
