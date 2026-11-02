---
title: Quick Start
description: Scan your music library, queue tracks, and play with kanariv
---

# Quick Start

After building with the `dev` intent, try these commands:

This walkthrough uses `~/Music` as an example. Replace it with the directory
that contains your audio files. The scanner records metadata in SQLite; it
does not copy or move your media.

```sh
# 1. Scan your music library into the database
./build/dev/kanariv library scan --path ~/Music

# 2. Search for tracks
./build/dev/kanariv library search "miles davis" --limit 10

# 3. Add tracks to the queue
./build/dev/kanariv queue add ~/Music/Kind\ of\ Blue/*.flac

# 4. Play
./build/dev/kanariv play

# 5. Watch status (10 Hz shared-memory snapshots)
./build/dev/kanariv status --watch --interval 1000
```

## Managing the daemon

kanariv uses a client-daemon architecture. The daemon can be managed explicitly:

```sh
# Start the daemon
./build/dev/kanariv start

# Check status as JSON
./build/dev/kanariv status --json

# Pause playback
./build/dev/kanariv pause

# Resume playback
./build/dev/kanariv resume

# Stop playback
./build/dev/kanariv stop

# Shut down the daemon
./build/dev/kanariv shutdown
```

## Play a file directly

You can play a path without creating a permanent library queue:

```sh
# Play a single track
./build/dev/kanariv ~/Music/album/track.flac

# Play a directory recursively and save it to the library
./build/dev/kanariv ~/Music/album --recursive --save
```

## Get help

```sh
kanariv --help
kanariv <command> --help
```

Most commands communicate with the daemon; configuration commands work without
one, and playback commands can start it automatically.

## A repeatable first session

Use this sequence when validating a new build:

```sh
# Inspect the daemon and available audio devices
./build/dev/kanariv status --json
./build/dev/kanariv device list

# Search the indexed library, then add a specific result
./build/dev/kanariv library search "miles davis" --limit 10
./build/dev/kanariv queue clear
./build/dev/kanariv queue add ~/Music/Kind\ of\ Blue/*.flac

# Control playback and inspect the current track
./build/dev/kanariv play
./build/dev/kanariv status --json
```

If a command reports that the daemon is unavailable, run `start` explicitly
and retry. For headless environments, use the no-audio test mode described in
the [installation guide](/getting-started/installation).

## Next steps

- [Command-Line Client](/guides/cli) — all commands and options
- [Configuration](/guides/configuration) — configure daemon and client behavior
- [Library Management](/guides/library) — scan, search, ratings, playlists
