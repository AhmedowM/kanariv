package com.kanariv.app;

import android.app.Application;
import android.util.Log;

/**
 * Application class for kanariv.
 *
 * Ensures the native library is loaded as early as possible so JNI
 * methods are available when the foreground service starts.
 */
public class KanarivApp extends Application {
    private static final String TAG = "KanarivApp";

    @Override
    public void onCreate() {
        super.onCreate();
        Log.d(TAG, "application onCreate");
        System.loadLibrary("kanariv");
        System.loadLibrary("kanariv_android");
    }
}
