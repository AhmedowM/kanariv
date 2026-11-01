---
title: Download
description: Download caudio builds and source packages.
---

# Download caudio

Prebuilt downloads are not published yet. This page will become the stable
home for release artifacts once the first public build is deployed.

## What will be available

- **Windows** — MSVC 2022 packages and command-line binaries
- **Linux** — distribution-appropriate archives and packages
- **macOS** — AppleClang-compatible release archives
- **FreeBSD** — source and tested binary artifacts where available
- **Source** — tagged source archives for reproducible builds

## Build it today

Until releases are published, build directly from the repository:

```sh
git clone https://github.com/AhmedowM/caudio.git
cd caudio
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test
```

See [Installation](/getting-started/installation) for compiler and FFmpeg
requirements, or [Build from Source](/guides/build-from-source) for advanced
build modes and packaging.

## Release status

The project is under active development. Follow the repository for release
announcements and consult the [release notes](/release-notes) for changes.
