---
title: Command-Line Client
description: Complete CLI reference for kanariv commands and options
---

# Command-Line Client

The `kanariv` executable is a synchronous client for the kanariv daemon. Most
commands communicate with the daemon; configuration commands work without one,
and playback commands can start it automatically.

## Global options

```sh
kanariv [global options] <command> [command options]
```

| Option | Description |
|---|---|
| `--help` | Show help |
| `--version` | Show version |
| `--log-level <level>` | Log level: trace, debug, info, warn, error |
| `--config <path>` | Path to config file |

## Commands

### `library scan`

Scan a directory and add tracks to the library database.

```sh
kanariv library scan --path ~/Music
kanariv library scan --path ~/Music --mode full
kanariv library scan --path ~/Music --library-id 2
```

| Option | Description | Default |
|---|---|---|
| `--path` | Directory to scan | required |
| `--mode` | Scan mode: sampled or full | sampled |
| `--library-id` | Library id to associate tracks with | 1 |

### `library search`

Search the library using FTS5 full-text search.

```sh
kanariv library search "miles davis" --limit 10
kanariv library search "kind of blue" --limit 20 --offset 0
```

| Option | Description | Default |
|---|---|---|
| Query text | Search string | required |
| `--limit` | Maximum results | 20 |
| `--offset` | Pagination offset | 0 |

### `library list`

List all tracks in the library.

```sh
kanariv library list --limit 50
```

### `queue add`

Add tracks to the queue.

```sh
kanariv queue add ~/Music/track.flac
kanariv queue add ~/Music/*.flac --position 0
```

| Option | Description | Default |
|---|---|---|
| `--position` | Insert position (-1 = append) | -1 |

### `queue list`

List items in the current queue.

```sh
kanariv queue list
```

### `play`

Start or resume playback.

```sh
kanariv play
```

### `pause`

Pause playback.

```sh
kanariv pause
```

### `resume`

Resume playback from paused state.

```sh
kanariv resume
```

### `stop`

Stop playback.

```sh
kanariv stop
```

### `seek`

Seek to a position in seconds.

```sh
kanariv seek 120.5
```

### `status`

Show current playback status.

```sh
kanariv status
kanariv status --json
kanariv status --watch --interval 1000
```

| Option | Description | Default |
|---|---|---|
| `--json` | Output as JSON | false |
| `--watch` | Watch status continuously | false |
| `--interval` | Watch interval in ms | 1000 |

### `start`

Start the daemon.

```sh
kanariv start
```

### `shutdown`

Shut down the daemon.

```sh
kanariv shutdown
```

### `config`

Manage configuration.

```sh
kanariv config list --json
kanariv config get dbPath
kanariv config set logLevel 3
```

### Playing a path directly

```sh
# Play a single track
kanariv ~/Music/album/track.flac

# Play a directory recursively and save to library
kanariv ~/Music/album --recursive --save
```

## Next steps

- [Configuration](/guides/configuration) — runtime configuration options
- [API Reference](/reference/api/) — C++ library API documentation
- [IPC Protocol](/concepts/ipc-protocol) — daemon protocol details
