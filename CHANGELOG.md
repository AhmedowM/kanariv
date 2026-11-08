#Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- **BREAKING** Project renamed from `caudio` to `kanariv`. The C++ namespace
  is now `kanariv::`, public headers live under `include/kanariv/`, the CMake
  package is `find_package(kanariv CONFIG REQUIRED)` with `kanariv::<component>`
  targets, the CLI binary is `kanariv` (`kanariv.exe`), build options use the
  `KANARIV_*` prefix, and runtime paths moved to `~/.config/kanariv` and
  `~/.local/share/kanariv` (`kanariv.db`, `kanariv.log`).

### Added
- OS media-session integration: Windows System Media Transport Controls
  (SMTC), Linux MPRIS v2.2 over D-Bus, and macOS Now Playing / remote
  commands. Publishes metadata, artwork, playback state, volume,
  repeat/shuffle and position, and handles play/pause/stop/next/previous/seek
  from the system UI (`f46fa28`, `47a3fe1`, `7b73788`)
- `library stats --top-tracks/--top-albums/--top-genres/--top-artists`
  listening reports with multi-artist splitting (`0192df0`)
- `library list --sort <field> --order <asc|desc>` sorting on library
  listing (`5f529de`)
- `tag get cover` / `tag get cover --out FILE` cover-art output, including
  embedded cover art extraction (ID3v2 APIC, FLAC picture blocks)
  (`3152f88`)
- `Client::waitForStatus()` + `StatusWait` IPC command; `status --watch`
  uses the status-stream API instead of polling (`4ecc9af`)
- `--columns` allowlist on `library list`; `device test --tone` plays a
  440 Hz sine for 1s (`fed372f`)
- Playback-policy config keys: `clearStopsPlayback`, `repeatDefault`,
  `autostart` (`3d20bb0`)
- Track rating (`track rate`), smart-playlist creation (`playlist create
  --smart`), and `library maintenance report|prune` (`08ee5e7`,
  `0bee82f`, `92e1c06`)
- `ParseSmartQuery` smart-playlist query parser and evaluation
  (`41aa8eb`)
- `TrackQuery` filters: rating, year_min/max, play_count_min/max,
  duration_min/max (`9a861c3`)
- `Service::Impl` Host wrapper and `RuntimePaths` for in-process daemon
  embedding (`659e9e2`)
- `SourceRef` track-source identity and `Database::findBySource()`
  (`567c36c`)
- `PathSource` and `DescriptorSource` read transports for media scanning
  (`264a4dd`)
- Android NDK builds and CI matrix (`a77cb8a`)
- CMake `source`/`runtime`/`host` component targets plus Phase A tests
  (`3b252a2`)
- `caudio bookmark add|list|remove|goto` for per-track position bookmarks,
  with optional notes and `--json` output (`52f3dbe`)
- Cross-session resume: the engine persists its resume position and the new
  `autoResumeOnPlay` config key restores it on the next play (`7891ef8`,
  `ae70457`)
- `caudio::config` component for the shared config model and path helpers;
  `app`, `client`, `service` and the CLI link it directly (`030901c`)
- `ipc::ShmStatusReader` and `ipc::ShmStatusWriter`, splitting the shared
  status block into read and write surfaces (`92e6b4f`)
- `types/format.hpp` and `types/parse.hpp` for `PlaybackState`/`RepeatMode`
  formatting and parsing (`693f4a9`)
- `Database::libraryStats()` with optional most-played tracks, and
  `types::LibraryStats` as the single stats shape (`38e68e3`)
- `App::status`, `App::info`, `App::setVolume`, and `Client::probe` (`f76f6cf`)
- `scripts/configure.py` intent resolver (`dev`, `ci`, `release`,
  `release-lto`, `minsize`, `minsize-lto`, `modules`, `default`) with
  `--toolchain auto|gcc|clang|msvc|apple`, `--arch`, `--generator` and
  `--build`/`--test`/`--install`/`--package` actions; CI drives builds
  through it (`89413d6`)
- CMake build presets mirroring every intent (`4f8d23e`)
- `CAUDIO_API` export macro on installed public classes and free functions;
  component DLLs export exactly that surface (`ac04be4`)
- Vendor runtime DLLs (`sqlite3`, `blake3`, `tag`) ship beside `caudio.exe`
  so a process uses one copy of each (`5b60c83`)
- `caudio::contracts::IDatabase` implemented by `Database`; `Service::create()`
  and `Service::attachDatabase()` take a `std::shared_ptr<IDatabase>`
  (`4c8ecd3`, `723e10f`, `cee5940`, `19ff24d`)
- `shared-abi` workflow covers the ELF/Mach-O halves of the shared build:
  per-component export surface, installed tree, downstream
  `find_package(caudio)` consumer and package contents (`c4ac853`)

### Changed
- `tag get`/`tag edit` and `track rate` accept a track id or file path
  (non-numeric args resolve via `TagGet{path}` → `db_->findByPath()`);
  `TagGet.id` is now optional with a new `path` field; static bash/zsh/fish
  completion scripts added (`1d9a179`)
- Finer IPC error exit codes: InvalidArg → 106, NotFound → 109, State →
  105, default → 1; `queue add --search` with no matches exits non-zero
  instead of 0; man page EXIT STATUS updated (`1d9a179`)
- Centralized shared `Engine`/`Database` types into the `types` component
  with improved documentation (`1c12dae`, `a7149b5`)
- Enhanced public API documentation and method descriptions
  (`4a4d83c`, `1448fce`)
- Reorganized public engine and database methods (`be4a8f1`)
- CMake `types` target made INTERFACE; added contracts module files
  (`97d0cbe`)
- Fixed MSVC LTCG + sqlite3 export; skip brew LLVM on macOS Intel
  (`e5a113f`)
- Set `$ORIGIN` RUNPATH on bundled Linux FFmpeg libs; install
  `CXX_MODULES` file set in dev (`a4128c5`)
- Public headers grouped by domain (`types/`, `media/`, `ipc/command/`,
  `ipc/result/`, `engine/`, `db/`) and CMake targets split per component;
  `ipc` now depends only on `config` and `utils` (`071074a`, `135c327`,
  `030901c`)
- Canonical spellings: `App::play`, `App::queueList`, `App::queueSwitch`,
  `Engine::stats`, and `Database::listQueueItems`/`listHistory`/
  `listBookmarks`/`listLibraries`; option structs renamed to
  `DatabaseOptions`, `PlayerOptions` and `tagFields` (`63bfc03`, `d9f11cb`,
  `693f4a9`, `5cbc4ec`)
- `libraryStats` is one IPC `LibraryStats` command with optional `top_n`;
  totals are always computed, `most_played` is optional, and the detailed
  JSON `"type"` is now `"LibraryStats"` (`38e68e3`)
- `App` owns status/info/volume and preview/daemon-host seams; the `app`
  target no longer links `player`, `db` or `service`, and `OutputFormatter`
  moved to the CLI shell (`f76f6cf`, `76ac406`, `be4fab5`, `636283e`)
- Raw locking/handle access on `Database`/`Engine` is private behind
  `db::DatabaseAccess` / `engine::detail::ServiceAccess`; media scanning is
  split at the `media`/`db` seam (`7348165`, `6eedbf8`)
- `IpcServer`, the file log sink, write thread, MPSC queue, move-only
  function and engine runtime state are internal (`cda41ff`, `0902275`)
- Shared builds are the default: per-component DLLs without a `_shared`
  suffix plus the vendor DLLs, with static archives opting in through
  `CAUDIO_BUILD_STATIC`/`configure.py --static`; tests always link the static
  flavour (`ac04be4`, `b7bc553`, `5b60c83`)
- Component DLLs link each other's object files directly instead of
  whole-archive static copies, so transitive duplicates and
  `--allow-multiple-definition` are gone (`ac04be4`)
- Install splits runtime (DLLs) from dev (import/static libs), reads
  component targets from a directory property instead of a hand-maintained
  list, and always installs `combined` (`a879a9b`, `412c4ec`, `5b60c83`)
