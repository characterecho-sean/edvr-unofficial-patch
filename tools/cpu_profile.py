#!/usr/bin/env python3
"""Bounded WPR CPU capture for an installed EDVR game; never edits the game.

Run --self-test without elevation. Capture requires an administrator token.
--dry-run validates and prints the plan without creating files or sessions.
The private WPR instance is stopped on game exit, timeout or handled failure.
Use --smoke-first to validate the real provider/decoder before waiting for Elite.
"""
import sys
sys.dont_write_bytecode = True
import argparse
import csv
import ctypes
import hashlib
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
PROFILE = ROOT / "tools" / "cpu_profile" / "edvr_cpu.wprp"
ANALYZER = ROOT / "build" / "cpu_profile" / "EdvrCpuProfile.dll"
SMOKE = ROOT / "build" / "openxr_trace_test.exe"
GAME = "EliteDangerous64.exe"


def source_hashes():
    names = ("tools/cpu_profile/Program.cs", "tools/cpu_profile/EdvrCpuProfile.csproj",
             "tools/cpu_profile/Directory.Build.props",
             "tools/cpu_profile/edvr_cpu.wprp", "src/common/native_cpu_trace_events.h")
    # Normalize source line endings so git's Windows checkout conversion is inert.
    return {name: hashlib.sha256((ROOT / name).read_text(encoding="utf-8").encode("utf-8")).hexdigest()
            for name in names}


def validate_analyzer():
    stamp = ANALYZER.parent / "validated-build.json"
    if not stamp.is_file():
        raise ValueError("Build and test the analyzer first: tools/cpu_profile.py --build-analyzer")
    data = json.loads(stamp.read_text(encoding="utf-8"))
    if data.get("sources") != source_hashes() or data.get("analyzer_sha256") != hashlib.sha256(ANALYZER.read_bytes()).hexdigest():
        raise ValueError("CPU analyzer or schema changed; rerun --build-analyzer before capture")


def build_analyzer(args):
    dotnet = shutil.which("dotnet")
    if not dotnet:
        raise ValueError("The optional profiling analyzer requires the .NET 8 SDK")
    selected = args.trace_event_directory or os.environ.get("EDVR_TRACE_EVENT_DIR")
    if selected:
        candidates = [Path(selected)]
    else:
        base = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft Visual Studio"
        candidates = sorted(base.glob("*/[!I]*/Common7/IDE/PrivateAssemblies"), reverse=True)
    required = ("Microsoft.Diagnostics.Tracing.TraceEvent.dll", "Microsoft.Diagnostics.FastSerialization.dll")
    dependency = next((path.resolve() for path in candidates if all((path / name).is_file() for name in required)), None)
    if dependency is None:
        raise ValueError("TraceEvent assemblies were not found; supply --trace-event-directory. No download is performed.")
    output = ANALYZER.parent
    intermediate = str(output / "obj") + os.sep
    command = [dotnet, "build", str(ROOT / "tools/cpu_profile/EdvrCpuProfile.csproj"),
               "-c", "Release", "--nologo", "--output", str(output),
               "-p:TraceEventDirectory=" + str(dependency),
               "-p:BaseIntermediateOutputPath=" + intermediate,
               "-p:MSBuildProjectExtensionsPath=" + intermediate]
    if args.dry_run:
        print(json.dumps(dict(command=command, self_test=[dotnet, str(ANALYZER), "--self-test"]), indent=2))
        print("[edvr] dry run: wrote nothing; no compiler or analyzer was started.")
        return 0
    print(invoke(command, timeout=180), flush=True)
    print(invoke([dotnet, str(ANALYZER), "--self-test"]), flush=True)
    stamp = dict(sources=source_hashes(), trace_event_directory=str(dependency),
                 analyzer_sha256=hashlib.sha256(ANALYZER.read_bytes()).hexdigest())
    (output / "validated-build.json").write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    return 0


