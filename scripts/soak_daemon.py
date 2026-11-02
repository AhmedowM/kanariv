#!/usr/bin/env python3
"""Daemon loop soak gate (release-gate procedure, NOT a unit test).

Reproduces the "daemon exit under 1s-loop load" incident class: short
fixtures looping on repeat-all while transport commands hammer the daemon.
Runs the daemon with `--daemon --foreground` as a supervised child, so a
death yields the process exit code plus a stderr tail with zero code changes.

Why a script and not a ctest case: the gate needs real audio output (CI
sets KANARIV_TEST_NOAUDIO=1, so it could never reproduce there), it runs
several minutes (unacceptable in the default suite), and only a supervisor
process observes daemon death directly.

Usage:
    py -3 scripts/soak_daemon.py --cli build/dev/kanariv.exe
    py -3 scripts/soak_daemon.py --cli ... --iterations 50   # quick smoke
    py -3 scripts/soak_daemon.py --cli ... --no-audio        # degraded churn
                                                            # (no playback
                                                            # steps; CI-safe)

Exit codes: 0 pass, 1 soak failure (daemon died/hung or a step deviated),
2 environment/setup failure (bad CLI, daemon never came up, no audio for
the audio deck). The temp dir is kept on failure (path printed) and
removed on success unless --keep-dir.
"""

import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Deck: steady-state commands, each expected to exit 0. `next` on repeat-all
# wraps forever, so the engine never idles at natural end mid-run.
AUDIO_DECK = [
    ["next"],
    ["status"],
    ["seek", "0"],
    ["status"],
    ["pause"],
    ["resume"],
    ["status"],
]

# Degraded deck: read-only churn, no playback state machine involved.
NOAUDIO_DECK = [
    ["status"],
    ["queue", "list"],
    ["library", "list"],
]


def write_sine_wav(path, seconds, freq=440.0, rate=44100):
    frames = int(seconds * rate)
    data = bytearray()
    for i in range(frames):
        v = int(32767 * 0.5 * math.sin(2.0 * math.pi * freq * i / rate))
        data += struct.pack("<h", v)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


