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
| `dbPath` | string | XDG/LOCALAPPDATA derived | SQLite database file path |
| `configPath` | string | XDG/LOCALAPPDATA derived | Configuration file path |
| `device` | string | `"auto"` | Audio device id (`"auto"` for default). Opaque stable id from `kanariv device list --json`; a device name or a legacy numeric index is also accepted |
| `deviceName` | string | `""` | Display name paired with `device`; used when the id is no longer enumerated, before falling back to the default device with a warning |
| `logLevel` | integer | `2` | 0=trace, 1=debug, 2=info, 3=warn, 4=error |
| `socketPath` | string | `""` | Explicit socket/named pipe path (empty = derived) |
| `autoResumeOnPlay` | boolean | `false` | Resume persisted track on play after daemon restart |
| `clearStopsPlayback` | boolean | `true` | Stop playback when queue is cleared |
| `repeatDefault` | string | `"off"` | Default repeat mode for new and switched-to queues: `"off"`, `"one"`, `"all"` (case-insensitive; unknown values mean `off`) |
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

Socket/pipe, PID, and lock names embed a 16-hex FNV-1a 64-bit digest of the
canonical UTF-8 `dbPath` (forward slashes). The digest is specified rather than
inherited from `std::hash`, so C++ binaries built with different standard
libraries agree. These names are a **breaking on-disk rename** from the previous
8-hex `std::hash` identifier: old `kanariv-<id>.sock`/`.pid`/`.lock` files are
orphaned after upgrade and must be removed manually; upgrade the daemon and
clients together (or set `socketPath`).

Configuration is saved atomically: `saveConfig` writes `<configPath>.tmp`,
checks every write, then renames it over the target, so an interrupted save
cannot truncate the live config file.

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
| `KANARIV_BUILD_COMBINED` | `ON` | Build the combined `kanariv` umbrella shared library (shared builds only) |
| `KANARIV_BUILD_CLI` | `ON` | Build the `kanariv` executable |
| `KANARIV_ENABLE_TESTS` | `OFF` | Build Catch2 tests |
| `KANARIV_ENABLE_MODULES` | `OFF` | Build C++23 module interfaces |
| `KANARIV_BUILD_DOCS` | `OFF` | Build API documentation with Doxygen |
| `KANARIV_TEST_NOAUDIO` | `OFF` | Skip audio-device tests |
| `FFmpeg_ROOT` | unset | Override FFmpeg installation path |
