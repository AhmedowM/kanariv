package com.kanariv.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.Base64;
import android.util.Log;

import java.io.File;

/**
 * Foreground service that hosts the kanariv daemon in-process.
 *
 * The service loads the native library, starts the Host (in-process service)
 * via JNI, and manages the Android MediaSession and media notification.
 * Metadata and playback state are pushed from the native media-session
 * backend; this class mirrors them into the platform MediaSession so the
 * system media controls (lock screen, notification shade, quick settings,
 * Bluetooth/headset buttons) show real title/artist/artwork, progress and a
 * working seek bar.
 */
public class KanarivService extends Service implements AudioManager.OnAudioFocusChangeListener {
    private static final String TAG = "KanarivService";
    private static final int FOREGROUND_ID = 1;
    private static final String CHANNEL_ID = "kanariv_playback";

    private static final String ACTION_START_DAEMON = "com.kanariv.app.action.START_DAEMON";
    private static final String ACTION_PREV = "com.kanariv.app.action.PREV";
    private static final String ACTION_PLAY_PAUSE = "com.kanariv.app.action.PLAY_PAUSE";
    private static final String ACTION_NEXT = "com.kanariv.app.action.NEXT";
    private static final String ACTION_STOP = "com.kanariv.app.action.STOP";

    private MediaSession mMediaSession;
    private AudioManager mAudioManager;
    private boolean mAudioFocused = false;
    private boolean mResumeOnFocusGain = false;
    private Handler mCallbackHandler;
    private HandlerThread mCallbackThread;
    private NotificationActionReceiver mNotificationReceiver;

    // Cached now-playing state, mirrored from the native media-session backend.
    private String mTitle = "";
    private String mArtist = "";
    private String mAlbum = "";
    private String mMediaId = "";
    private Bitmap mArtwork;
    private long mDurationMs = 0;
    private long mPositionMs = 0;
    private int mPlaybackState = PlaybackState.STATE_NONE;
    private boolean mHasMetadata = false;

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
        registerNotificationReceiver();

        KanarivBridge.setCallbackInstance(new KanarivBridge() {
            @Override
            public void onMediaMetadata(String title, String artist, String album,
                                        String artworkPath, String artworkMime,
                                        String artworkBase64, long trackId) {
                mCallbackHandler.post(() -> updateMediaMetadata(title, artist, album,
                        artworkPath, artworkBase64, trackId));
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
                // Volume is applied natively in the engine.
            }
        });
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.d(TAG, "service onStartCommand");
        // Promote to foreground before starting the daemon: startForegroundService
        // requires this within a few seconds, and daemon startup touches disk.
        startForegroundCompat(buildNotification());
        if (intent == null || ACTION_START_DAEMON.equals(intent.getAction())) {
            if (!KanarivBridge.isDaemonRunning()) {
                startDaemon();
            }
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        Log.d(TAG, "service onDestroy");
        unregisterNotificationReceiver();
        stopDaemon();
        KanarivBridge.setCallbackInstance(null);
        releaseMediaSession();
        stopForegroundCompat();
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

        // The platform constructor is deprecated since API 29 in favour of
        // MediaSession.Builder, but the Builder is absent from some SDK
        // platform stubs; the constructor remains functional and is the most
        // portable option for this in-process session.
        mMediaSession = new MediaSession(this, "Kanariv");

        mMediaSession.setCallback(new MediaSession.Callback() {
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
        mMediaSession.setMediaButtonReceiver(
                PendingIntent.getBroadcast(this, 0,
                        new Intent(Intent.ACTION_MEDIA_BUTTON)
                                .setClass(this, MediaButtonReceiver.class),
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        mMediaSession.setActive(true);
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
                                     String artworkPath, String artworkBase64,
                                     long trackId) {
        if (mMediaSession == null) return;

        mTitle = title != null ? title : "";
        mArtist = artist != null ? artist : "";
        mAlbum = album != null ? album : "";
        mMediaId = trackId != 0 ? Long.toString(trackId) : "";
        mHasMetadata = true;
        mArtwork = decodeArtwork(artworkPath, artworkBase64);
        publishMetadata();
        updateNotification();
    }

    private void publishMetadata() {
        if (mMediaSession == null || !mHasMetadata) return;

        MediaMetadata.Builder builder = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, mTitle)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, mArtist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, mAlbum)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, mDurationMs);
        if (!mMediaId.isEmpty()) {
            builder.putString(MediaMetadata.METADATA_KEY_MEDIA_ID, mMediaId);
        }
        if (mArtwork != null) {
            builder.putBitmap(MediaMetadata.METADATA_KEY_ART, mArtwork);
            builder.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, mArtwork);
        }
        mMediaSession.setMetadata(builder.build());
    }