class Soak:
    def __init__(self, args):
        self.args = args
        self.cli = str(Path(args.cli).resolve())
        self.tmp = Path(tempfile.mkdtemp(prefix="kanariv-soak-"))
        self.cfg = self.tmp / "config.json"
        self.db = self.tmp / "library.db"
        self.log = self.tmp / "daemon-stderr.log"
        self.daemon = None
        self.failures = []

    def run_cli(self, argv, expect=0):
        """Run one CLI command; return (ok, combined_output)."""
        cmd = [self.cli, "--config", str(self.cfg)] + argv
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=self.args.cmd_timeout)
        except subprocess.TimeoutExpired:
            return False, "TIMEOUT after %ss: %s" % (self.args.cmd_timeout, " ".join(argv))
        out = (p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace"))
        if p.returncode != expect:
            return False, "exit=%d (want %d) [%s]: %s" % (
                p.returncode, expect, " ".join(argv), out.strip()[-500:])
        return True, out

    def daemon_alive(self):
        return self.daemon is not None and self.daemon.poll() is None

    def fail(self, where, detail):
        alive = self.daemon_alive()
        code = self.daemon.poll() if self.daemon else "n/a"
        tail = ""
        try:
            tail = self.log.read_text(errors="replace")[-2000:]
        except OSError:
            pass
        self.failures.append("FAIL at %s (daemon alive=%s exit=%s): %s\n-- daemon log tail --\n%s"
                             % (where, alive, code, detail, tail))

    def setup(self):
        if not Path(self.cli).is_file():
            print("no such CLI binary: %s" % self.cli)
            return False
        self.cfg.write_text('{"dbPath": "%s"}' % self.db.as_posix())
        for name, secs in (("loop-a.wav", 0.3), ("loop-b.wav", 0.5), ("loop-c.wav", 1.0)):
            write_sine_wav(str(self.tmp / name), secs)
        logf = open(self.log, "wb")
        self.daemon = subprocess.Popen(
            [self.cli, "--config", str(self.cfg), "--daemon", "--foreground"],
            stdout=logf, stderr=subprocess.STDOUT)
        # Readiness poll (mirrors the golden harness: 150 x 100 ms, bounded).
        deadline = time.time() + self.args.ready_timeout
        while time.time() < deadline:
            if not self.daemon_alive():
                print("daemon exited during startup, code=%s" % self.daemon.poll())
                return False
            ok, _ = self.run_cli(["status"])
            if ok:
                break
            time.sleep(0.1)
        else:
            print("daemon never came up")
            return False
        if self.args.no_audio:
            return True
        for wav in ("loop-a.wav", "loop-b.wav", "loop-c.wav"):
            ok, out = self.run_cli(["library", "add", str(self.tmp / wav)])
            if not ok:
                print("setup library add failed: %s" % out)
                return False
            ok, out = self.run_cli(["queue", "add", str(self.tmp / wav)])
            if not ok:
                print("setup queue add failed: %s" % out)
                return False
        ok, out = self.run_cli(["queue", "repeat", "all"])
        if not ok:
            print("setup repeat all failed: %s" % out)
            return False
        ok, out = self.run_cli(["play"])
        if not ok:
            print("setup play failed (no audio device?): %s" % out)
            return False
        return True

    def loop(self):
        deck = NOAUDIO_DECK if self.args.no_audio else AUDIO_DECK
        n = self.args.iterations
        for i in range(1, n + 1):
            for argv in deck:
                ok, out = self.run_cli(argv)
                if not ok:
                    self.fail("iter %d/%d cmd [%s]" % (i, n, " ".join(argv)), out)
                    return False
                if "daemon not running" in out:
                    self.fail("iter %d/%d cmd [%s]" % (i, n, " ".join(argv)),
                              "daemon unreachable: %s" % out.strip()[-300:])
                    return False
            if not self.daemon_alive():
                self.fail("iter %d/%d" % (i, n), "daemon died mid-iteration")
                return False
            if i % 25 == 0 or i == n:
                print("iter %d/%d ok" % (i, n), flush=True)
        return True

    def teardown(self):
        ok, out = self.run_cli(["shutdown"])
        if not ok:
            self.fail("shutdown", out)
            return False
        try:
            self.daemon.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.fail("shutdown", "daemon did not exit within 15 s; killing")
            self.daemon.kill()
            return False
        # Prove the socket is really down, then prove a clean restart works.
        ok, _ = self.run_cli(["status"], expect=1)
        ok2, out2 = self.run_cli(["start"])
        if not ok2:
            self.fail("restart", out2)
            return False
        ok3, out3 = self.run_cli(["status"])
        if not ok3:
            self.fail("post-restart status", out3)
            return False
        self.run_cli(["shutdown"])
        try:
            self.daemon.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.daemon.kill()
        return True

    def run(self):
        if not self.setup():
            self.cleanup(keep=True)
            return 2
        ok = self.loop()
        if ok:
            ok = self.teardown()
        if ok:
            print("SOAK PASSED (%d iters, %s deck, dir %s)"
                  % (self.args.iterations,
                     "no-audio" if self.args.no_audio else "audio", self.tmp))
        else:
            print("SOAK FAILED:\n%s" % "\n".join(self.failures))
            print("temp dir kept: %s" % self.tmp)
        self.cleanup(keep=not ok or self.args.keep_dir)
        return 0 if ok else 1

    def cleanup(self, keep):
        if self.daemon_alive():
            self.daemon.kill()
        if not keep:
            shutil.rmtree(self.tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="kanariv daemon loop soak gate")
    ap.add_argument("--cli", required=True, help="path to the kanariv CLI binary")
    ap.add_argument("--iterations", type=int, default=300)
    ap.add_argument("--cmd-timeout", type=float, default=15.0)
    ap.add_argument("--ready-timeout", type=float, default=20.0)
    ap.add_argument("--no-audio", action="store_true",
                    help="degraded churn deck (no playback steps)")
    ap.add_argument("--keep-dir", action="store_true")
    args = ap.parse_args()
    if args.iterations < 1:
        print("--iterations must be >= 1")
        return 2
    return Soak(args).run()


if __name__ == "__main__":
    sys.exit(main())
