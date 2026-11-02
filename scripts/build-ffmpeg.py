#!/usr/bin/env python3
"""Build a trimmed decode-only FFmpeg prefix from source.

Usage:
    python3 scripts/build-ffmpeg.py [--prefix <dir>] [--source-tarball URL | --source-dir DIR] [--force] [--help]
    python3 scripts/build-ffmpeg.py --print-flags

Default prefix is ./build/ffmpeg. If the prefix directory already exists,
the build is skipped unless --force is specified. The script defaults to
building the trimmed FFmpeg (always) unless the prefix directory exists,
in which case it falls back to the default system FFmpeg provider.

Configure flags and the pinned version are parsed from
cmake/FFmpegTrimmed.cmake (single source of truth -- the module's list
items contain no spaces, so token extraction is exact). Produces a shared-
only install tree (<prefix>/bin, include, lib) suitable for
KANARIV_TRIMMED_ROOT. Host-arch only by default; Android cross-compiles are
supported with --android-ndk, --android-abi, and --android-api.

--pack tars the prefix for deps-private upload. The asset name encodes the
host OS/arch (ffmpeg-trimmed-<ver>-<os>-<arch>.tar.gz); name it honestly --
a misnamed macos-x86_64 asset once shipped arm64 binaries.

Windows runs need a MINGW environment (MSYSTEM=UCRT64 set -- easiest via
ucrt64.exe); bare MSYS without MSYSTEM makes FFmpeg configure refuse.
"""

import argparse
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def cmake_module() -> Path:
    return repo_root() / "cmake" / "FFmpegTrimmed.cmake"


def trim_flags_file() -> Path:
    return repo_root() / "cmake" / "ffmpeg_trimmed_flags.txt"


def parse_trim_version(text: str) -> str:
    """Extract pinned version from cmake/FFmpegTrimmed.cmake."""
    m = re.search(r'set\(KANARIV_FFMPEG_TRIM_VERSION\s+"([^"]+)"', text)
    if not m:
        print("[ERROR] KANARIV_FFMPEG_TRIM_VERSION not found in FFmpegTrimmed.cmake",
              file=sys.stderr)
        sys.exit(1)
    return m.group(1)


def read_trim_flags() -> list[str]:
    """Read configure flags from cmake/ffmpeg_trimmed_flags.txt (one per line)."""
    flags_file = trim_flags_file()
    if not flags_file.exists():
        print(f"[ERROR] flags file not found: {flags_file}", file=sys.stderr)
        sys.exit(1)
    flags: list[str] = []
    for line in flags_file.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            flags.append(stripped)
    if not flags:
        print(f"[ERROR] no flags found in {flags_file}", file=sys.stderr)
        sys.exit(1)
    return flags


def asset_name(version: str, android_abi: str | None = None) -> str:
    if android_abi:
        return f"ffmpeg-trimmed-{version}-android-{android_abi}.tar.gz"
    os_name = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(
        platform.system(), platform.system().lower())
    machine = platform.machine().lower()
    arch = {"x86_64": "x86_64", "amd64": "x86_64",
            "aarch64": "arm64", "arm64": "arm64"}.get(machine, machine)
    return f"ffmpeg-trimmed-{version}-{os_name}-{arch}.tar.gz"


ANDROID_ABIS = {
    "arm64-v8a": ("aarch64", "armv8-a", "aarch64-linux-android"),
    "armeabi-v7a": ("arm", "armv7-a", "armv7a-linux-androideabi"),
    "x86_64": ("x86_64", "x86-64", "x86_64-linux-android"),
    "x86": ("x86", "i686", "i686-linux-android"),
}