- Root `CMakeLists.txt` is split into `cmake/Install.cmake`,
  `BundleFFmpeg.cmake`, `Packaging.cmake`, `Tests.cmake` and
  `Version.cmake` (`c9cb685`, `a98de0c`, `853eecd`)
- Component dependency edges are `PRIVATE` (`player`→`media`,
  `engine`→`db`, `service`→engine/db/media/player, `app`→client/ipc/media),
  so consumers no longer inherit them (`a99cf27`, `627673c`, `7e80137`,
  `0dcee28`, `af52655`)
- `Database`/`App` private members and helpers moved into `Impl`, internal
  headers inlined or moved into `.cpp`, precompiled headers removed and the
  header self-containment gate made opt-in (`b294445`, `f028047`, `c06e1ec`,
  `2548fb6`, `44a1ae0`)
- `App::setDaemonHost`/`setPreviewHost` renamed `attachDaemonHost`/
  `attachPreviewHost`; `fmtClock` is now `caudio::utils::formatClock`;
  k-prefixed constants renamed to camelCase (`19ff24d`, `c06e1ec`,
  `ac3ecd6`)
- Trimmed FFmpeg is consumed as a prebuilt prefix (`FFmpeg_ROOT`/
  `CAUDIO_TRIMMED_ROOT`); `scripts/build-ffmpeg.py` (renamed from
  `build-trimmed.py`) defaults to `build/ffmpeg`, and the trim flags live in
  `cmake/ffmpeg_trimmed_flags.txt` (`849aeac`, `3b37a93`, `6cb8fa1`)
- Warnings are errors (`-Werror`/`/WX`) for first-party code, while
  third-party targets build quietly (`21514ee`, `4f8d23e`)
- `Track`, the wire enums and engine events are packed smaller (8-bit enums,
  reordered `Track`), and `SpscRing` masks instead of dividing for
  power-of-two capacities (`2024f09`)

### Removed
- Deprecated spellings deleted ahead of v2.0.0:
  `include/caudio/legacy_aliases.hpp`, the `caudio/<comp>/core.hpp`
  wrappers, old `App`/`Database`/`Engine`/`Player` member spellings,
  `DbOpts`/`PlayerOpts`/`kTagFields`, the moved record aliases
  (`DeviceInfo`, `DeviceList`, `TrackMetadata`, `LibraryStatsData`,
  `LibraryStatsDetailedData`, `ScanMode` and the scan forwarders),
  `LibraryStatsDetailed`, `ShmStatusHandle`/`createShmStatus`, and the
  `ipc::{BookmarkEntry,DeviceInfo,HistoryEntry}` result aliases
  (`b1d0379`, `53bcce8`, `e52c5aa`)
- `Service::shmHandle()` (`e52c5aa`)
- Dead public API: `Client::snapshotStatus`, `AudioOutput::isPlaying`,
  `Database::queuePeek*`, and the `IpcChannel`/`frameMessage`/
  `deframeMessage` framing layer; `fillFromRing` moved to an internal
  helper (`d8e4b48`)

### Fixed
- Playback history is now accounted per track: `next`/`prev` reset the
  accumulated-playback window, so a following track is no longer marked
  played on entry; the 60%-or-90s threshold applies to each track.
- `library stats --top-albums/--top-genres/--top-artists` now select the
  detailed text renderer; previously only `--top-tracks` did and the other
  reports printed the one-line summary.
- Windows path handling is UTF-8 end to end: scanning, storing, probing and
  playing files whose names contain characters outside the ANSI code page
  (e.g. `İ`) no longer aborts the scan with `Io ReadFile hdr failed: 109`
  (ERROR_BROKEN_PIPE). A failing file is named in the daemon log, handler
  exceptions become error replies instead of a silent connection close, and
  `library scan` allows up to 30 minutes for large directories.
- Android NDK: removed local-cache from setup-ndk to resolve missing NDK
  tools (`7d49970`)
- CI Windows: dropped MinGW from ci.yml, moved to compilers.yml/modules.yml
  (`066d9ea`)
- MSVC: avoid shadowed source variable in db JSON import (`bb27b78`)
- History: count accumulated playback time correctly (`ef4e51d`)
- CI: support MinGW and FreeBSD toolchains (`a86e451`, `deaae99`)
- Modules: support GCC module toolchain versions (`21ab46b`)
- CMake: build module contracts and types as static targets (`e51f820`)
- CI: select LLVM 21 in FreeBSD builds; add FreeBSD build verification
  (`8827c1c`, `d8ee278`)
- Resumed playback starts at the persisted position: the engine seeks the
  decoder before the output ring is prerolled, instead of playing ~100 ms of
  the track head until the client-side seek arrives (`8a3a2fa`)
- Daemon teardown of a playing track (`next`/`prev`/`play` of another
  track) no longer races the decode thread, which crashed the daemon under
  loop load (`021222a`)
- The IPC accept loop recovers from a broken pipe instead of terminating
  (`71c704d`)
- Windows config-directory resolution now matches the other platforms
  (`120a928`)
- Shared-memory status segments use POSIX-compliant names and `shm_open`
  flags, so the reader/writer round-trip works on macOS
- A queue that ends naturally with repeat off now clears its persisted
  resume position, so the next `play` starts the queue instead of seeking to
  the end and looping play/stop (`19ff24d`)
- macOS CI builds select AppleClang through `apple.cmake` and honor
  `CAUDIO_ALLOW_UNSUPPORTED_COMPILER` (`959caab`, `c065b16`)
- Unix shared installs put the per-component `.so`/`.dylib` and vendor libs
  in the runtime archive under `lib/` and give every installed library a
  relative RPATH, so `bin/caudio` runs from an unpacked runtime archive
  without `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH`; macOS additionally sets
  TagLib's install name to `@rpath` and rewrites FFmpeg references inside
  component dylibs (`44f5186`)
- macOS FFmpeg bundle re-points SONAME symlinks (e.g. `libavcodec.63.dylib`)
  at their versioned real file with a relative target, so `@rpath/<soname>`
  resolves in an unpacked runtime archive instead of dangling on brew's
  absolute-target symlink
- On Unix shared builds the `.so`/`.dylib` (no separate import lib exists) is
  installed into both the runtime and dev archives: the runtime archive stays
  self-contained to run, and the dev archive carries the libraries a
  `find_package(caudio)` consumer needs to link, mirroring the Windows
  `.dll`→runtime / `.lib`→dev split
- OS media-session updates no longer go stale after ~30 s of playback: the
  engine's bounded diagnostic event queue is never drained by the daemon, so
  it filled with `Progress` samples and `pushEvent()` then skipped callback
  dispatch entirely. Natural track ends and auto-advance never reached
  SMTC/MPRIS, leaving the previous track with a pause button. Callbacks are
  now dispatched regardless of queue state, `Progress` samples only fill half
  of the queue, and stopping at the end of a queue keeps the last now-playing
  metadata instead of blanking it

## [v1.8.0] - 2026-10-19

### Added
- Header self-containment target: every public header must compile
  standalone
- `-Werror=deprecated-declarations` for in-tree GCC/Clang builds plus a
  CI guard (`scripts/check_legacy_aliases.py`), so new code cannot adopt
  the deprecated spellings

### Changed
- Track <-> JSON codec now lives in `caudio::utils`
  (`caudio/utils/record_json.hpp` and `record_json.cpp`); `caudio::ipc`
  no longer links `caudio::db`, so the client side stops pulling sqlite
  just to serialize protocol payloads
- Shared records and wire enums are spelled `caudio::types::` across the
  tree and in the module mirrors

### Deprecated
- `caudio::db::`/`caudio::engine::` spellings of the moved types (via
  `legacy_aliases.hpp`) and `caudio::db::trackToJson`/`trackFromJson`
  emit deprecation warnings; both are removed in v2.0.0

## [v1.7.0] - 2026-10-18

### Added
- `caudio version [--json]`: prints the version plus build provenance
  (compiler, C++ standard, runtime, build mode, OS, arch) with no daemon
  and no config read; `--version` still prints only the version string
