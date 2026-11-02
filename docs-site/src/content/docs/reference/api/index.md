---
title: API Reference
description: C++23 library API reference for all kanariv components
---

# API Reference

kanariv provides nine installable library components plus an optional CLI.
Public headers under `include/kanariv/` are canonical, with opt-in C++23
modules that mirror them.

## Components

The API pages below are rendered from a custom intermediate model built from
Doxygen XML. The site does not use Doxygen's generated HTML: namespaces,
classes, structs, enums, functions, overloads, inheritance, parameters, and
return documentation are rendered in the same shell as the rest of this site.

| Component | Description |
|---|---|
| [Utils](/reference/api/utils) | Errors, logging, JSON, queues, threads, versioning |
| [Types](/reference/api/types) | Shared value types (Track, Playlist, QueueItem, etc.) |
| [Contracts](/reference/api/contracts) | Backend-neutral persistence interface |
| [Media](/reference/api/media) | Reading, decoding, metadata, library discovery |
| [Player](/reference/api/player) | Audio output and playback primitives |
| [Database](/reference/api/db) | SQLite storage and FTS5 search |
| [Engine](/reference/api/engine) | Queue, shuffle, repeat, history, playback state |
| [IPC](/reference/api/ipc) | Daemon protocol, framing, configuration |
| [Client](/reference/api/client) | Daemon client and output formatting |
| [Service](/reference/api/service) | Daemon service and in-process host |
| [Config](/reference/api/config) | Configuration management |

## Using the library

### CMake

```cmake
find_package(kanariv CONFIG REQUIRED)

add_executable(my_player main.cpp)
target_link_libraries(my_player PRIVATE kanariv::engine kanariv::db)
```

### C++ (headers)

```cpp
#include <kanariv.hpp>
#include <memory>
#include <print>

int main() {
    auto database = kanariv::db::Database::open(":memory:");
    if (!database) {
        std::println(stderr, "database open failed: {}",
                     database.error().message);
        return 1;
    }

    auto engine = kanariv::engine::Engine::create();
    if (!engine) {
        std::println(stderr, "engine create failed: {}",
                     engine.error().message);
        return 1;
    }

    if (auto result = engine.value()->attachDatabase(
            std::shared_ptr<kanariv::db::Database>(
                std::move(database.value())));
        !result) {
        std::println(stderr, "attach failed: {}", result.error().message);
        return 1;
    }

    return engine.value()->play() ? 0 : 1;
}
```

### C++23 modules

```cpp
import kanariv.media;

int main() {
    auto tracks = kanariv::media::scanTracksToVector(".");
    return tracks ? 0 : 1;
}
```

## Error handling

All fallible operations return `std::expected<T, kanariv::utils::Error>`.
Errors carry a `StatusCode` and an optional diagnostic message:

```cpp
if (auto result = engine.value()->play(); !result) {
    std::println(stderr, "play failed: {}",
                 result.error().message);
}
```

Common status codes: `Ok`, `InvalidArg`, `NotFound`, `Unsupported`, `Io`,
`Device`, `State`, `NoMem`, `Internal`, `AlreadyExists`, `Busy`, `Corrupt`,
`NoSpace`.

## Thread safety

Each component documents its own thread-safety guarantees. In general:

- Public methods on `Player`, `Engine`, `Client` are thread-safe
- `Service` is not thread-safe for concurrent method calls
- `IDatabase` implementations must serialize their own access
- `Reader` interface is thread-compatible (single-threaded use expected)

## Next steps

- [CLI Reference](/reference/cli) — command-line reference
- [Configuration Reference](/reference/config) — config file reference
- [IPC Protocol](/concepts/ipc-protocol) — daemon protocol details