def validate_smoke_report(report):
    """A successful decoder must distinguish our actual busy and Sleep phases."""
    if not report.get("coverageComplete") or report.get("analyzedFrameCount") != 60:
        raise ValueError("Synthetic CPU trace has incomplete coverage or missing frame markers")
    frames = report.get("frames", [])
    if sorted(frame["sequence"] for frame in frames) != list(range(1, 61)):
        raise ValueError("Synthetic CPU trace did not preserve the complete frame sequence")
    busy = [frame for frame in frames if frame["sequence"] <= 30]
    sleeping = [frame for frame in frames if frame["sequence"] > 30]
    busy_running = sum(frame["runningUs"] for frame in busy)
    sleep_waiting = sum(frame["waitingUs"] for frame in sleeping)
    busy_wall = sum(frame["endUs"] - frame["startUs"] for frame in busy)
    sleep_wall = sum(frame["endUs"] - frame["startUs"] for frame in sleeping)
    if busy_running < busy_wall * 0.5 or sleep_waiting < sleep_wall * 0.5:
        raise ValueError("Synthetic CPU decoder did not distinguish known busy execution from Sleep")
    if sum(frame.get("sampleStackCount", 0) for frame in busy) == 0:
        raise ValueError("Synthetic CPU trace has no busy-phase sampled stacks")
    return dict(busy_running_ms=round(busy_running / 1000, 2), sleep_waiting_ms=round(sleep_waiting / 1000, 2))


def invoke(command, timeout=120):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, errors="replace", timeout=timeout,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise RuntimeError("Command failed (%s): %s\n%s\n%s" %
                           (result.returncode, subprocess.list2cmdline(command),
                            result.stdout[-6000:], result.stderr[-2000:]))
    return result.stdout


def write_status(directory, state, **values):
    path = directory / "status.json"
    record = {}
    if path.exists():
        record = json.loads(path.read_text(encoding="utf-8"))
    record.update(values)
    record.update(state=state, updated_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    temporary = directory / "status.pending"
    temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
    print("[edvr] cpu profile: " + state, flush=True)


def session_command(wpr, operation, *arguments, instance):
    # Instance must be last; never operate on WPR's default/another task's session.
    return [str(wpr), operation, *map(str, arguments), "-instancename", instance]


def record(wpr, profile, output, instance, workload, runner=invoke):
    start = session_command(wpr, "-start", str(profile) + "!EDVRCPU", instance=instance)
    stop = session_command(wpr, "-stop", output, instance=instance)
    cancel = session_command(wpr, "-cancel", instance=instance)
    # A failed start can leave partially-created sessions. The randomized
    # instance belongs only to this invocation, including in the error path.
    try:
        runner(start)
    except BaseException:
        try:
            runner(cancel)
        except Exception:
            pass
        raise
    failure = None
    value = None
    try:
        value = workload()
    except BaseException as exc:
        failure = exc
    try:
        runner(stop, timeout=180)
    except BaseException:
        try:
            runner(cancel)
        except Exception:
            pass
        raise
    if failure:
        raise failure
    return value


class WindowsProcess:
    """Hold a process handle to avoid PID reuse while waiting for its exit."""
    def __init__(self, pid):
        from ctypes import wintypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                          wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        self.kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        self.kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.kernel.WaitForSingleObject.restype = wintypes.DWORD
        self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
                                                ctypes.POINTER(wintypes.FILETIME),
                                                ctypes.POINTER(wintypes.FILETIME),
                                                ctypes.POINTER(wintypes.FILETIME)]
        self.kernel.GetProcessTimes.restype = wintypes.BOOL
        self.pid = pid
        self.handle = self.kernel.OpenProcess(0x100000 | 0x1000, False, pid)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "OpenProcess failed for %s" % pid)

    def path(self):
        from ctypes import wintypes
        buffer = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(buffer))
        if not self.kernel.QueryFullProcessImageNameW(self.handle, 0, buffer, ctypes.byref(length)):
            raise OSError(ctypes.get_last_error(), "QueryFullProcessImageName failed")
        return Path(buffer.value).resolve()

    def exited(self):
        result = self.kernel.WaitForSingleObject(self.handle, 0)
        if result not in (0, 258):
            raise OSError(ctypes.get_last_error(), "WaitForSingleObject failed")
        return result == 0

    def creation_time(self):
        """Process creation time as seconds since the Unix epoch, in UTC."""
        from ctypes import wintypes
        created = wintypes.FILETIME()
        exited = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not self.kernel.GetProcessTimes(self.handle, ctypes.byref(created), ctypes.byref(exited),
                                           ctypes.byref(kernel), ctypes.byref(user)):
            raise OSError(ctypes.get_last_error(), "GetProcessTimes failed")
        ticks = (created.dwHighDateTime << 32) | created.dwLowDateTime
        return ticks / 10000000.0 - 11644473600.0

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def candidate_pids(text):
    result = []
    for row in csv.reader(io.StringIO(text)):
        if len(row) >= 2 and row[0].lower() == GAME.lower():
            if not row[1].isdigit():
                raise ValueError("Invalid PID in tasklist output")
            result.append(int(row[1]))
    return result


