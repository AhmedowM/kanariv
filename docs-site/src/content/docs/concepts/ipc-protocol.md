---
title: IPC Protocol
description: Daemon client protocol — framing, commands, results, errors
---

# IPC Protocol

caudio's daemon communicates with clients over a JSON-over-IPC protocol with
4-byte big-endian length framing.

## Message framing

Every IPC message is:

```
+--------+---------------------+
| 4 bytes|   N bytes JSON  |
| (BE u32)|   payload       |
+--------+---------------------+
```

The 4-byte prefix is the length of the JSON payload in bytes, encoded as a
big-endian `uint32_t`.

## Request format

```json
{
  "id": 123,
  "cmd": {
    "type": "play",
    ...
  }
}
```

| Field | Type | Description |
|---|---|---|
| `id` | uint32 | Request identifier (matched in reply) |
| `cmd` | object | Command object with `type` discriminator |

## Reply format

```json
{
  "id": 123,
  "ok": true,
  "result": { ... }
}
```

or on error:

```json
{
  "id": 123,
  "ok": false,
  "error": {
    "code": "NotFound",
    "message": "track not found"
  }
}
```

| Field | Type | Description |
|---|---|---|
| `id` | uint32 | Matches the request id |
| `ok` | bool | `true` for success, `false` for error |
| `result` | object | Command-specific result (when `ok=true`) |
| `error` | object | `code` + `message` (when `ok=false`) |

## Command types

Commands are organized by group:

| Group | Types |
|---|---|
| `playback` | play, pause, resume, stop, seek, volume |
| `queue` | add, list, clear, shuffle, repeat |
| `library` | scan, search, list, rate, stats |
| `playlist` | create, add, remove, list, reorder |
| `bookmark` | add, list, remove |
| `config` | get, set, list |
| `system` | status, shutdown |
| `device` | list devices |
| `history` | list, clear |
| `tag` | batch operations |
| `system` | shutdown |

## Error codes

| Code | Meaning |
|---|---|
| `Ok` | Success |
| `InvalidArg` | Invalid caller input |
| `NotFound` | Entity not found |
| `Unsupported` | Operation not supported |
| `Io` | I/O error |
| `Device` | Audio device error |
| `State` | Invalid lifecycle state |
| `NoMem` | Memory allocation failure |
| `Internal` | Internal invariant violation |
| `AlreadyExists` | Entity already exists |
| `Busy` | Resource busy or queue full |
| `Corrupt` | Data corruption |
| `NoSpace` | No space left |

## Shared memory status

The daemon also publishes lock-free playback snapshots to shared memory at
10 Hz for lightweight clients that cannot use the socket:

- Fields: position, duration, volume, muted, state, track_id, queue_size,
  title, artist
- Seqlock-protected: readers retry on sequence mismatch
- No IPC round-trip required

## Next steps

- [API Reference: IPC](/reference/api/ipc) — C++ IPC API
- [Command-Line Client](/guides/cli) — CLI that uses this protocol
