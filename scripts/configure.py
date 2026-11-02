#!/usr/bin/env python3
"""Intent-driven configure/build/test helper for kanariv.

Replaces the compiler x generator x variant preset fan-out with one intent
plus explicit environment options. The script only resolves the command line
and delegates to cmake/ctest; it contains no build orchestration.

Examples:
    python scripts/configure.py dev
    python scripts/configure.py dev --toolchain gcc --build-dir build/dev-gcc
    python scripts/configure.py ci --toolchain clang --test
    python scripts/configure.py release-lto --toolchain gcc --build --package
    python scripts/configure.py android --ndk D:\android-ndk-r30 --build

--cc/--cxx are exported as CC/CXX for the configure process. The toolchain
files still pin bare compiler names (gcc/g++/clang/cl/cl), so version
selection is a PATH/update-alternatives concern, not a CC/CXX concern.
"""

from __future__ import annotations

import argparse
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLCHAINS = ("auto", "gcc", "clang", "msvc", "apple", "android")
GENERATORS = ("ninja", "vs")
INTENTS = (
    "dev",
    "docs",
    "ci",
    "release",
    "release-lto",
    "minsize",
    "minsize-lto",
    "modules",
    "android",
    "default",
)

DEFAULT_CACHE = {
    "CMAKE_BUILD_TYPE": "Debug",
    "KANARIV_BUILD_CLI": "ON",
    "KANARIV_ENABLE_TESTS": "OFF",
    "KANARIV_ENABLE_EXAMPLES": "OFF",
    "KANARIV_ENABLE_MODULES": "OFF",
    "KANARIV_ENABLE_SANITIZERS": "OFF",
    "KANARIV_BUILD_DOCS": "OFF",
    "KANARIV_TEST_NOAUDIO": "OFF",
    "KANARIV_REQUIRE_GIT_VERSION": "OFF",
    "KANARIV_USE_TRIMMED_FFMPEG": "OFF",
    "KANARIV_ALLOW_UNSUPPORTED_COMPILER": "OFF",
    "KANARIV_LINK_SECTION_GC": "OFF",
    "KANARIV_BUILD_SHARED": "ON",
    "CMAKE_INTERPROCEDURAL_OPTIMIZATION": "OFF",
}

INTENT_CACHE = {
    "dev": {
        "CMAKE_BUILD_TYPE": "Debug",
        "KANARIV_ENABLE_TESTS": "ON",
        "KANARIV_ENABLE_EXAMPLES": "ON",
    },
    "docs": {
        "CMAKE_BUILD_TYPE": "Debug",
        "KANARIV_BUILD_DOCS": "ON",
        "KANARIV_BUILD_CLI": "OFF",
        "KANARIV_ENABLE_TESTS": "OFF",
        "KANARIV_ENABLE_EXAMPLES": "OFF",
    },
    "ci": {
        "CMAKE_BUILD_TYPE": "Debug",
        "KANARIV_ENABLE_TESTS": "ON",
        "KANARIV_TEST_NOAUDIO": "ON",
    },
    "release": {
        "CMAKE_BUILD_TYPE": "Release",
        "KANARIV_REQUIRE_GIT_VERSION": "ON",
    },
    "release-lto": {
        "CMAKE_BUILD_TYPE": "Release",
        "KANARIV_REQUIRE_GIT_VERSION": "ON",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION": "ON",
    },
    "minsize": {"CMAKE_BUILD_TYPE": "MinSizeRel"},
    "minsize-lto": {
        "CMAKE_BUILD_TYPE": "MinSizeRel",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION": "ON",
        "KANARIV_LINK_SECTION_GC": "ON",
    },
    "modules": {
        "CMAKE_BUILD_TYPE": "Debug",
        "KANARIV_ENABLE_MODULES": "ON",
        "KANARIV_BUILD_CLI": "OFF",
        "KANARIV_ENABLE_TESTS": "OFF",
        "KANARIV_ENABLE_EXAMPLES": "OFF",
    },
    "android": {
        "CMAKE_BUILD_TYPE": "Release",
        "KANARIV_BUILD_CLI": "OFF",
        "KANARIV_BUILD_STATIC": "OFF",
        "KANARIV_ENABLE_TESTS": "OFF",
        "KANARIV_ENABLE_EXAMPLES": "OFF",
        "KANARIV_ENABLE_MODULES": "OFF",
        "KANARIV_BUILD_DOCS": "OFF",
        "KANARIV_BUILD_SHARED": "ON",
    },
    "default": {
        "CMAKE_BUILD_TYPE": "Debug",
        "KANARIV_BUILD_CLI": "ON",
        "KANARIV_ENABLE_TESTS": "OFF",
        "KANARIV_ENABLE_EXAMPLES": "OFF",
    },
}

