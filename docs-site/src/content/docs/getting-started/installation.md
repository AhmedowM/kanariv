---
title: Installation
description: Build and install kanariv from source on Linux, Windows, macOS, and FreeBSD
---

# Installation

kanariv is built from source with CMake. The preferred method is the project's
intent-driven configure script, which keeps local builds aligned with CI.

## Prerequisites

| Tool | Minimum Version | Notes |
|---|---|---|
| CMake | 3.31+ | Required for `FILE_SET CXX_MODULES` and import-graph scanning |
| Ninja | 1.11+ | Required for C++23 modules; recommended for all builds |
| Python | 3.9+ | For `scripts/configure.py` |
| Git | 2.30+ | For version stamping from `git describe --tags` |
| Compiler | GCC 13+, LLVM Clang 19+, AppleClang (Xcode 26), or MSVC 2022 | C++23 required; GCC 14+ for modules |
| FFmpeg | Recent dev packages | `libavformat`, `libavcodec`, `libavutil`, `libswresample` |
| Doxygen | 1.9+ | Only required for `KANARIV_BUILD_DOCS=ON` |

### Windows notes

- Prefer a Ninja shell with Visual Studio environment variables (`vcvars`)
- Exclude the repo from Windows Defender real-time scanning — it multiplies
  configure times
- MinGW GCC is also supported

## Install via the configure script (recommended)

Clone the repository:

```sh
git clone https://github.com/AhmedowM/kanariv.git
cd kanariv
```

Configure a development tree with tests and examples:

```sh
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test
```

This configures, builds, and runs the test suite in one command. Useful
variations:

```sh
# List available options
python scripts/configure.py --help
python scripts/configure.py dev --help

# CI-style headless tree (no audio device required)
python scripts/configure.py ci --toolchain clang --build-dir build/ci --build --test

# C++23 modules build
python scripts/configure.py modules --toolchain clang --build-dir build/modules-clang --build

# Release build with a specific FFmpeg installation
python scripts/configure.py release --toolchain clang \
  --ffmpeg-root /opt/ffmpeg --build-dir build/release-clang
```

Wrapper intents: `dev`, `ci`, `release`, `release-lto`, `minsize`,
`minsize-lto`, `modules`, `android`.

## Bare CMake

Useful for packaging or integrating into an existing build:

```sh
cmake -S . -B build/custom -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=23 \
  -DKANARIV_ENABLE_TESTS=ON \
  -DKANARIV_BUILD_SHARED=ON
cmake --build build/custom --parallel 4
ctest --test-dir build/custom --output-on-failure --parallel 4
```

For modules, add `-DKANARIV_ENABLE_MODULES=ON` and use Ninja.

## Platform-specific notes

### Linux

Install FFmpeg development packages from your distribution, e.g.:

```sh
# Ubuntu/Debian
sudo apt-get install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

### macOS

Install Ninja and FFmpeg with Homebrew, then use `--toolchain apple` for
Xcode AppleClang or `--toolchain clang` for Homebrew LLVM.

### FreeBSD

The tested path is FreeBSD 15.1 with GCC 14 and system FFmpeg.

### Android

Build FFmpeg for Android, then configure the native library:

```sh
python scripts/build-ffmpeg.py --android-ndk <path> \
  --android-abi arm64-v8a --android-api 24 --prefix build/ffmpeg-android-arm64
python scripts/configure.py android --ndk <path> \
  --ffmpeg-root build/ffmpeg-android-arm64 --build
```

The `android/` directory also contains a Java app that hosts the daemon
in-process and integrates with the system media controls. See
`android/README.md` for the APK build, install and verification steps.

## Headless test runs

To run tests without an audio device:

```sh
KANARIV_TEST_NOAUDIO=1 ctest --test-dir build --output-on-failure -j4
```

## Installing

```sh
cmake --install build/dev --prefix "$PWD/stage"
```

Packages use ZIP on Windows and TGZ elsewhere.

## Next steps

- [Quick Start](/getting-started/quickstart) — scan, queue, and play your first track
- [Command-Line Client](/guides/cli) — complete CLI reference
- [Build from Source guide](/guides/build-from-source) — detailed build options
