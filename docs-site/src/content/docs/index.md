---
title: caudio
description: Headless local music player and reusable C++23 library
---

# caudio

**caudio** is a headless, local music player and a reusable C++23 library. It
decodes audio with FFmpeg, stores the library and playback state in SQLite, and
exposes a command-line client backed by a daemon. There is no GUI and no cloud
service.

## What caudio is

- A **headless music player** — controlled via a command-line client or any
  client that speaks the JSON-over-IPC protocol
- A **C++23 library** — nine installable components with public headers under
  `include/caudio/`, plus opt-in C++23 modules
- **Local-first** — your music library stays on your machine; no accounts, no
  cloud, no telemetry

## Core capabilities

| Capability | Details |
|---|---|
| Gapless playback | Sample-accurate seeking, track transitions without audible gaps |
| FFmpeg decoding | MP3, FLAC, OGG/Vorbis, AAC/M4A, Opus, WAV, WMA and more |
| SQLite library | FTS5 full-text search, ratings, play counts, bookmarks, statistics |
| JSON IPC | 4-byte big-endian framed JSON protocol between client and daemon |
| Shared-memory status | Lock-free playback snapshots at 10 Hz for lightweight clients |
| C++23 modules | Opt-in `import caudio.*` for supported compilers |

## Supported platforms

| Platform | Toolchain | Status |
|---|---|---|
| Linux | GCC 13+ | Supported (headers build); GCC 14+ for modules |
| Linux | LLVM Clang 19+ | Supported (headers and modules) |
| Windows | MSVC 2022 | Supported |
| macOS | Xcode 26 / AppleClang | Supported |
| FreeBSD 15.1 | GCC 14 | Supported |

## Quick start

```sh
# Clone
git clone https://github.com/AhmedowM/caudio.git
cd caudio

# Configure, build, and test (development tree)
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test

# Scan your music library
./build/dev/caudio library scan --path ~/Music

# Search and play
./build/dev/caudio library search "miles davis" --limit 10
./build/dev/caudio queue add ~/Music/Kind\ of\ Blue/*.flac
./build/dev/caudio play
```

See the [Quick Start guide](/getting-started/quickstart) for a step-by-step
walkthrough, or the [Installation guide](/getting-started/installation) for
platform-specific setup.

## Next steps

- [Installation](/getting-started/installation) — platform-specific build setup
- [Quick Start](/getting-started/quickstart) — your first scan, queue, and play
- [Command-Line Client](/guides/cli) — full CLI reference
- [API Reference](/reference/api/) — C++ library API documentation
