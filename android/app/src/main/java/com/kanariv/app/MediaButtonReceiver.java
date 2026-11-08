package com.kanariv.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;

/**
 * Broadcast receiver for media button events (headset buttons, lock-screen
 * controls, Bluetooth media keys).
 *
 * Receives {@link Intent#ACTION_MEDIA_BUTTON} and forwards the key event to
 * {@link KanarivBridge}, which issues the corresponding command to the native
 * daemon.
 */
public class MediaButtonReceiver extends BroadcastReceiver {
    private static final String TAG = "MediaButtonReceiver";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !Intent.ACTION_MEDIA_BUTTON.equals(intent.getAction())) {
            return;
        }

        Bundle extras = intent.getExtras();
        if (extras == null) {
            return;
        }

        KeyEvent keyEvent = (KeyEvent) extras.get(Intent.EXTRA_KEY_EVENT);
        if (keyEvent == null) {
            return;
        }

        int keyCode = keyEvent.getKeyCode();
        int action = keyEvent.getAction();

        if (action != KeyEvent.ACTION_DOWN) {
            return;
        }

        Log.d(TAG, "media button: keyCode=" + keyCode);

        switch (keyCode) {
            case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                KanarivBridge.play();
                break;
            case KeyEvent.KEYCODE_MEDIA_PAUSE:
                KanarivBridge.pause();
                break;
            case KeyEvent.KEYCODE_MEDIA_PLAY:
                KanarivBridge.play();
                break;
            case KeyEvent.KEYCODE_MEDIA_STOP:
                KanarivBridge.stop();
                break;
            case KeyEvent.KEYCODE_MEDIA_NEXT:
                KanarivBridge.next();
                break;
            case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
                KanarivBridge.prev();
                break;
            case KeyEvent.KEYCODE_MEDIA_REWIND:
                KanarivBridge.seek(-10.0);
                break;
            case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD:
                KanarivBridge.seek(10.0);
                break;
            default:
                Log.d(TAG, "unhandled media key: " + keyCode);
                break;
        }
    }
}
