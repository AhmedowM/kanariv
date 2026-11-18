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
  "deviceName": "",
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
| `device` | string | `"auto"` | Audio device id (opaque stable id, device name, or legacy index; `"auto"` = default) |
| `deviceName` | string | `""` | Display name paired with `device`, used when the id stops resolving |
| `logLevel` | int | `2` | 0=trace, 1=debug, 2=info, 3=warn, 4=error |
| `socketPath` | string | derived from dbPath | Explicit socket path |
| `autoResumeOnPlay` | bool | `false` | Resume persisted track on play after restart |
| `clearStopsPlayback` | bool | `true` | Stop playback when queue is cleared |
| `repeatDefault` | string | `"off"` | `"off"`, `"one"`, or `"all"` (case-insensitive). Stored on newly created queues and adopted when switching to a queue |
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

The endpoint identifier is the FNV-1a 64-bit digest of the canonical UTF-8
database path (forward slashes), written as 16 lowercase hex digits, so it
is identical across compilers and standard libraries. The socket/pipe, PID
file, and lock file derived from one `dbPath` share that digest.

**Upgrade note (endpoint rename):** older releases used an 8-hex identifier
derived from `std::hash<std::string>`. After upgrading, `kanariv-<old-id>.sock`,
`.pid`, and `.lock` files are orphaned (Windows named pipes are recreated under
the new name). Stop the daemon and remove those stale files manually; kanariv
does not delete them. Upgrade the daemon and every CLI together — an older
client derives the old name and will report the daemon as not running. Setting
`socketPath` explicitly overrides derivation and works for mixed versions.

## ServiceConfig (C++ API)

For in-process service hosts, `kanariv::service::ServiceConfig` composes
`kanariv::config::Config` (reachable as `cfg.base`) and adds the service's own
switches:

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
