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
        // Load the JNI bridge; its DT_NEEDED entries pull in the kanariv
        // component libraries. Do not load the combined libkanariv.so: it
        // contains a second copy of the service/media-session classes and
        // would shadow the component libraries used by the bridge.
        System.loadLibrary("kanariv_android");
    }
}