def find_game(executable):
    listing = invoke(["tasklist", "/FI", "IMAGENAME eq " + GAME, "/FO", "CSV", "/NH"], timeout=15)
    found = None
    try:
        for pid in candidate_pids(listing):
            try:
                process = WindowsProcess(pid)
            except OSError:
                continue  # A process may disappear between inventory and open.
            try:
                try:
                    candidate = process.path()
                except OSError:
                    continue  # It may exit after OpenProcess but before the path query.
                if os.path.normcase(str(candidate)) == os.path.normcase(str(executable)):
                    if process.exited():
                        continue
                    if found:
                        if not found.exited():
                            raise RuntimeError("More than one matching Elite process; capture refused")
                        found.close()
                    found, process = process, None
            finally:
                if process:
                    process.close()
        if found and found.exited():
            found.close()
            return None
        return found
    except BaseException:
        if found:
            found.close()
        raise


def wait_for_game(executable, timeout, finder=find_game, clock=time.monotonic, sleep=time.sleep):
    end = clock() + timeout
    while True:
        process = finder(executable)
        if process:
            return process
        if clock() >= end:
            raise TimeoutError("No matching Elite process appeared within the waiting limit")
        sleep(min(1.0, max(0.0, end - clock())))


def wait_for_exit(process, timeout, clock=time.monotonic, sleep=time.sleep):
    end = clock() + timeout
    while not process.exited():
        if clock() >= end:
            return "capture_limit"
        sleep(min(0.5, max(0.0, end - clock())))
    return "game_exit"


def native_log_time(line):
    try:
        value = datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S.%f")
    except (TypeError, ValueError):
        return None
    return value.replace(tzinfo=timezone.utc).timestamp()


def flight_version(target, pid, expected, process_created):
    # Keep all flight-log discovery/reading in the sanctioned log module.
    import edvr_log
    for directory in edvr_log.log_dirs_for(str(target), "openxr"):
        for _, _, path in edvr_log.find_logs(directory, "openxr"):
            if Path(path).name.endswith("_%d.log" % pid):
                line, actual, _ = edvr_log.version_line(edvr_log.read_text(path))
                logged = native_log_time(line)
                # Native timestamps truncate to milliseconds; allow that precision
                # only, so a recent old log cannot pass after PID reuse.
                if logged is None or logged < process_created - 0.001:
                    continue
                if not edvr_log.version_matches(actual, expected):
                    raise ValueError("Captured flight build %r does not match %r" % (actual, expected))
                return dict(log=path, version=actual)
    raise FileNotFoundError("No fresh native flight log found for captured PID %d" % pid)


def wait_for_flight_version(target, process, expected, timeout=30, finder=flight_version,
                            clock=time.monotonic, sleep=time.sleep):
    created = process.creation_time()
    end = clock() + timeout
    while True:
        try:
            verified = finder(target, process.pid, expected, created)
        except FileNotFoundError:
            if process.exited():
                raise RuntimeError("Elite exited before its native EDVR log appeared")
            if clock() >= end:
                raise TimeoutError("No fresh native EDVR log appeared within %d seconds" % timeout)
            sleep(min(0.25, max(0.0, end - clock())))
            continue
        if process.exited():
            raise RuntimeError("Elite exited during native EDVR build verification")
        return verified


def analyze(dotnet, analyzer, trace, pid, output):
    if output.exists():
        raise ValueError("Analysis output already exists: " + str(output))
    return invoke([str(dotnet), str(analyzer), "--input", str(trace), "--pid", str(pid),
                   "--output", str(output)], timeout=300)


