---
title: Build from Source
description: Detailed build options, CMake presets, and configuration reference
---

# Build from Source

This guide covers the full build system: configure intents, CMake presets, bare
CMake, and all cache options.

## Configure script (recommended)

The `scripts/configure.py` wrapper selects the repository's toolchain policy,
supports cross-platform compiler selection, and keeps local commands aligned
with CI.

```sh
# Development build with examples, tests, and four jobs
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test

# Show all options
python scripts/configure.py --help
python scripts/configure.py dev --help
```

### Wrapper intents

| Intent | Purpose |
|---|---|
| `dev` | Debug development tree with tests and examples |
| `ci` | Headless CI-style tree (`CAUDIO_TEST_NOAUDIO` enabled) |
| `release` | Release-oriented build |
| `release-lto` | Release with link-time optimization |
| `minsize` | Minimize binary size |
| `minsize-lto` | Minimize size with LTO |
| `modules` | Build C++23 module interfaces |
| `android` | Android native library target |

### Common invocations

```sh
# Configure only
python scripts/configure.py dev --toolchain clang

# Configure, build, and test
python scripts/configure.py dev --toolchain clang --build --test

# With documentation
python scripts/configure.py dev --toolchain clang --docs --build

# With sanitizers
python scripts/configure.py dev --toolchain clang --sanitizers --build --test

# Specific build directory
python scripts/configure.py dev --toolchain clang --build-dir build/my-dev --build
```

## CMake presets

Presets are convenient when the host compiler is already on `PATH`:

```sh
cmake --list-presets
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --test-dir build/dev --output-on-failure --parallel 4
```

Available presets: `dev`, `ci`, `release`, `release-lto`, `minsize`,
`minsize-lto`, `modules`.

Presets use Ninja and the project toolchain dispatcher. Use the wrapper when
you need an explicit compiler, architecture, FFmpeg root, or CI-equivalent
invocation.

## Bare CMake

Useful for packaging, unusual generators, or integrating caudio into an
existing build:

```sh
cmake -S . -B build/custom -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCAUDIO_ENABLE_TESTS=ON \
  -DCAUDIO_BUILD_SHARED=ON
cmake --build build/custom --parallel 4
ctest --test-dir build/custom --output-on-failure --parallel 4
```

For modules, add `-DCAUDIO_ENABLE_MODULES=ON` and use Ninja.

## Configuration reference

| Option | Default | Description |
|---|---|---|
| `CAUDIO_BUILD_SHARED` | `ON` | Build per-component shared libraries |
| `CAUDIO_BUILD_STATIC` | Depends on shared | Build static component libraries |
| `CAUDIO_BUILD_CLI` | `ON` | Build the `caudio` executable |
| `CAUDIO_ENABLE_TESTS` | `OFF` | Build Catch2 tests |
| `CAUDIO_ENABLE_EXAMPLES` | `OFF` | Build the examples |
| `CAUDIO_ENABLE_MODULES` | `OFF` | Build/install C++23 module interfaces |
| `CAUDIO_ENABLE_SANITIZERS` | `OFF` | Enable AddressSanitizer and UBSan |
| `CAUDIO_BUILD_DOCS` | `OFF` | Build API docs with Doxygen |
| `CAUDIO_TEST_NOAUDIO` | `OFF` | Skip audio-device tests |
| `CAUDIO_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during build |
| `CAUDIO_USE_TRIMMED_FFMPEG` | `OFF` | Use decode-only trimmed FFmpeg |
| `FFmpeg_ROOT` | unset | Override FFmpeg installation |

## Building documentation

```sh
python scripts/configure.py dev --toolchain clang --docs --build
cmake --build build/dev --target doc-check --parallel 4
```

HTML output: `build/docs/html/index.html`
XML output: `build/docs/xml/index.xml`

## Using as a library

```cmake
find_package(caudio CONFIG REQUIRED)

add_executable(my_player main.cpp)
target_link_libraries(my_player PRIVATE caudio::engine caudio::db)
```

```cpp
#include <caudio.hpp>
#include <memory>
#include <print>

int main() {
    auto database = caudio::db::Database::open(":memory:");
    if (!database) {
        std::println(stderr, "database open failed: {}",
                     database.error().message);
        return 1;
    }

    auto engine = caudio::engine::Engine::create();
    if (!engine) {
        std::println(stderr, "engine create failed: {}",
                     engine.error().message);
        return 1;
    }

    return engine.value()->play() ? 0 : 1;
}
```

Runnable examples are in `examples/` — see the repository for the full list.

## Next steps

- [Command-Line Client](/guides/cli) — CLI reference
- [Configuration](/guides/configuration) — runtime configuration
- [API Reference](/reference/api/) — C++ library API documentation
