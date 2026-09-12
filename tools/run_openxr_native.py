"""Run the standalone OpenXR diagnostic in a bounded, hidden child process.

The runtime override applies only to the child. No install or registry writes.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def seconds(value: str) -> int:
    if not value.isascii() or not value.isdigit() or not 1 <= int(value) <= 60:
        raise argparse.ArgumentTypeError("seconds must be an integer from 1 to 60")
    return int(value)


def existing_absolute(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"existing absolute file required: {value}")
    return path.resolve()


def active_manifest(override: Path | None) -> Path | None:
    if override:
        return override
    inherited = os.environ.get("XR_RUNTIME_JSON")
    if inherited:
        return existing_absolute(inherited)
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                               r"SOFTWARE\Khronos\OpenXR\1", 0,
                               winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                return existing_absolute(winreg.QueryValueEx(key, "ActiveRuntime")[0])
        except FileNotFoundError:
            pass
    return None


def run_child(command: list[str], output: Path, environment: dict[str, str],
              timeout: float, metadata: dict, *, dry_run: bool = False) -> int:
    if output.exists():
        raise ValueError(f"output already exists; choose a new directory: {output}")
    if dry_run:
        print(json.dumps({"command": command, "output": str(output),
                          "timeout_seconds": timeout, "writes": False}, indent=2))
        return 0
    output.mkdir(parents=True, exist_ok=False)
    receipt = dict(metadata, command=command, started_utc=datetime.now(timezone.utc).isoformat(),
                   timeout_seconds=timeout, timed_out=False, exit_code=None)
    start = time.monotonic()
    try:
        if os.name == "nt":
            ctypes.windll.kernel32.SetErrorMode(3)
        with (output / "output.log").open("wb") as log:
            flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
            with subprocess.Popen(command, cwd=ROOT, env=environment, stdout=log,
                                  stderr=subprocess.STDOUT, creationflags=flags) as child:
                try:
                    receipt["exit_code"] = child.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    receipt["timed_out"] = True
                    child.kill()  # Only this runner's child, never runtime services.
                    receipt["exit_code"] = child.wait(timeout=10)
        code = 124 if receipt["timed_out"] else (0 if receipt["exit_code"] == 0 else 1)
    except OSError as error:
        receipt["launch_error"] = str(error)
        code = 1
    finally:
        receipt["elapsed_seconds"] = round(time.monotonic() - start, 3)
        receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"[edvr] native diagnostic {'completed' if code == 0 else 'failed or incomplete'}: {output}")
    if code != 0:
        print(f"[edvr] inspect output.log and receipt.json (child exit {receipt['exit_code']})")
    return code


def self_test() -> int:
    checks = 0

    def check(condition: bool) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(f"runner check {checks}")

    with tempfile.TemporaryDirectory(prefix="edvr_native_runner_") as temp:
        root = Path(temp)
        env = os.environ.copy()
        env["XR_RUNTIME_JSON"] = "child-only-fixture"
        original = os.environ.get("XR_RUNTIME_JSON")
        command = [sys.executable, "-c", "import os; print(os.environ['XR_RUNTIME_JSON'])"]
        check(run_child(command, root / "dry", env, 10, {}, dry_run=True) == 0)
        check(not (root / "dry").exists())
        check(run_child(command, root / "pass", env, 10, {}) == 0)
        check("child-only-fixture" in (root / "pass" / "output.log").read_text())
        check(os.environ.get("XR_RUNTIME_JSON") == original)
        check(json.loads((root / "pass" / "receipt.json").read_text())["exit_code"] == 0)
        try:
            run_child(command, root / "pass", env, 10, {})
            check(False)
        except ValueError:
            check(True)
        check(run_child([sys.executable, "-c", "raise SystemExit(7)"], root / "fail", env, 10, {}) == 1)
        check(json.loads((root / "fail" / "receipt.json").read_text())["exit_code"] == 7)
        check(run_child([sys.executable, "-c", "import time; time.sleep(5)"], root / "timeout", env, .1, {}) == 124)
        check(json.loads((root / "timeout" / "receipt.json").read_text())["timed_out"])
        check(run_child([str(root / "missing.exe")], root / "missing", env, 10, {}) == 1)
        check("launch_error" in json.loads((root / "missing" / "receipt.json").read_text()))
        for invalid in ("0", "61", "-1", "1x", "1.5", ""):
            try:
                seconds(invalid)
                check(False)
            except argparse.ArgumentTypeError:
                check(True)
        check(seconds("1") == 1 and seconds("60") == 60)
    print(f"run_openxr_native: {checks} checks passed (no OpenXR runtime)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", help="absolute installed OpenXR loader DLL path")
    parser.add_argument("--runtime", help="absolute runtime JSON; child environment only")
    parser.add_argument("--seconds", type=seconds, default=10)
    parser.add_argument("--output", type=Path, help="new output directory (never overwrites a capture)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    actual = sys.argv[1:] if argv is None else argv
    if args.self_test:
        if actual != ["--self-test"]:
            parser.error("--self-test must be used alone")
        return self_test()
    if not args.loader:
        parser.error("--loader is required")
    try:
        loader = existing_absolute(args.loader)
        runtime = existing_absolute(args.runtime) if args.runtime else None
        manifest = active_manifest(runtime)
        executable = existing_absolute(str(ROOT / "build" / "openxr_native_test.exe"))
        output = (args.output or ROOT / "build" / ("openxr-native-" + datetime.now().strftime("%Y%m%d-%H%M%S"))).resolve()
        environment = os.environ.copy()
        if runtime:
            environment["XR_RUNTIME_JSON"] = str(runtime)
        metadata = {"executable": str(executable), "executable_sha256": digest(executable),
                    "loader": str(loader), "loader_sha256": digest(loader),
                    "runtime_manifest": str(manifest) if manifest else None,
                    "runtime_manifest_sha256": digest(manifest) if manifest else None,
                    "explicit_runtime_override": bool(runtime), "seconds": args.seconds}
        command = [str(executable), "--loader", str(loader), "--seconds", str(args.seconds)]
        return run_child(command, output, environment, args.seconds + 40, metadata, dry_run=args.dry_run)
    except (OSError, ValueError) as error:
        print(f"[edvr] {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