- `caudio.types` layer (headers + C++23 module): dependency-free shared
  value types -- the wire enums `RepeatMode`/`PlaybackState` and the
  records `Track`/`Playlist`/`QueueItem`/`HistoryEntry`/`Bookmark` -- now
  owned by `caudio::types` and re-exported from `caudio::engine` and
  `caudio::db` as source-compatible aliases
- Opt-in `CAUDIO_ENABLE_CCACHE` (ccache/sccache launcher); per-component
  precompiled headers and MSVC `/MP` parallel compilation

### Changed
- Public headers no longer expose internals: `Engine`, `Service`,
  `IpcServer`, `IpcClient`, `Database`, `WriterThread`, `FileLogSink` and
  the player classes are pimpl'd and pull in forward declarations only;
  platform headers dropped from the public interface. Downstream source
  stays compatible and must recompile (class layouts changed)
- Dependency edges tightened: `shm_status` moved from `service` to `ipc`,
  `ipc` no longer depends on `engine`, `client` no longer depends on
  `service`; FFmpeg now links only into `player`

### Removed
- Stale CI workflows `llvm-linux.yml` and `apple-clang.yml` (covered by
  the release and bake-off pipelines); `docs/known-issues.md` (GitHub
  issues is the record); `docs/polyglot-integration.md` (untracked `.dev/`)

## [v1.6.0] - 2026-10-17

### Added
- Release archives for every GitHub x64+arm64 runner: Linux (x86_64,
  arm64), macOS (arm64, x86_64), Windows (x86_64, arm64)
- `scripts/build-ffmpeg.py` and a deps workflow build the trimmed
  decode-only FFmpeg on all six OS/arch legs, published to the
  deps-private release used at build time

### Changed
- Shipping matrix: MinSizeRel for both the runtime and the dev archive
  from a single build; AppleClang on macOS (no brew LLVM), LLVM Clang on
  Linux and Windows
- `CAUDIO_BUNDLE_FFMPEG_MACOS` defaults ON: macOS archives bundle the
  trimmed dylibs with `@rpath` fixups and run standalone
- Linux archives bundle versioned FFmpeg `.so` files with SONAME links
- Removed the `release-lto-trimmed-*` presets; the `CAUDIO_TRIMMED_ROOT`
  cascade selects the trimmed provider with a system fallback

## [v1.5.0] - 2026-10-14

### Fixed
- Playback heap: the decoder converts through a fixed window so
  libswresample never buffers input internally — a playing track held
  ~47 MB in one resampler block and RSS sat at 64-71 MB; residue is now
  bounded decoder-side (~1 frame, cleared on seek/close) and playback RSS
  stays flat
- Probe sizes capped at scan and decode open (1 MB / 0.5 s) instead of
  FFmpeg defaults
- `stop` releases decoder/output/ring: a stopped daemon no longer retains
  tens of MB of playback buffers (replay re-opens transparently)

## [v1.4.1] - 2026-10-13

### Fixed
- Log thread tag trimmed to 6 hex digits (was full 64-bit hash)

### Added
- Live log-level control: `config set logLevel` accepts names or 0-4,
  rejects garbage without storing, canonicalizes to int, and applies to
  the running daemon immediately; hand-written names tolerated on load
  and import

## [v1.4.0] - 2026-10-13

### Added
- Daemon log file: `<db-dir>/caudio.log` with timestamp/level/thread
  formatting, 1 MB × 3-generation rotation, startup/shutdown lifecycle
  lines; `--foreground` (`start --foreground`) also echoes to stderr via
  the new `ServiceConfig::logToStderr`
- Logger threading: `DbOpts`/`EngineConfig`/`PlayerOpts`/`AudioOutput::Config`
  and `Decoder::open` take a non-owning `Logger*` (null = silent); the
  daemon passes its file logger down so db open, engine create/open,
  decoder open and audio output open emit; new `Logger::enabled()` fast
  check for skipping expensive debug arguments
- Daemon message catalog: unplayable-track and device failures warn with
  file + error, track starts log at debug, queue-lock contention logs the
  operation at debug (monitor autoNext stays silent), every dispatched
  command logs its JSON at debug, scans report start/finish counts with
  per-file metadata failures at debug, tag edits log at info, SHM loss
  warns instead of failing silently

### Fixed
- Log levels unified: `LogLevel` gains `Trace` so the enum matches the CLI
  `--log-level trace|debug|info|warn|error` names 1:1 (trace=0 … error=4);
  the shell parses via the shared `parseLogLevel` and the daemon clamps to
  the full range — `--log-level info` no longer lands on Warn

## [v1.3.0] - 2026-10-12

### Added
- File tag sync: `tag edit` retags audio in place via TagLib 2.3.2
  (fetched lazily like CLI11/Catch2, linked privately into `service`) and
  re-registers the row under a stable id with fresh fingerprint; the loaded
  track suspends around the write and resumes from its position with paused
  state and history mark preserved; 15-format writable set, the rest rejected
  with a clear error
- `Engine::suspendCurrentTrack` / `resumeAfterTagWrite` (atomic close/reopen
  around out-of-band file modification; no-op if the user moved on)
- `test_tag_write` round-trip suite (9 formats), file-sync CLI golden,
  aiff/tta/wv fixtures, `tta` decode guard

### Changed
- Scanner extension set widened to the writable formats
  (.mp4/.asf/.aiff/.aif/.ape/.mpc/.wv/.oga)
- `extractMetadata` falls back to audio-stream metadata and format key
  aliases (`albumartist`, `discnumber`, `WM/Year`, …)
- Man/README `tag edit` notes describe file-first behavior

### Fixed
- Ogg/Opus tags were invisible to scan (stream-level dicts never read)
- TagLib fetch/build warnings silenced (`BUILD_TESTING` off, CppUnit
  disabled, `-w` on the vendored-style target)

## [v1.2.0] - 2026-10-12

### Added
- `preview --keep-playing`: opt out of pausing a playing daemon while
  auditioning a file

### Changed
- `preview` now transient-ducks the daemon (pause while auditioning, resume
  afterwards if still paused; silent and best-effort, daemon-less runs
  unchanged)
- `OutputFormatter::print` dispatches via `std::visit` overloads (one case
  per function; shared track-detail block); exhaustive visit fails at
  compile time on new `Result` alternatives instead of printing "Unknown"

## [v1.1.0] - 2026-10-11

### Added
- Decode-whitelist fixtures (`sample.mp3/.flac/.m4a/.opus/.wma`) with
  decode guard tests, plus `scripts/generate_fixtures.py` to regenerate them
- `cmake/FFmpegTrimmed.cmake`: decode-only FFmpeg whitelist (pinned 8.1.2,
  shared libs), consumed via `CAUDIO_USE_TRIMMED_FFMPEG` / `FFmpeg_ROOT`
  with fallback to the default provider; `ci-trimmed-{gcc,clang,msvc}` and
  `release-lto-trimmed-{gcc,clang}` presets
- Per-OS trim CI jobs (MinGW, MSVC, Linux, macOS) fed by
  `build-ffmpeg-deps.yml` release assets

### Changed
- Release archives ship trimmed FFmpeg (linked set ~116 MB → ~4 MB)
- Vendored SQLite/miniaudio trimmed (`SQLITE_OMIT_*`, `MA_NO_*`); scanner
  recognizes `.opus`/`.wma`

## [v1.0.0] - 2026-10-10

### Fixed
- Transient `engine busy` on rapid `next`/`prev`: the CLI retries once, and
  transport commands wait up to 500 ms on the queue lock before giving up
- Slow start after a killed daemon: a stale pid file is liveness-checked and
  skipped instead of death-watched (clean start either way)

### Changed
- Shared libraries stay `OFF` and experimental for 1.0 (no shipped artifact
  uses them; the `SH-1…SH-5` fix batch is deferred post-1.0)
- SHM status documented as event-driven (published on dispatch, no 10 Hz
  writer); `OutputFormatter` stays in the SDK

## [v1.0.0-rc4] - 2026-10-10

### Changed
- Release archives split into runtime and `-dev` SDK twins (arch in
  filenames, one format per OS, stripped binaries, `SHA256SUMS`);
  module sources ship in `dev`
