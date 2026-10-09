"""Exercise the actual graphics DLL with optional vScreen hooks disabled.

Each case runs the WARP Present fixture in an isolated temporary directory.
No OpenXR loader, headset runtime, game install or registry is touched.

The Explorer Cam cases run the same minimal configuration -- vScreen installs
transport-only, so it has no frame boundary -- and ask the DLL's own log for the
lines Explorer Cam's frame tick writes. That tick used to ride vScreen's boundary
and never ran here (review 2026-10-08, finding 3); one case starts with
hotkey.explorer_cam EMPTY and binds it while the game runs.
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
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
CASES = (("shared", "off"), ("private", "off"), ("live", "off"),
         ("live", "swap"), ("live", "live"))


def configuration(mode: str, probe: str, directory: Path, *, log: bool = False, hotkey: str | None = None) -> str:
    # transition_flash=0 still observes camera history. Zero buffer bytes
    # explicitly disables that observer in this fixture as well.
    # `log` turns the DLL's log on (the Explorer Cam cases read it); `hotkey` adds
    # a [hotkey] explorer_cam line ("" is the empty value, None leaves it unsaid).
    text = f"""[fix]
black_void = 0
panel_distance = 1
transition_flash = 0
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
enabled = {1 if log else 0}
"""
    if hotkey is not None:
        text += f"[hotkey]\nexplorer_cam = {hotkey}\n"
    return text


# Explorer Cam, through the real DLL in the minimal configuration. Each case: the
# hotkey line as written first, the line it is rewritten to while the game runs (None:
# no rewrite), and the pieces of the DLL's log that prove the frame tick ran -- in order.
EXPLORER_CASES = (
    ("default key", None, None,
     ("explorer cam: hotkey bound: F5 (vk 0x74", "explorer cam: Explorer Cam armed: F5 enters it")),
    ("empty, then bound live", "", "F5",
     ("explorer cam: hotkey.explorer_cam is empty: Explorer Cam is off",
      "explorer cam: hotkey bound: F5 (vk 0x74",
      "explorer cam: Explorer Cam armed again (hotkey.explorer_cam = F5)")),
)


def explorer_log_problem(text: str, expected: tuple[str, ...]) -> str | None:
    """None when every expected piece is in the log, in order; else what is missing."""
    at = 0
    for piece in expected:
        found = text.find(piece, at)
        if found < 0:
            return f"missing or out of order: {piece!r}"
        at = found + len(piece)
    return None


def run_explorer(executable: Path, proxy: Path) -> int:
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    for name, first, then, expected in EXPLORER_CASES:
        with tempfile.TemporaryDirectory(prefix="edvr-explorer-") as temporary:
            directory = Path(temporary).resolve()
            staged = directory / "d3d11.dll"
            shutil.copyfile(proxy, staged)
            (directory / "edvr.ini").write_text(configuration("shared", "off", directory, log=True, hotkey=first), encoding="utf-8")
            command = [str(executable), "--graphics-proxy", str(staged), "--explorer-live"]
            if then is not None:
                replacement = directory / "edvr-rebind.ini"
                replacement.write_text(configuration("shared", "off", directory, log=True, hotkey=then), encoding="utf-8")
                command.append(str(replacement))
            print(f"[edvr] explorer cam fixture: {name}", flush=True)
            try:
                completed = subprocess.run(command, cwd=directory, creationflags=flags,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           timeout=45, check=False)
            except subprocess.TimeoutExpired:
                print("[edvr] explorer cam fixture timed out; its child was terminated")
                return 1
            output = completed.stdout.decode("utf-8", errors="replace")
            print(output, end="" if output.endswith("\n") else "\n", flush=True)
            if completed.returncode != 0 or "explorer_live: PASS" not in output:
                print(f"[edvr] explorer cam fixture failed (exit {completed.returncode})")
                return 1
            logs = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in sorted((directory / "logs").glob("*.log")))
            problem = explorer_log_problem(logs, expected)
            if problem:
                print(f"[edvr] explorer cam fixture '{name}': the DLL's log does not show Explorer Cam's frame tick running ({problem})")
                print("[edvr] (vScreen is transport-only in this configuration: the tick must be device_hook.cpp's own; "
                      f"the log read was {len(logs)} bytes, {len(logs.splitlines())} lines)")
                print("\n".join(line for line in logs.splitlines() if "explorer cam" in line.lower())[:2000])
                return 1
    print(f"[edvr] explorer cam: {len(EXPLORER_CASES)} cases showed the frame tick running with vScreen transport-only")
    return 0


def run_matrix(executable: Path, proxy: Path, *, dry_run: bool = False) -> int:
    for path in (executable, proxy):
        if not path.is_absolute() or not path.is_file():
            raise ValueError(f"existing absolute file required: {path}")
    if dry_run:
        print(f"Would run {len(CASES)} isolated WARP cases and {len(EXPLORER_CASES)} Explorer Cam cases using {executable} and {proxy}; no writes or child processes.")
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
    return run_explorer(executable, proxy)


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

        def test_explorer_log_judge(self):
            good = ("explorer cam: hotkey.explorer_cam is empty: Explorer Cam is off: nothing is installed\n"
                    "explorer cam: hotkey bound: F5 (vk 0x74, mods 0x0); watched\n"
                    "explorer cam: Explorer Cam armed again (hotkey.explorer_cam = F5): fallback eye\n")
            expected = EXPLORER_CASES[1][3]
            self.assertIsNone(explorer_log_problem(good, expected))
            self.assertIn("missing", explorer_log_problem("", expected))
            self.assertIn("missing", explorer_log_problem(good.replace("armed again", "armed"), expected))
            self.assertIn("missing", explorer_log_problem(good.replace("vk 0x74", "vk 0x75"), expected))
            lines = good.splitlines()
            self.assertIn("out of order", explorer_log_problem("\n".join(reversed(lines)), expected))

        def test_explorer_cases_ask_for_the_live_rewrite(self):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                exe, dll = directory / "test.exe", directory / "d3d11.dll"
                exe.write_bytes(b"exe"); dll.write_bytes(b"dll")
                seen = []
                def child(command, **kwargs):
                    cwd = Path(kwargs["cwd"])
                    seen.append((command[3:], (cwd / "edvr.ini").read_text(encoding="utf-8"),
                                 (cwd / "edvr-rebind.ini").read_text(encoding="utf-8") if (cwd / "edvr-rebind.ini").is_file() else None))
                    return subprocess.CompletedProcess(command, 0, b"explorer_live: PASS\n")
                with mock.patch.object(subprocess, "run", side_effect=child), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(run_explorer(exe, dll), 1)   # the log is empty: judged a failure, not a pass
                self.assertEqual(seen[0][0], ["--explorer-live"])
                self.assertNotIn("explorer_cam", seen[0][1])
                self.assertIsNone(seen[0][2])
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary).resolve()
                exe, dll = directory / "test.exe", directory / "d3d11.dll"
                exe.write_bytes(b"exe"); dll.write_bytes(b"dll")
                seen.clear()
                original = EXPLORER_CASES
                with mock.patch.object(sys.modules[__name__], "EXPLORER_CASES", original[1:]), \
                     mock.patch.object(subprocess, "run", side_effect=child), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(run_explorer(exe, dll), 1)
                command, first, rebind = seen[0]
                self.assertEqual(command[0], "--explorer-live")
                self.assertEqual(len(command), 2)
                self.assertRegex(first, r"(?m)^explorer_cam = $")
                self.assertRegex(rebind, r"(?m)^explorer_cam = F5$")
                self.assertRegex(first, r"(?m)^enabled = 1$")

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
