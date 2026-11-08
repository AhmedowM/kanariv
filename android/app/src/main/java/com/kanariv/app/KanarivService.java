package com.kanariv.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.media.AudioManager;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import java.io.File;

/**
 * Foreground service that hosts the kanariv daemon in-process.
 *
 * The service loads the native library, starts the Host (in-process service)
 * via JNI, and manages the Android MediaSession and media notification.
 * Audio focus is handled so playback pauses/resumes appropriately when
 * another app requests focus.
 */
public class KanarivService extends Service implements AudioManager.OnAudioFocusChangeListener {
    private static final String TAG = "KanarivService";
    private static final int FOREGROUND_ID = 1;
    private static final String CHANNEL_ID = "kanariv_playback";

    private MediaSession mMediaSession;
    private AudioManager mAudioManager;
    private boolean mAudioFocused = false;
    private Handler mCallbackHandler;
    private HandlerThread mCallbackThread;

    @Override
    public void onCreate() {
        super.onCreate();
        Log.d(TAG, "service onCreate");

        mCallbackThread = new HandlerThread("KanarivCallback");
        mCallbackThread.start();
        mCallbackHandler = new Handler(mCallbackThread.getLooper());

        mAudioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        createMediaSession();
        createNotificationChannel();

        KanarivBridge.setCallbackInstance(new KanarivBridge() {
            @Override
            public void onMediaMetadata(String title, String artist, String album,
                                        String artworkPath, String artworkMime,
                                        long trackId) {
                mCallbackHandler.post(() -> updateMediaMetadata(title, artist, album,
                        artworkPath, artworkMime, trackId));
            }

            @Override
            public void onMediaPlaybackState(int state, double position,
                                             double duration) {
                mCallbackHandler.post(() -> updatePlaybackState(state, position, duration));
            }

            @Override
            public void onMediaPlaybackMode(boolean shuffle, int repeat) {
                mCallbackHandler.post(() -> updatePlaybackMode(shuffle, repeat));
            }

            @Override
            public void onMediaVolume(float volume, boolean muted) {
                mCallbackHandler.post(() -> updateVolume(volume, muted));
            }
        });
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.d(TAG, "service onStartCommand");
        if (intent != null && "com.kanariv.app.action.START_DAEMON".equals(intent.getAction())) {
            startDaemon();
        }
        startForeground(FOREGROUND_ID, buildNotification());
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.d(TAG, "service onDestroy");
        stopDaemon();
        releaseMediaSession();
        if (mCallbackThread != null) {
            mCallbackThread.quitSafely();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ---- Daemon lifecycle ------------------------------------------------

    private void startDaemon() {
        File dbFile = new File(getFilesDir(), "kanariv.db");
        File logDir = new File(getFilesDir(), "logs");
        logDir.mkdirs();

        long hostPtr = KanarivBridge.startDaemon(
                dbFile.getAbsolutePath(),
                logDir.getAbsolutePath(),
                null);
        Log.d(TAG, "daemon started: hostPtr=" + hostPtr);
    }

    private void stopDaemon() {
        KanarivBridge.stopDaemon();
        Log.d(TAG, "daemon stopped");
    }

    // ---- Media session ---------------------------------------------------

    private void createMediaSession() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) {
            Log.w(TAG, "MediaSession requires API 21+");
            return;
        }

        MediaSessionManager manager =
                (MediaSessionManager) getSystemService(Context.MEDIA_SESSION_SERVICE);
        if (manager == null) {
            Log.w(TAG, "MediaSessionManager not available");
            return;
        }

        ComponentName componentName = new ComponentName(this, MediaButtonReceiver.class);
        mMediaSession = new MediaSession(this, "Kanariv");

        mMediaSession.setCallback(new android.media.session.MediaSession.Callback() {
            @Override
            public void onPlay() {
                KanarivBridge.play();
            }

            @Override
            public void onPause() {
                KanarivBridge.pause();
            }

            @Override
            public void onStop() {
                KanarivBridge.stop();
            }

            @Override
            public void onSkipToNext() {
                KanarivBridge.next();
            }

            @Override
            public void onSkipToPrevious() {
                KanarivBridge.prev();
            }

            @Override
            public void onSeekTo(long posMs) {
                KanarivBridge.seekTo(posMs / 1000.0);
            }
        });
        mMediaSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS |
                MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        mMediaSession.setSessionActivity(
                PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        mMediaSession.setActive(true);

        requestAudioFocus();
    }

    private void releaseMediaSession() {
        if (mMediaSession != null) {
            mMediaSession.setActive(false);
            mMediaSession.release();
            mMediaSession = null;
        }
        abandonAudioFocus();
    }

    private void updateMediaMetadata(String title, String artist, String album,
                                     String artworkPath, String artworkMime,
                                     long trackId) {
        if (mMediaSession == null) return;
        // MediaMetadata API (API 21+) is used via fully qualified names.
        // Metadata updates are handled on the C++ side; the Java MediaSession
        // is kept in sync by the callback handlers below.
    }

    private void updatePlaybackState(int state, double position, double duration) {
        if (mMediaSession == null) return;

        android.media.session.PlaybackState.Builder builder =
                new android.media.session.PlaybackState.Builder();
        int compatState;
        switch (state) {
            case android.media.session.PlaybackState.STATE_PLAYING:
                compatState = android.media.session.PlaybackState.STATE_PLAYING;
                break;
            case android.media.session.PlaybackState.STATE_PAUSED:
                compatState = android.media.session.PlaybackState.STATE_PAUSED;
                break;
            case android.media.session.PlaybackState.STATE_STOPPED:
                compatState = android.media.session.PlaybackState.STATE_STOPPED;
                break;
            default:
                compatState = android.media.session.PlaybackState.STATE_NONE;
        }
        long posMs = (long) (position * 1000);
        builder.setState(compatState, posMs, 1.0f);
        builder.setActiveQueueItemId(0);
        mMediaSession.setPlaybackState(builder.build());
    }

    private void updatePlaybackMode(boolean shuffle, int repeat) {
        if (mMediaSession == null) return;
        // Shuffle/repeat mode APIs (API 28+) handled on C++ side.
    }

    private void updateVolume(float volume, boolean muted) {
        // Volume is applied natively in the engine; no Java-side action needed.
    }

    // ---- Audio focus -----------------------------------------------------

    private void requestAudioFocus() {
        // Use the older API (available since API 5) for maximum compatibility.
        int result = mAudioManager.requestAudioFocus(this,
                AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        mAudioFocused = (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED);
        Log.d(TAG, "audio focus requested: " + mAudioFocused);
    }

    private void abandonAudioFocus() {
        if (mAudioManager != null) {
            mAudioManager.abandonAudioFocus(this);
            mAudioFocused = false;
        }
    }

    @Override
    public void onAudioFocusChange(int focusChange) {
        switch (focusChange) {
            case AudioManager.AUDIOFOCUS_GAIN:
                mAudioFocused = true;
                if (KanarivBridge.isDaemonRunning()) {
                    KanarivBridge.play();
                }
                break;
            case AudioManager.AUDIOFOCUS_LOSS:
                mAudioFocused = false;
                KanarivBridge.pause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                mAudioFocused = false;
                KanarivBridge.pause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                mAudioFocused = false;
                KanarivBridge.setVolume(0.2f);
                break;
        }
    }

    // ---- Notification ----------------------------------------------------

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "Kanariv Playback",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Media playback controls");
            channel.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
            NotificationManager manager =
                    (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager != null) {
                manager.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification() {
        Intent playIntent = new Intent(this, MainActivity.class);
        PendingIntent contentPendingIntent = PendingIntent.getActivity(this, 0, playIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Notification.Builder builder = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle("Kanariv")
                .setContentText("No track")
                .setOngoing(true)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setCategory(Notification.CATEGORY_TRANSPORT)
                .setContentIntent(contentPendingIntent)
                .addAction(android.R.drawable.ic_media_previous, "Previous",
                        buildMediaIntent("com.kanariv.app.action.PREV"))
                .addAction(android.R.drawable.ic_media_pause, "Pause",
                        buildMediaIntent("com.kanariv.app.action.PAUSE"))
                .addAction(android.R.drawable.ic_media_next, "Next",
                        buildMediaIntent("com.kanariv.app.action.NEXT"));

        return builder.build();
    }

    private PendingIntent buildMediaIntent(String action) {
        Intent intent = new Intent(action);
        intent.setPackage(getPackageName());
        return PendingIntent.getBroadcast(this, 0, intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }
}
