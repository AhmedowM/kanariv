package com.kanariv.app;

import android.Manifest;
import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Main activity for the kanariv Android app.
 *
 * Provides a minimal UI to start/stop the daemon, control playback, and
 * import audio files into the library.
 */
public class MainActivity extends Activity {
    private static final String TAG = "KanarivMain";

    private static final int REQUEST_PICK_AUDIO = 1;
    private static final int REQUEST_NOTIFICATIONS = 2;

    private TextView mStatusText;
    private Button mStartButton;
    private Button mStopButton;
    private Button mPlayButton;
    private Button mPauseButton;
    private Button mImportButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                        != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS},
                    REQUEST_NOTIFICATIONS);
        }

        mStatusText = findViewById(R.id.status_text);
        mStartButton = findViewById(R.id.start_button);
        mStopButton = findViewById(R.id.stop_button);
        mPlayButton = findViewById(R.id.play_button);
        mPauseButton = findViewById(R.id.pause_button);
        mImportButton = findViewById(R.id.import_button);

        mStartButton.setOnClickListener(v -> {
            Intent intent = new Intent(this, KanarivService.class);
            intent.setAction("com.kanariv.app.action.START_DAEMON");
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(intent);
            } else {
                startService(intent);
            }
            // The service starts the daemon asynchronously; refresh the status
            // once it has had a moment to come up.
            mStatusText.postDelayed(this::updateStatus, 750);
        });

        mStopButton.setOnClickListener(v -> {
            stopService(new Intent(this, KanarivService.class));
            updateStatus();
        });

        mPlayButton.setOnClickListener(v -> KanarivBridge.play());
        mPauseButton.setOnClickListener(v -> KanarivBridge.pause());

        mImportButton.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
            intent.setType("audio/*");
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(intent, REQUEST_PICK_AUDIO);
        });

        updateStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateStatus();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        Log.d(TAG, "onActivityResult: requestCode=" + requestCode + " resultCode=" + resultCode + " data=" + data);
        if (requestCode == REQUEST_PICK_AUDIO && resultCode == Activity.RESULT_OK && data != null) {
            Log.d(TAG, "onActivityResult: got URI: " + data.getData());
            importAudioFile(data.getData());
        } else {
            Log.d(TAG, "onActivityResult: not a valid result, ignoring");
        }
    }

    private void updateStatus() {
        boolean running = KanarivBridge.isDaemonRunning();
        mStatusText.setText(running ? "Daemon: running" : "Daemon: stopped");
        mStartButton.setEnabled(!running);
        mStopButton.setEnabled(running);
        mPlayButton.setEnabled(running);
        mPauseButton.setEnabled(running);
        mImportButton.setEnabled(running);
        Log.d(TAG, "status updated: running=" + running);
    }

    // ------------------------------------------------------------------
    // Audio file import
    // ------------------------------------------------------------------

    private void importAudioFile(Uri uri) {
        Log.d(TAG, "importAudioFile: uri=" + uri);
        ContentResolver resolver = getContentResolver();
        String displayName = getDisplayName(uri);
        Log.d(TAG, "importAudioFile: displayName=" + displayName);

        File importDir = new File(getExternalFilesDir(Environment.DIRECTORY_MUSIC), "imported");
        if (!importDir.exists() && !importDir.mkdirs()) {
            Log.e(TAG, "failed to create import dir: " + importDir);
            Toast.makeText(this, "Failed to create import directory", Toast.LENGTH_SHORT).show();
            return;
        }
        Log.d(TAG, "importAudioFile: importDir=" + importDir.getAbsolutePath());

        File targetFile = new File(importDir, displayName);
        Log.d(TAG, "importAudioFile: targetFile=" + targetFile.getAbsolutePath());
        try (InputStream in = resolver.openInputStream(uri);
             OutputStream out = new FileOutputStream(targetFile)) {
            if (in == null) {
                Log.e(TAG, "failed to open input stream for uri: " + uri);
                Toast.makeText(this, "Failed to read audio file", Toast.LENGTH_SHORT).show();
                return;
            }
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) != -1) {
                out.write(buf, 0, n);
            }
        } catch (IOException e) {
            Log.e(TAG, "failed to copy audio file", e);
            Toast.makeText(this, "Failed to copy audio file: " + e.getMessage(),
                    Toast.LENGTH_SHORT).show();
            return;
        }

        Log.d(TAG, "imported audio file: " + targetFile.getAbsolutePath());

        boolean ok = KanarivBridge.libraryAdd(targetFile.getAbsolutePath());
        Log.d(TAG, "libraryAdd returned: " + ok);
        if (ok) {
            Toast.makeText(this, "Added to library: " + displayName, Toast.LENGTH_SHORT).show();
        } else {
            Toast.makeText(this, "Failed to add to library: " + displayName,
                    Toast.LENGTH_SHORT).show();
        }
        updateStatus();
    }

    private String getDisplayName(Uri uri) {
        String name = "imported_audio";
        ContentResolver resolver = getContentResolver();
        android.database.Cursor cursor = null;
        try {
            cursor = resolver.query(uri, null, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) {
                    name = cursor.getString(idx);
                }
            }
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return name;
    }
}