- Daemon hardening: self-retiring connection threads, atomic
  listen-handle takeover, owner-only endpoint, client pool detach
- CLI polish: exit-code contract locked at 0/1, strict seek ranges,
  man/README synced with `--help`

### Added
- `scripts/soak_daemon.py`: foreground-supervised daemon loop soak gate

### Fixed
- History timestamps used the monotonic clock (every date 1970-01-01);
  wall-clock `wallMs()` now backs `started_at`/`completed_at`/`last_played`

## [v0.46.1] - 2026-10-10

### Changed
- Release archives split into runtime (`caudio-<ver>-<os>-<arch>`) and
  `-dev` SDK twins, one format per OS (ZIP on Windows, TGZ elsewhere);
  release binaries are stripped and filenames carry the architecture
- Release page ships archives + `SHA256SUMS` only (`VERSION` and
  `CHANGELOG.md` attachments dropped as redundant)

### Added
- `scripts/soak_daemon.py`: foreground-supervised daemon loop soak gate
  (300-iteration audio deck plus a degraded no-audio deck)
- Opt-in `CAUDIO_BUNDLE_FFMPEG_MACOS` for standalone mac archives (brew
  dylib bundling with @rpath fixup; off by default, pending validation
  on a Mac)

## [v0.46.0] - 2026-10-09

### Changed
- Mirror `include/` in the module layout (`modules/caudio/<comp>.cppm`
  with `<comp>/*.cppm` partitions); standalone `caudio.config`, new
  `caudio.app` (`:core`, `:format`, `:paths`); umbrellas slimmed to pure
  re-exports
- Phase 1 of the app split: handlers return `Outcome`/`BatchResult` data
  and the shell renders (`render`/`renderBatch`/`renderError`); `detail.*`
  is lib-internal string builders now, shell owns color detection

### Fixed
- Resume/restart/next/prev/create lines reading moved-from results
  (reads hoisted above the `Outcome` construction; `cli playback
  one-liners` golden is green again)

## [v0.45.0] - 2026-10-08

### Changed
- Flatten module umbrellas to `modules/` root and shorten partition file
  names (`<comp>/core.cppm`, `<comp>/types.cppm`); the `caudio.player:core`
  partition drops its legacy `player_core` name
- Move version helpers from `caudio::utils` to their own `caudio::version`
  namespace (standalone `version.hpp`, like `config.hpp`)
- Hide generated version constants in `caudio::detail`; public API uses
  `caudio::version::version()` etc.; shell and service status updated
- Apply include-what-you-use to sources and headers (direct includes;
  pure re-export umbrellas untouched)
- Third-party include dirs (`vendor/`, fetched CLI11) are now `-isystem`;
  own dirs stay plain `-I`
- Release workflow rework: `release.yml` matrix, CI dispatch, Windows
  Clang preference, README sync

### Added
- `scripts/format_all.py` and `scripts/tidy_all.py` (format check/run and
  clang-tidy runner with header analysis mode)

## [v0.44.0] - 2026-10-08

### Changed
- Daemon performance: fewer allocations, wakeups and status overhead
- Harden daemon lifecycle and minsize presets
- Extract CLI behavior into the `caudio::app` library (`App` orchestrator
  with transport, playback, queue, playlist, library, tags, system, config
  and preview policies; thin CLI shell)
- Shorten component header names (`<comp>/core.hpp`, `<comp>/types.hpp`);
  public API unchanged, only include paths moved
- Shorten component source names (`<comp>/core.cpp`); no functional change

## [v1.0.0-rc3] - 2026-10-04

### Added
- All v0.43.x CLI behavior program changes promoted to the 1.0.0-rc3
  release candidate: forgiving transport, volume mute-restore, natural-end
  stop, queue/list rework, queue add/remove/create/delete/load redesign,
  playlist create/add/load/import, library scan/search/stats, tag field
  selector, config import validation, play autostart, local config get,
  direct file play with temp queues.
- Direct play: bare `caudio PATH...` plays files immediately in a new
  queue (temporary, purged on daemon shutdown; `--save` keeps it).
  `play` and direct play autostart the daemon when down (quiet under
  `--json`). `config get` answers from local file without daemon.
- New commands: `queue tracks [--order added|playback]`, `queue create`,
  `queue delete`, `playlist create`, `playlist add [--id/--path]`,
  `playlist load --replace`, `tag get [FIELD]`, `playlist tracks --json`,
  `config get --json`, `queue add --playlist [--replace]`.
- New flags: `library stats --most-played/--queue/--playlist`,
  `library scan --full-hash`, `queue add --id/--playlist/--recursive`,
  `queue remove --id/--pos`, `tag get [FIELD]`, `config get --json`,
  `playlist add --id/--path`, `playlist load --replace`,
  `queue add --id/--playlist/--recursive/--replace`.

### Changed
- **Behavior**: Play after stop replays the stopped track; pause/resume/
  prev warn instead of failing when idle/at start; resume-when-stopped
  plays from the cursor; natural track end stops on repeat Off (manual
  `next` still wraps); volume mute/zero remembers the level for unmute
  (persisted), `Volume: muted` shown only when muted; bare `queue
  repeat` cycles off/all/one; `seek`/`config set` print nothing on
  success; duplicate adds warn without duplicating (server-side) and
  exit success; `playlist load` targets a new queue; `playlist save`
  tolerates legacy duplicate queue rows.
- **Output**: `queue list` lists queues (with active/temp markers),
  `queue queues` hidden alias; `queue tracks`, `playlist tracks`,
  search results and import reports use `Playlist`/`Queues` headers
  with counts; `library scan` reports added tracks; `library search`
  matches filenames with match highlighting; `config import` validates
  before replacing; switch/tag-get name missing ids; `--log-level`
  rejects bad values.
- **Removed**: `library scan --mode` (use `--full-hash`), `library stats
  --detailed`, bare-numeric `queue remove`, `queue add` bare-query FTS
  fallback (use `--search`); `preview` is deprecated (hidden from help,
  use direct play).

### Fixed
- `config list` returning zero entries (array-only indexed access on
  objects); relative-seek whitespace detection; `playlist tracks`
  header; rescan duplicating untouched files; `queue add` skipping
  metadata extraction; missing-file vs empty-glob errors; separator-
  insensitive path removal/import; negative `history --limit`; man page
  EXAMPLES/FILES garble plus new EXIT STATUS section.
- Fingerprint modes documented as incompatible (cross-mode rescans
  re-add); new scan rows store generic paths.

## [v0.43.2] - 2026-10-04

### Fixed
- Negative `history --limit` values fall back to the default instead of
  reaching SQL.

## [v0.43.1] - 2026-10-04

### Added
- `play` and direct play autostart the daemon when it is down (quiet
  under `--json`).
- `config get` answers from the local file; no daemon needed.

## [v0.43.0] - 2026-10-04

### Added
- Direct play: bare `caudio PATH...` plays files immediately in a new
  queue via a `PlayFiles` command (temporary and purged on daemon
  shutdown; `--save` keeps it permanently). Shared ensure-track helper
  backs the add paths. `preview` stays as a hidden legacy alias.
- `queue list` shows a Temp marker; `queues.temp` column with migration.

### Fixed
- `play` while playing restarts the current track instead of starting the
  new queue (direct-play flow stops first).

## [v0.42.0] - 2026-10-04

### Added
- `library scan --full-hash` replaces `--mode`; output is `X tracks
  added` only; rescan guard (path+mtime) stops untouched files from
  duplicating, including across fingerprint modes.
- `library search` matches filenames; table
  `id/title/artist/album/genre/filename` with query highlighting.
- `library stats --most-played N`, `--queue all|ID` (repeatable),
  `--playlist ID` (repeatable); `--detailed` removed.
- `library remove` confirmations name the track
  (`Removed from library <label>`); files on disk untouched.
- `playlist create NAME`, `playlist add PID (--id ID… | PATH…
  [--recursive])` with duplicate warns; `tag get [FIELD]` with raw or
  single-key JSON output.
