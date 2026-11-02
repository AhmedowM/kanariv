# kanariv

[![CI](https://github.com/AhmedowM/kanariv/actions/workflows/ci.yml/badge.svg)](https://github.com/AhmedowM/kanariv/actions/workflows/ci.yml)
[![Version](https://img.shields.io/github/v/release/AhmedowM/kanariv)](CHANGELOG.md)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-%3E%3D3.28-red)](CMakeLists.txt)

`kanariv` is a headless, local music player and a reusable C++23 library. It
decodes audio with FFmpeg, stores the library and playback state in SQLite,
and exposes a command-line client backed by a daemon. There is no GUI and no
cloud service.

## Contents

- [Highlights](#highlights)
- [Support status](#support-status)
- [Dependencies](#dependencies)
- [Build from source](#build-from-source)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Use as a library](#use-as-a-library)
- [C++23 modules](#c23-modules)
- [Documentation and packaging](#documentation-and-packaging)
- [Contributing](#contributing)
- [License](#license)

## Highlights

- Gapless playback with sample-accurate seeking.
- FFmpeg-backed decoding and metadata extraction.
- SQLite library management with FTS5 search.
- Queues, playlists, shuffle, repeat, history, bookmarks, and statistics.
- JSON IPC with 4-byte big-endian framing.
- Lock-free shared-memory status updates at 10 Hz for lightweight clients.
- Nine installable libraries, an optional CLI, and opt-in C++23 modules.

## Support status

The versions below are the versions currently tested by the project. Newer
versions may work, but are not part of this compatibility statement.

| Platform | Tested toolchain | Status |
| --- | --- | --- |
| Linux | GCC 13 | Supported for the traditional headers build |
| Linux | LLVM Clang 19 | Supported for the traditional headers and modules builds |
| Linux | GCC 14+ | Supported; GCC 14 is the minimum for the modules build |
| Windows 2022 runner | MSVC 19.44 (Visual Studio 2022 toolset) | Supported |
| macOS 15+ | Xcode 26 / AppleClang | Supported |
| FreeBSD 15.1 | GCC 14 | Supported and tested |
| FreeBSD 15.1 | System Clang | Not fully supported: the system headers lack required C++23 library features |
| FreeBSD 15.1 | LLVM 21 packages | Not fully tested: the installed compiler still resolves against the system C++ headers |

The C++23 modules build requires a compiler and generator with import-graph
scanning. In practice, use GCC 14+, LLVM Clang 19+, or MSVC 2022. GCC 13 is
supported for the canonical headers build, but not for modules.

## Dependencies

### Required build tools

- **CMake 3.28 or newer**. CMake's `FILE_SET CXX_MODULES` support is required.
- **Ninja 1.11 or newer**. Ninja is required for the modules build and is the
  recommended generator for every platform.
- **Python 3.9 or newer** for `scripts/configure.py`.
- **Git 2.30 or newer** when configuring from a repository checkout.

### Compiler and runtime dependencies

- **FFmpeg** with `libavformat`, `libavcodec`, `libavutil`, and
  `libswresample`. A compatible system installation is preferred.
- **TagLib 2.3.2** is fetched during configuration for file-tag writing.
- A C++23 standard library providing `std::expected` and `std::print`.
  The project supplies small compatibility shims where libc++ does not yet
  provide the required C++23 facilities.

FFmpeg can be supplied by the system, by a package manager, or by the
project's fetch logic. Release and CI builds may use the project's trimmed
FFmpeg 8.1.2 packages. To use a known installation, set `FFmpeg_ROOT` or
pass `--ffmpeg-root` to the configure script.

### Fetched and vendored dependencies

The following dependencies are fetched only when their feature is enabled:

| Dependency | Version | Used for |
| --- | --- | --- |
| Catch2 | 3.16.0 | Tests |
| CLI11 | 2.7.2 | CLI |
| TagLib | 2.3.2 | File-tag writing |

The following sources are vendored in [`vendor/`](vendor/):

| Dependency | Version | License |
| --- | --- | --- |
| SQLite amalgamation | 3.53.4 | Public Domain |
| nlohmann/json | 3.12.0 | MIT |
| miniaudio | 0.11.25 | MIT |
| BLAKE3 | 1.8.7 | Apache-2.0 OR CC0-1.0 |

See [`vendor/README.md`](vendor/README.md) for upstream links and provenance.

## Build from source

### 1. Clone

```sh
git clone https://github.com/AhmedowM/kanariv.git
cd kanariv
```

### 2. Choose a configure method

The preferred method is the project wrapper. It selects the repository's
toolchain policy, supports cross-platform compiler selection, and keeps local
commands aligned with CI.

```sh
# Preferred: configure, build, and test a Debug development tree.
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test
```

Useful wrapper options:

```sh
# List the wrapper's available options and intents.
python scripts/configure.py --help
python scripts/configure.py dev --help

# Configure a headless CI-style tree.
python scripts/configure.py ci --toolchain gcc --build-dir build/ci-gcc

# Configure and build the optional C++23 modules.
python scripts/configure.py modules --toolchain clang --build-dir build/modules-clang --build

# Configure a release-oriented tree with a specific FFmpeg installation.
python scripts/configure.py release --toolchain clang \
  --ffmpeg-root /opt/ffmpeg --build-dir build/release-clang
```

The wrapper accepts intents including `dev`, `ci`, `release`, `release-lto`,
`minsize`, `minsize-lto`, and `modules`. Use `--build` to invoke the build and
`--test` to run CTest after configuration.

#### CMake presets

Presets are convenient when the host compiler is already on `PATH`:

```sh
cmake --list-presets
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --test-dir build/dev --output-on-failure --parallel 4
```

Available presets are `dev`, `ci`, `release`, `release-lto`, `minsize`,
`minsize-lto`, and `modules`. Presets use Ninja and the project toolchain
dispatcher; use the wrapper when you need an explicit compiler, architecture,
FFmpeg root, or CI-equivalent invocation.

#### Bare CMake

Bare CMake is useful for packaging, unusual generators, or integrating kanariv
into an existing build:

```sh
cmake -S . -B build/custom -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=23 \
  -DKANARIV_ENABLE_TESTS=ON \
  -DKANARIV_BUILD_SHARED=ON
cmake --build build/custom --parallel 4
ctest --test-dir build/custom --output-on-failure --parallel 4
```

For modules, add `-DKANARIV_ENABLE_MODULES=ON` and use Ninja. For a complete
list of project options, inspect the [configuration reference](#configuration).

### Platform notes

- **Linux:** install FFmpeg development packages from your distribution.
- **macOS:** install Ninja and FFmpeg with Homebrew, then use
  `--toolchain apple` for Xcode AppleClang or `--toolchain clang` for Homebrew
  LLVM.
- **Windows:** use a Ninja shell with Visual Studio environment variables for
  MSVC, or use LLVM Clang targeting the MSVC ABI. MinGW GCC is also supported.
- **FreeBSD:** the tested path is FreeBSD 15.1 with GCC 14 and system FFmpeg.
- **Android:** configure the native shared-library target with
  `python scripts/build-ffmpeg.py --android-ndk <path>` followed by
  `python scripts/configure.py android --ndk <path> --ffmpeg-root <prefix>`.
  The current target does not include Java/Kotlin or application lifecycle
  code. The x86 variant automatically disables FFmpeg assembly because its
  32-bit assembly is not PIC-compatible with Android shared libraries.

Headless test runs can avoid opening an audio device:

```sh
python scripts/configure.py ci --toolchain clang --build-dir build/ci \
  --build --test
# The ci intent enables KANARIV_TEST_NOAUDIO.
```

## Quick start

After building with the `dev` intent:

```sh
./build/dev/kanariv library scan --path ~/Music
./build/dev/kanariv library search "miles davis" --limit 10
./build/dev/kanariv queue add ~/Music/Kind\ of\ Blue/*.flac
./build/dev/kanariv play
./build/dev/kanariv status --watch --interval 1000
```

The daemon can also be managed explicitly:

```sh
./build/dev/kanariv start
./build/dev/kanariv status --json
./build/dev/kanariv pause
./build/dev/kanariv shutdown
```

Or play a path directly without creating a permanent library queue:

```sh
./build/dev/kanariv ~/Music/album/track.flac
./build/dev/kanariv ~/Music/album --recursive --save
```

Run `kanariv --help` or `kanariv <command> --help` for the complete CLI
reference. Most commands communicate with the daemon; configuration commands
work without one, and playback commands can start it automatically.

## Configuration

Important cache options:

| Option | Default | Description |
| --- | --- | --- |
| `KANARIV_BUILD_SHARED` | `ON` | Build per-component shared libraries |
| `KANARIV_BUILD_STATIC` | Depends on shared mode | Build static component libraries |
| `KANARIV_BUILD_CLI` | `ON` | Build the `kanariv` executable |
| `KANARIV_ENABLE_TESTS` | `OFF` | Build Catch2 tests |
| `KANARIV_ENABLE_EXAMPLES` | `OFF` | Build the examples |
| `KANARIV_ENABLE_MODULES` | `OFF` | Build/install C++23 module interfaces |
| `KANARIV_ENABLE_SANITIZERS` | `OFF` | Enable AddressSanitizer and UndefinedBehaviorSanitizer where supported |
| `KANARIV_BUILD_DOCS` | `OFF` | Build public API documentation (HTML and XML) with Doxygen |
| `KANARIV_TEST_NOAUDIO` | `OFF` | Skip tests that require an audio device |
| `KANARIV_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during the build |
| `KANARIV_USE_TRIMMED_FFMPEG` | `OFF` | Use a decode-only trimmed FFmpeg build |
| `FFmpeg_ROOT` | unset | Override the FFmpeg installation |

## Use as a library

The public headers under [`include/kanariv/`](include/kanariv/) are canonical.
Install exports provide these targets:

| Target | Responsibility |
| --- | --- |
| `kanariv::utils` | Errors, logging, JSON, queues, threads, and versioning |
| `kanariv::types` | Shared value types |
| `kanariv::contracts` | Backend-neutral interfaces |
| `kanariv::media` | Reading, decoding, metadata, and library discovery (`Source`, `PathSource`, `DescriptorSource`, `Reader`) |
| `kanariv::player` | Audio output and playback primitives |
| `kanariv::db` | SQLite storage and FTS5 search |
| `kanariv::engine` | Queue, shuffle, repeat, history, and playback state |
| `kanariv::ipc` | Daemon protocol, framing, and configuration |
| `kanariv::client` | Daemon client and output formatting |
| `kanariv::service` | Daemon service (`Service`) and in-process service host (`Host`) |
| `kanariv::config` | Configuration management (`Config`, `RuntimePaths`, canonical path helpers) |

```cmake
find_package(kanariv CONFIG REQUIRED)

add_executable(my_player main.cpp)
target_link_libraries(my_player PRIVATE kanariv::engine kanariv::db)
```

```cpp
#include <kanariv.hpp>
#include <memory>
#include <print>

int main() {
    auto database = kanariv::db::Database::open(":memory:");
    if (!database) {
        std::println(stderr, "database open failed: {}", database.error().message);
        return 1;
    }

    auto engine = kanariv::engine::Engine::create();
    if (!engine) {
        std::println(stderr, "engine create failed: {}", engine.error().message);
        return 1;
    }

    if (auto result = engine.value()->attachDatabase(
            std::shared_ptr<kanariv::db::Database>(std::move(database.value())));
        !result) {
        std::println(stderr, "database attach failed: {}", result.error().message);
        return 1;
    }

    return engine.value()->play() ? 0 : 1;
}
```

### Media sources

`kanariv::media::Source` is an abstract factory for readers over a media source.
Two implementations ship with the library:

- `kanariv::media::PathSource` — backed by an ordinary filesystem path; seekable
  and re-openable. Preserves existing desktop behavior.
- `kanariv::media::DescriptorSource` — backed by a caller-supplied native file
  descriptor (`FileDescriptor`). Single-use: ownership transfers to the reader
  on the first successful `open()`. Descriptors are never persisted; a host
  reopens the source per operation using a stable source identity.

```cpp
#include <kanariv/media/reader.hpp>
#include <kanariv/media/source.hpp>

kanariv::media::PathSource source{"/Music/track.flac"};
auto reader = source.open(); // FileReader
```

### Service host (in-process)

`kanariv::service::Host` runs a `kanariv::service::Service` in-process without
desktop daemon scaffolding (no lock file, PID file, socket, IPC server, or
shared memory). It owns the worker thread and exposes `requestStop()`,
`wait()`, `stop()`, `running()`, `ready()`, `error()`, and `service()`. Use
`ServiceConfig::inProcess` with explicit `kanariv::config::RuntimePaths` for
host-private directories such as Android app-private storage.

```cpp
#include <kanariv/service/host.hpp>

kanariv::service::ServiceConfig cfg;
cfg.runtime.database = "/data/app/kanariv/kanariv.db";
cfg.runtime.logs = "/data/app/kanariv/logs";
auto host = kanariv::service::Host::create(cfg);
// ... use host->service() ...
host->stop();
```

Runnable examples are in [`examples/`](examples/): the original minimal
player, player/database demo, and engine queue/history demo. The
[`examples/downstream/`](examples/downstream/) project is intentionally
separate from the repository build: it uses `find_package(kanariv CONFIG
REQUIRED)` and demonstrates one small consumer per public component, followed
by database/engine composition, media discovery, configuration backup, and
the minimal player. Build it against an installed kanariv tree with:

```sh
cmake -S examples/downstream -B build/examples-downstream \
  -DCMAKE_PREFIX_PATH=/path/to/kanariv/prefix
cmake --build build/examples-downstream --parallel 4
```

The standalone examples are grouped as follows:

- `types.cpp`, `contracts.cpp`, `utils.cpp`, `config.cpp`, `media.cpp`,
  `player.cpp`, `database.cpp`, `engine.cpp`, `ipc.cpp`, `client.cpp`,
  `service.cpp`, and `app.cpp` show the smallest useful include and API for
  each public component.
- `library.cpp` combines database and engine-facing types without requiring
  the daemon.
- `fingerprint.cpp` demonstrates batch media discovery, while
  `config_backup.cpp` demonstrates a small configuration automation utility.
- `mini_player.cpp` is the complete one-file playback example.

Module-only consumers are in [`examples/modules/`](examples/modules/). They
use `import kanariv.*` rather than public headers and require a module-enabled
installation plus a compiler that supports C++23 import-graph scanning.

## C++23 modules

Modules are opt-in and mirror the public library targets. Configure them with
the `modules` intent:

```sh
python scripts/configure.py modules --toolchain clang \
  --build-dir build/modules-clang --build
```

```cpp
import kanariv.media;

int main() {
    auto tracks = kanariv::media::scanTracksToVector(".");
    return tracks ? 0 : 1;
}
```

Available named modules include `kanariv`, `kanariv.utils`, `kanariv.media`,
`kanariv.player`, `kanariv.db`, `kanariv.engine`, `kanariv.ipc`,
`kanariv.service`, and `kanariv.client`, with partitions such as
`kanariv.db:scan`. Consumers rebuild BMIs with their own compiler; prebuilt
BMIs are not portable across compilers or incompatible flags.

## Documentation and packaging

Build API documentation and install the project:

```sh
python scripts/configure.py dev --toolchain clang --docs \
  --build-dir build/docs
cmake --build build/docs --target doc-check --parallel 4

cmake --install build/dev --prefix "$PWD/stage"
cmake --build build/dev --target package
```

The generated API documentation is under the build documentation directory.
The CLI man page is [`docs/man/kanariv.1`](docs/man/kanariv.1). Packages use ZIP
on Windows and TGZ elsewhere.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for coding conventions, test
requirements, documentation, and pull-request guidance.

## License

kanariv is released under the [MIT License](LICENSE).
