---
title: Architecture
description: kanariv system architecture, component graph, and daemon design
---

# Architecture

kanariv is a headless music player built as a reusable C++23 library with a
client-daemon architecture.

## Component graph

<div class="architecture-diagram" role="img" aria-label="kanariv client daemon architecture">
  <div class="architecture-node"><strong>kanariv CLI</strong><small>CLI11, JSON IPC, output formatting</small></div>
  <div class="architecture-arrow" aria-hidden="true">↓ JSON IPC · 4-byte big-endian framing</div>
  <div class="architecture-node">
    <strong>Daemon / Service</strong>
    <small>Coordinates the runtime components</small>
    <div class="architecture-services">
      <div class="architecture-node"><strong>Engine</strong><small>Queue and playback state</small></div>
      <div class="architecture-node"><strong>Database</strong><small>SQLite and FTS5</small></div>
      <div class="architecture-node"><strong>Player</strong><small>Decode and output</small></div>
      <div class="architecture-node"><strong>IPC</strong><small>Server and framing</small></div>
      <div class="architecture-node"><strong>Media</strong><small>Scan and decode</small></div>
      <div class="architecture-node"><strong>Config</strong><small>Paths and loading</small></div>
    </div>
  </div>
  <div class="architecture-arrow" aria-hidden="true">↓ shared memory · 10 Hz</div>
  <div class="architecture-node"><strong>Lock-free status snapshots</strong><small>Lightweight clients consume playback state</small></div>
</div>

## Library targets

| Target | Responsibility |
|---|---|
| `kanariv::utils` | Errors, logging, JSON, queues, threads, versioning |
| `kanariv::types` | Shared value types |
| `kanariv::contracts` | Backend-neutral persistence interface (`IDatabase`) |
| `kanariv::media` | Reading, decoding, metadata, library discovery |
| `kanariv::player` | Audio output and playback primitives |
| `kanariv::db` | SQLite storage and FTS5 search |
| `kanariv::engine` | Queue, shuffle, repeat, history, playback state |
| `kanariv::ipc` | Daemon protocol, framing, configuration |
| `kanariv::client` | Daemon client and output formatting |
| `kanariv::service` | Daemon service and in-process service host |
| `kanariv::config` | Configuration management |

## Daemon design

The daemon owns the `Service`, which coordinates:

1. **Database** — SQLite with FTS5, WAL mode, `cache_size=-8000`,
   `busy_timeout=5000`
2. **Engine** — Queue, shuffle, repeat, cursor, playback state
3. **Player** — Decode thread, ring buffer, miniaudio output
4. **IPC server** — JSON-over-socket/named-pipe with 4-byte big-endian framing
5. **Shared memory** — Lock-free playback snapshots at 10 Hz

## Client design

The `Client` is synchronous: each `send()` opens a transport connection, sends
one length-prefixed IPC request, waits for the reply, and closes the connection.
Clients do not own or start the daemon.

## Thread model

- **Control thread:** Application calls `play()`, `pause()`, `stop()`, `seek()`
- **Decode thread:** Dedicated `jthread` running `decodeLoop()`
- **Audio thread:** miniaudio callback (real-time priority)
- **IPC server thread:** Handles client connections serially

State transitions use `compare_exchange` for lock-free control where possible.

## Error handling

All fallible operations return `std::expected<T, kanariv::utils::Error>`.
Errors are not thrown across module boundaries.

## C++23 modules

Modules mirror the public headers and are opt-in. Consumers rebuild BMIs with
their own compiler; prebuilt BMIs are not portable across compilers.