- `playlist import` returns matched/skipped/duplicate counts in the CLI
  result (was daemon stderr only); separator-insensitive path matching.
- `config import` validates JSON shape and known-key types before atomic
  replace; `config get --json` returns `{key: value}`.
- `switch` and `tag get` name missing ids; `--log-level` rejects bad values.

### Changed
- Duplicate-only adds exit success (warn on stderr, no rows created).

### Fixed
- `playlist save` tolerates legacy duplicate queue rows.

## [v0.41.0] - 2026-10-04

### Added
- `queue add` takes repeatable file/folder/glob PATHs (sorted client-side
  expansion), `--id ID`, `--playlist ID [--replace]`, `--recursive`;
  missing library rows auto-added with metadata extraction; appended
  tracks extend the shuffle permutation; per-track `Added …` lines plus
  `X tracks added` total; empty globs hint on stderr with success.
- `queue remove` requires `--id` (library id) or `--pos` (queue
  position) — no bare numbers; also accepts PATH/glob/folder selectors;
  per-track `Removed from queue <…>` confirmations, no count.
- `queue create NAME` / `queue delete QID` (active queue guarded).
- `playlist load` targets a NEW queue (playback and current queue stay
  intact); `--replace` overwrites the active queue; `--play` switches
  and plays. `queue add --playlist` appends playlist tracks.

### Changed
- Server-side duplicate suppression: warn-only re-adds create no rows
  (previously message-only — the server still duplicated).

## [v0.40.0] - 2026-10-04

### Added
- `queue tracks [--order added|playback] [--json]` lists the active queue
  in playback (shuffle) order with a current-track marker (`>`, green on
  TTY); `--order added` shows insertion order.
- `queue list` now lists queues for real (ID/Name/Tracks/Active);
  `queue queues` stays as a hidden alias; new `Queues` IPC result.

### Changed
- Natural track end with repeat Off **stops** (cursor parked past end,
  later `play` wraps to head); manual `next` still wraps in every mode
  (`autoNext()`/`advanceLocked()` split in engine).

## [v0.39.0] - 2026-10-04

### Added
- Volume mute-restore model: `mute`/`volume 0` snapshots the level into
  persisted `pre_mute_volume`; `unmute` restores it (was fixed 50%).
  `Volume: muted` prints only when muted, else `Volume: N%`; `status`
  reports the real mute state (was hardcoded `no`).

### Changed
- Play after stop replays the stopped track (`stop()` rewinds the
  cursor; no-op when already stopped).
- `pause` warns `nothing playing` instead of failing when idle; `resume`
  plays from the cursor when stopped and warns `already playing` when
  playing; `prev` warns `at queue start` instead of failing.

### Fixed
- `config list` returning zero entries (array-only indexed access on JSON
  objects); relative-seek whitespace detection (`" +5"` applied as
  absolute); man EXAMPLES/FILES de-garbled plus new EXIT STATUS section
  (0/1/105/106/109).

## [v0.38.1] - 2026-10-04

### Added
- `seek` prints nothing on success; `config set` silent on success;
  `config get --json`; `playlist tracks --json` with `Playlist (N
  tracks):` header (was `Queue (N tracks):`).

### Changed
- Bare `queue shuffle` reports the resulting state; bare `queue repeat`
  cycles off→all→one→off and reports it (was no-op `Repeat toggled`).

## [v0.38.0] - 2026-10-03

### Added
- CLI confirmations instead of bare `OK`, remedy hints on daemon-down
  errors, valid grammars echoed on validation errors, compact watch
  lines, transport one-liners without ids/queue positions, `--json` on
  mutating commands.
- Hermetic golden test harness (`tests/test_cli_golden.cpp`) with unique
  temp dirs per run.

## [1.0.0-rc2] - 2026-10-02

### Added
- nlohmann/json v3.12.0 vendored in `vendor/nlohmann/` (`json.hpp`, `json_fwd.hpp`):
  eliminates ~180MB git clone on every configure, zero network, zero FetchContent
  logic. `CMakeLists.txt` simplified to single `CAUDIO_NLOHMANN_PRIVATE_INCLUDE`.

### Changed
- `GIT_SHALLOW TRUE` on all FetchContent declares (nlohmann_json, CLI11, Catch2):
  reduces fresh configure time from ~93s to ~15–25s on typical CI networks.

## [0.37.1] - 2026-10-02

### Fixed
- macOS/brew-LLVM build: libc++ ships neither `std::generator` nor
  `std::move_only_function`, so `db/scan` and `db/write_thread` now use
  in-tree `caudio::utils::Generator` (coroutine-based,
  `utils/generator.hpp`) and `caudio::utils::MoveOnlyFunction`
  (type-erased, `utils/function.hpp`)
- Brew libc++ toolchain hardened: `-cxx-isystem` (wins over FFmpeg
  `-isystem` regardless of flag order), `-stdlib=libc++` at compile and
  link time, versioned `libc++*.dylib` lookup
- CI macOS setup: `brew update` before install, LLVM >= 20 version probe;
  packages built as TGZ+ZIP

## [0.37.0] - 2026-10-01

### Added
- Compiler toolchain files (`cmake/toolchains/`: `gcc`, `clang`, `msvc`,
  host-default `auto`, shared brew-LLVM resolver) + `CompilerPolicy.cmake`:
  Apple/GCC and Apple/AppleClang fail at configure (dummy audio backend /
  C++23 gaps; `CAUDIO_ALLOW_UNSUPPORTED_COMPILER` escape hatch), MinGW warns
  on binary size
- Compiler preset variants (`dev`/`ci`/`modules`/`release`/`release-lto`/
  `minsize` × `-gcc`/`-clang`/`-msvc`) over hidden `*-base` purpose presets;
  bare presets now select the host-default compiler via `auto`; `custom`
  preset for arbitrary build dirs; `-msvc` presets use the Visual Studio
  2026 generator (no vcvars needed)
- CI matrix across compilers: Linux `ci-gcc`, macOS `ci-clang` (brew LLVM),
  Windows `ci-gcc` (MinGW) + `ci-msvc`

### Changed
- **BREAKING (install)** Library de-noise: `db/detail`, `db/fingerprint`,
  `db/fts`, `db/queue` internals, `db/schema`, `db/statement`,
  `db/stmt_helpers`, `db/transaction`, `engine/history`, `engine/shuffle`
  moved to `src/` (not installed); `_impl` splits merged; fat headers
  slimmed; `HistoryEntry` unified on `db::`; vendor headers no longer
  installed
- Uniform `component_core` naming (`client_core`, `service_core`,
  `engine_core`, module `:core` partitions)
- Clang targets the native MSVC ABI on Windows (MinGW `--target` override
  and `-fsized-deallocation` dropped after full verification)
- `vendor/` include dirs marked `SYSTEM` (vendored headers stay
  warning-clean under all compilers); `_CRT_SECURE_NO_WARNINGS` now also
  covers Clang-on-Windows

### Fixed
- Downstream module exports: `sqlite3`/`blake3` dangling
  `INSTALL_INTERFACE` vendor path removed (broke every `find_package`
  configure); deleted `service :paths`/`:status`/`:audio` partitions
  (wrapped non-installed `src/` internals, broke downstream BMI rebuild);
  full 7-module downstream import smoke green
- MSVC warnings: `#pragma GCC` guarded (`thread.cpp`), `lk` shadowing
  renamed (`write_thread.cpp`), `int64_t`→`int32_t` narrowing casts
  (`queue.cpp`)

## [0.36.0] - 2026-09-28

### Added
- `caudio::utils::Json` opaque JSON value type (`utils/json.hpp`, backend in
  `src/utils/json.cpp`, `modules/utils/json.cppm` partition): `Json`/`JsonRef`/`JsonError`,
  insertion-order preserving, throwing `get<T>` matching previous backend behavior

### Changed
- **BREAKING (build)** Vendored sqlite target renamed `caudio` -> `sqlite3`
  (artifact `libsqlite3.a` / `sqlite3.lib`, exported as `caudio::sqlite3`),
  content-named like `blake3`; the only `caudio`-named artifact is the opt-in
  shared umbrella (`combined` -> `libcaudio`)
