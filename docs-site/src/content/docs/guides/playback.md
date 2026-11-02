---
title: Playback
description: Playback controls, gapless playback, queue management
---

# Playback

kanariv provides gapless playback with sample-accurate seeking, managed through
the command-line client or the C++ `kanariv::player` and `kanariv::engine`
libraries.

## Basic playback

```sh
kanariv play          # Start or resume
kanariv pause         # Pause
kanariv resume        # Resume from paused
kanariv stop          # Stop
kanariv seek 120.5    # Seek to 120.5 seconds
kanariv status        # Show current status
kanariv status --json # Status as JSON
```

## Queue management

```sh
kanariv queue add ~/Music/track.flac          # Add track
kanariv queue add ~/Music/*.flac --pos 0      # Add at position
kanariv queue list                            # List queue
kanariv queue clear                           # Clear queue
kanariv queue shuffle                         # Shuffle queue
kanariv queue repeat on|one|off               # Set repeat mode
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
#include <kanariv/player/player.hpp>

auto opts = kanariv::player::PlayerOptions{};
opts.sampleRate = 48000;
opts.channels = 2;

auto player = kanariv::player::Player::create(opts);
player.value()->open("/path/to/track.flac");
player.value()->play();
```

### Engine

```cpp
#include <kanariv/engine/engine.hpp>

auto engine = kanariv::engine::Engine::create();
engine.value()->attachDatabase(database);
engine.value()->play();
```

## Next steps

- [Command-Line Client](/guides/cli) — CLI reference
- [API Reference](/reference/api/) — player and engine API
