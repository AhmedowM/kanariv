---
title: Playback
description: Playback controls, gapless playback, queue management
---

# Playback

caudio provides gapless playback with sample-accurate seeking, managed through
the command-line client or the C++ `caudio::player` and `caudio::engine`
libraries.

## Basic playback

```sh
caudio play          # Start or resume
caudio pause         # Pause
caudio resume        # Resume from paused
caudio stop          # Stop
caudio seek 120.5    # Seek to 120.5 seconds
caudio status        # Show current status
caudio status --json # Status as JSON
```

## Queue management

```sh
caudio queue add ~/Music/track.flac          # Add track
caudio queue add ~/Music/*.flac --pos 0      # Add at position
caudio queue list                            # List queue
caudio queue clear                           # Clear queue
caudio queue shuffle                         # Shuffle queue
caudio queue repeat on|one|off               # Set repeat mode
```

## Playback state machine

```
Stopped <-> Playing <-> Paused
     ^         |          |
     |         v          v
     +-----> Error <------+
```

| State | Description |
|---|---|
| `Stopped` | No track playing |
| `Playing` | Audio playback active |
| `Paused` | Playback paused, position retained |
| `Error` | Decoder or output failure |

## Gapless playback

Track transitions are gapless by default:

- Ring buffer prerolls to half capacity before `play()` returns
- Seeking resets the ring buffer and updates position atomically
- No allocations in the hot path (decode loop or audio callback)
- Decode thread maintains steady fill rate; audio thread consumes

## C++ API

### Player

```cpp
#include <caudio/player/player.hpp>

auto opts = caudio::player::PlayerOptions{};
opts.sampleRate = 48000;
opts.channels = 2;

auto player = caudio::player::Player::create(opts);
player.value()->open("/path/to/track.flac");
player.value()->play();
```

### Engine

```cpp
#include <caudio/engine/engine.hpp>

auto engine = caudio::engine::Engine::create();
engine.value()->attachDatabase(database);
engine.value()->play();
```

## Next steps

- [Command-Line Client](/guides/cli) — CLI reference
- [API Reference](/reference/api/) — player and engine API
