# kanariv-rs (experimental Rust components)

An isolated Rust exploration in three layers:

1. `kanariv-ipc` — a reusable, FFI-free codec and transport for the existing
   IPC wire. It models all 65 commands and all 26 result kinds key-for-key
   against `src/ipc/protocol.cpp`, with a hardened frame parser and mutation
   tests.
2. `kanariv-cli` (binary `kanariv-rs`) — a frontend-only CLI that talks to the
   existing C++ daemon over that wire.
3. `kanariv-db` — an experimental, link-free reimplementation of the SQLite
   persistence layer (schema, FTS5 search, queue positioning, playlists,
   history, bookmarks, lyrics). It is a parity experiment only: the daemon
   does not link it.
4. `kanariv-db-ffi` — a read-only C ABI (`cdylib`) over `kanariv-db`, plus a
   standalone C++ parity executable. This measures cross-language cost and ABI
   maintenance; it does not implement `IDatabase` and is never linked into the
   daemon.

None of these crates links C++ code, uses FFI, or touches decoding or the
audio thread — the audio path stays entirely C++ by design.

The C++ `kanariv` binary remains canonical. Rust targets build alongside it
and are disabled unless explicitly enabled.

## Workspace layout

```
rust/
  Cargo.toml          # workspace (kanariv-ipc + kanariv-cli + kanariv-db)
  kanariv-ipc/        # wire codec + transport (no CLI policy)
    src/frame.rs      # [u32 BE length][JSON], 16 MiB cap
    src/command.rs    # typed Command, exact C++ JSON
    src/reply.rs      # typed Reply/Payload parsing
    src/model.rs      # Track, Playlist, Status records, wire enums
    tests/            # golden schema, reply fixtures, mutation/fuzz
  kanariv-cli/        # CLI, config discovery, daemon lifecycle
  kanariv-db/         # experimental SQLite parity crate (not linked)
    src/schema.rs     # schema/pragmas/triggers, byte-identical to core.cpp
    src/store.rs      # CRUD, 3-stage search, queue gaps, history, playlists
    tests/schema_parity.rs  # compares SCHEMA against src/db/core.cpp
    tests/store.rs          # behavior tests (16 total across both files)
  kanariv-db-ffi/     # read-only C ABI pilot (cdylib, not linked)
    include/kanariv/db_ffi.h  # C contract: open/close/search/get/string_free
    src/lib.rs                # JSON-returning ABI + status mapping
    parity.cpp                # C++ harness: same DB through both layers
  CMakeLists.txt      # optional KANARIV_ENABLE_RUST_CLI / _RUST_DB / _RUST_DB_FFI
```

## Scope

