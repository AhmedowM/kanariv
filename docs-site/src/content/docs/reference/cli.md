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
| `status [--json] [--watch \| --follow] [--interval <ms>]` | Show playback status |
| `preview <file> [--keep-playing]` | Play a file locally without queueing it |
| `lyrics get --live` | Follow lyrics as playback advances |

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

## Continuous status

`status --watch` and `status --follow` are aliases; pass at most one. The stream
reconnects automatically when the daemon restarts: each outage prints one notice
on stderr and retries with bounded exponential backoff (200 ms doubling up to
2 s). `--interval <ms>` sets the poll interval and is not capped. With `--json`,
one compact status object is printed per line, so the stream can be piped into
`jq` or other line-oriented tools.

`status --watch`, `lyrics get --live`, `preview`, and `start --foreground` all
stop cleanly on Ctrl+C (also SIGTERM on POSIX) and exit with status 0. `preview`
and `lyrics get --live` print what they are doing before playback starts and a
closing line when they finish.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Success, including a stream stopped with Ctrl+C |
| `1` | Runtime failure, including a failed or partial multi-track batch |
| `105` | Daemon is not running, is in a state that rejects the command, or failed to start |
| `106` | Invalid argument rejected by command validation |
| `109` | Requested track, queue entry, playlist, or config key not found |

The exit code is the same with and without `--json`: JSON never changes process
status. In `--json` mode a failed or partial batch still writes its JSON
document to stdout and diagnostics to stderr, and exits with code `1`.

Read-only commands never create a configuration file. Defaults are persisted
only by commands that mutate state (including an explicit daemon start); if
saving fails the CLI prints a warning and continues.

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