    private void updatePlaybackState(int androidState, double position, double duration) {
        if (mMediaSession == null) return;

        mPlaybackState = androidState;
        mPositionMs = Math.max(0, (long) (position * 1000));
        long newDurationMs = Math.max(0, (long) (duration * 1000));
        boolean durationChanged = newDurationMs != mDurationMs;
        mDurationMs = newDurationMs;

        long actions = PlaybackState.ACTION_PLAY
                | PlaybackState.ACTION_PAUSE
                | PlaybackState.ACTION_PLAY_PAUSE
                | PlaybackState.ACTION_STOP
                | PlaybackState.ACTION_SKIP_TO_NEXT
                | PlaybackState.ACTION_SKIP_TO_PREVIOUS
                | PlaybackState.ACTION_SEEK_TO;

        PlaybackState.Builder builder = new PlaybackState.Builder()
                .setActions(actions)
                .setState(androidState, mPositionMs, 1.0f)
                .setBufferedPosition(mDurationMs);
        mMediaSession.setPlaybackState(builder.build());

        // Duration arrives with the playback state, so refresh metadata once
        // it is known; Android needs METADATA_KEY_DURATION for the seek bar.
        if (durationChanged) {
            publishMetadata();
        }

        if (androidState == PlaybackState.STATE_PLAYING) {
            requestAudioFocus();
        } else if (androidState == PlaybackState.STATE_STOPPED
                || androidState == PlaybackState.STATE_NONE) {
            abandonAudioFocus();
        }

        updateNotification();
    }

    private void updatePlaybackMode(boolean shuffle, int repeat) {
        // Android's MediaSession has no first-class shuffle/repeat state.
        // Keep the values cached for future custom actions/extras.
        Log.d(TAG, "playback mode: shuffle=" + shuffle + " repeat=" + repeat);
    }

    private Bitmap decodeArtwork(String artworkPath, String artworkBase64) {
        try {
            if (artworkBase64 != null && !artworkBase64.isEmpty()) {
                byte[] bytes = Base64.decode(artworkBase64, Base64.DEFAULT);
                return BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
            }
            if (artworkPath != null && !artworkPath.isEmpty()) {
                return BitmapFactory.decodeFile(artworkPath);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "failed to decode artwork", e);
        }
        return null;
    }

    // ---- Audio focus -----------------------------------------------------

    private void requestAudioFocus() {
        if (mAudioManager == null || mAudioFocused) return;
        int result = mAudioManager.requestAudioFocus(this,
                AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        mAudioFocused = (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED);
        Log.d(TAG, "audio focus requested: " + mAudioFocused);
    }

    private void abandonAudioFocus() {
        if (mAudioManager != null && mAudioFocused) {
            mAudioManager.abandonAudioFocus(this);
            mAudioFocused = false;
        }
    }

    // ---- Notification action receiver ------------------------------------

    private void registerNotificationReceiver() {
        mNotificationReceiver = new NotificationActionReceiver();
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_PREV);
        filter.addAction(ACTION_PLAY_PAUSE);
        filter.addAction(ACTION_NEXT);
        filter.addAction(ACTION_STOP);
        filter.setPriority(IntentFilter.SYSTEM_HIGH_PRIORITY);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(mNotificationReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(mNotificationReceiver, filter);
        }
    }

    private void unregisterNotificationReceiver() {
        if (mNotificationReceiver != null) {
            try {
                unregisterReceiver(mNotificationReceiver);
            } catch (IllegalArgumentException e) {
                // already unregistered
            }
            mNotificationReceiver = null;
        }
    }

