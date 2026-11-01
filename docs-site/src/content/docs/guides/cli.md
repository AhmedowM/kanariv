---
title: Command-Line Client
description: Complete CLI reference for caudio commands and options
---

# Command-Line Client

The `caudio` executable is a synchronous client for the caudio daemon. Most
commands communicate with the daemon; configuration commands work without one,
and playback commands can start it automatically.

## Global options

```sh
caudio [global options] <command> [command options]
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
caudio library scan --path ~/Music
caudio library scan --path ~/Music --mode full
caudio library scan --path ~/Music --library-id 2
```

| Option | Description | Default |
|---|---|---|
| `--path` | Directory to scan | required |
| `--mode` | Scan mode: sampled or full | sampled |
| `--library-id` | Library id to associate tracks with | 1 |

### `library search`

Search the library using FTS5 full-text search.

```sh
caudio library search "miles davis" --limit 10
caudio library search "kind of blue" --limit 20 --offset 0
```

| Option | Description | Default |
|---|---|---|
| Query text | Search string | required |
| `--limit` | Maximum results | 20 |
| `--offset` | Pagination offset | 0 |

### `library list`

List all tracks in the library.

```sh
caudio library list --limit 50
```

### `queue add`

Add tracks to the queue.

```sh
caudio queue add ~/Music/track.flac
caudio queue add ~/Music/*.flac --position 0
```

| Option | Description | Default |
|---|---|---|
| `--position` | Insert position (-1 = append) | -1 |

### `queue list`

List items in the current queue.

```sh
caudio queue list
```

### `play`

Start or resume playback.

```sh
caudio play
```

### `pause`

Pause playback.

```sh
caudio pause
```

### `resume`

Resume playback from paused state.

```sh
caudio resume
```

### `stop`

Stop playback.

```sh
caudio stop
```

### `seek`

Seek to a position in seconds.

```sh
caudio seek 120.5
```

### `status`

Show current playback status.

```sh
caudio status
caudio status --json
caudio status --watch --interval 1000
```

| Option | Description | Default |
|---|---|---|
| `--json` | Output as JSON | false |
| `--watch` | Watch status continuously | false |
| `--interval` | Watch interval in ms | 1000 |

### `start`

Start the daemon.

```sh
caudio start
```

### `shutdown`

Shut down the daemon.

```sh
caudio shutdown
```

### `config`

Manage configuration.

```sh
caudio config list --json
caudio config get dbPath
caudio config set logLevel 3
```

### Playing a path directly

```sh
# Play a single track
caudio ~/Music/album/track.flac

# Play a directory recursively and save to library
caudio ~/Music/album --recursive --save
```

## Next steps

- [Configuration](/guides/configuration) — runtime configuration options
- [API Reference](/reference/api/) — C++ library API documentation
- [IPC Protocol](/concepts/ipc-protocol) — daemon protocol details
