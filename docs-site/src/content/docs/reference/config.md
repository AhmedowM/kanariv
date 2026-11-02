---
title: Configuration Reference
description: Configuration file format and canonical path reference
---

# Configuration Reference

kanariv reads configuration from a JSON file and exposes canonical path helpers
for runtime artifacts.

## Configuration file

Default location: XDG/LOCALAPPDATA derived, e.g.
`~/.local/share/kanariv/config.json`

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
| `dbPath` | string | XDG/LOCALAPPDATA derived | SQLite database file path |
| `configPath` | string | XDG/LOCALAPPDATA derived | Configuration file path |
| `device` | string | `"auto"` | Audio device ID (`"auto"` for default) |
| `logLevel` | integer | `2` | 0=trace, 1=debug, 2=info, 3=warn, 4=error |
| `socketPath` | string | `""` | Explicit socket/named pipe path (empty = derived) |
| `autoResumeOnPlay` | boolean | `false` | Resume persisted track on play after daemon restart |
| `clearStopsPlayback` | boolean | `true` | Stop playback when queue is cleared |
| `repeatDefault` | string | `"off"` | Default repeat mode: `"off"`, `"one"`, `"all"` |
| `autostart` | boolean | `false` | Auto-start playback when track added to empty queue |

## Config commands

```sh
# List all configuration as JSON
kanariv config list --json

# Get a single value
kanariv config get logLevel

# Set a value
kanariv config set logLevel 3
```

Legacy snake_case keys (`db_path`, `log_level`) are also supported.

## Canonical paths (C++ API)

The `kanariv::config` module derives runtime paths from `dbPath`:

| Function | Platform behavior |
|---|---|
| `socketPathFor(dbPath)` | Named pipe on Windows; socket file on POSIX under XDG_RUNTIME_DIR |
| `pidPathFor(dbPath)` | PID file under `%LOCALAPPDATA%/kanariv` (Windows) or XDG data dir |
| `lockPathFor(dbPath)` | Lock file next to PID file |
| `logPathFor(dbPath)` | `<db-dir>/kanariv.log`; falls back to temp dir |

## ServiceConfig (in-process hosts)

`kanariv::service::ServiceConfig` extends `Config` for in-process use:

```cpp
kanariv::service::ServiceConfig cfg;
cfg.runtime.database = "/data/app/kanariv/kanariv.db";
cfg.runtime.logs = "/data/app/kanariv/logs";
cfg.inProcess = true;
```

## CMake cache options

| Option | Default | Description |
|---|---|---|
| `KANARIV_BUILD_SHARED` | `ON` | Build per-component shared libraries |
| `KANARIV_BUILD_CLI` | `ON` | Build the `kanariv` executable |
| `KANARIV_ENABLE_TESTS` | `OFF` | Build Catch2 tests |
| `KANARIV_ENABLE_MODULES` | `OFF` | Build C++23 module interfaces |
| `KANARIV_BUILD_DOCS` | `OFF` | Build API documentation with Doxygen |
| `KANARIV_TEST_NOAUDIO` | `OFF` | Skip audio-device tests |
| `FFmpeg_ROOT` | unset | Override FFmpeg installation path |
