#Contributing to kanariv - cpp

## Development Setup

### Prerequisites

| Tool | Minimum Version | Notes |
|---|---|---|
| **Compiler** | GCC 13+ / LLVM Clang 19+ / AppleClang from Xcode 26 | GCC 13 supports the headers build; GCC 14+ is required for modules. C++23 is required. |
| **CMake** | 3.31+ | Required for module dependency scanning and `FILE_SET CXX_MODULES` |
| **Ninja** | 1.11+ | **Required** -- C++23 modules only work reliably with Ninja generator |
| **FFmpeg** | recent (tools + dev packages) | `libavformat`, `libavcodec`, `libavutil`, `libswresample`. CI uses Ubuntu system FFmpeg; auto-fetched if not found when `KANARIV_WITH_FETCH_FFMPEG=ON` |
| **Doxygen** | 1.9+ | Required for `KANARIV_BUILD_DOCS=ON`; `dot` (Graphviz) is optional for graphs |
| **Catch2** | 3.16.0 | System package preferred, else shallow-fetched via `FetchContent` when `KANARIV_ENABLE_TESTS=ON` |
| **Git** | 2.30+ | For `git describe --tags` version stamping |

> **Windows note:** first configures fetch shallow dependency copies per
> preset build dir (gitignored); exclude the repo from Windows Defender
> real-time scanning — it otherwise multiplies git + configure times.
> Prefer a Ninja + `vcvars` shell over the Visual Studio generator for
> iteration speed.

### Clone and Configure

The preferred workflow is the intent-driven configure script. It keeps local
builds aligned with CI and can configure, build, test, package, and select a
toolchain in one command.

```sh
git clone https://github.com/AhmedowM/kanariv.git
cd kanariv

# Recommended: development build with examples, tests, and four jobs
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test

# Optional modules build (GCC 14+, LLVM Clang 19+, or MSVC 2022)
python scripts/configure.py modules --toolchain clang --build

# Build the Android FFmpeg dependency, then the native kanariv library
python scripts/build-ffmpeg.py --android-ndk D:\android-ndk-r30 `
  --android-abi arm64-v8a --android-api 24 --prefix build\ffmpeg-android-arm64
python scripts/configure.py android --ndk D:\android-ndk-r30 `
  --ffmpeg-root build\ffmpeg-android-arm64 --build
```

Inspect all wrapper options with:

```sh
python scripts/configure.py --help
python scripts/configure.py dev --help
```

Android builds require an NDK-built FFmpeg prefix supplied with
`--ffmpeg-root` (or `FFmpeg_ROOT`). The Android CMake intent produces the
native component libraries; the Java app and JNI bridge under `android/` are
built separately with Gradle (see `android/README.md`). The Android x86 FFmpeg
variant disables assembly because FFmpeg's 32-bit x86 assembly is not
position-independent when linked into Android shared libraries.

Use CMake presets when the compiler is already configured in the environment:

```sh
cmake --list-presets
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --test-dir build/dev --output-on-failure --parallel 4
```

Bare CMake remains supported for packaging and custom integrations:

```sh
cmake -S . -B build/custom -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DKANARIV_ENABLE_TESTS=ON
cmake --build build/custom --parallel 4
ctest --test-dir build/custom --output-on-failure --parallel 4
```

For a complete option list, see the configuration reference in
[`README.md`](README.md). Do not add a new module-only API: public headers
remain canonical and module partitions mirror them.

### Optional configure features

```sh
# Build documentation
python scripts/configure.py dev --toolchain clang --docs --build

# Enable sanitizers on supported Unix toolchains
python scripts/configure.py dev --toolchain clang --sanitizers --build --test

# CI-style, headless tests
python scripts/configure.py ci --toolchain clang --jobs 4 --build --test
```

### Build and Test

```sh
cmake --build build -j4
ctest --test-dir build -j4 --output-on-failure

#Run a single test binary
./build/dev-clang-ninja-x64/test_engine_queue --success

#Manual CLI smoke test
./build/kanariv --version
./build/kanariv start
./build/kanariv status
./build/kanariv shutdown
```

> **Audio tests beep:** `AudioOutput::create` (`ma_device_start`) beeps on desktop.
> For silent/headless runs: `KANARIV_TEST_NOAUDIO=1 ctest --test-dir build --output-on-failure -j4` -- tests that would beep
> check `kanariv::test::noAudio()` / `KANARIV_SKIP_IF_NOAUDIO()` in `tests/common.hpp`
> and `SKIP` when set. CI sets `KANARIV_TEST_NOAUDIO=1` automatically. For silent
> ring exercises without a device use `kanariv::test::makeDummyRing()` (no `ma_device_init`).

### Documentation

```sh
cmake --preset docs
cmake --build --preset docs --parallel 4
# or: python scripts/configure.py docs --build
# HTML: build/docs/docs/html/index.html
# XML:  build/docs/docs/xml/index.xml
# Open: build/docs/docs/html/index.html
```

