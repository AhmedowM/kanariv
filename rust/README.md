# kanariv-rs (experimental Rust CLI)

Phase 1 of the Rust integration exploration: a **frontend-only CLI** that talks
to the existing C++ daemon over the existing IPC wire. It links no C++ code,
uses no FFI, and never touches decoding or the audio thread — the audio path
stays entirely C++ by design.

The C++ `kanariv` binary remains canonical. This crate builds `kanariv-rs`
alongside it and is disabled unless explicitly enabled.

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
cargo test
cargo fmt --check
cargo clippy --all-targets -- -D warnings
```

Via CMake (off by default; the C++ build is untouched when disabled):

```sh
python scripts/configure.py dev --toolchain clang \
  --define KANARIV_ENABLE_RUST_CLI=ON --build
# or: cmake -DKANARIV_ENABLE_RUST_CLI=ON ...
```

The CMake target `kanariv_rs` runs `cargo build --release` and stages the
binary next to the C++ `kanariv` executable. `kanariv_rs_check` runs rustfmt,
clippy, and the test suite.

On memory-constrained hosts (8 GB and below), parallel rustc/clang jobs can
exhaust RAM and fail with `memory allocation ... failed` or
`STATUS_STACK_BUFFER_OVERRUN`. Cap the job counts, for example:

```sh
CARGO_BUILD_JOBS=2 cmake --build build/rust-cli-clang --target kanariv_rs_check
```

## Talking to the daemon

The Rust CLI is a client only:

- It speaks the 4-byte big-endian length + JSON wire protocol
  (`rust/kanariv-cli/src/ipc/`), with the same 16 MB frame cap.
- It locates the socket/pid files exactly like `kanariv::config`, including
  the `std::hash`-derived `kanariv-<hex8>` name.
- `start` spawns the C++ daemon: `KANARIV_DAEMON_BIN` when set, otherwise a
  `kanariv`/`kanariv.exe` sibling of the running executable.

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
