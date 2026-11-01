---
title: Player — caudio::player
description: Audio output and playback primitives (Player, State, PlayerOptions)
---

# Player — `caudio::player`

The `caudio::player` component provides audio output and playback primitives
with gapless playback support.

## Player

```cpp
#include <caudio/player/player.hpp>

struct PlayerOptions {
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    caudio::utils::Logger* logger = nullptr;
};

class Player {
public:
    using ExpectedVoid = std::expected<void, Error>;
    using ExpectedPlayer = std::expected<std::unique_ptr<Player>, Error>;

    static ExpectedPlayer create(const PlayerOptions& opts = {});
    ~Player();

    ExpectedVoid open(std::string_view path);
    ExpectedVoid openReader(std::unique_ptr<media::Reader> reader);
    ExpectedVoid play();
    ExpectedVoid pause();
    ExpectedVoid resume();
    ExpectedVoid stop();
    ExpectedVoid seek(double seconds);
    ExpectedVoid setVolume(float volume);

    State state() const noexcept;
    std::chrono::duration<double> position() const noexcept;
};

enum class State : uint8_t {
    Stopped = 0, Playing = 1, Paused = 2, Error = 3
};
```

## Usage

```cpp
auto player = caudio::player::Player::create();
player.value()->open("/path/to/track.flac");
player.value()->play();
// ...
player.value()->stop();
```

## Thread safety

Public methods are thread-safe. State transitions use atomic `compare_exchange`
for lock-free control where possible.

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/player/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/player)