- nlohmann/json is now private (backend for `src/utils/json.cpp` only):
  `db/json.hpp` + `ipc/protocol.hpp` signatures use `caudio::utils::Json`;
  `ipc::detail::trackToJson/trackFromJson` deleted in favor of `db::` versions;
  `fromJson<Result>` trap fixed; `JSON_Install`, `find_dependency(nlohmann_json)`
  and `CAUDIO_JSON_EXTRA_INCLUDE` removed -- `install()` ships zero JSON headers

## [1.0.0-rc1] - 2026-09-28

### Changed
- **BREAKING (build)** Vendored sqlite static lib renamed `caudio_sqlite` ->
  `caudio` (artifact `libcaudio.a` / `caudio.lib`, exported as `caudio::caudio`)
- Executable target renamed `caudio` -> `caudio_cli` (shipped binary is still
  `caudio` / `caudio.exe` via `OUTPUT_NAME`); shared `combined` still produces
  `libcaudio`
- Dependencies to latest: nlohmann/json 3.11.3 -> 3.12.0, CLI11 2.4.2 -> 2.7.2,
  Catch2 3.7.1 -> 3.16.0, sqlite 3.46.1 -> 3.53.4 (BLAKE3 1.8.7 and miniaudio
  0.11.25 already current)

## [0.35.2] - 2026-09-27

### Fixed
- CPack output pinned to the build dir (`CPACK_OUTPUT_DIRECTORY`); packages
  previously landed in the repo root and CI uploaded a raw `caudio.exe`
- Windows packages now bundle runtime DLLs next to the executable (FFmpeg
  `avcodec/avformat/avutil/swresample` + MinGW `libstdc++/libgcc/libwinpthread`),
  so archives run on bare machines
- Release uploads accept only `*.tar.gz` / `*.zip` and fail if missing

## [0.35.1] - 2026-09-27

### Removed
- `tui` stub subcommand (+ stale "planned v0.28.0" promises in README/man)

### Added
- `VERSION` file + precedence (`-DCAUDIO_PROJECT_VERSION` -> git tag ->
  `VERSION` file -> `0.1.0`+warning); `VERSION`-vs-tag drift guarded in CI
- `CAUDIO_BUILD_CLI` option (OFF skips CLI11 fetch + executable)
- `caudio::print`/`println` facade (`utils/print.hpp`): works around missing
  `std::__open_terminal`/`__write_to_terminal` in msys2 MinGW GCC 16 libstdc++
- `tests/sanitizers/lsan.supp`: narrow suppression for the FFmpeg
  probe-buffer drip (system libavutil, tracked, all other reports fatal)

### Changed
- **BREAKING (build)** Shared `*_shared`/`combined` targets gated behind
  `CAUDIO_BUILD_SHARED=OFF` by default
- Components declare `PUBLIC cxx_std_23` (downstream module consumers need it)
- `checkPidAlive` includes `<signal.h>`; `truncate15` defined in-header
- macOS: `pthread_setname_np` Apple form, `MA_NO_COREAUDIO` for GNU builds
- CI on presets; `--allow-multiple-definition` skipped on Darwin; Windows
  toolchain resolved via msys2-location; FFmpeg DLLs on PATH for tests
- Tree normalized to ASCII (BOMs stripped, mojibake fixed)
- clangd pointed at the preset compile database (`.vscode/settings.json`)

## [0.35.0] - 2026-09-27

### Added
- `CMakePresets.json`: 19 configure presets (`default`, `dev`, `ci`, `ci-sanitizers`, `modules`, `minimal`, `shared`, `release`, `release-lto`, `minsize`, `docs`, `all`, plus `-no-cli` splits, `minsize-lto`, `release-native`); build + test presets; CI migrated to `cmake --preset`
- New options: `CAUDIO_BUILD_SHARED` (default OFF -- gates `*_shared` + `combined` whole-archive maze), `CAUDIO_BUILD_CLI` (default ON -- OFF skips CLI11 fetch + executable), `CAUDIO_REQUIRE_GIT_VERSION` (default OFF -- fails fast with no git tag)
- Lazy FetchContent: `find_package(nlohmann_json/CLI11)` first, fetch only as fallback

### Changed
- **BREAKING (build)** Shared `*_shared`/`combined` targets no longer built by default; opt in with `-DCAUDIO_BUILD_SHARED=ON`
- Tag-less configure emits a loud warning (fallback `0.1.0` is not a release build)
- `test_player_integration` now defines `TEST_DATA_DIR` (fixes fixture lookup in nested `build/<preset>` trees)
- Removed stale `e.g. "v0.25.4"` version strings from `version.hpp` / `version.hpp.in` / `ipc/result.hpp`
- `engine.hpp` slimmed 21 -> 12 includes
- Private `config::detail` moved to `src/config_detail.hpp`; stale test narration deleted
- Removed `tui` stub subcommand (+ stale "planned v0.28.0" promises in README/man)
- Version precedence: `-DCAUDIO_PROJECT_VERSION` -> git tag -> `VERSION` file -> `0.1.0`+warning; `VERSION` drift guarded in CI

## [0.34.3] - 2026-09-27

### Changed
- nlohmann/json propagated via installed package (`find_dependency` + PUBLIC link); `json_fwd` in public headers
- Decoder collapsed to a single pImpl class (`IDecoder`/`DecoderRegistry`/`decoder_common` deleted)
- Windows TUs include real `<windows.h>` first (hand-rolled declarations removed)
- Hygiene: pruned duplicates, stale refs, warning leaks

## [0.34.2] - 2026-09-26

### Removed
- Deleted unused `ipc_channel_unix.cpp` / `ipc_channel_win.cpp` implementations

## [0.34.1] - 2026-09-26

### Changed
- Module interface units moved to `modules/` mirroring `src/`; backfilled 0.25.6-0.34.0 release notes

## [0.34.0] - 2026-09-26

### Changed
- **BREAKING** Library namespace `caudio::cli` split into `caudio::ipc` (commands, results, protocol) and `caudio::config` (config, paths); module `caudio.cli` renamed to `caudio.ipc`

## [0.33.0] - 2026-09-26

### Changed
- **BREAKING** `RepeatMode::Queue` renamed to `All` (matches CLI `repeat all` and JSON `"all"`)
- Docs: refreshed references, split CI smoke into header-mode and modules-ON jobs

## [0.32.1] - 2026-09-26

### Changed
- CMake: single test helper, factored install rules, `FindFFmpeg.cmake` ships with the package, docs moved to `cmake/Docs.cmake`, TGZ/ZIP-only CPack
- CMake: removed `WITH_FFMPEG` fiction (FFmpeg is unconditionally required) and `CAUDIO_ENABLE_CCACHE` option

## [0.32.0] - 2026-09-26

### Changed
- Hex helpers `toHex`/`fromHex` moved from `db::internal` to `caudio::utils`

## [0.31.1] - 2026-09-26

### Changed
- Service split: 1200-line `dispatch()` divided into `dispatch_playback/queue/library/config.cpp` via `handle()` overloads; `service_detail` divided into `service_paths`/`service_status`/`service_audio`; duplicate fingerprint/extension helpers removed

## [0.31.0] - 2026-09-26

### Changed
- Public headers no longer include `sqlite3.h`/`blake3.h`/`miniaudio.h` (forward declarations, `AudioOutput` pImpl, `SqliteCloser`)
- `IDecoder`, `Service`, `Client` promoted to public headers; test-only `AudioOutput` methods removed
- Added `caudio::utils::clampVolume` (`utils/math.hpp`)

## [0.30.0] - 2026-09-26

### Changed
- **BREAKING** CMake library `cli_shared` renamed to `ipc` (`caudio::ipc`); CLI app code compiles into the executable only

## [0.29.0] - 2026-09-26

### Changed
- **BREAKING** Removed redundant version accessors (`Engine`/`Database`/`caudio::version()`, `gitHash()`); only `caudio::utils` helpers and raw constants remain
- Removed pure-forwarder `OutputFormatter` string helpers

