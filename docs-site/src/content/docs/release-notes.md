---
title: Release Notes
description: caudio release history and changelog
---

# Release Notes

## v1.8.0 (latest)

- Daemon soak gate green (minsize, 300/300 audio and no-audio decks)
- 206/206 tests on dev-clang and dev-msvc
- Modules build on GCC 14+ and downstream header/module examples are green
- C++23 modules support with import-graph scanning

## v1.7.0

- Nine installable libraries
- JSON IPC with 4-byte big-endian framing
- Lock-free shared-memory status at 10 Hz
- Gapless playback with sample-accurate seeking

## Earlier releases

See the [CHANGELOG.md](https://github.com/AhmedowM/caudio/blob/main/CHANGELOG.md)
for the complete release history.

## Versioning

caudio follows semantic versioning. Releases are built from:
- Linux: `release-lto-gcc`
- macOS/Windows: `release-lto-clang`