VS_GENERATORS = {17: "Visual Studio 17 2022", 18: "Visual Studio 18 2026"}
VS_ARCH = {"x64": "x64", "arm64": "ARM64", "x86": "Win32"}
VCVARS_NAME = {"x64": "vcvars64.bat", "x86": "vcvars32.bat"}
MAC_ARCH = {"x64": "x86_64", "arm64": "arm64", "x86": "i386"}
VSWHERE_REQUIRES = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def normalize_arch(value: str) -> str:
    v = value.lower()
    if v in ("x64", "x86_64", "amd64"):
        return "x64"
    if v in ("arm64", "aarch64"):
        return "arm64"
    if v in ("x86", "i386", "i686", "win32"):
        return "x86"
    return v


def host_arch() -> str:
    return normalize_arch(platform.machine() or "x64")


def is_windows() -> bool:
    return platform.system() == "Windows"


def resolve_toolchain(requested: str, generator: str) -> str:
    if requested != "auto":
        return requested
    system = platform.system()
    if system == "Darwin":
        return "clang"
    if system == "Windows":
        if generator == "vs":
            return "msvc"
        if shutil.which("clang++"):
            return "clang"
        if os.environ.get("VCINSTALLDIR") or os.environ.get("VSINSTALLDIR"):
            return "msvc"
        return "gcc"
    return "gcc"


def fail(message: str, code: int = 2) -> None:
    print(f"configure.py: error: {message}", file=sys.stderr)
    raise SystemExit(code)