## [0.28.0] - 2026-09-26

### Changed
- **BREAKING** Removed compat shims (`ShuffleMode`, `getState`/`getPosition`, `attachDb`, `getCached` pair, `getTrackName`/`listTracksSimple`, span-deprecated `bindBlob`, C-string `Error`, service `socketPathFor`, hex wrappers)
- `shouldMarkPlayedEx` renamed to `shouldMarkPlayed`

## [0.27.1] - 2026-09-26

### Fixed
- Build: removed self-nesting `src/caudio` junction; private headers use relative includes

## [0.27.0] - 2026-09-25

### Changed
- Promoted umbrella include paths (`<caudio.hpp>`, `<caudio/db.hpp>`, ...), privatized detail headers, unified angle-bracket includes
- C++23 modules are now opt-in (`CAUDIO_ENABLE_MODULES=OFF` by default); headers are canonical for tests, CLI and examples

## [0.26.1] - 2026-09-22

### Fixed
- IPC: restored `IpcServer` framing, bound client timeout worker leak, removed CLI11 from public header

### Changed
- Reusable IPC/service/client code moved to `include/caudio`, flattened `json` usage to `nlohmann::ordered_json`

## [0.26.0] - 2026-09-21

### Changed
- Dual header/source layout introduced: public headers installed, implementations moved to `.cpp` files across utils/db/engine/player/service/client/CLI

## [0.25.6] - 2026-09-15

### Changed
- Tests: `CAUDIO_TEST_NOAUDIO` build flag + env switch, dummy ring helper
- Internal: string-view `Error` construction; CI switched to GCC

## [0.25.5] - 2026-09-15

