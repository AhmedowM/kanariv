---
title: kanariv
description: A local-first, headless music player and reusable C++23 library.
---

# A music engine for your local library

**kanariv** is a headless music player and reusable C++23 library for people who
want a fast, scriptable, local-first audio stack. It decodes media with
FFmpeg, persists library and playback state in SQLite, and exposes the same
engine through a command-line client and a documented IPC protocol.

No account is required. There is no cloud catalog and no GUI to get in the
way of automation.

## Why kanariv

### Local by default

Your files, metadata, queues, ratings, and playback history remain on your
machine. The daemon owns the local state; clients can be replaced without
moving your library.

### A real library, not just a player

Scan a directory, search with SQLite FTS5, rate tracks, create playlists, keep
history, and resume playback with bookmarks and statistics.

### Built for integration

The public library is split into focused components, while JSON IPC and
lock-free shared-memory status make it practical to build lightweight clients
in other languages.

## Core capabilities

| Capability | What it provides |
|---|---|
| Gapless playback | Sample-accurate seeking and transitions without audible gaps |
| Broad decoding | FFmpeg-backed MP3, FLAC, OGG/Vorbis, AAC/M4A, Opus, WAV, WMA, and more |
| Library search | SQLite persistence with FTS5, ratings, play counts, bookmarks, and statistics |
| Scriptable control | CLI commands for scanning, queueing, playback, playlists, and configuration |
| JSON IPC | 4-byte big-endian framing with request IDs, typed commands, and structured errors |
| Low-overhead status | Lock-free playback snapshots published to shared memory at 10 Hz |
| C++23 distribution | Nine installable libraries plus optional named modules |

## Start here

1. [Download](/download) — see release status and current source options
2. [Installation](/getting-started/installation) — prepare your compiler and FFmpeg
3. [Quick Start](/getting-started/quickstart) — scan, search, queue, and play
4. [Architecture](/concepts/architecture) — understand the daemon and library layers

## A five-minute preview

```sh
git clone https://github.com/AhmedowM/kanariv.git
cd kanariv
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test
./build/dev/kanariv library scan --path ~/Music
./build/dev/kanariv library search "miles davis" --limit 10
./build/dev/kanariv queue add ~/Music/Kind\ of\ Blue/*.flac
./build/dev/kanariv play
```

## Supported platforms

| Platform | Toolchain | Status |
|---|---|---|
| Linux | GCC 13+ or LLVM Clang 19+ | Supported; GCC 14+ for modules |
| Windows | MSVC 2022 | Supported |
| macOS | Xcode 26 / AppleClang | Supported |
| FreeBSD 15.1 | GCC 14 | Supported |

The [API reference](/reference/api/) is intentionally being integrated from
Doxygen XML separately from these curated guides.
