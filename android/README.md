# Kanariv Android app

This directory contains the Android front end: a Java app that hosts the
native kanariv daemon in-process through JNI and mirrors its playback state
into an Android `MediaSession` and a `MediaStyle` foreground notification.
The system media controls (notification shade, lock screen, quick settings,
Bluetooth and headset buttons) therefore show real metadata, artwork,
progress, a working seek bar, and play/pause/next/previous actions.

The Java sources live under `app/src/main/java/com/kanariv/app/`; the JNI
bridge and the Android media-session backend are the C++ files directly in
this directory. The daemon itself is the normal kanariv C++ code under
`../src`.

## Requirements

- JDK 17 or newer (JDK 21 is used in CI).
- Android SDK with platform and build-tools 36.
- Android NDK 30 (`ANDROID_NDK_ROOT`).
- Gradle 8.x or newer (`gradle` on `PATH`).
- `adb` for installing and inspecting the device.

The app currently packages `arm64-v8a` only, matching the prebuilt native
libraries under `../build/android-arm64`.

## Build

1. Build the native component libraries for Android. From the repository
   root:

   ```sh
   python scripts/configure.py android --ndk "$ANDROID_NDK_ROOT" \
     --ffmpeg-root <ffmpeg-prefix>
   cmake --build build/android-arm64 -j
   ```

2. Build the JNI library against those prebuilt components:

   ```sh
   cmake -S android -B android/build/cmake-test -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
   cmake --build android/build/cmake-test -j
   ```

3. Copy the component libraries and the JNI library into the app's
   `jniLibs` directory. `.so` files are ignored by Git, so this step is
   needed after every native rebuild:

   ```sh
   cp build/android-arm64/*.so android/app/src/main/jniLibs/arm64-v8a/
   cp android/build/cmake-test/libkanariv_android.so \
      android/app/src/main/jniLibs/arm64-v8a/
   ```

4. Build the debug APK:

   ```sh
   export JAVA_HOME=/path/to/jdk-21
   gradle -p android :app:assembleDebug
   ```

The APK is written to
`android/app/build/outputs/apk/debug/app-debug.apk`.

## Install and run

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.kanariv.app/.MainActivity
```

Tap **START DAEMON**, then **IMPORT AUDIO** to add a track and **PLAY** to
start playback. Grant the notification permission when prompted (Android 13+).

## Verify the media session

```sh
adb shell dumpsys media_session | grep -A 12 "Kanariv com.kanariv.app"
adb shell dumpsys notification --noredact | grep -A 20 "pkg=com.kanariv.app"
adb logcat -s KanarivBridge KanarivService KanarivMain
```

A healthy session reports `state=PLAYING`/`PAUSED`, a non-zero
`buffered position`, and `metadata: size=7` with the track title, artist and
album. The notification should show the artwork, title/artist, the play/pause
button and the seek bar.

Media keys can be exercised without touching the device:

```sh
adb shell cmd media_session dispatch play-pause
adb shell cmd media_session dispatch next
adb shell cmd media_session dispatch previous
```

## Architecture notes

- `KanarivBridge` loads only `libkanariv_android.so`. The combined
  `libkanariv.so` is not packaged or loaded, because it contains a second
  copy of the service and media-session classes that would shadow the
  component libraries.
- `JNI_OnLoad` registers `createAndroid` through
  `MediaSession::setAndroidFactory()`, so `libservice.so` can create the
  Android backend without linking JNI code into the core library.
- Playback state is translated from the engine enum to the stable
  `android.media.session.PlaybackState` constants in C++ before crossing
  JNI.
- Notification action buttons and headset buttons call
  `nativeTogglePlayPause`, which asks the engine whether it is playing and
  dispatches the opposite command.
