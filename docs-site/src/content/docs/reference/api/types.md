---
title: Types — caudio::types
description: Shared value types — Track, Playlist, QueueItem, SourceRef, and more
---

# Types — `caudio::types`

The `caudio::types` component provides dependency-free, shared value types used
across the db, engine, ipc, and app layers.

## Core types

| Type | Description |
|---|---|
| `Track` | A library track with metadata |
| `Playlist` | A playlist definition |
| `QueueItem` | An item in a playback queue |
| `HistoryEntry` | A playback history record |
| `Bookmark` | A position bookmark in a track |
| `SourceRef` | Persisted source identity |
| `Library` | A library root definition |
| `EngineState` | Persisted engine state |
| `PlaybackStats` | Aggregate playback statistics |
| `TrackQuery` | Track filter/query parameters |
| `DeviceInfo` | Audio device information |
| `LibraryStats` | Library totals and top-N reports |

## Source kinds

```cpp
enum class SourceKind : std::uint8_t {
    path = 0,        // Ordinary filesystem path
    external = 1,    // Host-issued stable identifier
    imported = 2,    // App-private imported copy
    unavailable = 3  // Source cannot currently be reopened
};
```

## Repeat modes

```cpp
enum class RepeatMode : std::uint8_t {
    Off = 0,   // Linear advance with wrap
    All = 1,   // Repeat the full queue
    One = 2,   // Repeat current track
};
```

## Playback states

```cpp
enum class PlaybackState : std::uint8_t {
    Stopped = 0,  // No track playing
    Ready = 1,    // Initialized, ready to play
    Playing = 2,  // Audio playback active
    Paused = 3,   // Paused, position retained
};
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/types/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/types)
