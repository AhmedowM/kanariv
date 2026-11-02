---
title: C++23 Modules
description: Using kanariv as C++23 modules instead of traditional headers
---

# C++23 Modules

kanariv provides opt-in C++23 modules that mirror the public library headers.
Modules are available for compilers with import-graph scanning support
(GCC 14+, LLVM Clang 19+, MSVC 2022).

## Building modules

```sh
python scripts/configure.py modules --toolchain clang --build-dir build/modules-clang --build
```

Or with bare CMake:

```sh
cmake -S . -B build/modules -G Ninja \
  -DKANARIV_ENABLE_MODULES=ON \
  -DCMAKE_CXX_STANDARD=23
cmake --build build/modules --parallel 4
```

## Available modules

| Module | Header equivalent |
|---|---|
| `kanariv` | `<kanariv.hpp>` (umbrella) |
| `kanariv.utils` | `<kanariv/utils.hpp>` |
| `kanariv.types` | `<kanariv/types.hpp>` |
| `kanariv.contracts` | `<kanariv/contracts/database.hpp>` |
| `kanariv.media` | `<kanariv/media.hpp>` |
| `kanariv.player` | `<kanariv/player.hpp>` |
| `kanariv.db` | `<kanariv/db.hpp>` |
| `kanariv.engine` | `<kanariv/engine.hpp>` |
| `kanariv.ipc` | `<kanariv/ipc.hpp>` |
| `kanariv.client` | `<kanariv/client.hpp>` |
| `kanariv.service` | `<kanariv/service.hpp>` |
| `kanariv.config` | `<kanariv/config.hpp>` |

Partitions such as `kanariv.db:scan` are also available.

## Using modules

```cpp
import kanariv.media;

int main() {
    auto tracks = kanariv::media::scanTracksToVector(".");
    return tracks ? 0 : 1;
}
```

```cpp
import kanariv.engine;
import kanariv.db;

int main() {
    auto engine = kanariv::engine::Engine::create();
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
