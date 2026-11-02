---
title: Configuration
description: Runtime configuration options for kanariv daemon and client
---

# Configuration

kanariv reads configuration from a JSON config file and provides canonical path
helpers for socket, PID, lock, and log file locations.

## Config structure

```json
{
  "dbPath": "~/.local/share/kanariv/library.db",
  "configPath": "~/.local/share/kanariv/config.json",
  "device": "auto",
  "logLevel": 2,
  "socketPath": "",
  "autoResumeOnPlay": false,
  "clearStopsPlayback": true,
  "repeatDefault": "off",
  "autostart": false
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `dbPath` | path | XDG/LOCALAPPDATA derived | Database file path |
| `configPath` | path | XDG/LOCALAPPDATA derived | Config file path |
| `device` | string | `"auto"` | Audio device ID |
| `logLevel` | int | `2` | 0=trace, 1=debug, 2=info, 3=warn, 4=error |
| `socketPath` | string | derived from dbPath | Explicit socket path |
| `autoResumeOnPlay` | bool | `false` | Resume persisted track on play after restart |
| `clearStopsPlayback` | bool | `true` | Stop playback when queue is cleared |
| `repeatDefault` | string | `"off"` | `"off"`, `"one"`, or `"all"` |
| `autostart` | bool | `false` | Auto-start playback when track added to empty queue |

## Config commands

```sh
# List all configuration as JSON
kanariv config list --json

# Get a single value
kanariv config get logLevel

# Set a value
kanariv config set logLevel 3
```

## Canonical paths

The `kanariv::config` module provides path helpers derived from `dbPath`:

| Function | Returns |
|---|---|
| `socketPathFor(dbPath)` | Daemon socket/named pipe path |
| `pidPathFor(dbPath)` | PID file path |
| `lockPathFor(dbPath)` | Lock file path |
| `logPathFor(dbPath)` | Daemon log file path |

- **Windows:** Socket is a named pipe; PID/lock files under `%LOCALAPPDATA%/kanariv`
- **POSIX:** Socket/PID/lock under `$XDG_RUNTIME_DIR/kanariv` or
  `$XDG_DATA_HOME/kanariv` or `~/.local/share/kanariv`

## ServiceConfig (C++ API)

For in-process service hosts, `kanariv::service::ServiceConfig` extends
`kanariv::config::Config`:

```cpp
kanariv::service::ServiceConfig cfg;
cfg.runtime.database = "/data/app/kanariv/kanariv.db";
cfg.runtime.logs = "/data/app/kanariv/logs";
cfg.inProcess = true;  // No desktop daemon scaffolding
auto host = kanariv::service::Host::create(cfg);
```

## Next steps

- [Command-Line Client](/guides/cli) — CLI reference
- [API Reference](/reference/api/) — C++ library API documentation