def vswhere(args: list[str]) -> str:
    pf86 = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    exe = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not exe.is_file():
        return ""
    try:
        result = subprocess.run(
            [str(exe), *args],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


def vswhere_install_root() -> str:
    for extra in (["-requires", VSWHERE_REQUIRES], []):
        root = vswhere(["-latest", "-products", "*", *extra,
                        "-property", "installationPath"])
        if root:
            return root.splitlines()[0].strip()
    return ""


def vswhere_version() -> str:
    for extra in (["-requires", VSWHERE_REQUIRES], []):
        version = vswhere(["-latest", "-products", "*", *extra,
                           "-property", "installationVersion"])
        if version:
            return version.splitlines()[0].strip()
    return ""


def vcvars_from_root(root: Path, name: str) -> Path | None:
    candidates = (
        root / "VC" / "Auxiliary" / "Build" / name,
        root / "Auxiliary" / "Build" / name,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def vs7_roots() -> list[Path]:
    try:
        import winreg
    except ImportError:
        return []
    roots: list[Path] = []
    for view in (winreg.KEY_WOW64_32KEY, 0):
        try:
            key = winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"SOFTWARE\Microsoft\VisualStudio\SxS\VS7",
                0,
                winreg.KEY_READ | view,
            )
        except OSError:
            continue
        with key:
            index = 0
            while True:
                try:
                    _, value, _ = winreg.EnumValue(key, index)
                except OSError:
                    break
                if value:
                    roots.append(Path(value))
                index += 1
    return roots


def vcvars_invocation(arch: str) -> tuple[str, list[str]]:
    if arch == "arm64":
        # vcvarsarm64.bat does not exist; vcvarsall.bat cross-compiles from
        # an x64 host or targets the native ARM64 toolset on an ARM64 host.
        target = "arm64" if host_arch() == "arm64" else "x64_arm64"
        return "vcvarsall.bat", [target]
    return VCVARS_NAME.get(arch, "vcvars64.bat"), []


def discover_vcvars(explicit: str | None,
                    arch: str) -> tuple[Path | None, str, list[str]]:
    name, vcvars_args = vcvars_invocation(arch)

    if explicit:
        path = Path(explicit)
        if path.is_dir():
            direct = path / name
            if direct.is_file():
                return direct, "--vcvars", vcvars_args
            nested = vcvars_from_root(path, name)
            if nested is not None:
                return nested, "--vcvars", vcvars_args
            fail(f"--vcvars directory does not contain {name}: {path}")
        if not path.is_file():
            fail(f"--vcvars batch not found: {path}")
        args = vcvars_args if path.name.lower() == name.lower() else []
        return path, "--vcvars", args

    for var in ("VSINSTALLDIR", "VCINSTALLDIR"):
        root = os.environ.get(var)
        if root:
            candidate = vcvars_from_root(Path(root), name)
            if candidate is not None:
                return candidate, var, vcvars_args

    root = vswhere_install_root()
    if root:
        candidate = vcvars_from_root(Path(root), name)
        if candidate is not None:
            return candidate, "vswhere", vcvars_args

    for root in vs7_roots():
        candidate = vcvars_from_root(root, name)
        if candidate is not None:
            return candidate, "VS7 registry", vcvars_args

    if shutil.which("cl"):
        return None, "cl already on PATH", []

    expected = Path(
        os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    ) / "Microsoft Visual Studio" / "<version>" / "VC" / "Auxiliary" / "Build" / name
    fail(
        f"MSVC toolchain not found (expected {expected}). "
        f"Pass --vcvars <path-to-{name}> or run from a VS developer shell."
    )


def vs_generator_name() -> str:
    version = vswhere_version()
    if not version:
        fail(
            "cannot determine the Visual Studio version for --generator vs "
            "(vswhere.exe not found or returned nothing); install VS with the "
            "C++ workload or use --generator ninja"
        )
    try:
        major = int(version.split(".", 1)[0])
    except ValueError:
        fail(f"unrecognized Visual Studio version from vswhere: {version!r}")
    generator = VS_GENERATORS.get(major)
    if generator is None:
        fail(
            f"Visual Studio {version} is not supported by this script "
            f"(known: {', '.join(f'{k} -> {v}' for k, v in VS_GENERATORS.items())})"
        )
    return generator


def capture_vcvars_env(vcvars: Path, vcvars_args: list[str],
                       base_env: dict[str, str]) -> dict[str, str]:
    # A temp batch file avoids cmd.exe's nested-quote rules on the argv list.
    invocation = f'call "{vcvars}"'
    if vcvars_args:
        invocation += " " + " ".join(vcvars_args)
    try:
        with tempfile.TemporaryDirectory(prefix="kanariv-vcvars-") as tmp:
            script = Path(tmp) / "capture.bat"
            script.write_text(
                f"@echo off\r\n{invocation} >nul 2>&1\r\nset\r\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmd", "/d", "/c", str(script)],
                capture_output=True,
                text=True,
                env=base_env,
                check=False,
            )
    except OSError as exc:
        fail(f"failed to run {vcvars}: {exc}", 1)
    if result.returncode != 0:
        fail(f"vcvars failed ({result.returncode}): {vcvars}", 1)

    merged = {k: v for k, v in base_env.items() if k.lower() != "path"}
    for line in result.stdout.splitlines():
        key, sep, value = line.partition("=")
        if not sep or not key or key.startswith("="):
            continue
        if key.lower() == "path":
            merged["Path"] = value
        else:
            merged[key] = value

    path_value = merged.get("Path") or merged.get("PATH") or ""
    if not shutil.which("cl", path=path_value):
        fail(f"cl.exe does not resolve after running {vcvars}", 1)
    return merged


def needs_vcvars(toolchain: str, generator: str) -> bool:
    return toolchain == "msvc" and generator == "ninja"


def build_env(args: argparse.Namespace, vcvars: Path | None,
              vcvars_args: list[str]) -> dict[str, str]:
    env = dict(os.environ)
    if vcvars is not None:
        env = capture_vcvars_env(vcvars, vcvars_args, env)
    if args.cc:
        env["CC"] = args.cc
    if args.cxx:
        env["CXX"] = args.cxx
    return env


def shell_command(cmd: list[str]) -> str:
    if is_windows():
        return subprocess.list2cmdline(cmd)
    return shlex.join(cmd)


def run(cmd: list[str], env: dict[str, str]) -> int:
    print(f"+ {shell_command(cmd)}", flush=True)
    try:
        result = subprocess.run(cmd, env=env)
    except OSError as exc:
        print(f"configure.py: failed to run {cmd[0]}: {exc}", file=sys.stderr)
        return 1
    return result.returncode


