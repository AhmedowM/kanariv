#!/usr/bin/env python3
"""Run clang-tidy over the repo compilation database (report or safe fix).

Usage:
    python3 scripts/tidy_all.py [--build-dir DIR] [--fix] [--checks CHECKS]
                                [--export-fixes DIR] [-j N] [--runner PATH]
                                [--clang-tidy PATH] [file-regex...]
    python3 scripts/tidy_all.py --headers [--checks CHECKS]
                                [--export-fixes DIR] [-j N]

Thin wrapper around LLVM's run-clang-tidy: picks the repo .clang-tidy
config, verifies the compilation database, parallelizes. Default mode only
reports. With --fix only the curated safe set runs; pass --checks
explicitly to override. Module partitions (*.cppm) are out of scope for
now: use a header-mode dev build directory (`build/dev` by default).

--headers analyzes every public header as its own TU (headers are never
main files in the DB, so plain runs never fix them). Each header inherits
compile flags from a same-component TU. --fix is not supported with
--headers: review the exported YAML and apply per component after review;
default checks is misc-include-cleaner.
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SAFE_FIX_CHECKS = ("modernize-use-nullptr,modernize-use-override,"
                     "modernize-use-equals-default,modernize-use-equals-delete,"
                     "modernize-deprecated-headers,performance-*")
DEFAULT_BUILD_DIR = "build/dev"
# run-clang-tidy matches file args with re.search against DB paths. Default
# allowlist keeps repo sources only: fetched third-party TUs (_deps) would
# otherwise dominate the run (108 of 191 TUs in build/dev).
DEFAULT_SOURCE_FILTER = r"^(?!.*_deps.*).*[\\/](src|include|cli|tests|examples)[\\/]"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_runner(explicit: str | None) -> list[str]:
    if explicit:
        return [sys.executable, explicit]
    on_path = shutil.which("run-clang-tidy")
    if on_path:
        return [sys.executable, on_path]
    llvm_bin = Path(shutil.which("clang-tidy") or "")
    sibling = llvm_bin.parent / "run-clang-tidy" if llvm_bin else None
    if sibling is not None and sibling.exists():
        return [sys.executable, str(sibling)]
    print("[ERROR] run-clang-tidy not found (pass --runner PATH)", file=sys.stderr)
    sys.exit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", default=DEFAULT_BUILD_DIR,
                    help="build tree with compile_commands.json")
    ap.add_argument("--fix", action="store_true",
                    help="apply fixes (restricted to safe checks unless --checks given)")
    ap.add_argument("--checks", default=None,
                    help="clang-tidy checks filter (default: repo .clang-tidy; with --fix: safe set)")
    ap.add_argument("--export-fixes", default=None,
                    help="write suggested fixes as YAML here for review instead of applying")
    ap.add_argument("-j", type=int, default=os.cpu_count(),
                    help="parallel jobs (default: cpu count)")
    ap.add_argument("--runner", default=None, help="run-clang-tidy script path")
    ap.add_argument("--clang-tidy", default=None, help="clang-tidy binary (default: PATH lookup)")
    ap.add_argument("--headers", action="store_true",
                    help="analyze public headers as own TUs (needs --export-fixes for fixes)")
    ap.add_argument("files", nargs="*",
                    help="file path regexes, e.g. 'src.utils.error.cpp' "
                         "(default: repo sources, _deps excluded)")
    args = ap.parse_args()

    db = repo_root() / args.build_dir / "compile_commands.json"
    if not db.exists():
        print(f"[ERROR] no compilation database at {db} (configure the dev intent first)",
              file=sys.stderr)
        return 2

    tidy = args.clang_tidy or shutil.which("clang-tidy")
    if not tidy:
        print("[ERROR] clang-tidy not on PATH (pass --clang-tidy PATH)", file=sys.stderr)
        return 2
    if args.headers:
        return run_headers(args, tidy, db)

    cmd = find_runner(args.runner)
    cmd += ["-clang-tidy-binary", tidy, "-p", str(db.parent), "-j", str(args.j)]

    if args.fix and args.checks is None:
        # = form: a value starting with '-' would parse as more flags.
        cmd += ["-checks=-*," + SAFE_FIX_CHECKS]
    elif args.checks is not None:
        # -*, prefix: repo .clang-tidy is wide, and clang-tidy unions
        # command-line -checks with the config instead of replacing it.
        checks = args.checks if args.checks.startswith("-*,") else "-*," + args.checks
        cmd += ["-checks=" + checks]
    if args.fix:
        if not shutil.which("clang-apply-replacements"):
            print("[ERROR] --fix needs clang-apply-replacements on PATH", file=sys.stderr)
            return 2
        cmd += ["-fix"]
    if args.export_fixes:
        cmd += ["-export-fixes", args.export_fixes]
    cmd += args.files if args.files else [DEFAULT_SOURCE_FILTER]

    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=repo_root())
    return r.returncode


def db_flags(entry: dict) -> list[str]:
    if "arguments" in entry:
        toks = list(entry["arguments"])
    else:
        toks = shlex.split(entry["command"], posix=True)
    flags: list[str] = []
    skip_next = False
    for i, tok in enumerate(toks):
        if i == 0:
            continue  # compiler executable
        if skip_next:
            skip_next = False
            continue
        if tok in ("-o", "-c"):
            skip_next = True
            continue
        flags.append(tok)
    return flags


def run_headers(args: argparse.Namespace, tidy: str, db: Path) -> int:
    if args.fix:
        print("[ERROR] --fix is not supported with --headers (review YAML first)", file=sys.stderr)
        return 2
    if args.files:
        print("[ERROR] file regexes are not supported with --headers", file=sys.stderr)
        return 2
    checks = args.checks if args.checks else "-*,misc-include-cleaner"
    if not checks.startswith("-*,"):
        checks = "-*," + checks
    entries = json.loads(db.read_text(encoding="utf-8"))
    by_comp: dict[str, list[str]] = {}
    fallback: list[str] = []
    for e in entries:
        f = e["file"].replace("\\", "/")
        if "_deps" in f or "/vendor/" in f:
            continue
        flags = db_flags(e)
        if not fallback and ("/src/" in f or "/cli/" in f):
            fallback = flags
        parts = Path(f).parts
        if "src" in parts:
            by_comp.setdefault(parts[parts.index("src") + 1], flags)
    if not fallback:  # last resort: first usable entry
        for e in entries:
            f = e["file"].replace("\\", "/")
            if "_deps" not in f and "/vendor/" not in f:
                fallback = db_flags(e)
                break
    inc = repo_root() / "include" / "kanariv"
    headers = sorted(inc.rglob("*.hpp"))
    if args.export_fixes:
        outdir = Path(args.export_fixes)
        outdir.mkdir(parents=True, exist_ok=True)

    def one(header: Path) -> tuple[str, int]:
        rel = header.relative_to(inc)
        comp = rel.parts[0] if len(rel.parts) > 1 else ""
        flags = by_comp.get(comp, fallback)
        cmd = [tidy, str(header), "-checks=" + checks]
        yaml = None
        if args.export_fixes:
            yaml = str(Path(args.export_fixes) / (rel.with_suffix("").as_posix().replace("/", "-") + ".yaml"))
            cmd += ["--export-fixes=" + yaml]
        cmd += ["--"] + flags
        r = subprocess.run(cmd, cwd=repo_root(), capture_output=True, text=True)
        if not args.export_fixes and (r.stdout or r.stderr):
            print(r.stdout, end="")
            print(r.stderr, end="", file=sys.stderr)
        return (rel.as_posix(), r.returncode)

    bad = 0
    with ThreadPoolExecutor(max_workers=args.j) as ex:
        for rel, rc in ex.map(one, headers):
            print(f"{rel}: exit {rc}")
            bad += rc != 0
    print(f"{len(headers)} header(s) analyzed, {bad} with errors")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