def plan(args):
    import install_edvr
    import edvr_log
    target = Path(install_edvr.resolve_target(args.target)).resolve()
    executable = target / GAME
    if not executable.is_file():
        raise ValueError("Game executable is missing: " + str(executable))
    receipt = install_edvr.verify_native_receipt(str(target / "edvr_native_receipt.json"), str(target))
    expected = edvr_log.expected_version(args.expect_build, str(ROOT))
    dotnet = shutil.which("dotnet")
    wpr = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "wpr.exe"
    for path in (PROFILE, ANALYZER, wpr, *([SMOKE] if args.smoke_first else [])):
        if not path.is_file():
            raise ValueError("Required tool is missing: " + str(path))
    if not dotnet:
        raise ValueError("dotnet runtime is required for CPU trace analysis")
    validate_analyzer()
    output = Path(args.output).resolve() if args.output else ROOT / "build" / (
        "cpu-profile-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6])
    if output.exists():
        raise ValueError("Use a new output directory: " + str(output))
    return dict(target=str(target), executable=str(executable), expected_build=expected,
                installed_files=receipt["files"], output=str(output), wpr=str(wpr),
                profile=str(PROFILE), analyzer=str(ANALYZER), dotnet=dotnet,
                instance="EDVRCPU_" + uuid.uuid4().hex[:12], wait_seconds=args.wait_seconds,
                max_seconds=args.max_seconds, smoke_first=args.smoke_first)


def smoke_test_capture(p, directory):
    trace = directory / "smoke.etl"
    smoke_result = {}

    def workload():
        process = subprocess.Popen([str(SMOKE), "--cpu-trace-smoke"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        smoke_result["pid"] = process.pid
        try:
            stdout, stderr = process.communicate(timeout=30)
        except BaseException:
            process.kill()
            process.communicate()
            raise
        (directory / "smoke-output.txt").write_text(stdout + stderr, encoding="utf-8")
        if process.returncode:
            raise RuntimeError("C++ marker smoke fixture failed: " + stdout[-4000:] + stderr[-1000:])

    record(p["wpr"], p["profile"], trace, p["instance"] + "_smoke", workload)
    report = directory / "smoke-report.json"
    print(analyze(p["dotnet"], p["analyzer"], trace, smoke_result["pid"], report), flush=True)
    validated = validate_smoke_report(json.loads(report.read_text(encoding="utf-8")))
    return dict(pid=smoke_result["pid"], report=str(report), **validated)


def capture(args):
    p = plan(args)
    if args.dry_run:
        print(json.dumps(p, indent=2))
        print("[edvr] dry run: wrote nothing; started no workload or trace session.")
        return 0
    if os.name != "nt" or not ctypes.windll.shell32.IsUserAnAdmin():
        raise PermissionError("CPU scheduling capture requires a Windows administrator process (UAC approval).")
    directory = Path(p["output"])
    directory.mkdir(parents=True, exist_ok=False)
    process = None
    try:
        write_status(directory, "preparing", **p)
        if args.smoke_first:
            write_status(directory, "smoke_recording")
            smoke = smoke_test_capture(p, directory)
            write_status(directory, "smoke_passed", smoke=smoke)
        write_status(directory, "waiting_for_game")
        process = wait_for_game(Path(p["executable"]), p["wait_seconds"])
        write_status(directory, "verifying_build", pid=process.pid)
        verified = wait_for_flight_version(Path(p["target"]), process, p["expected_build"])
        write_status(directory, "starting", flight=verified)

        def workload():
            write_status(directory, "recording")
            reason = wait_for_exit(process, p["max_seconds"])
            write_status(directory, "saving", stop_reason=reason)
            return reason

        trace = directory / "flight.etl"
        record(p["wpr"], p["profile"], trace, p["instance"], workload)
        write_status(directory, "analyzing", flight=verified, trace=str(trace))
        report = directory / "report.json"
        print(analyze(p["dotnet"], p["analyzer"], trace, process.pid, report), flush=True)
        write_status(directory, "complete", report=str(report))
        return 0
    except BaseException as exc:
        write_status(directory, "failed", error=str(exc))
        raise
    finally:
        if process:
            process.close()


def self_test():
    checks = 0

    def check(value):
        nonlocal checks
        checks += 1
        if not value:
            raise AssertionError("CPU profile check %d failed" % checks)

    check(candidate_pids('"EliteDangerous64.exe","123","Console","1","2 K"\n') == [123])
    check(candidate_pids('INFO: No tasks are running which match the specified criteria.\n') == [])
    check(candidate_pids('"Other.exe","5"\n') == [])
    command = session_command("wpr", "-stop", "a b.etl", instance="EDVRCPU_unique")
    check(command == ["wpr", "-stop", "a b.etl", "-instancename", "EDVRCPU_unique"])
    calls = []

    def runner(command, **kw):
        calls.append(command)
        return ""

    check(record("wpr", "profile", "out.etl", "EDVRCPU_test", lambda: 42, runner) == 42)
    check([c[1] for c in calls] == ["-start", "-stop"])
    check(all(c[-2:] == ["-instancename", "EDVRCPU_test"] for c in calls))
    calls.clear()
    try:
        record("wpr", "profile", "out.etl", "EDVRCPU_test", lambda: (_ for _ in ()).throw(ValueError("fixture")), runner)
        check(False)
    except ValueError:
        check([c[1] for c in calls] == ["-start", "-stop"])
    for failure in ("-start", "-stop"):
        calls.clear()

        def failed_runner(command, **kw):
            calls.append(command)
            if command[1] == failure:
                raise RuntimeError("fixture")
            return ""

        try:
            record("wpr", "profile", "out.etl", "EDVRCPU_test", lambda: None, failed_runner)
            check(False)
        except RuntimeError:
            check(calls[-1][1] == "-cancel")
            check(all(c[-1] == "EDVRCPU_test" for c in calls))
    for failure in ("-start", "-stop"):
        calls.clear()

        def failed_cleanup_runner(command, **kw):
            calls.append(command)
            if command[1] in (failure, "-cancel"):
                raise RuntimeError(command[1])
            return ""

        try:
            record("wpr", "profile", "out.etl", "EDVRCPU_test", lambda: None,
                   failed_cleanup_runner)
            check(False)
        except RuntimeError as exc:
            check(str(exc) == failure)
            check(calls[-1][1] == "-cancel")

    expected_executable = Path(r"C:\fixture\EliteDangerous64.exe")
    fake_processes = []
    fake_paths = {}

    class FakeWindowsProcess:
        def __init__(self, pid):
            self.pid = pid
            self.closed = False
            fake_processes.append(self)

        def path(self):
            value = fake_paths[self.pid]
            if isinstance(value, BaseException):
                raise value
            return value

        def exited(self):
            return False

        def close(self):
            self.closed = True

    fake_paths.update({10: OSError("vanished"), 11: expected_executable})
    listing = '\n'.join(('"EliteDangerous64.exe","10"', '"EliteDangerous64.exe","11"'))
    with mock.patch(__name__ + ".invoke", return_value=listing):
        with mock.patch(__name__ + ".WindowsProcess", FakeWindowsProcess):
            found = find_game(expected_executable)
    check(found.pid == 11)
    check(next(p for p in fake_processes if p.pid == 10).closed)
    check(not found.closed)
    found.close()
    check(found.closed)

    fake_processes.clear()
    fake_paths.clear()
    fake_paths.update({12: Path(r"C:\other\EliteDangerous64.exe")})
    with mock.patch(__name__ + ".invoke", return_value='"EliteDangerous64.exe","12"'):
        with mock.patch(__name__ + ".WindowsProcess", FakeWindowsProcess):
            check(find_game(expected_executable) is None)
    check(fake_processes[0].closed)

    fake_processes.clear()
    fake_paths.clear()
    fake_paths.update({13: expected_executable, 14: expected_executable})
    listing = '\n'.join(('"EliteDangerous64.exe","13"', '"EliteDangerous64.exe","14"'))
    try:
        with mock.patch(__name__ + ".invoke", return_value=listing):
            with mock.patch(__name__ + ".WindowsProcess", FakeWindowsProcess):
                find_game(expected_executable)
        check(False)
    except RuntimeError:
        check(all(p.closed for p in fake_processes))

    current = [0.0]

    def sleep(value):
        current[0] += value

    try:
        wait_for_game(Path("fixture"), 2, finder=lambda _: None, clock=lambda: current[0], sleep=sleep)
        check(False)
    except TimeoutError:
        check(current[0] == 2)
    process = mock.Mock()
    process.exited.side_effect = [False, False, True]
    check(wait_for_exit(process, 3, clock=lambda: current[0], sleep=sleep) == "game_exit")
    process.exited.side_effect = None
    process.exited.return_value = False
    check(wait_for_exit(process, 2, clock=lambda: current[0], sleep=sleep) == "capture_limit")

    current[0] = 0.0
    process = mock.Mock(pid=77)
    process.creation_time.return_value = 1234.5
    process.exited.return_value = False
    attempts = [FileNotFoundError("pending"), FileNotFoundError("pending"),
                {"log": "fresh.log", "version": "fixture"}]

    def version_finder(target, pid, expected, created):
        check((target, pid, expected, created) == (Path("target"), 77, "fixture", 1234.5))
        value = attempts.pop(0)
        if isinstance(value, BaseException):
            raise value
        return value

    verified = wait_for_flight_version(Path("target"), process, "fixture", timeout=2,
                                       finder=version_finder, clock=lambda: current[0], sleep=sleep)
    check(verified["log"] == "fresh.log" and current[0] == 0.5)
    process.exited.return_value = True
    try:
        wait_for_flight_version(Path("target"), process, "fixture", timeout=2,
                                finder=lambda *unused: (_ for _ in ()).throw(FileNotFoundError()),
                                clock=lambda: current[0], sleep=sleep)
        check(False)
    except RuntimeError:
        check(True)
    current[0] = 0.0
    process.exited.return_value = False
    try:
        wait_for_flight_version(Path("target"), process, "fixture", timeout=2,
                                finder=lambda *unused: (_ for _ in ()).throw(FileNotFoundError()),
                                clock=lambda: current[0], sleep=sleep)
        check(False)
    except TimeoutError:
        check(current[0] == 2.0)

    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        import edvr_log
        process_created = datetime(2026, 1, 2, 3, 4, 5, tzinfo=timezone.utc).timestamp()
        stale = directory / "stale_321.log"
        stale.write_text("2025-01-02 03:04:05.000 UTC pid=321 tid=1 "
                         "module_init,version=fixture,durable_log=1\n", encoding="utf-8")
        fresh_bad = directory / "bad_321.log"
        fresh_bad.write_text("2026-01-02 03:04:05.100 UTC pid=321 tid=1 "
                             "module_init,version=wrong,durable_log=1\n", encoding="utf-8")
        fresh_good = directory / "good_321.log"
        fresh_good.write_text("2026-01-02 03:04:05.200 UTC pid=321 tid=1 "
                              "module_init,version=fixture,durable_log=1\n", encoding="utf-8")
        with mock.patch.object(edvr_log, "log_dirs_for", return_value=[str(directory)]):
            with mock.patch.object(edvr_log, "find_logs", return_value=[("", "openxr", str(stale))]):
                try:
                    flight_version(directory, 321, "fixture", process_created)
                    check(False)
                except FileNotFoundError:
                    check(True)
            with mock.patch.object(edvr_log, "find_logs", return_value=[
                    ("", "openxr", str(fresh_bad)), ("", "openxr", str(stale))]):
                try:
                    flight_version(directory, 321, "fixture", process_created)
                    check(False)
                except ValueError:
                    check(True)
            with mock.patch.object(edvr_log, "find_logs", return_value=[
                    ("", "openxr", str(fresh_good)), ("", "openxr", str(stale))]):
                check(flight_version(directory, 321, "fixture", process_created)["log"] == str(fresh_good))

        target = directory / "game"
        target.mkdir()
        (target / GAME).write_text("fixture", encoding="utf-8")
        system = directory / "Windows"
        (system / "System32").mkdir(parents=True)
        (system / "System32" / "wpr.exe").write_text("fixture", encoding="utf-8")
        profile = directory / "fixture.wprp"
        profile.write_text("fixture", encoding="utf-8")
        analyzer = directory / "fixture.dll"
        analyzer.write_text("fixture", encoding="utf-8")

        def snapshot():
            return [(str(path.relative_to(directory)), path.is_dir(),
                     b"" if path.is_dir() else path.read_bytes())
                    for path in sorted(directory.rglob("*"), key=lambda item: str(item))]

        stamp = {"sources": source_hashes(), "analyzer_sha256": hashlib.sha256(analyzer.read_bytes()).hexdigest()}
        (analyzer.parent / "validated-build.json").write_text(json.dumps(stamp), encoding="utf-8")
        before = snapshot()
        args = argparse.Namespace(dry_run=True, target="frontier", expect_build="HEAD",
                                  output=str(directory / "absent"), wait_seconds=10,
                                  max_seconds=10, smoke_first=False)
        import install_edvr
        with mock.patch.object(install_edvr, "resolve_target", return_value=str(target)):
            with mock.patch.object(install_edvr, "verify_native_receipt", return_value={"files": []}):
                with mock.patch.object(edvr_log, "expected_version", return_value="fixture"):
                    with mock.patch(__name__ + ".PROFILE", profile), \
                            mock.patch(__name__ + ".ANALYZER", analyzer), \
                            mock.patch(__name__ + ".invoke",
                                       side_effect=AssertionError("dry-run started a session")), \
                            mock.patch(__name__ + ".subprocess.Popen",
                                       side_effect=AssertionError("dry-run started a workload")), \
                            mock.patch(__name__ + ".shutil.which", return_value="dotnet"), \
                            mock.patch.dict(os.environ, {"SystemRoot": str(system)}):
                        check(capture(args) == 0)
        check(before == snapshot())
        write_status(directory, "fixture", pid=123)
        write_status(directory, "done")
        status = json.loads((directory / "status.json").read_text(encoding="utf-8"))
        check(status["pid"] == 123 and status["state"] == "done")
        with mock.patch(__name__ + ".ANALYZER", analyzer):
            validate_analyzer()
            analyzer.write_bytes(b"changed fixture")
            try:
                validate_analyzer()
                check(False)
            except ValueError:
                check(True)
        dependency = directory / "TraceEvent"
        dependency.mkdir()
        for name in ("Microsoft.Diagnostics.Tracing.TraceEvent.dll", "Microsoft.Diagnostics.FastSerialization.dll"):
            (dependency / name).write_bytes(b"fixture")
        before = snapshot()
        with mock.patch(__name__ + ".shutil.which", return_value="dotnet"), \
                mock.patch(__name__ + ".invoke", side_effect=AssertionError("dry-run invoked compiler")):
            check(build_analyzer(argparse.Namespace(trace_event_directory=str(dependency), dry_run=True)) == 0)
        check(before == snapshot())
    with mock.patch("sys.stderr", io.StringIO()):
        try:
            main(["--capture", "--dry-run", "--max-seconds", "301"])
            check(False)
        except SystemExit as exc:
            check(exc.code == 2)
    profile = ET.parse(PROFILE).getroot()
    memory = profile.find("./Profiles/Profile[@Id='EDVRCPU.Verbose.Memory']")
    check(memory is not None and memory.attrib.get("LoggingMode") == "Memory")
    keywords = {element.attrib["Value"] for element in profile.findall("./Profiles/SystemProvider/Keywords/Keyword")}
    stacks = {element.attrib["Value"] for element in profile.findall("./Profiles/SystemProvider/Stacks/Stack")}
    check({"CSwitch", "ReadyThread", "SampledProfile", "Loader", "ProcessThread"}.issubset(keywords))
    check({"CSwitch", "ReadyThread", "SampledProfile"}.issubset(stacks))
    provider = profile.find("./Profiles/EventProvider")
    check(provider is not None and provider.attrib["Name"].upper() == "D3885FA1-0B70-44F1-AF88-63B2012B111E")
    report = dict(coverageComplete=True, analyzedFrameCount=60, frames=[
        dict(sequence=i, startUs=i * 100000, endUs=(i + 1) * 100000,
             runningUs=100000 if i <= 30 else 0, waitingUs=0 if i <= 30 else 100000,
             sampleStackCount=10 if i <= 30 else 0) for i in range(1, 61)])
    check(validate_smoke_report(report)["busy_running_ms"] == 3000)
    report["frames"][0]["sequence"] = 2
    try:
        validate_smoke_report(report)
        check(False)
    except ValueError:
        check(True)
    report["frames"][0]["sequence"] = 1
    for frame in report["frames"]:
        frame["runningUs"] = 0
    try:
        validate_smoke_report(report)
        check(False)
    except ValueError:
        check(True)
    print("[edvr] CPU profile self-test: %d checks passed" % checks)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--build-analyzer", action="store_true")
    parser.add_argument("--trace-event-directory", help="existing local TraceEvent assembly directory")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--target", default="frontier")
    parser.add_argument("--expect-build", default="HEAD")
    parser.add_argument("--output", help="new capture directory")
    parser.add_argument("--wait-seconds", type=int, default=600)
    parser.add_argument("--max-seconds", type=int, default=300)
    parser.add_argument("--smoke-first", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.build_analyzer:
        try:
            return build_analyzer(args)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            print("[edvr] ERROR: " + str(exc), file=sys.stderr)
            return 1
    if not args.capture:
        parser.error("choose --capture, --build-analyzer or --self-test")
    if not 1 <= args.wait_seconds <= 3600 or not 1 <= args.max_seconds <= 300:
        parser.error("wait-seconds must be 1..3600 and max-seconds must be 1..300")
    try:
        return capture(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print("[edvr] ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
