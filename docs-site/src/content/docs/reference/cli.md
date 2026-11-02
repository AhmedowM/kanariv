---
title: CLI Reference
description: Complete command-line interface reference for kanariv
---

# CLI Reference

The `kanariv` executable provides commands for library management, playback,
queue control, configuration, and daemon lifecycle.

## Synopsis

```sh
kanariv [options] <command> [command options]
```

## Global options

| Option | Description |
|---|---|
| `--help`, `-h` | Show help and exit |
| `--version`, `-v` | Show version and exit |
| `--log-level <level>` | Set log level (trace, debug, info, warn, error) |
| `--config <path>` | Path to configuration file |

## Commands

### Library

| Command | Description |
|---|---|
| `library scan --path <dir>` | Scan directory and add tracks |
| `library search <query>` | Full-text search |
| `library list` | List all tracks |
| `library rate --track-id <id> --rating <n>` | Rate a track |
| `library stats` | Library and playback statistics |

### Queue

| Command | Description |
|---|---|
| `queue add <path...>` | Add tracks to queue |
| `queue list` | List queue items |
| `queue clear` | Clear queue |
| `queue shuffle` | Shuffle queue |
| `queue repeat <mode>` | Set repeat mode (on/one/off) |

### Playback

| Command | Description |
|---|---|
| `play` | Start or resume playback |
| `pause` | Pause playback |
| `resume` | Resume playback |
| `stop` | Stop playback |
| `seek <seconds>` | Seek to position |
| `status [--json] [--watch]` | Show playback status |

### Daemon

| Command | Description |
|---|---|
| `start` | Start daemon |
| `shutdown` | Shut down daemon |

### Configuration

| Command | Description |
|---|---|
| `config list [--json]` | List configuration |
| `config get <key>` | Get config value |
| `config set <key> <value>` | Set config value |

### Playlists

| Command | Description |
|---|---|
| `playlist create <name>` | Create playlist |
| `playlist add --playlist-id <id> --track-id <id>` | Add track |
| `playlist list [--playlist-id <id>]` | List playlists or tracks |

### Bookmarks

| Command | Description |
|---|---|
| `bookmark add --track-id <id> --position <ms>` | Add bookmark |
| `bookmark list --track-id <id>` | List bookmarks |

## Examples

```sh
# Scan music library
kanariv library scan --path ~/Music

# Search and play
kanariv library search "miles davis" --limit 10
kanariv queue add ~/Music/Kind\ of\ Blue/*.flac
kanariv play

# Direct file playback
kanariv ~/Music/album/track.flac
kanariv ~/Music/album --recursive --save

# Daemon management
kanariv start
kanariv status --json
kanariv shutdown
```

Run `kanariv --help` or `kanariv <command> --help` for command-specific help.
