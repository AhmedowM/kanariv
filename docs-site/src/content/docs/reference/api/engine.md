---
title: Engine — caudio::engine
description: Queue, shuffle, repeat, history, and playback state
---

# Engine — `caudio::engine`

The `caudio::engine` component manages queue, shuffle, repeat, history, and
playback state.

## Engine

```cpp
#include <caudio/engine/engine.hpp>

class Engine {
public:
    using ExpectedEngine = std::expected<std::unique_ptr<Engine>, Error>;

    static ExpectedEngine create();
    ~Engine();

    Expected<void> play();
    Expected<void> pause();
    Expected<void> resume();
    Expected<void> stop();
    Expected<void> seek(double seconds);
    Expected<void> setVolume(float volume);

    // Queue operations
    Expected<void> queueAdd(int64_t trackId, int64_t position = -1);
    Expected<void> queueClear();
    Expected<void> queueShuffle();
    Expected<void> queueRepeat(RepeatMode mode);

    // State
    PlaybackState state() const noexcept;
    std::chrono::duration<double> position() const noexcept;
};
```

## See also

- [Full Doxygen API reference]({{ doxygenUrl }}/index.html) — generated from
  `include/caudio/engine/`
- [Source on GitHub](https://github.com/AhmedowM/caudio/tree/main/include/caudio/engine)
