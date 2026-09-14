"""Exercise the actual graphics DLL with optional vScreen hooks disabled.

Each case runs the WARP Present fixture in an isolated temporary directory.
No OpenXR loader, headset runtime, game install or registry is touched.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
CASES = (("shared", "off"), ("private", "off"), ("live", "off"),
         ("live", "swap"), ("live", "live"))


def configuration(mode: str, probe: str, directory: Path) -> str:
    # transition_flash=0 still observes camera history. Zero buffer bytes
    # explicitly disables that observer in this fixture as well.
    return f"""[fix]
black_void = 0
panel_distance = 1
transition_flash = 0
head_offset_gate = 0
temporal_aa = off
share_exposure = 0
[advanced]
app_gpu_timing = 0
panel_hooks_always = 0
camera_buffer_bytes = 0
context_hook_mode = {mode}
context_hook_probe = {probe}
texture_lod_bias = 0
[log]
dir = {directory / 'logs'}
enabled = 0
"""


def run_matrix(executable: Path, proxy: Path, *, dry_run: bool = False) -> int:
    for path in (executable, proxy):
        if not path.is_absolute() or not path.is_file():
            raise ValueError(f"existing absolute file required: {path}")
    if dry_run:
        print(f"Would run {len(CASES)} isolated WARP cases using {executable} and {proxy}; no writes or child processes.")
        return 0
    if os.name == "nt":
        ctypes.windll.kernel32.SetErrorMode(3)
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    for mode, probe in CASES:
        with tempfile.TemporaryDirectory(prefix="edvr-transport-") as temporary:
            directory = Path(temporary).resolve()
            staged = directory / "d3d11.dll"
            shutil.copyfile(proxy, staged)
            (directory / "edvr.ini").write_text(configuration(mode, probe, directory), encoding="utf-8")
            expectation = "--transport-only" if probe == "off" else "--unavailable"
            command = [str(executable), "--graphics-proxy", str(staged), expectation]
            print(f"[edvr] transport fixture: mode={mode}, probe={probe}", flush=True)
            try:
                completed = subprocess.run(command, cwd=directory, creationflags=flags,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           timeout=45, check=False)
            except subprocess.TimeoutExpired:
                print("[edvr] transport fixture timed out; its child was terminated")
                return 1
            output = completed.stdout.decode("utf-8", errors="replace")
            print(output, end="" if output.endswith("\n") else "\n", flush=True)
            if completed.returncode != 0:
                print(f"[edvr] transport fixture failed with exit {completed.returncode}")
                return 1
            if probe == "off" and "transport_only: PASS" not in output:
                print("[edvr] transport fixture did not confirm the minimal hook route")
                return 1
    print(f"[edvr] transport matrix: {len(CASES)} cases passed (no OpenXR runtime)")
    return 0


def self_test() -> int:
    class Contracts(unittest.TestCase):
        def test_dry_run_has_no_side_effects(self):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                exe, dll = directory / "test.exe", directory / "d3d11.dll"
                exe.write_bytes(b"exe"); dll.write_bytes(b"dll")
                before = {p.name: p.read_bytes() for p in directory.iterdir()}
                with mock.patch.object(tempfile, "TemporaryDirectory", side_effect=AssertionError("created directory")), \
                     mock.patch.object(shutil, "copyfile", side_effect=AssertionError("copied file")), \
                     mock.patch.object(subprocess, "run", side_effect=AssertionError("started child")), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(run_matrix(exe, dll, dry_run=True), 0)
                self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir()})

        def test_child_failure_stops_and_cleans_staging(self):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                exe, dll = directory / "test.exe", directory / "d3d11.dll"
                exe.write_bytes(b"exe"); dll.write_bytes(b"dll")
                staged = []
                def fail(command, **kwargs):
                    target = Path(command[2]); staged.append(target.parent)
                    self.assertEqual(target.read_bytes(), b"dll")
                    self.assertTrue((target.parent / "edvr.ini").is_file())
                    self.assertEqual(command[-1], "--transport-only")
                    return subprocess.CompletedProcess(command, 7, b"failed\n")
                with mock.patch.object(subprocess, "run", side_effect=fail) as child, \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(run_matrix(exe, dll), 1)
                    self.assertEqual(child.call_count, 1)
                self.assertTrue(staged and all(not p.exists() for p in staged))
                self.assertEqual(dll.read_bytes(), b"dll")

        def test_timeout_stops_and_cleans_staging(self):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                exe, dll = directory / "test.exe", directory / "d3d11.dll"
                exe.write_bytes(b"exe"); dll.write_bytes(b"dll")
                staged = []
                def timeout(command, **kwargs):
                    staged.append(Path(command[2]).parent)
                    raise subprocess.TimeoutExpired(command, 45)
                with mock.patch.object(subprocess, "run", side_effect=timeout), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(run_matrix(exe, dll), 1)
                self.assertTrue(staged and all(not p.exists() for p in staged))

    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Contracts))
    return 0 if result.wasSuccessful() else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--executable", type=Path, default=ROOT / "build/openxr_present_test.exe")
    parser.add_argument("--graphics-proxy", type=Path, default=ROOT / "build/d3d11.dll")
    args = parser.parse_args()
    if args.self_test:
        if args.dry_run:
            parser.error("--self-test cannot be combined with --dry-run")
        return self_test()
    try:
        return run_matrix(args.executable, args.graphics_proxy, dry_run=args.dry_run)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
