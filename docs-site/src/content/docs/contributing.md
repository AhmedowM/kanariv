---
title: Contributing
description: How to contribute to kanariv — setup, style, commit conventions, PR process
---

# Contributing

Thank you for contributing to kanariv! This guide covers development setup,
coding conventions, and the pull request process.

## Development setup

```sh
git clone https://github.com/AhmedowM/kanariv.git
cd kanariv

# Recommended: development build with tests
python scripts/configure.py dev --toolchain clang --jobs 4 --build --test
```

See the [Installation guide](/getting-started/installation) for full
prerequisites and platform notes.

## Coding conventions

- **Formatter:** `clang-format` using `.clang-format` at repo root
- **Linter:** `clang-tidy` using `.clang-tidy` (optional)
- **Naming:** `PascalCase` for types, `camelCase` for functions/variables,
  `snake_case` for files and locals
- **Headers:** Public API in `include/kanariv/` (`*.hpp`), sources in `src/`
  (`*.cpp`). C++23 modules (`*.cppm`) mirror headers
- **Errors:** Return `std::expected<T, utils::Error>`; use existing
  `StatusCode` values; do not throw across module boundaries

Run `clang-format -i` on changed files before committing.

## Commit style

This project uses [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <description>

[optional body]

[optional footer(s)]
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `build`,
`ci`

Scopes: `cli`, `engine`, `db`, `player`, `utils`, `service`, `ipc`, `cmake`,
`vendor`, `docs`

Examples:

```
feat(cli): add library list + enhanced stats + info subcommand
fix(engine): align decodeLoop and preroll sample vs frame with player
docs: comprehensive Doxygen documentation for all public APIs
```

Keep the subject line under 72 characters. Use `BREAKING CHANGE:` footer for
API-breaking changes.

## Pull request process

1. **Branch** from `main`:
   ```sh
   git checkout -b feat/my-feature
   ```
2. **Implement with tests.** No logic changes to `src/` without corresponding
   test updates. New IPC commands need a `Command` variant, a
   `Service::handle()` overload, and `tests/test_ipc.cpp` coverage.
3. **Configure and test locally:**
   ```sh
   cmake -B build -G Ninja -DKANARIV_ENABLE_TESTS=ON
   cmake --build build -j4
   ctest --test-dir build -j4
   cmake -B build -G Ninja -DKANARIV_BUILD_DOCS=ON \
     && cmake --build build --target doc-check
   ```
4. **Format** changed files with `clang-format`.
5. **Push** and open a PR against `main`. Include summary of changes, any
   `BREAKING CHANGE` notes, and screenshots or `kanariv --help` output for CLI
   changes.
6. **CI** must pass (build + tests on Linux/Windows/macOS).
7. **Review** — address feedback; squash or rebase as requested.

## License

By contributing, you agree that your contributions will be licensed under the
MIT License.
