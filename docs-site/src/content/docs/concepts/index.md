---
title: Concepts
description: Core concepts behind the caudio architecture and protocol.
---

# Concepts

Understand how caudio is structured before integrating it into an application.
The system is deliberately split into a local service, focused C++ libraries,
and small clients that communicate through stable boundaries.

## The runtime model

The daemon owns the mutable state: the database, queue, playback engine,
decoder lifecycle, and audio output. Clients send commands and receive
structured results. This lets a terminal client, a script, and a future GUI
share the same behavior without duplicating playback logic.

- [Architecture](/concepts/architecture) — components and daemon design
- [IPC Protocol](/concepts/ipc-protocol) — framing, commands, and responses

## Design principles

- **Local-first:** the database and media files stay on the host.
- **Explicit boundaries:** library components communicate through contracts
  rather than reaching into each other's implementation details.
- **Predictable failure:** fallible operations use structured errors and IPC
  replies expose an error code plus message.
- **Low overhead:** shared-memory status avoids polling the daemon for every
  progress update.

## Where to go next

Read [Installation](/getting-started/installation) to build the service, then
follow [Quick Start](/getting-started/quickstart) to exercise the runtime.
For integration work, pair this section with the generated
[API reference](/reference/api/).
