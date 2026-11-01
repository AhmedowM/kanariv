---
title: Quick Start
description: Scan your music library, queue tracks, and play with caudio
---

# Quick Start

After building with the `dev` intent, try these commands:

This walkthrough uses `~/Music` as an example. Replace it with the directory
that contains your audio files. The scanner records metadata in SQLite; it
does not copy or move your media.

```sh
# 1. Scan your music library into the database
./build/dev/caudio library scan --path ~/Music

# 2. Search for tracks
./build/dev/caudio library search "miles davis" --limit 10

# 3. Add tracks to the queue
./build/dev/caudio queue add ~/Music/Kind\ of\ Blue/*.flac

# 4. Play
./build/dev/caudio play

# 5. Watch status (10 Hz shared-memory snapshots)
./build/dev/caudio status --watch --interval 1000
```

## Managing the daemon

caudio uses a client-daemon architecture. The daemon can be managed explicitly:

```sh
# Start the daemon
./build/dev/caudio start

# Check status as JSON
./build/dev/caudio status --json

# Pause playback
./build/dev/caudio pause

# Resume playback
./build/dev/caudio resume

# Stop playback
./build/dev/caudio stop

# Shut down the daemon
./build/dev/caudio shutdown
```

## Play a file directly

You can play a path without creating a permanent library queue:

```sh
# Play a single track
./build/dev/caudio ~/Music/album/track.flac

# Play a directory recursively and save it to the library
./build/dev/caudio ~/Music/album --recursive --save
```

## Get help

```sh
caudio --help
caudio <command> --help
```

Most commands communicate with the daemon; configuration commands work without
one, and playback commands can start it automatically.

## A repeatable first session

Use this sequence when validating a new build:

```sh
# Inspect the daemon and available audio devices
./build/dev/caudio status --json
./build/dev/caudio device list

# Search the indexed library, then add a specific result
./build/dev/caudio library search "miles davis" --limit 10
./build/dev/caudio queue clear
./build/dev/caudio queue add ~/Music/Kind\ of\ Blue/*.flac

# Control playback and inspect the current track
./build/dev/caudio play
./build/dev/caudio status --json
```

If a command reports that the daemon is unavailable, run `start` explicitly
and retry. For headless environments, use the no-audio test mode described in
the [installation guide](/getting-started/installation).

## Next steps

- [Command-Line Client](/guides/cli) — all commands and options
- [Configuration](/guides/configuration) — configure daemon and client behavior
- [Library Management](/guides/library) — scan, search, ratings, playlists