def build_cache(args: argparse.Namespace, intent: str,
                toolchain: str) -> dict[str, str]:
    cache = dict(DEFAULT_CACHE)
    cache.update(INTENT_CACHE[intent])

    if args.no_cli:
        cache["KANARIV_BUILD_CLI"] = "OFF"
    if args.no_tests:
        cache["KANARIV_ENABLE_TESTS"] = "OFF"
    if args.no_examples:
        cache["KANARIV_ENABLE_EXAMPLES"] = "OFF"
    if args.docs:
        cache["KANARIV_BUILD_DOCS"] = "ON"
    if args.no_audio:
        cache["KANARIV_TEST_NOAUDIO"] = "ON"
    if args.sanitizers:
        cache["KANARIV_ENABLE_SANITIZERS"] = "ON"
    if args.modules:
        cache["KANARIV_ENABLE_MODULES"] = "ON"
    if args.trimmed_ffmpeg:
        cache["KANARIV_USE_TRIMMED_FFMPEG"] = "ON"
    if args.static:
        cache["KANARIV_BUILD_STATIC"] = "ON"
    if args.require_git_version:
        cache["KANARIV_REQUIRE_GIT_VERSION"] = "ON"
    if args.allow_unsupported_compiler:
        cache["KANARIV_ALLOW_UNSUPPORTED_COMPILER"] = "ON"

    if args.extra_cxx_flags:
        cache["KANARIV_EXTRA_CXX_FLAGS"] = args.extra_cxx_flags
    if args.extra_c_flags:
        cache["KANARIV_EXTRA_C_FLAGS"] = args.extra_c_flags
    if args.extra_link_flags:
        cache["KANARIV_EXTRA_LINK_FLAGS"] = args.extra_link_flags
    return cache


def configure_command(args: argparse.Namespace, toolchain: str,
                      generator: str, arch: str, build_dir: Path,
                      cache: dict[str, str], generator_name: str,
                      ninja: str | None) -> list[str]:
    root = repo_root()
    cmd = [
        "cmake",
        "-S", str(root),
        "-B", str(build_dir),
        "-G", generator_name,
    ]
    if generator == "vs":
        cmd += ["-A", VS_ARCH.get(arch, arch)]
    if toolchain == "android":
        cmd.append(f"-DCMAKE_TOOLCHAIN_FILE={root / 'cmake' / 'android.cmake'}")
        if args.ndk:
            cmd.append(f"-DCMAKE_ANDROID_NDK={args.ndk}")
        cmd.append(f"-DANDROID_ABI={args.android_abi}")
        cmd.append(f"-DANDROID_PLATFORM=android-{args.android_api}")
    else:
        cmd += [
            f"-DCMAKE_TOOLCHAIN_FILE={root / 'cmake' / 'toolchain.cmake'}",
            f"-DKANARIV_TOOLCHAIN={toolchain}",
        ]
    for key, value in cache.items():
        cmd.append(f"-D{key}={value}")
    if args.ffmpeg_root:
        ffmpeg_root = Path(args.ffmpeg_root)
        if not ffmpeg_root.is_absolute():
            ffmpeg_root = repo_root() / ffmpeg_root
        ffmpeg_root = ffmpeg_root.resolve()
        cmd += [
            f"-DFFmpeg_ROOT={ffmpeg_root}",
            f"-DCMAKE_PREFIX_PATH={ffmpeg_root}",
        ]
    for define in args.define:
        cmd.append(f"-D{define}")
    if platform.system() == "Darwin":
        cmd.append(f"-DCMAKE_OSX_ARCHITECTURES={MAC_ARCH.get(arch, arch)}")
    if needs_vcvars(toolchain, generator):
        cmd.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
    return cmd


def build_type(cache: dict[str, str]) -> str:
    return cache["CMAKE_BUILD_TYPE"]


def action_commands(args: argparse.Namespace, build_dir: Path,
                    generator: str, config: str) -> list[tuple[str, list[str]]]:
    actions: list[tuple[str, list[str]]] = []
    if args.build:
        cmd = ["cmake", "--build", str(build_dir), "-j", str(args.jobs)]
        if args.intent == "docs":
            cmd += ["--target", "doc-check"]
        if generator == "vs":
            cmd += ["--config", config]
        actions.append(("build", cmd))
    if args.test:
        cmd = [
            "ctest",
            "--test-dir", str(build_dir),
            "--output-on-failure",
            "-j", str(args.jobs),
        ]
        if generator == "vs":
            cmd += ["-C", config]
        actions.append(("test", cmd))
    if args.install:
        cmd = ["cmake", "--install", str(build_dir)]
        if generator == "vs":
            cmd += ["--config", config]
        actions.append(("install", cmd))
    if args.package:
        cmd = ["cmake", "--build", str(build_dir), "--target", "package"]
        if generator == "vs":
            cmd += ["--config", config]
        actions.append(("package", cmd))
    return actions


