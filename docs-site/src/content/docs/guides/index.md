---
title: Guides
description: Practical guides for building, configuring, and using caudio.
---

# Guides

Practical walkthroughs for building caudio, managing a local library, and
using the command-line client. Start with the guide that matches the job you
are doing, then use the CLI and configuration references for exact options.

## Build and configure

- [Build from Source](/guides/build-from-source) — configure and compile caudio
- [Configuration](/guides/configuration) — configure the daemon, database, and audio device
- [C++23 Modules](/guides/modules) — opt into named modules

## Use the player

- [Command-Line Client](/guides/cli) — commands and options
- [Library Management](/guides/library) — scan, search, and organize music
- [Playback](/guides/playback) — queues and playback control

## Recommended workflow

1. Build a development tree with tests enabled.
2. Scan one music directory and inspect the search results.
3. Configure the database and daemon paths before making a long-running setup.
4. Put the CLI behind your scripts or connect a custom client over IPC.

The [API reference](/reference/api/) covers library symbols separately; these
guides focus on workflows and operational decisions.