def android_tools(ndk: Path, abi: str, api: int) -> tuple[list[str], str]:
    """Return FFmpeg configure arguments and the NDK C compiler path."""
    host_tag = {
        "Windows": "windows-x86_64",
        "Linux": "linux-x86_64",
        "Darwin": "darwin-x86_64",
    }.get(platform.system())
    if not host_tag:
        print(f"[ERROR] unsupported host for Android NDK: {platform.system()}",
              file=sys.stderr)
        sys.exit(2)
    arch, cpu, triple = ANDROID_ABIS[abi]
    tool_dir = ndk / "toolchains" / "llvm" / "prebuilt" / host_tag / "bin"
    compiler_suffix = ""
    tool_suffix = ".exe" if platform.system() == "Windows" else ""
    tools = {
        "cc": tool_dir / f"{triple}{api}-clang{compiler_suffix}",
        "cxx": tool_dir / f"{triple}{api}-clang++{compiler_suffix}",
        "strip": tool_dir / f"llvm-strip{tool_suffix}",
        "nm": tool_dir / f"llvm-nm{tool_suffix}",
        "ar": tool_dir / f"llvm-ar{tool_suffix}",
        "ranlib": tool_dir / f"llvm-ranlib{tool_suffix}",
    }
    missing = [str(path) for path in tools.values() if not path.is_file()]
    if missing:
        print("[ERROR] Android NDK tools not found:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        sys.exit(1)
    flags = [
        "--enable-cross-compile", "--target-os=android",
        f"--arch={arch}", f"--cpu={cpu}",
        *[f"--{name}={path.as_posix()}" for name, path in tools.items()],
    ]
    if abi == "x86":
        # FFmpeg's 32-bit x86 assembly uses absolute relocations that cannot
        # be linked into Android shared libraries as position-independent code.
        flags.append("--disable-asm")
    return flags, str(tools["cc"])


def _bash_has(bash: str, prog: str) -> bool:
    """Ask msys bash itself whether prog resolves (exact build-env semantics).

    Needed because an msys2 Python sees PATH with Win32 semantics and cannot
    parse its colon-separated POSIX entries -- shutil.which misses tools
    that the build shell finds perfectly well.
    """
    r = subprocess.run([bash, "-c", f"command -v {prog}"],
                       capture_output=True, text=True)
    return r.returncode == 0 and bool(r.stdout.strip())


def find_bash(explicit: str | None) -> str:
    """Locate a real msys/cygwin bash on Windows.

    Bare shutil.which("bash") is unusable: Win32 search order finds
    System32's WSL stub before anything on PATH. Prefer an explicit path,
    then well-known install roots, then PATH minus the stub.
    """
    if explicit:
        if not Path(explicit).is_file():
            print(f"[ERROR] --bash not found: {explicit}", file=sys.stderr)
            sys.exit(1)
        return explicit
    for root in (os.environ.get("MSYS2_ROOT", ""),
                 r"C:\msys64", r"C:\tools\msys64", r"C:\cygwin64"):
        cand = Path(root) / "usr" / "bin" / "bash.exe" if root else None
        if cand is not None and cand.is_file():
            return str(cand)
    found = shutil.which("bash")
    if found and "system32" not in found.lower():
        return found
    print("[ERROR] no msys/cygwin bash on PATH (System32 WSL stub does not "
          "count) -- pass --bash explicitly or install MSYS2",
          file=sys.stderr)
    sys.exit(1)


def require(prog: str, hint: str) -> str:
    path = shutil.which(prog)
    if not path:
        print(f"[ERROR] '{prog}' not on PATH -- {hint}", file=sys.stderr)
        sys.exit(1)
    return path


def run(cmd: list[str], cwd: Path, dry_run: bool, log_tail: Path | None = None) -> None:
    print(f"+ {' '.join(cmd)}  (in {cwd})")
    if dry_run:
        return
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        if log_tail is not None and log_tail.exists():
            print(f"--- last 40 lines of {log_tail} ---")
            lines = log_tail.read_text(errors="replace").splitlines()
            print("\n".join(lines[-40:]))
        print(f"[ERROR] failed ({r.returncode}): {cmd[0]}", file=sys.stderr)
        sys.exit(1)


def fetch_tarball(url: str, dest: Path, dry_run: bool) -> None:
    print(f"+ download {url} -> {dest}")
    if dry_run:
        return
    try:
        urllib.request.urlretrieve(url, dest)
    except Exception as e:
        print(f"[ERROR] download failed: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"  {dest.stat().st_size} bytes")


def main() -> None:
    ap = argparse.ArgumentParser(description="Build trimmed decode-only FFmpeg.")
    ap.add_argument("--prefix", type=Path,
                    help="install prefix (becomes KANARIV_TRIMMED_ROOT). Default: ./build/ffmpeg")
    ap.add_argument("--source-tarball", default=None,
                    help="FFmpeg source tarball URL (default: ffmpeg.org release for pinned version)")
    ap.add_argument("--source-dir", type=Path, default=None,
                    help="use local FFmpeg source dir instead of downloading")
    ap.add_argument("--ffmpeg-version", default=None,
                    help="override pinned version from FFmpegTrimmed.cmake")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                    help="make -j level (default: cpu count)")
    ap.add_argument("--extra-configure-args", default="",
                    help="space-separated extra args appended after the whitelist "
                         "(e.g. cross-compile --arch/--target-os/--cc)")
    ap.add_argument("--toolchain", default="",
                    help="FFmpeg --toolchain value (default: auto-detect; "
                         "use msvc with clang-cl on Windows)")
    ap.add_argument("--android-ndk", type=Path, default=None, metavar="PATH",
                    help="Android NDK root (ANDROID_NDK_ROOT is used when omitted)")
    ap.add_argument("--android-abi", default="arm64-v8a",
                    choices=sorted(ANDROID_ABIS),
                    help="Android ABI (default: arm64-v8a)")
    ap.add_argument("--android-api", type=int, default=24,
                    help="Android API level (default: 24)")
    ap.add_argument("--bash", default=None,
                    help="bash binary for running configure on Windows "
                         "(default: well-known MSYS2/Cygwin roots, then PATH "
                         "excluding the System32 WSL stub)")
    ap.add_argument("--pack", nargs="?", const="auto", default=None, metavar="OUT.tar.gz",
                    help="tar the prefix for upload (default name encodes host os/arch)")
    ap.add_argument("--print-flags", action="store_true",
                    help="print the extracted configure flags, build nothing")
    ap.add_argument("--dry-run", action="store_true",
                    help="print commands without running them")
    ap.add_argument("--force", action="store_true",
                    help="force rebuild even if prefix directory exists")
    args = ap.parse_args()

    if args.android_api < 21:
        ap.error("--android-api must be >= 21")
    android_ndk = args.android_ndk
    if android_ndk is None:
        ndk_env = (os.environ.get("ANDROID_NDK_ROOT")
                   or os.environ.get("ANDROID_NDK_HOME"))
        if ndk_env:
            android_ndk = Path(ndk_env)
    android = android_ndk is not None
    if android and args.toolchain:
        ap.error("--toolchain cannot be combined with Android options")
    if android:
        android_flags, android_cc = android_tools(
            android_ndk.resolve(), args.android_abi, args.android_api)
    else:
        android_flags, android_cc = [], ""

    text = cmake_module().read_text(encoding="utf-8")
    pinned_version = parse_trim_version(text)
    flags = read_trim_flags()
    version = args.ffmpeg_version or pinned_version

    if args.print_flags:
        print("\n".join(flags))
        return

    # Set default prefix
    if not args.prefix:
        suffix = f"-android-{args.android_abi}" if android else ""
        args.prefix = Path(f"./build/ffmpeg{suffix}").resolve()

    # Skip rebuild if directory exists and --force is not specified
    if args.prefix.exists() and not args.force:
        print(f"[INFO] prefix directory {args.prefix} already exists. Skipping rebuild.")
        print(f"[INFO] Use --force to rebuild even if the prefix directory exists.")
        return

    if not args.dry_run:
        # Dry runs print commands without touching the machine, so tool
        # presence is checked only for real builds.
        cc = android_cc or os.environ.get("CC") or "cc"
        if platform.system() == "Windows":
            bash = find_bash(args.bash)
            missing = ["make"] if not _bash_has(bash, "make") else []
            if android:
                if not Path(cc).is_file():
                    missing.append(cc)
            elif not _bash_has(bash, cc.split()[0]):
                missing.append(cc.split()[0])
            if missing:
                print(f"[ERROR] not found via {bash}: {', '.join(missing)} -- "
                      "Windows builds need MSYS2 tooling on PATH",
                      file=sys.stderr)
                sys.exit(1)
        else:
            if not shutil.which(cc.split()[0]):
                print(f"[ERROR] C compiler '{cc}' not on PATH (set CC to override)",
                      file=sys.stderr)
                sys.exit(1)
            require("make", "install build essentials (Linux) / Xcode CLT (macOS)")

    prefix = args.prefix.resolve()
    with tempfile.TemporaryDirectory(prefix="trimmed-ffmpeg-") as tmp:
        tmpdir = Path(tmp)
        if args.source_dir:
            src = args.source_dir.resolve()
            if not (src / "configure").exists():
                print(f"[ERROR] no configure in --source-dir {src}", file=sys.stderr)
                sys.exit(1)
        else:
            url = args.source_tarball or f"https://ffmpeg.org/releases/ffmpeg-{version}.tar.xz"
            archive = tmpdir / url.rsplit("/", 1)[-1]
            fetch_tarball(url, archive, args.dry_run)
            print(f"+ extract {archive.name}")
            if not args.dry_run:
                try:
                    with tarfile.open(archive) as tf:
                        tf.extractall(tmpdir)
                except Exception as e:
                    print(f"[ERROR] extract failed: {e}", file=sys.stderr)
                    sys.exit(1)
                tops = [p for p in tmpdir.iterdir() if p.is_dir()]
                if len(tops) != 1 or not (tops[0] / "configure").exists():
                    print("[ERROR] unexpected tarball layout", file=sys.stderr)
                    sys.exit(1)
                src = tops[0]
            else:
                src = tmpdir / f"ffmpeg-{version}"
        build = tmpdir / "build"
        if not args.dry_run:
            build.mkdir()
        # POSIX forms: on Windows these cross into bash (which would eat
        # backslashes), while Path-as-cwd stays native for subprocess.
        configure = [
            (src / "configure").as_posix(),
            f"--prefix={prefix.as_posix()}",
            *flags,
            *android_flags,
        ]
        if args.toolchain:
            configure.append(f"--toolchain={args.toolchain}")
        if args.extra_configure_args:
            configure.extend(shlex.split(args.extra_configure_args))
        cc_env = os.environ.get("CC") if not android else None
        if cc_env:
            # Explicit --cc: FFmpeg configure does not reliably honor a bare
            # $CC from the environment (observed: cross-target CC silently
            # ignored, host arch built). As a list element the value keeps
            # its spaces intact -- no shell splitting.
            configure.append(f"--cc={cc_env}")
        if platform.system() == "Windows":
            configure = [find_bash(args.bash), *configure]
        # ffbuild/config.log carries the real compiler error when configure
        # fails; the temp dir (and the log) is deleted afterwards, so print
        # the tail now or the evidence is gone.
        run(configure, build, args.dry_run, log_tail=build / "ffbuild" / "config.log")
        run(["make", f"-j{args.jobs}"], build, args.dry_run)
        run(["make", "install"], build, args.dry_run)

        if platform.system() == "Darwin" and not args.dry_run:
            # Make the trimmed FFmpeg archive relocatable: rewrite each dylib's
            # install_name from the absolute build-machine path to
            # @rpath/<soname> so consumers are not tied to wherever FFmpeg was
            # built (e.g. /Users/runner/work/caudio/caudio vs
            # /Users/runner/work/kanariv/kanariv). Re-sign afterwards because
            # install_name_tool invalidates code signatures on macOS.
            libdir = prefix / "lib"
            ffmpeg_prefixes = ("libavcodec", "libavformat", "libavutil",
                               "libswresample")
            for dylib in sorted(libdir.glob("*.dylib")):
                if dylib.is_symlink():
                    continue
                name = dylib.name
                # SONAME: libavcodec.63.1.102.dylib -> libavcodec.63.dylib
                #         libavcodec.62.dylib   -> libavcodec.62.dylib  (single component)
                m = re.match(r"^(.+)\.[0-9]+(\.[0-9]+)*\.dylib$", name)
                if m:
                    soname = f"{m.group(1)}.dylib"
                else:
                    soname = name
                if not soname.startswith(ffmpeg_prefixes):
                    continue
                subprocess.run(
                    ["install_name_tool", "-id", f"@rpath/{soname}", str(dylib)],
                    check=True,
                )
                refs = subprocess.run(
                    ["otool", "-L", str(dylib)],
                    check=True, capture_output=True, text=True,
                ).stdout
                for line in refs.splitlines()[1:]:
                    old = line.split()[0]
                    if old.startswith("/"):
                        target = Path(old).name
                        m2 = re.match(r"^(.+)\.[0-9]+(\.[0-9]+)*\.dylib$", target)
                        if m2:
                            tsoname = f"{m2.group(1)}.dylib"
                        else:
                            tsoname = target
                        if target.startswith(ffmpeg_prefixes):
                            subprocess.run(
                                ["install_name_tool", "-change", old,
                                 f"@rpath/{tsoname}", str(dylib)],
                                check=False,
                            )
                subprocess.run(
                    ["codesign", "--force", "-s", "-", str(dylib)],
                    check=False,
                )
        if platform.system() == "Windows" and not args.dry_run:
            # DLLs belong in bin/ next to the import libs in lib/ (gyan/choco
            # layout); the CMake bundling resolves them via <libdir>/../bin.
            bindir = prefix / "bin"
            bindir.mkdir(exist_ok=True)
            for dll in (prefix / "lib").glob("*.dll"):
                shutil.copy2(dll, bindir / dll.name)

    print(f"trimmed FFmpeg {version} installed to {prefix}")
    print(f"configure with: -DKANARIV_TRIMMED_ROOT={prefix}")
    if args.pack:
        out = (args.pack if args.pack != "auto" else
               str(prefix.parent / asset_name(version, args.android_abi if android else None)))
        print(f"+ pack {prefix} -> {out}")
        if not args.dry_run:
            # Contents at the tar root (same layout as `tar -C prefix .`),
            # so extracting with -C <root> yields <root>/lib etc. directly.
            with tarfile.open(out, "w:gz") as tf:
                for child in sorted(prefix.iterdir()):
                    tf.add(child, arcname=child.name)
        print(f"upload {out} as a deps-private release asset")


if __name__ == "__main__":
    main()