Implemented (parity with the C++ shell's slice):

| Group | Commands |
| --- | --- |
| Lifecycle | `start [--foreground]`, `shutdown` |
| Transport | `play`, `pause`, `resume`, `restart`, `stop`, `next`, `prev`, `seek` |
| Reporting | `status`, `info`, `volume`, `version` |
| Queue | `queue list`, `queue queues`, `queue tracks`, `queue switch`, `queue create`, `queue delete`, `queue clear`, `queue shuffle`, `queue repeat` |

Not implemented yet (the C++ CLI still owns these): `playlist`, `library`,
`tag`, `history`, `config`, `device`, direct-path play, `status --watch`,
`--daemon` hosting.

`status --watch` exits with a clear error instead of polling; a status-stream
API is the planned successor.

## Build

```sh
cargo build --release            # from rust/
cargo test --workspace
cargo fmt --all --check
cargo clippy -p kanariv-cli -p kanariv-ipc -p kanariv-db -p kanariv-db-ffi --all-targets -- -D warnings
```

Via CMake (off by default; the C++ build is untouched when disabled):

```sh
python scripts/configure.py dev --toolchain clang \
  --define KANARIV_ENABLE_RUST_CLI=ON --build
# or: cmake -DKANARIV_ENABLE_RUST_CLI=ON ...

python scripts/configure.py dev --toolchain clang \
  --define KANARIV_ENABLE_RUST_DB=ON
cmake --build build/dev-clang-ninja-x64 --target kanariv_db_check

python scripts/configure.py dev --toolchain clang \
  --define KANARIV_ENABLE_RUST_DB_FFI=ON
cmake --build build/dev-clang-ninja-x64 --target test_rust_db_ffi
ctest -R test_rust_db_ffi --output-on-failure
```

The CMake target `kanariv_rs` runs `cargo build --release -p kanariv-cli` and
stages the binary next to the C++ `kanariv` executable. `kanariv_rs_check`
runs rustfmt, clippy, and the CLI/IPC test suites. `kanariv_db_check` runs the
same gates for `kanariv-db`; `kanariv_db_ffi_check` does so for the ABI crate.
Neither is part of the default build.

On memory-constrained hosts (8 GB and below), parallel rustc/clang jobs can
exhaust RAM and fail with `memory allocation ... failed` or
`STATUS_STACK_BUFFER_OVERRUN`. Cap the job counts, for example:

```sh
CARGO_BUILD_JOBS=2 cmake --build build/rust-cli-clang --target kanariv_rs_check
```

## kanariv-db (experimental)

`kanariv-db` reimplements the persistence slice of `kanariv::db::Database`
directly on rusqlite 0.42 with bundled SQLite (FTS5 enabled). It exists to
measure whether a Rust DB is worth pursuing, not to ship.

Parity mechanisms:

- `tests/schema_parity.rs` reads `src/db/core.cpp`, extracts the `schema`
  string literal, and asserts byte-for-byte equality with `SCHEMA`. The C++
  file remains the single source of truth; the test fails if either drifts.
- `tests/store.rs` exercises the behaviors that are easy to get subtly wrong:
  fingerprint uniqueness, source/library-id normalization, `date_added == 0`
  -> NULL, the three-stage search (exact MATCH -> prefix MATCH -> escaped
  LIKE with FTS errors tolerated), FTS trigger propagation on update/delete,
  queue gaps/midpoint insertion/renormalization, transactional batch rollback,
  playlist reorder, history + play-count updates, bookmarks/lyrics upsert, and
  cascade deletes.

Implemented surface: tracks (insert/update/get/lookup/delete/rating/dirty/
list/search), queues and queue items (including batch enqueue and positioning),
manual playlists, libraries, playback history and stats, bookmarks, and lyrics.

Known gaps (intentionally untouched in this experiment): write batching, smart
playlists, engine-state persistence, JSON import/export, and
scanning/maintenance. The daemon does not link the crate, so the C++ layer
remains authoritative. Because rusqlite 0.42 needs a newer compiler, the crate
declares `rust-version = "1.89.0"` while the workspace MSRV stays 1.75 for the
IPC/CLI crates.

The parity harness also caught a real defect in this crate: the FTS join used
the unqualified shared SELECT list, so `title`/`artist` were ambiguous and
every FTS stage failed silently into the LIKE fallback. The fields are now
qualified, and `search_uses_fts_stemming_not_only_like` guards the FTS path
(a stemming-only match that LIKE cannot produce).

## kanariv-db-ffi (C ABI pilot)

`rust/kanariv-db-ffi` builds a small `cdylib` exposing:

```c
KanarivDbHandle* kanariv_db_ffi_open(const char* path, char** error);
void             kanariv_db_ffi_close(KanarivDbHandle* handle);
int32_t          kanariv_db_ffi_search_json(KanarivDbHandle*, const char* query,
                                            int32_t limit, char** json, char** error);
int32_t          kanariv_db_ffi_get_track_json(KanarivDbHandle*, int64_t id,
                                               char** json, char** error);
void             kanariv_db_ffi_string_free(char* value);
```

Status codes are 0/1/2/3 (ok / invalid argument / not found / internal), and
every returned string is owned by the caller. Search returns a JSON array;
lookup returns one JSON object. JSON keeps the ABI to scalars and byte strings
while still exercising allocation, serialization, and error mapping.

`parity.cpp` opens a temporary on-disk database, inserts tracks through the
C++ `Database`, then queries the same file through the ABI and compares:
result sets for nine queries (FTS, prefix, LIKE fallback, literal `%`, operator
stripping, quoted phrase, empty/blank, no match), full field equality for
lookup, and the error mappings. It runs as CTest case `test_rust_db_ffi`.

The harness also prints a timing line. Measured with both sides in Release
(`release` intent, clang, 2000 iterations of `search("Abb", 10)` on a
four-track database), the medians were ~320 us for the C++ `searchTracks`
call and ~590 us for `search_json`, with ~10% of the Rust number coming from
JSON plus ABI marshaling (the crate alone measured ~540 us). Most of the
remaining time sits in SQLite statement handling on both sides, not at the
boundary. Treat these as ballpark figures: the corpus is tiny, run-to-run
noise is around 20% on a desktop Windows host, both sides prepare statements
per call, and Rust links bundled SQLite 3.54.1 while C++ uses the system
3.49.1. A realistic-size library benchmark is needed before drawing cost
conclusions.

Deliberately not done: implementing `IDatabase`, sharing an open connection or
mutex across the boundary, and linking the ABI into the daemon. Those are the
expensive, risky steps this pilot is meant to inform.

## Talking to the daemon

`kanariv-ipc` is a client-side codec only:

- It speaks the 4-byte big-endian length + JSON wire protocol
  (`rust/kanariv-ipc/src/frame.rs`), with the same 16 MiB frame cap enforced
  before allocation.
- `Command` covers every variant of `ipc::toJson(const Command&)`, including
  C++'s explicit `null` optionals and `idOrIndex`/`newName`/`deltaPct`
  spellings.
- `Reply` covers every variant of `ipc::toJson(const Result&)` plus
  `LibraryStatsDetailed`; unknown future types become `Reply::Other` and keep
  their raw JSON.
- The CLI locates the socket/pid files exactly like `kanariv::config`,
  including the `std::hash`-derived `kanariv-<hex8>` name.
- `start` spawns the C++ daemon: `KANARIV_DAEMON_BIN` when set, otherwise a
  `kanariv`/`kanariv.exe` sibling of the running executable.

The codec tests are the executable schema contract: golden JSON for every
command, fixtures for every reply kind, and deterministic random/mutation
tests that assert the frame and reply parsers never panic on arbitrary input
(no nightly or libFuzzer required).

The socket name embeds `std::hash<std::string>` of the database path, which is
implementation-defined. The port covers the MSVC STL (FNV-1a 64, used by
windows-msvc targets) and libstdc++ (MurmurHash64A, used by unix targets)
layouts; both were verified against the real C++ `std::hash` output.
libc++ on macOS uses a CityHash variant that is not reproduced; there
`socketPath` must be set explicitly in the config. The same escape hatch
applies when the daemon was built with a different C++ runtime than the Rust
target assumes (for example a MinGW daemon on Windows).

## Notes

- Exit codes match the C++ shell: `106` invalid argument, `109` not found,
  `105` daemon not running / bad state, `1` other runtime errors, and `1` for
  argument-parse failures.
- Human output mirrors `cli/src/shell/output_formatter.cpp`; `--json` re-encodes
  the same IPC result payload.
- `info` renders `last_played` in UTC; the C++ formatter uses local time.
- `version` reports Rust build provenance in the C++ label layout; the values
  naturally differ (compiler/std/runtime).

Known output differences (content-equivalent):

- Windows line endings: C++ stdout runs in text mode and emits CRLF; Rust
  writes LF. Strip `\r` when diffing captures.
- JSON key order: the daemon preserves insertion order, serde_json emits
  sorted keys. Payloads are structurally identical; object key order is not
  part of the wire contract.
- Malformed invocations: clap and CLI11 word their parse errors differently
  (both exit 1).
