---
title: C++23 Modules
description: Using caudio as C++23 modules instead of traditional headers
---

# C++23 Modules

caudio provides opt-in C++23 modules that mirror the public library headers.
Modules are available for compilers with import-graph scanning support
(GCC 14+, LLVM Clang 19+, MSVC 2022).

## Building modules

```sh
python scripts/configure.py modules --toolchain clang --build-dir build/modules-clang --build
```

Or with bare CMake:

```sh
cmake -S . -B build/modules -G Ninja \
  -DCAUDIO_ENABLE_MODULES=ON \
  -DCMAKE_CXX_STANDARD=23
cmake --build build/modules --parallel 4
```

## Available modules

| Module | Header equivalent |
|---|---|
| `caudio` | `<caudio.hpp>` (umbrella) |
| `caudio.utils` | `<caudio/utils.hpp>` |
| `caudio.types` | `<caudio/types.hpp>` |
| `caudio.contracts` | `<caudio/contracts/database.hpp>` |
| `caudio.media` | `<caudio/media.hpp>` |
| `caudio.player` | `<caudio/player.hpp>` |
| `caudio.db` | `<caudio/db.hpp>` |
| `caudio.engine` | `<caudio/engine.hpp>` |
| `caudio.ipc` | `<caudio/ipc.hpp>` |
| `caudio.client` | `<caudio/client.hpp>` |
| `caudio.service` | `<caudio/service.hpp>` |
| `caudio.config` | `<caudio/config.hpp>` |

Partitions such as `caudio.db:scan` are also available.

## Using modules

```cpp
import caudio.media;

int main() {
    auto tracks = caudio::media::scanTracksToVector(".");
    return tracks ? 0 : 1;
}
```

```cpp
import caudio.engine;
import caudio.db;

int main() {
    auto engine = caudio::engine::Engine::create();
    return engine ? 0 : 1;
}
```

## Notes

- Consumers rebuild BMIs with their own compiler
- Prebuilt BMIs are not portable across compilers or incompatible flags
- Public headers remain canonical; module partitions mirror them
- Do not add new module-only APIs

## Next steps

- [API Reference](/reference/api/) — C++ library API documentation
- [Build from Source](/guides/build-from-source) — build options
