---
title: Getting Started
description: Install kanariv, build your first project, and start playing music.
---

# Getting Started

Choose a path to start using kanariv:

- [Overview](/getting-started/overview) — understand the project and its capabilities
- [Installation](/getting-started/installation) — install prerequisites and build
- [Quick Start](/getting-started/quickstart) — scan, queue, and play your first track

## The shortest path

If you already have a C++23 compiler, Ninja, Python, and FFmpeg development
libraries installed:

```sh
git clone https://github.com/AhmedowM/kanariv.git
cd kanariv
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test
```

Then scan a directory and play a track:

```sh
./build/dev/kanariv library scan --path ~/Music
./build/dev/kanariv library search "album title" --limit 10
./build/dev/kanariv queue add ~/Music/album/*.flac
./build/dev/kanariv play
```

## How the pieces fit

The command-line client is intentionally thin. It talks to a local daemon,
which coordinates the database, library scanner, queue, decoder, and audio
output. Configuration is kept outside the source tree so a development build
and an installed build can use the same runtime model.

## What kanariv is

kanariv is a headless, local music player and a reusable C++23 library. It
decodes audio with FFmpeg, stores the library and playback state in SQLite, and
exposes a command-line client backed by a daemon. There is no GUI and no cloud
service.

## Highlights

- **Gapless playback** with sample-accurate seeking
- **FFmpeg-backed decoding** and metadata extraction
- **SQLite library management** with FTS5 search
- **Queues, playlists, shuffle, repeat, history, bookmarks, and statistics**
- **JSON IPC** with 4-byte big-endian framing
- **Lock-free shared-memory status updates** at 10 Hz for lightweight clients
- **Nine installable libraries**, an optional CLI, and opt-in C++23 modules

## Supported platforms

See the [Installation guide](/getting-started/installation) for the full
support matrix and toolchain requirements.

## Library components

kanariv provides nine installable library targets plus an optional CLI:

| Target | Responsibility |
|---|---|
| `kanariv::utils` | Errors, logging, JSON, queues, threads, and versioning |
| `kanariv::types` | Shared value types |
| `kanariv::contracts` | Backend-neutral interfaces |
| `kanariv::media` | Reading, decoding, metadata, and library discovery |
| `kanariv::player` | Audio output and playback primitives |
| `kanariv::db` | SQLite storage and FTS5 search |
| `kanariv::engine` | Queue, shuffle, repeat, history, and playback state |
| `kanariv::ipc` | Daemon protocol, framing, and configuration |
| `kanariv::client` | Daemon client and output formatting |
| `kanariv::service` | Daemon service and in-process service host |
| `kanariv::config` | Configuration management |

## C++23 modules

Modules are opt-in and mirror the public library targets. Available named
modules include `kanariv`, `kanariv.utils`, `kanariv.media`, `kanariv.player`,
`kanariv.db`, `kanariv.engine`, `kanariv.ipc`, `kanariv.service`, and
`kanariv.client`.

Configure with:

```sh
python scripts/configure.py modules --toolchain clang --build
```

Consumers use `import kanariv.*` rather than public headers.

## Next steps

- [Installation](/getting-started/installation) — build from source
- [Quick Start](/getting-started/quickstart) — scan, queue, play
- [API Reference](/reference/api/) — C++ library API documentation
- [Download](/download) — release status and source availability