### Added
- `docs` + `man` + `service` packaging -- `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `docs/man/caudio.1` (roff), `packaging/caudio.service` (systemd user unit); Doxygen docs wiring via `docs/Doxyfile.in`
- Packaging pipeline -- install `caudio` binary, C++23 module sources (`src/` + `cli/src/` for BMI rebuild), man page, systemd unit; `CPack` per-platform generators (`TGZ`/`ZIP` + `DEB`/`RPM` on Linux, `NSIS` on Windows, `DragNDrop` on macOS)

### Changed
- Version wiring to `import caudio.utils` -- `cmake/version.hpp.in` (moved from `version.hpp.in`), `src/utils/version.cppm` (`version()`/`versionString()`/`shortVersion()`/`versionCommit()`), `Engine::version()`/`Database::version()` + `staticVersion()`, `Status.version` via `service_detail::buildStatus` + `protocol` + `output_formatter`, CLI `--version` now `versionFull` (`v0.25.5`) (`301f7c8`)

### Fixed
- `cmake/version.hpp.in` clang-format fix

## [0.25.4] - 2026-09-14

### Added
- Comprehensive Doxygen documentation for all public APIs (`2331e88`)

## [0.25.3] - 2026-09-13

### Added
- `library list` -- filtered library listing with `--query`/`--artist`/`--album`/`--genre`/`--limit`/`--offset`/`--json`
- `library stats --detailed` -- detailed stats with most-played and total play time (`LibraryStatsDetailed`)
- `info` -- current track info with metadata and play statistics (`Info` command)

## [0.25.2] - 2026-09-13

### Fixed
- Metadata extraction + display fallback for missing tags (`b499553`)

## [0.25.1] - 2026-09-13

### Added
- Device management -- `device list`/`set`/`test` (miniaudio device enumeration)

## [0.25.0] - 2026-09-13

### Added
- Playback history -- `history list [--limit N] [--json]` / `history clear`
- TUI preview polish -- `tui` shows `status` + guidance (`cedac0f`)

## [0.24.1] - 2026-09-13

### Added
- Multi-queue switch -- `queue switch <qid>` with `active_queue_id` in engine, `QueueSwitch` IPC command and `QueueQueues` listing (`4b7e70d`)

## [0.24.0] - 2026-09-13

### Added
- Library add/remove -- `library add <path> [--recursive]`, `library remove <id>` with metadata extraction and fingerprint
- Tag editing -- `tag edit <id> <field>` / `tag get <id>` with field validation
- Config reset -- `config reset [key]` to restore defaults
- Status watch mode -- `status --watch`/`--follow` with `--interval <ms>` for continuous polling

## [0.23.3] - 2026-09-13

### Added
- Playlist operations -- `playlist rename` / `export` (m3u/pls/json) / `import` (m3u)

### Fixed
- Queue shuffle/repeat -- correctly toggles when no arg provided (`3c6f4e8`)

## [0.23.2] - 2026-09-12

### Changed
- CMake: target-scoped sanitizers, deduplicated sqlite, export fix, `FindFFmpeg` + `ExternalProject` fallback (`02816e3`)
- Tests: renamed `helpers_test.hpp` -> `common.hpp`, deduped `tempDbPath`/`safeRemoveDb`

### Fixed
- `test_reader` parallel race and compiler warnings (`6871ee1`)

## [0.23.1] - 2026-09-12

### Changed
- CMake: removed `CAUDIO_WITH_FFMPEG` in favor of `CAUDIO_WITH_FETCH_FFMPEG`, DRY component split (`ac6ad42`)
- Tests: removed `MockClock` and stub tests, redundant `create_directories` cleanup

## [0.23.0] - 2026-09-12

### Changed
- Vendor: BLAKE3 dispatch fix, dead vendor files removed, README added

## [0.22.0] - 2026-09-12

### Changed
- **BREAKING** C++23 module moves per audit (`8d935af`, `aee0cf7`)
- **BREAKING** C++23 module naming and thin-aggregator removal -- `caudio.db`, `caudio.engine`, `caudio.utils` partition renames (`43506aa`)

## [0.21.0] - 2026-09-12

### Changed
- **BREAKING** C++23 type renames per audit -- `StatusCode`, `Error`, `Result<T>`, `EngineState`, `QueueState` (`c69338e`)
- Modernization per audit: RAII guards, `std::expected` error handling, `std::jthread`/`std::stop_token`, concept constraints

### Added
- Polyglot integration strategy (`docs/polyglot-integration.md`) -- Rust metadata, Tauri GUI, Python `nanobind`, Go sidecar
- Single-module BMI build fix (`bbc1f47`) -- `CMAKE_POSITION_INDEPENDENT_CODE ON`, static libs own BMIs, shared variants use `whole-archive`

## [0.20.0] - 2026-09-11

### Added
- IPC layer: channel abstraction, server/client modules, single framing layer, canonical socket paths
- Client SDK: `IpcClient` wrapper and output formatter
- Service daemon: owns Engine/DB, dispatches library/playlist/config commands, flock single-instance, SHM status at 10 fps, ordered_json config
- CLI: App dispatch and main entry, background `start` with daemon flag

### Fixed
- SHM lock-free atomics, Windows single-instance, module hygiene, spawn portability, pipe fixes

## [0.19.2] - 2026-09-10

### Fixed
- DB/service: batch scan and queue transactions

## [0.19.1] - 2026-09-10

### Fixed
- DB/log: unify cache API, queue hygiene, search lock, log clean

## [0.19.0] - 2026-09-10

### Fixed
- Engine/ring: harden withTransaction, seek, ring memory order

## [0.18.2] - 2026-09-10

### Fixed
- Engine: next wraps/reshuffles at end, play when Playing serializes

## [0.18.1] - 2026-09-10

### Fixed
- Engine: play resumes when paused, queue persists via cursor, prev for non-shuffle

## [0.18.0] - 2026-09-10

### Fixed
- Engine: align decodeLoop and preroll sample vs frame with player
- Engine/player: keep queue on peek for status, start audio output
- DB: bypass queue statement cache, include sqlite error

## [0.17.0] - 2026-09-10

### Fixed
- CLI: start spawn robust with longer poll and correct CreateProcess
- Service: ensure db parent dirs exist and default path is writable
- CLI: use config for db path, fix Windows pipe path
- Service: named pipe on Windows for socket path
- Queue: handle file paths directly

## [0.16.0] - 2026-09-10

### Added
- Daemon: flock single-instance, SHM status at 10 fps

### Fixed
- Service: disable flock on Windows, rely on socket bind
- CLI: run start in background, add daemon flag

### Tests
- Daemon command roundtrip and playback control tests

## [0.15.0] - 2026-09-09

### Added
- IPC: shared command/result/protocol modules
- IPC: channel abstraction and server/client modules
- Client: IpcClient wrapper and output formatter
- Service: own Engine/DB and dispatch commands
- CLI: App dispatch and main entry
- Service: wire library/playlist/config dispatch

## [0.14.0] - 2026-09-08

### Fixed
- Engine: serialize seek with decode loop to prevent race

### Changed
- DB/utils/engine: dedup helpers, add RAII, modernize thread/log
- DB/utils: split queue partition, restrict MpscQueue copy, simplify atomics
- Build/player/engine: clean warnings and ownership

### Tests
- Expanded coverage for db, output, scan, search and engine

## [0.13.0] - 2026-09-07

### Fixed
- Engine queue deadlock, `MpscQueue` hardening, `scan` encapsulation
- DB helper dedup, audio output hardening, CMake simplification

## [0.12.0] - 2026-09-06

### Changed
- DB: split database monolith into internal partitions; fix sanitizeFtsTerm and exports

## [0.11.2] - 2026-09-06

### Fixed
- Player: correct sample vs frame count in audio callback
- Player: cached file size in `FileReader`, seek thread-safety

## [0.11.1] - 2026-09-06

### Fixed
- Player: correct FfmpegDecoder AVIO cleanup double-free

## [0.11.0] - 2026-09-06

### Added
- DB/player: BLAKE3 64K head-tail fingerprint
- DB: cache prepared statements per connection
- Player: facade with jthread decode and miniaudio output
- DB: harden WriteOp RAII and wire Database flush + write batch size

## [0.10.0] - 2026-09-06

### Changed
- Engine: split monolithic module into partitions

### Added
- CI: sanitizers and linting
- Tests: engine/db/player parity and review fixes
- Player: fix FFmpeg streaming and playback

## [0.9.0] - 2026-09-05

### Changed
- Player: FFmpeg-only -- removed `miniaudio`/`dr_*` decoder leftovers
- Build: FFmpeg provider cascade (system -> vcpkg -> prebuilt -> source)

### Fixed
- Player: FFmpeg streaming and playback, hybrid streaming for all formats
- Player: miniaudio streaming cleanup, remove beep

## [0.8.0] - 2026-09-05

### Added
- DB/engine: complete core modules
- Player: hybrid MP3 decoding (memory for small files, streaming for large)

## [0.7.0] - 2026-09-05

### Added
- Player: miniaudio device playback with 440Hz fallback
- Examples: `caudio_mini` example with miniaudio decoder playback

## [0.6.0] - 2026-09-05

### Changed
- Player: `dr_*`/`stb_vorbis` replaced with miniaudio `ma_decoder` as primary

## [0.5.0] - 2026-09-05

### Added
- DB/player/utils: AudioOutput, database modules, and test fixes
- Player: output module import (GCC 14.2 module ICE workaround)

## [0.4.0] - 2026-09-04

### Added
- Build: CMake 3.28 modules scaffold
- Utils: Result, Arena, Ring, Queue ports
- Player: Reader and DecoderRegistry with dr fallbacks, drwav unification
- Player: FFmpeg primary decoder with dr fallback
- Tests: real vorbis fixture

[0.34.0]: https://github.com/AhmedowM/caudio/compare/v0.33.0...v0.34.0
[0.33.0]: https://github.com/AhmedowM/caudio/compare/v0.32.1...v0.33.0
[0.32.1]: https://github.com/AhmedowM/caudio/compare/v0.32.0...v0.32.1
[0.32.0]: https://github.com/AhmedowM/caudio/compare/v0.31.1...v0.32.0
[0.31.1]: https://github.com/AhmedowM/caudio/compare/v0.31.0...v0.31.1
[0.31.0]: https://github.com/AhmedowM/caudio/compare/v0.30.0...v0.31.0
[0.30.0]: https://github.com/AhmedowM/caudio/compare/v0.29.0...v0.30.0
[0.29.0]: https://github.com/AhmedowM/caudio/compare/v0.28.0...v0.29.0
[0.28.0]: https://github.com/AhmedowM/caudio/compare/v0.27.1...v0.28.0
[0.27.1]: https://github.com/AhmedowM/caudio/compare/v0.27.0...v0.27.1
[0.27.0]: https://github.com/AhmedowM/caudio/compare/v0.26.1...v0.27.0
[0.26.1]: https://github.com/AhmedowM/caudio/compare/v0.26.0...v0.26.1
[0.26.0]: https://github.com/AhmedowM/caudio/compare/v0.25.6...v0.26.0
[0.25.6]: https://github.com/AhmedowM/caudio/compare/v0.25.5...v0.25.6
[0.25.5]: https://github.com/AhmedowM/caudio/releases/tag/v0.25.5
[0.25.4]: https://github.com/AhmedowM/caudio/compare/v0.25.3...v0.25.4
[0.25.3]: https://github.com/AhmedowM/caudio/compare/v0.25.2...v0.25.3
[0.25.2]: https://github.com/AhmedowM/caudio/compare/v0.25.1...v0.25.2
[0.25.1]: https://github.com/AhmedowM/caudio/compare/v0.25.0...v0.25.1
[0.25.0]: https://github.com/AhmedowM/caudio/compare/v0.24.1...v0.25.0
[0.24.1]: https://github.com/AhmedowM/caudio/compare/v0.24.0...v0.24.1
[0.24.0]: https://github.com/AhmedowM/caudio/compare/v0.23.3...v0.24.0
[0.23.3]: https://github.com/AhmedowM/caudio/compare/v0.23.2...v0.23.3
[0.23.2]: https://github.com/AhmedowM/caudio/compare/v0.23.1...v0.23.2
[0.23.1]: https://github.com/AhmedowM/caudio/compare/v0.23.0...v0.23.1
[0.23.0]: https://github.com/AhmedowM/caudio/compare/v0.22.0...v0.23.0
[0.22.0]: https://github.com/AhmedowM/caudio/compare/v0.21.0...v0.22.0
[0.21.0]: https://github.com/AhmedowM/caudio/compare/v0.20.0...v0.21.0
[0.20.0]: https://github.com/AhmedowM/caudio/releases/tag/v0.20.0
[0.19.2]: https://github.com/AhmedowM/caudio/compare/v0.19.1...v0.19.2
[0.19.1]: https://github.com/AhmedowM/caudio/compare/v0.19.0...v0.19.1
[0.19.0]: https://github.com/AhmedowM/caudio/compare/v0.18.2...v0.19.0
[0.18.2]: https://github.com/AhmedowM/caudio/compare/v0.18.1...v0.18.2
[0.18.1]: https://github.com/AhmedowM/caudio/compare/v0.18.0...v0.18.1
[0.18.0]: https://github.com/AhmedowM/caudio/compare/v0.17.0...v0.18.0
[0.17.0]: https://github.com/AhmedowM/caudio/compare/v0.16.0...v0.17.0
[0.16.0]: https://github.com/AhmedowM/caudio/compare/v0.15.0...v0.16.0
[0.15.0]: https://github.com/AhmedowM/caudio/compare/v0.14.0...v0.15.0
[0.14.0]: https://github.com/AhmedowM/caudio/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/AhmedowM/caudio/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/AhmedowM/caudio/compare/v0.11.2...v0.12.0
[0.11.2]: https://github.com/AhmedowM/caudio/compare/v0.11.1...v0.11.2
[0.11.1]: https://github.com/AhmedowM/caudio/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/AhmedowM/caudio/compare/v0.10.0...v0.11.0
[0.10.0]: https://github.com/AhmedowM/caudio/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/AhmedowM/caudio/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/AhmedowM/caudio/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/AhmedowM/caudio/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/AhmedowM/caudio/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/AhmedowM/caudio/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/AhmedowM/caudio/releases/tag/v0.4.0