    /**
     * Receiver for notification action buttons. All actions go through
     * KanarivBridge so the daemon remains the single source of truth.
     */
    public static class NotificationActionReceiver extends BroadcastReceiver {
        private static final String TAG = "NotificationActionReceiver";

        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null) {
                return;
            }
            String action = intent.getAction();
            Log.d(TAG, "notification action: " + action);
            if (action == null) {
                return;
            }
            switch (action) {
                case ACTION_PREV:
                    KanarivBridge.prev();
                    break;
                case ACTION_PLAY_PAUSE:
                    KanarivBridge.togglePlayPause();
                    break;
                case ACTION_NEXT:
                    KanarivBridge.next();
                    break;
                case ACTION_STOP:
                    KanarivBridge.stop();
                    break;
                default:
                    Log.d(TAG, "unhandled notification action: " + action);
                    break;
            }
        }
    }

    @Override
    public void onAudioFocusChange(int focusChange) {
        switch (focusChange) {
            case AudioManager.AUDIOFOCUS_GAIN:
                mAudioFocused = true;
                if (mResumeOnFocusGain) {
                    mResumeOnFocusGain = false;
                    KanarivBridge.play();
                }
                break;
            case AudioManager.AUDIOFOCUS_LOSS:
                mAudioFocused = false;
                mResumeOnFocusGain = false;
                KanarivBridge.pause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                mAudioFocused = false;
                mResumeOnFocusGain = mPlaybackState == PlaybackState.STATE_PLAYING;
                KanarivBridge.pause();
                break;
            case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                KanarivBridge.setVolume(0.2f);
                break;
        }
    }

    // ---- Notification ----------------------------------------------------

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, getString(R.string.channel_name),
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription(getString(R.string.channel_description));
            channel.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
            channel.setShowBadge(false);
            channel.setSound(null, null);
            channel.enableVibration(false);
            NotificationManager manager =
                    (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager != null) {
                manager.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification() {
        PendingIntent contentPendingIntent = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Notification.MediaStyle style = new Notification.MediaStyle()
                .setMediaSession(mMediaSession != null ? mMediaSession.getSessionToken() : null)
                .setShowActionsInCompactView(0, 1, 2);

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }

        boolean playing = mPlaybackState == PlaybackState.STATE_PLAYING;
        String title = mHasMetadata && !mTitle.isEmpty() ? mTitle : "Kanariv";
        String text = mHasMetadata ? subtitle() : getString(R.string.notification_no_track);

        builder.setStyle(style)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(title)
                .setContentText(text)
                .setSubText(mHasMetadata && !mAlbum.isEmpty() ? mAlbum : null)
                .setLargeIcon(mArtwork)
                .setOnlyAlertOnce(true)
                .setOngoing(playing)
                .setShowWhen(false)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setCategory(Notification.CATEGORY_TRANSPORT)
                .setContentIntent(contentPendingIntent)
                .addAction(action(R.drawable.ic_prev, getString(R.string.notification_previous),
                        ACTION_PREV))
                .addAction(action(playing ? R.drawable.ic_pause : R.drawable.ic_play,
                        getString(playing ? R.string.notification_pause
                                : R.string.notification_play),
                        ACTION_PLAY_PAUSE))
                .addAction(action(R.drawable.ic_next, getString(R.string.notification_next),
                        ACTION_NEXT));

        return builder.build();
    }

    private String subtitle() {
        if (!mArtist.isEmpty() && !mAlbum.isEmpty()) {
            return mArtist + " \u2014 " + mAlbum;
        }
        if (!mArtist.isEmpty()) {
            return mArtist;
        }
        if (!mAlbum.isEmpty()) {
            return mAlbum;
        }
        return getString(R.string.notification_no_track);
    }

    private Notification.Action action(int iconRes, String title, String action) {
        return new Notification.Action.Builder(
                Icon.createWithResource(this, iconRes), title, buildMediaIntent(action)).build();
    }

    private PendingIntent buildMediaIntent(String action) {
        Intent intent = new Intent(action);
        intent.setPackage(getPackageName());
        return PendingIntent.getBroadcast(this, action.hashCode(), intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private void updateNotification() {
        NotificationManager manager =
                (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (manager != null) {
            manager.notify(FOREGROUND_ID, buildNotification());
        }
    }

    private void startForegroundCompat(Notification notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(FOREGROUND_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        } else {
            startForeground(FOREGROUND_ID, notification);
        }
    }

    private void stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
    }
}