Man page is at `docs/man/kanariv.1` -- preview with `man ./docs/man/kanariv.1` or `groff -man -Tascii docs/man/kanariv.1 | less`.

The documentation site in `docs-site/` must be updated whenever a change
affects user-visible behavior or the public development workflow, even when no
public C++ API changes are involved. This includes new or changed configuration
paths and defaults, CLI or TUI commands, service behavior, IPC behavior,
installation or build instructions, platform-specific behavior, and other
features that users need to discover. Update the relevant guide, reference
page, search metadata, or release note alongside the code change, and run the
site build before opening a pull request:

```sh
cd docs-site
npm ci
npm run build
```

The site build also regenerates the custom Doxygen API model and search index.

### Code Style

- **Formatter:** `clang-format` using `.clang-format` at repo root. Run before committing:
  ```sh
  clang-format -i include/**/*.hpp src/**/*.cpp cli/src/**/*.cpp tests/*.cpp examples/*.cpp
  ```
- **Linter:** `clang-tidy` using `.clang-tidy` (optional, not enforced in CI yet).
- **Naming:** `PascalCase` for types, `camelCase` for functions/variables, `snake_case` for files and locals. Match surrounding code; do not add `k`-prefix constants.
- **Headers/modules:** public API lives in `include/kanariv/` (`*.hpp`), and
  sources in `src/` (`*.cpp`). C++23 modules (`*.cppm`) mirror the headers;
  do not add new module-only APIs.
- **Errors:** return `std::expected<T, utils::Error>` and use the existing
  `StatusCode` values. Do not throw across module boundaries.

## Commit Style

This project uses [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <description>

[optional body]

[optional footer(s)]
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `build`, `ci`

Scopes: `cli`, `engine`, `db`, `player`, `utils`, `service`, `ipc`, `cmake`, `vendor`, `docs`

Examples:

```
feat(cli): add library list + enhanced stats + info subcommand
fix(engine): align decodeLoop and preroll sample vs frame with player
docs: comprehensive Doxygen documentation for all public APIs
refactor(cmake): SANITIZERS target-scoped, sqlite dedup
```

- Keep subject line <= 72 chars.
- Use `BREAKING CHANGE:` footer for API-breaking changes.

## Pull Request Process

1. **Branch** from `main` (or current release branch):
   ```sh
   git checkout -b feat/my-feature
   ```
2. **Implement** with tests. No logic changes to `src/` without corresponding test updates. New IPC commands need a `Command` variant in `include/kanariv/ipc/command.hpp`, a `Service::handle()` overload in `src/service/dispatch_*.cpp`, plus `tests/test_ipc.cpp` coverage.
3. **Configure and test** locally:
   ```sh
   cmake -B build -G Ninja -DKANARIV_ENABLE_TESTS=ON
   cmake --build build -j4
   ctest --test-dir build -j4
   cmake -B build -G Ninja -DKANARIV_BUILD_DOCS=ON && cmake --build build --target doc-check
   ```
4. **Format** changed files with `clang-format`.
5. **Push** and open a PR against `main`. Include:
   - Summary of changes (Added/Changed/Fixed).
   - Any `BREAKING CHANGE` notes.
   - Screenshots or `kanariv --help` output for CLI changes.
6. **CI** must pass (build + tests on Linux/Windows/macOS if configured).
7. **Review** -- address feedback; squash or rebase as requested.

## Project Structure

```
kanariv/
  include/kanariv/        # Public API (headers are canonical)
    kanariv.hpp alongside umbrellas: utils.hpp, player.hpp, db.hpp,
    engine.hpp, version.hpp, ipc.hpp, client.hpp, service.hpp, config.hpp
    db/ engine/ player/ ipc/ client/ service/  # per-area headers
    utils/               # error/result/log/ring/queue/thread/math
  src/<area>/            # Private sources (*.cpp) + private headers
    utils/ player/ db/ engine/ ipc/ service/ client/
    service/dispatch_*.cpp  # Service::handle() per command group
    service/service_paths.* service/service_status.* service/service_audio.*
                         # (*.cppm mirrors sit beside sources; KANARIV_ENABLE_MODULES=ON)
  cli/src/shell/        # Executable only: main, Shell dispatch, parse helpers
  cmake/components/      # Per-component build files (utils/player/db/engine/cli/combined)
  tests/                 # Catch2 tests (tests/common.hpp helpers)
  examples/              # mini_cpp, player_db_demo, engine_demo
  vendor/                # sqlite3.c, blake3.c, miniaudio.h (sources; only static archives install, headers don't)
```

## Reporting Issues

Open an issue with:

- `kanariv --version` and `cmake --version` / compiler version
- Config: `kanariv config list --json` if relevant
- Repro steps and expected vs actual behavior
- Logs: set `--log-level debug` or check daemon output

## License

By contributing, you agree that your contributions will be licensed under the same license as the project (MIT).