def normalize_argv(argv: list[str]) -> list[str]:
    # argparse treats a bare "-march=native" as an option, not as the value
    # for --extra-cxx-flags. Join single-dash values before parsing.
    value_flags = {"--extra-cxx-flags", "--extra-c-flags", "--extra-link-flags"}
    out: list[str] = []
    index = 0
    while index < len(argv):
        token = argv[index]
        if token in value_flags and index + 1 < len(argv):
            value = argv[index + 1]
            if value.startswith("-") and not value.startswith("--"):
                out.append(f"{token}={value}")
                index += 2
                continue
        out.append(token)
        index += 1
    return out


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    if argv is None:
        argv = sys.argv[1:]
    argv = normalize_argv(argv)
    parser = argparse.ArgumentParser(
        description="Configure kanariv from an intent plus explicit environment.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "intents: " + ", ".join(INTENTS) + "\n"
            "defaults: intent=dev, toolchain=auto, generator=ninja, "
            "arch=host, build/<intent>-<toolchain>-<generator>-<arch>\n"
            "CC/CXX are exported for configure; toolchain files still pin "
            "bare compiler names, so select versions via PATH."
        ),
    )
    parser.add_argument("intent", nargs="?", metavar="INTENT",
                        help="build intent (default: dev)")
    parser.add_argument("--intent", dest="intent_flag", metavar="INTENT",
                        help="build intent (alternative to the positional)")
    parser.add_argument("--toolchain", default="auto",
                        help="auto|gcc|clang|msvc|apple|android (default: auto)")
    parser.add_argument("--generator", default="ninja",
                        help="ninja|vs (default: ninja)")
    parser.add_argument("--arch", default=host_arch(),
                        help="target arch (default: host)")
    parser.add_argument("--build-dir", default=None,
                        help="build directory (default: "
                             "build/<intent>-<toolchain>-<generator>-<arch>)")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                        help="parallel jobs (default: CPU count)")
    parser.add_argument("--cc", default=None,
                        help="CC for the configure process (bare name; see epilog)")
    parser.add_argument("--cxx", default=None,
                        help="CXX for the configure process (bare name; see epilog)")
    parser.add_argument("--ndk", default=None, metavar="PATH",
                        help="Android NDK root (android intent; otherwise "
                             "ANDROID_NDK_ROOT is used)")
    parser.add_argument("--android-abi", default="arm64-v8a",
                        help="Android ABI (default: arm64-v8a)")
    parser.add_argument("--android-api", type=int, default=24,
                        help="Android API level (default: 24)")
    parser.add_argument("--vcvars", default=None, metavar="PATH",
                        help="vcvars batch file or its directory (MSVC + Ninja)")
    parser.add_argument("--ffmpeg-root", default=None, metavar="PATH",
                        help="value for -DFFmpeg_ROOT (trimmed CI uses "
                             "$KANARIV_TRIMMED_ROOT)")
    parser.add_argument("--define", action="append", default=[],
                        metavar="KEY=VALUE",
                        help="extra -D cache definition (repeatable)")
    parser.add_argument("--extra-cxx-flags", default="",
                        help="KANARIV_EXTRA_CXX_FLAGS")
    parser.add_argument("--extra-c-flags", default="",
                        help="KANARIV_EXTRA_C_FLAGS")
    parser.add_argument("--extra-link-flags", default="",
                        help="KANARIV_EXTRA_LINK_FLAGS")

    parser.add_argument("--no-cli", action="store_true",
                        help="KANARIV_BUILD_CLI=OFF")
    parser.add_argument("--no-tests", action="store_true",
                        help="KANARIV_ENABLE_TESTS=OFF")
    parser.add_argument("--no-examples", action="store_true",
                        help="KANARIV_ENABLE_EXAMPLES=OFF")
    parser.add_argument("--docs", action="store_true",
                        help="KANARIV_BUILD_DOCS=ON")
    parser.add_argument("--no-audio", action="store_true",
                        help="KANARIV_TEST_NOAUDIO=ON")
    parser.add_argument("--sanitizers", action="store_true",
                        help="KANARIV_ENABLE_SANITIZERS=ON")
    parser.add_argument("--modules", action="store_true",
                        help="KANARIV_ENABLE_MODULES=ON (does not disable the CLI)")
    parser.add_argument("--trimmed-ffmpeg", action="store_true",
                        help="KANARIV_USE_TRIMMED_FFMPEG=ON")
    parser.add_argument("--static", action="store_true",
                        help="KANARIV_BUILD_STATIC=ON (build static libs alongside shared)")
    parser.add_argument("--require-git-version", action="store_true",
                        help="KANARIV_REQUIRE_GIT_VERSION=ON")
    parser.add_argument("--allow-unsupported-compiler", action="store_true",
                        help="KANARIV_ALLOW_UNSUPPORTED_COMPILER=ON")

    parser.add_argument("--print", action="store_true",
                        help="print the configure command and exit")
    parser.add_argument("--build", action="store_true", help="run cmake --build")
    parser.add_argument("--test", action="store_true", help="run ctest")
    parser.add_argument("--install", action="store_true", help="run cmake --install")
    parser.add_argument("--package", action="store_true",
                        help="build the package target")

    args = parser.parse_args(argv)

    if args.intent and args.intent_flag and args.intent != args.intent_flag:
        parser.error("positional INTENT and --intent disagree")
    args.intent = args.intent_flag or args.intent or "dev"
    if args.intent not in INTENTS:
        parser.error(f"unknown intent {args.intent!r} "
                     f"(choose from {', '.join(INTENTS)})")
    if args.toolchain not in TOOLCHAINS:
        parser.error(f"unknown --toolchain {args.toolchain!r} "
                     f"(choose from {', '.join(TOOLCHAINS)})")
    if args.generator not in GENERATORS:
        parser.error(f"unknown --generator {args.generator!r} "
                     f"(choose from {', '.join(GENERATORS)})")
    if args.jobs < 1:
        parser.error("--jobs must be >= 1")
    for define in args.define:
        key, sep, _ = define.partition("=")
        if not sep or not key:
            parser.error(f"--define expects KEY=VALUE, got {define!r}")
    if args.generator == "vs" and not is_windows():
        parser.error("--generator vs is only available on Windows")
    if args.intent == "android" and args.toolchain not in ("auto", "android"):
        parser.error("android intent requires --toolchain android (or auto)")
    if args.toolchain == "android" and args.intent != "android":
        parser.error("--toolchain android requires the android intent")
    if args.intent == "android" and args.android_api < 21:
        parser.error("--android-api must be >= 21")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    toolchain = "android" if args.intent == "android" else resolve_toolchain(
        args.toolchain, args.generator)
    if args.generator == "vs" and toolchain != "msvc":
        fail("--generator vs requires --toolchain msvc (or auto on Windows)")
    if args.vcvars and toolchain != "msvc":
        fail("--vcvars only applies to --toolchain msvc")

    arch = normalize_arch(args.arch)
    if args.toolchain == "auto":
        print(f"configure.py: auto toolchain -> {toolchain}", file=sys.stderr)

    if args.generator == "vs":
        generator_name = vs_generator_name()
    else:
        generator_name = "Ninja"

    ninja = shutil.which("ninja")
    if needs_vcvars(toolchain, args.generator) and not ninja:
        fail("ninja not found on PATH (MSVC + Ninja requires it)", 1)

    if args.build_dir:
        build_dir = Path(args.build_dir)
        if not build_dir.is_absolute():
            build_dir = repo_root() / build_dir
    else:
        build_arch = args.android_abi if toolchain == "android" else arch
        build_dir = repo_root() / "build" / (
            "docs" if args.intent == "docs" else
            f"{args.intent}-{toolchain}-{args.generator}-{build_arch}")
    build_dir = build_dir.resolve()

    cache = build_cache(args, args.intent, toolchain)
    config = build_type(cache)
    command = configure_command(
        args, toolchain, args.generator, arch, build_dir,
        cache, generator_name, ninja)

    vcvars: Path | None = None
    vcvars_args: list[str] = []
    if needs_vcvars(toolchain, args.generator):
        vcvars, source, vcvars_args = discover_vcvars(args.vcvars, arch)
        if vcvars is not None:
            invocation = f'call "{vcvars}"'
            if vcvars_args:
                invocation += " " + " ".join(vcvars_args)
            print(
                f"configure.py: vcvars {source}: {invocation}",
                file=sys.stderr,
            )
        else:
            print(f"configure.py: {source}", file=sys.stderr)

    if args.print:
        print(shell_command(command))
        return 0

    env = build_env(args, vcvars, vcvars_args)
    if run(command, env) != 0:
        print("configure.py: configure failed", file=sys.stderr)
        return 1

    for name, cmd in action_commands(args, build_dir, args.generator, config):
        if run(cmd, env) != 0:
            print(f"configure.py: {name} failed", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
