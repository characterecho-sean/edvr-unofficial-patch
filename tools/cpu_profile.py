#!/usr/bin/env python3
"""Bounded WPR CPU capture for an installed EDVR game; never edits the game.

Run --self-test without elevation. Capture requires an administrator token.
--dry-run validates and prints the plan without creating files or sessions.
The private WPR instance is stopped on game exit, timeout or handled failure.
Use --smoke-first to validate the real provider/decoder before waiting for Elite.

Records in WPR file mode by default (buffers flush to disk; not a fixed ring);
pass --memory-ring for the old 512 x 1 MiB memory ring instead.
--start-key/--stop-key arm and end the flight leg on a hotkey (F1-F12, or a
single letter/digit); --start-after-seconds bounds or replaces the arm wait.
--gpu adds Windows' built-in GPU profile in the same private file-mode session.
Provider event counts establish coverage, not GPU busy time; the ETL retains
the queue events for GPU analysis. --gpu cannot be combined with --memory-ring.
--status-json samples Status.json at 4 Hz while recording (on by default when
the Frontier Saved Games copy exists) into <output>\\status_samples.jsonl.

Once recording starts, each installed EDVR DLL's embedded CodeView record is
matched, by PDB name and GUID+age, against build\\ and the install receipt's
own source directory; a match is copied into <output>\\symbols\\ so a later
rebuild of build\\ can never orphan an already-captured trace. Unmatched is
logged, not an error -- the game may be running an older build.
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
import math
import os
from pathlib import Path, PureWindowsPath
import shutil
import struct
import subprocess
import tempfile
import threading
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
    # Every analyzer source: the analyzer is split across several .cs files, and
    # a new one must invalidate the validated-build stamp like Program.cs does.
    sources = sorted("tools/cpu_profile/" + path.name
                     for path in (ROOT / "tools" / "cpu_profile").glob("*.cs"))
    names = (*sources, "tools/cpu_profile/EdvrCpuProfile.csproj",
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
    """Validate actual busy/Sleep CPU phases and explicitly mapped synthetic GPU data."""
    if not report.get("coverageComplete") or report.get("analyzedFrameCount") != 60:
        raise ValueError("Synthetic CPU trace has incomplete coverage or missing frame markers")
    frames = report.get("frames", [])
    if sorted(frame["sequence"] for frame in frames) != list(range(1, 61)):
        raise ValueError("Synthetic CPU trace did not preserve the complete frame sequence")
    coverage = report.get("applicationGpuCompletionCoverage", {})
    required_counts = dict(markerCount=60, explicitCpuMappings=60, legacyCpuFrames=0,
                           unavailableCpuMappings=0, schemaErrors=0, invalidMarkers=0,
                           duplicateGpuSequences=0, ambiguousCpuSequences=0,
                           gpuSequencesWithoutCpuWitness=0, eventsLost=0)
    if (report.get("eventsLost") != 0 or
            any(coverage.get(key) != value for key, value in required_counts.items()) or
            coverage.get("cycleStatuses") != {"valid": 60}):
        raise ValueError("Synthetic GPU completion trace has missing, malformed, lost or ambiguous mappings")
    for frame in frames:
        gpu = frame.get("applicationGpu", {})
        expected_token = 100000 + frame["sequence"]  # intentionally differs from XR
        expected_ms = 9.0 + ((frame["sequence"] - 1) % 3) * 0.1
        ms = gpu.get("ms")
        if (frame.get("cpuMarkerVersion") != 2 or frame.get("gpuSequence") != expected_token or
                gpu.get("producerSequence") != expected_token or
                gpu.get("status") != "valid" or gpu.get("valid") is not True or
                gpu.get("source") != 1 or gpu.get("reason") != 0 or
                not isinstance(ms, (int, float)) or not math.isfinite(ms) or abs(ms - expected_ms) > 0.000001 or
                not gpu.get("publicationUs") or not gpu.get("publicationQpc")):
            raise ValueError("Synthetic GPU decoder did not preserve explicit producer identity and known duration")
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
    return dict(busy_running_ms=round(busy_running / 1000, 2), sleep_waiting_ms=round(sleep_waiting / 1000, 2),
                synthetic_gpu_samples=60, synthetic_gpu_ms=[9.0, 9.1, 9.2],
                gpu_qualification="synthetic fixture durations; no GPU commands or hardware timing")


def invoke(command, timeout=120, preserve_on_interrupt=False, on_progress=None):
    arguments = [str(x) for x in command]
    options = dict(text=True, errors="replace",
                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if preserve_on_interrupt:
        # subprocess.run kills its child on KeyboardInterrupt. A WPR stop is
        # already saving the only trace: keep THAT child, not another stop.
        deadline = time.monotonic() + timeout
        interrupted = 0
        with subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options) as process:
            def defer_interrupt():
                nonlocal interrupted
                interrupted += 1
                print("[edvr] Ctrl+C deferred while saving; keep this window open. "
                      "The same WPR stop continues within its original timeout; closing the window cannot be protected.",
                      flush=True)
            def progress(phase):
                if on_progress:
                    try:
                        on_progress(phase, process.pid, interrupted)
                    except KeyboardInterrupt:
                        defer_interrupt()
                    except Exception as exc:
                        print("[edvr] save progress unavailable: " + type(exc).__name__, flush=True)
            progress("started")
            while True:
                try:
                    stdout, stderr = process.communicate(timeout=max(0, deadline - time.monotonic()))
                    result = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
                    break
                except KeyboardInterrupt:
                    defer_interrupt()
                    progress("interrupted")
                except subprocess.TimeoutExpired as exc:
                    process.kill()
                    exc.stdout, exc.stderr = process.communicate()
                    raise
                except BaseException:
                    process.kill()
                    raise
    else:
        result = subprocess.run(arguments, capture_output=True, timeout=timeout, **options)
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


def wpr_commands(wpr, profile, trace, instance, filemode, recordtempto, gpu=False):
    """Build the exact start/stop/cancel commands record() will run.

    Shared by plan() (to preview them in --dry-run) and record() (to run
    them), so the printed plan can never drift from what actually executes.
    """
    if gpu and not filemode:
        raise ValueError("--gpu requires file mode; --memory-ring is not supported")
    extra = (["-start", "GPU"] if gpu else []) + (
        ["-filemode", "-recordtempto", str(recordtempto)] if filemode else [])
    return dict(start=session_command(wpr, "-start", str(profile) + "!EDVRCPU", *extra, instance=instance),
                stop=session_command(wpr, "-stop", trace, instance=instance),
                cancel=session_command(wpr, "-cancel", instance=instance))


def record(wpr, profile, output, instance, workload, runner=invoke, filemode=False, recordtempto=None,
          on_started=None, on_stopped=None, gpu=False, on_save_progress=None):
    commands = wpr_commands(wpr, profile, output, instance, filemode, recordtempto, gpu)
    start, stop, cancel = commands["start"], commands["stop"], commands["cancel"]
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
        if on_started:
            on_started()
        value = workload()
    except BaseException as exc:
        failure = exc
    try:
        print("[edvr] saving trace (up to 180 seconds); keep this window open. "
              "Ctrl+C is deferred during saving; closing the window cannot be protected.", flush=True)
        runner(stop, timeout=180, preserve_on_interrupt=True, on_progress=on_save_progress)
    except KeyboardInterrupt:
        # Custom runners or forced interruption must not cancel an uncertain
        # stop merely because the caller was interrupted. No resume is inferred.
        raise
    except BaseException:
        try:
            runner(cancel)
        except Exception:
            pass
        raise
    if on_stopped:
        on_stopped()
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


_VK_FUNCTION_KEYS = {"F%d" % n: 0x6F + n for n in range(1, 13)}
_user32 = None


def virtual_key_code(name):
    """Map a hotkey name (F1..F12, or a single letter/digit) to its Windows virtual-key code."""
    upper = name.upper()
    if upper in _VK_FUNCTION_KEYS:
        return _VK_FUNCTION_KEYS[upper]
    if len(upper) == 1 and (upper.isalpha() or upper.isdigit()) and upper.isascii():
        return ord(upper)
    raise ValueError("Unsupported key name %r; use F1..F12 or a single letter/digit" % name)


def key_is_down(vk_code):
    """Default hotkey reader: GetAsyncKeyState's bit 0x8000, true only while the key is held.

    Works while the game window has focus. Never called by --self-test, which
    always supplies its own fake reader instead of touching user32.
    """
    global _user32
    if _user32 is None:
        _user32 = ctypes.WinDLL("user32", use_last_error=True)
        _user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
        _user32.GetAsyncKeyState.restype = ctypes.c_ushort
    return bool(_user32.GetAsyncKeyState(vk_code) & 0x8000)


def wait_for_exit(process, timeout, clock=time.monotonic, sleep=time.sleep, stop_vk=None, reader=key_is_down):
    end = clock() + timeout
    poll = 0.05 if stop_vk is not None else 0.5
    while not process.exited():
        if stop_vk is not None and reader(stop_vk):
            return "stop_key"
        if clock() >= end:
            return "capture_limit"
        sleep(min(poll, max(0.0, end - clock())))
    return "game_exit"


def wait_for_start_trigger(start_vk, delay_seconds, reader, clock=time.monotonic, sleep=time.sleep):
    """Block until the start key is pressed or the delay elapses, whichever comes first."""
    end = clock() + delay_seconds if delay_seconds is not None else None
    while True:
        if start_vk is not None and reader(start_vk):
            return "start_key"
        if end is not None and clock() >= end:
            return "start_after_seconds"
        sleep(0.05)


STATUS_SAMPLE_FIELDS = ("Flags", "Flags2", "Latitude", "Longitude", "Altitude", "Heading",
                        "PlanetRadius", "BodyName")


def utc_now_iso_ms(now=None):
    now = now or datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (now.microsecond // 1000)


def default_status_json():
    """The Frontier Status.json path, or None when it is not there (sampling stays off)."""
    candidate = Path(os.environ.get("USERPROFILE", "")) / "Saved Games" / "Frontier Developments" / \
        "Elite Dangerous" / "Status.json"
    return candidate if candidate.is_file() else None


def newest_journal(folder):
    """The most recently written Journal*.log in folder, or None."""
    try:
        candidates = list(Path(folder).glob("Journal*.log"))
    except OSError:
        return None
    if not candidates:
        return None
    return str(max(candidates, key=lambda path: path.stat().st_mtime_ns))


def status_sampler_tick(path, previous, now=utc_now_iso_ms):
    """Read and parse Status.json once. Returns (new_previous, sample_or_None).

    A partial write during the game's own replace-in-place raises ValueError
    (or OSError if the file is briefly absent); both are swallowed so the
    caller just retries on the next tick.
    """
    try:
        parsed = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return previous, None
    if parsed == previous:
        return previous, None
    sample = dict(utc=now(), timestamp=parsed.get("timestamp"))
    for key in STATUS_SAMPLE_FIELDS:
        if key in parsed:
            sample[key] = parsed[key]
    return parsed, sample


def run_status_sampler(path, out_path, stop_event, interval=0.25, sleep=time.sleep, now=utc_now_iso_ms):
    """Daemon-thread body: append a line to out_path whenever Status.json's content changes."""
    previous = None
    while not stop_event.is_set():
        previous, sample = status_sampler_tick(path, previous, now=now)
        if sample is not None:
            with open(out_path, "a", encoding="utf-8") as handle:
                handle.write(json.dumps(sample) + "\n")
        sleep(interval)


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


def analyze(dotnet, analyzer, trace, pid, output, gpu=False):
    if output.exists():
        raise ValueError("Analysis output already exists: " + str(output))
    return invoke([str(dotnet), str(analyzer), "--input", str(trace), "--pid", str(pid),
                   "--output", str(output)] + (["--gpu-coverage"] if gpu else []), timeout=300)


def gpu_profile_details(wpr, profile, runner=None):
    # Read-only WPR validation is allowed in --dry-run. No trace or output file
    # is created; use WPR's own installed GPU schema rather than hand-built ETW masks.
    runner = runner or invoke
    command = [str(wpr), "-profiledetails", str(profile) + "!EDVRCPU+GPU", "-filemode"]
    details = runner(command)
    required = ("EDVRCPU.Verbose.File", "GPU.Verbose.File", "Microsoft-Windows-DxgKrnl")
    if not all(value in details for value in required):
        raise ValueError("Combined EDVR CPU + built-in GPU file profile is unavailable")
    return dict(enabled=True, profile="GPU", preflight_command=command, profile_details=details,
                qualification="Provider event counts are coverage only, not GPU busy time; "
                              "GPU queues/preemption require ETL analysis.")


def validate_gpu_smoke_report(report):
    coverage = report.get("gpuProviderCoverage", {})
    counts = coverage.get("providerEvents", [])
    dxg = next((entry for entry in counts if entry.get("provider") == "Microsoft-Windows-DxgKrnl"), {})
    if report.get("eventsLost") != 0 or coverage.get("eventsLost") != 0 or dxg.get("systemWide", 0) <= 0:
        raise ValueError("GPU smoke has absent DxgKrnl events or reported event loss; flight capture was not armed")
    return dict(gpu_provider_coverage=coverage)


def installed_receipt(target):
    import install_edvr
    primary = target / "edvr_native_receipt.json"
    # Reinstalls preserve earlier receipts and write a new uniquely named sibling.
    candidates = [primary, *target.glob("edvr_native_receipt.json.pre-*.bak*")]
    candidates.sort(key=lambda path: path.stat().st_mtime_ns if path.is_file() else -1, reverse=True)
    errors = []
    for path in candidates:
        try:
            return install_edvr.verify_native_receipt(str(path), str(target)), str(path)
        except (OSError, ValueError) as exc:
            errors.append(str(exc))
    raise ValueError("No receipt verifies the active native install: " + (errors[0] if errors else str(target)))


IMAGE_DEBUG_TYPE_CODEVIEW = 2
MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
SYMBOL_KEYS = ("graphics", "runtime")


def _format_guid(raw16):
    """16 raw GUID bytes -> "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX".

    The first three fields are little-endian; the last two are a plain hex
    dump of the remaining bytes, unchanged. The same mixed-endian text form
    a PE CodeView record and a PDB's own Info Stream both use, so the two
    only need formatting once.
    """
    d1, d2, d3 = struct.unpack_from("<IHH", raw16, 0)
    d4 = raw16[8:16]
    return "%08X-%04X-%04X-%s-%s" % (d1, d2, d3, d4[:2].hex().upper(), d4[2:].hex().upper())


def _rva_to_file_offset(sections, rva):
    """Translate an image RVA to a file offset via the section table, or None."""
    for virtual_address, virtual_size, size_of_raw_data, pointer_to_raw_data in sections:
        span = max(virtual_size, size_of_raw_data)
        if virtual_address <= rva < virtual_address + span:
            return pointer_to_raw_data + (rva - virtual_address)
    return None


def codeview_record(dll_path):
    """The RSDS CodeView record embedded in a PE's debug directory, or None.

    Parsed by hand: the DOS header's e_lfanew locates the PE signature; the
    COFF header gives the section count and the optional header's size; the
    optional header's DataDirectory[6] (IMAGE_DIRECTORY_ENTRY_DEBUG) gives an
    RVA and size for the array of 28-byte IMAGE_DEBUG_DIRECTORY entries, so
    the section table is walked once to translate that RVA to a file offset.
    A Type 2 (CODEVIEW) entry's PointerToRawData/SizeOfData is itself a file
    offset: the RSDS record there is a 4-byte signature, a 16-byte GUID, a
    uint32 age, then a NUL-terminated UTF-8 PDB path.

    Returns None for anything that is not a well-formed PE with an RSDS
    CodeView entry -- including the older NB10 debug format, which this
    project's MSVC toolchain never emits. Raises only if dll_path can't be
    read; every format inconsistency past that point is reported as None
    rather than an exception, since a DLL built without /DEBUG is routine,
    not an error.
    """
    data = Path(dll_path).read_bytes()
    try:
        if len(data) < 0x40 or data[:2] != b"MZ":
            return None
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
            return None
        coff_off = e_lfanew + 4
        (_machine, section_count, _timestamp, _symtab_ptr, _symtab_count,
         opt_header_size, _characteristics) = struct.unpack_from("<HHIIIHH", data, coff_off)
        opt_header_off = coff_off + 20
        magic = struct.unpack_from("<H", data, opt_header_off)[0]
        if magic == 0x10B:      # PE32: DataDirectory starts at +96 in the optional header.
            data_directory_off = opt_header_off + 96
        elif magic == 0x20B:    # PE32+: ImageBase is 8 bytes wider, so +112 instead.
            data_directory_off = opt_header_off + 112
        else:
            return None
        debug_rva, debug_size = struct.unpack_from("<II", data, data_directory_off + 6 * 8)
        if not debug_rva or not debug_size:
            return None
        section_off = coff_off + 20 + opt_header_size
        sections = []
        for i in range(section_count):
            _name, virtual_size, virtual_address, size_of_raw_data, pointer_to_raw_data = \
                struct.unpack_from("<8sIIII", data, section_off + i * 40)
            sections.append((virtual_address, virtual_size, size_of_raw_data, pointer_to_raw_data))
        debug_dir_off = _rva_to_file_offset(sections, debug_rva)
        if debug_dir_off is None:
            return None
        for i in range(debug_size // 28):
            (_characteristics2, _timestamp2, _major, _minor, entry_type,
             size_of_data, _address_of_raw_data, pointer_to_raw_data) = \
                struct.unpack_from("<IIHHIIII", data, debug_dir_off + i * 28)
            if entry_type != IMAGE_DEBUG_TYPE_CODEVIEW:
                continue
            cv = data[pointer_to_raw_data:pointer_to_raw_data + size_of_data]
            if cv[:4] != b"RSDS":
                continue
            guid = _format_guid(cv[4:20])
            age = struct.unpack_from("<I", cv, 20)[0]
            raw_name = cv[24:]
            nul = raw_name.find(b"\x00")
            if nul != -1:
                raw_name = raw_name[:nul]
            pdb_name = PureWindowsPath(raw_name.decode("utf-8", errors="replace")).name
            return dict(pdb_name=pdb_name, guid=guid, age=age)
        return None
    except (struct.error, IndexError):
        return None


def pdb_signature(pdb_path):
    """The {guid, age} a PDB's Info Stream (MSF stream 1) declares, or None.

    Parsed by hand: the 32-byte MSF magic is followed by the superblock
    (BlockSize, FreeBlockMapBlock, NumBlocks, NumDirectoryBytes, Unknown,
    BlockMapAddr, each a little-endian uint32). BlockMapAddr is the block
    holding the Stream Directory's own block-number array; like every other
    minimal MSF reader, this assumes that array fits in a single block --
    true of every PDB this project has produced, though not guaranteed by
    the format for an enormous one. The Stream Directory itself is
    {NumStreams u32, StreamSizes[NumStreams] u32, then each stream's block
    numbers in turn}; stream 1 is the PDB Info Stream: {Version u32,
    Signature u32, Age u32, Guid 16 bytes}.

    Returns None for anything that is not a well-formed MSF7 container with
    a readable stream 1. Raises only if pdb_path can't be read.
    """
    data = Path(pdb_path).read_bytes()
    try:
        if data[:32] != MSF_MAGIC:
            return None
        block_size, _free_block_map, _num_blocks, num_directory_bytes, _reserved, block_map_addr = \
            struct.unpack_from("<IIIIII", data, 32)
        directory_block_count = -(-num_directory_bytes // block_size)  # ceil division
        directory_blocks = struct.unpack_from("<%dI" % directory_block_count, data,
                                              block_map_addr * block_size)
        directory_bytes = b"".join(data[block * block_size:(block + 1) * block_size]
                                   for block in directory_blocks)[:num_directory_bytes]
        stream_count = struct.unpack_from("<I", directory_bytes, 0)[0]
        if stream_count < 2:
            return None
        stream_sizes = struct.unpack_from("<%dI" % stream_count, directory_bytes, 4)
        offset = 4 + 4 * stream_count
        stream_blocks = []
        for size in stream_sizes:
            count = 0 if size in (0, 0xFFFFFFFF) else -(-size // block_size)
            stream_blocks.append(struct.unpack_from("<%dI" % count, directory_bytes, offset) if count else ())
            offset += 4 * count
        info_bytes = b"".join(data[block * block_size:(block + 1) * block_size]
                              for block in stream_blocks[1])[:stream_sizes[1]]
        age = struct.unpack_from("<I", info_bytes, 8)[0]
        guid = _format_guid(info_bytes[12:28])
        return dict(guid=guid, age=age)
    except (struct.error, IndexError, ZeroDivisionError):
        return None


def match_symbols(installed_files, candidate_dirs):
    """Match each installed EDVR DLL's CodeView record to a PDB.

    For every installed_files entry whose key is in SYMBOL_KEYS: reads the
    *installed* DLL's CodeView record -- the build actually running, which
    may be older than whatever build\\ holds right now -- then looks, in
    order, across candidate_dirs plus that entry's own source directory, for
    a PDB named pdb_name whose guid+age match. The first candidate directory
    with a same-named, same-signature PDB wins. A same-named PDB with a
    different signature does not count: matched stays False and pdb stays
    None, because symbolicating against the wrong PDB would be silently
    wrong rather than simply absent.

    Returns a list of {"key", "dll", "pdb_name", "guid", "age", "matched",
    "pdb"}, one entry per matching installed_files key. pdb_name/guid/age are
    None when the installed DLL carries no CodeView record at all (including
    when it can't be read); pdb is None unless matched is True.
    """
    results = []
    for entry in installed_files:
        if entry.get("key") not in SYMBOL_KEYS:
            continue
        dll = entry["target"]
        try:
            cv = codeview_record(dll)
        except OSError:
            cv = None
        result = dict(key=entry["key"], dll=dll,
                      pdb_name=cv["pdb_name"] if cv else None,
                      guid=cv["guid"] if cv else None,
                      age=cv["age"] if cv else None,
                      matched=False, pdb=None)
        if cv:
            seen = []
            for directory in [*candidate_dirs, Path(entry["source"]).parent]:
                directory = Path(directory)
                if directory in seen:
                    continue
                seen.append(directory)
                candidate = directory / cv["pdb_name"]
                if not candidate.is_file():
                    continue
                try:
                    signature = pdb_signature(candidate)
                except OSError:
                    signature = None
                if signature and signature["guid"] == cv["guid"] and signature["age"] == cv["age"]:
                    result.update(matched=True, pdb=str(candidate))
                    break
        results.append(result)
    return results


def plan(args):
    gpu = getattr(args, "gpu", False)
    if gpu and args.memory_ring:
        raise ValueError("--gpu requires file mode; --memory-ring is not supported")
    import install_edvr
    import edvr_log
    target = Path(install_edvr.resolve_target(args.target)).resolve()
    executable = target / GAME
    if not executable.is_file():
        raise ValueError("Game executable is missing: " + str(executable))
    receipt, receipt_path = installed_receipt(target)
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
    instance = "EDVRCPU_" + uuid.uuid4().hex[:12]
    start_vk = virtual_key_code(args.start_key) if args.start_key else None
    stop_vk = virtual_key_code(args.stop_key) if args.stop_key else None
    status_json = Path(args.status_json).resolve() if args.status_json else default_status_json()
    logging_mode = "memory" if args.memory_ring else "file"
    commands = wpr_commands(wpr, PROFILE, output / "flight.etl", instance, logging_mode == "file", output, gpu)
    symbols = dict(candidate_dirs=[str(ROOT / "build")],
                  dlls=[{"key": e["key"], "target": e["target"], "source": e["source"]}
                        for e in receipt["files"] if e.get("key") in SYMBOL_KEYS])
    result = dict(target=str(target), executable=str(executable), expected_build=expected, receipt=receipt_path,
                installed_files=receipt["files"], output=str(output), wpr=str(wpr),
                profile=str(PROFILE), analyzer=str(ANALYZER), dotnet=dotnet,
                instance=instance, wait_seconds=args.wait_seconds,
                max_seconds=args.max_seconds, smoke_first=args.smoke_first,
                logging_mode=logging_mode, start_command=commands["start"],
                stop_command=commands["stop"], cancel_command=commands["cancel"],
                start_vk=start_vk, stop_vk=stop_vk, start_after_seconds=args.start_after_seconds,
                status_json=str(status_json) if status_json else None, symbols=symbols)
    if gpu:
        result["gpu"] = gpu_profile_details(wpr, PROFILE)
    return result


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

    record(p["wpr"], p["profile"], trace, p["instance"] + "_smoke", workload,
          filemode=(p["logging_mode"] == "file"), recordtempto=directory, gpu=("gpu" in p))
    report = directory / "smoke-report.json"
    print(analyze(p["dotnet"], p["analyzer"], trace, smoke_result["pid"], report, gpu=("gpu" in p)), flush=True)
    decoded = json.loads(report.read_text(encoding="utf-8"))
    validated = validate_smoke_report(decoded)
    if "gpu" in p:
        validated.update(validate_gpu_smoke_report(decoded))
    return dict(pid=smoke_result["pid"], report=str(report), **validated)


def capture_symbols(p, directory):
    """Copy the PDB matching each installed EDVR DLL beside this capture.

    Runs once WPR is recording (on_started), so a later rebuild of build\\
    can never orphan an already-captured trace: whatever matches what is
    actually running is pulled in now. The symbols\\ subdirectory is created
    only once there is a matched PDB to put in it. Best-effort like the
    Status.json sampler -- a match or copy failure is logged to status.json
    and the console, never allowed to cost the flight already underway.
    """
    try:
        matches = match_symbols(p["installed_files"], [ROOT / "build"])
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print("[edvr] cpu profile: symbols: match failed: %s" % exc, flush=True)
        write_status(directory, "recording", symbols=dict(error=str(exc)))
        return []
    symbols_dir = directory / "symbols"
    copied = []
    for entry in matches:
        label = "matched" if entry["matched"] else "unmatched (game may be running an older build)"
        print("[edvr] cpu profile: symbols %s: %s guid=%s age=%s" %
             (label, entry["key"], entry["guid"], entry["age"]), flush=True)
        if entry["matched"]:
            try:
                symbols_dir.mkdir(parents=True, exist_ok=True)
                destination = symbols_dir / Path(entry["pdb"]).name
                shutil.copy2(entry["pdb"], destination)
                copied.append(str(destination))
            except OSError as exc:
                print("[edvr] cpu profile: symbols: copy failed for %s: %s" % (entry["key"], exc), flush=True)
    write_status(directory, "recording", symbols=dict(matches=matches, copied=copied))
    return copied


def capture(args, key_reader=key_is_down):
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
    sampler_stop = threading.Event()
    sampler_thread = None
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

        if p["start_vk"] is not None or p["start_after_seconds"] is not None:
            print("[edvr] cpu profile: armed at " + utc_now_iso_ms(), flush=True)
            write_status(directory, "armed")
            wait_for_start_trigger(p["start_vk"], p["start_after_seconds"], key_reader)

        def on_started():
            nonlocal sampler_thread
            now = utc_now_iso_ms()
            print("[edvr] cpu profile: recording started at " + now, flush=True)
            journal_path = newest_journal(Path(p["status_json"]).parent) if p["status_json"] else None
            write_status(directory, "recording", recording_started_utc=now, journal_path=journal_path)
            if p["status_json"]:
                sampler_thread = threading.Thread(
                    target=run_status_sampler,
                    args=(p["status_json"], directory / "status_samples.jsonl", sampler_stop),
                    daemon=True)
                sampler_thread.start()
            capture_symbols(p, directory)

        def on_stopped():
            now = utc_now_iso_ms()
            print("[edvr] cpu profile: recording stopped at " + now, flush=True)
            write_status(directory, "saving", recording_stopped_utc=now)

        def workload():
            reason = wait_for_exit(process, p["max_seconds"], stop_vk=p["stop_vk"], reader=key_reader)
            sampler_stop.set()
            write_status(directory, "saving", stop_reason=reason)
            return reason

        def on_save_progress(phase, child_pid, interrupts):
            details = dict(save_phase=phase, save_child_pid=child_pid,
                           save_interrupts=interrupts, save_timeout_seconds=180,
                           save_window_closure_protected=False)
            if phase == "started":
                details["save_started_utc"] = utc_now_iso_ms()
            write_status(directory, "saving", **details)

        trace = directory / "flight.etl"
        record(p["wpr"], p["profile"], trace, p["instance"], workload,
              filemode=(p["logging_mode"] == "file"), recordtempto=directory,
              on_started=on_started, on_stopped=on_stopped, gpu=("gpu" in p),
              on_save_progress=on_save_progress)
        write_status(directory, "analyzing", flight=verified, trace=str(trace))
        report = directory / "report.json"
        print(analyze(p["dotnet"], p["analyzer"], trace, process.pid, report, gpu=("gpu" in p)), flush=True)
        write_status(directory, "complete", report=str(report))
        return 0
    except BaseException as exc:
        try:
            stage = json.loads((directory / "status.json").read_text(encoding="utf-8")).get("state", "unknown")
        except (OSError, ValueError):
            stage = "unknown"
        write_status(directory, "failed", error=str(exc) or type(exc).__name__,
                     error_type=type(exc).__name__, failed_during=stage)
        raise
    finally:
        sampler_stop.set()
        if sampler_thread:
            sampler_thread.join(timeout=2)
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

    save_process = mock.MagicMock(pid=4321, returncode=0)
    save_process.__enter__.return_value = save_process
    save_process.communicate.side_effect = [KeyboardInterrupt(), KeyboardInterrupt(), ("saved", "")]
    progress = []
    with mock.patch(__name__ + ".subprocess.Popen", return_value=save_process) as opened, \
            mock.patch(__name__ + ".time.monotonic", side_effect=[100, 101, 102, 103]):
        check(invoke(["wpr", "-stop", "only.etl", "-instancename", "private"], timeout=10,
                     preserve_on_interrupt=True, on_progress=lambda *event: progress.append(event)) == "saved")
        check(opened.call_count == 1 and not save_process.kill.called)
        check([call.kwargs["timeout"] for call in save_process.communicate.call_args_list] == [9, 8, 7])
        check(progress == [("started", 4321, 0), ("interrupted", 4321, 1), ("interrupted", 4321, 2)])
    process = mock.MagicMock(pid=4321, returncode=0)
    process.__enter__.return_value = process
    process.communicate.return_value = ("saved", "")
    with mock.patch(__name__ + ".subprocess.Popen", return_value=process):
        check(invoke(["wpr", "-stop"], preserve_on_interrupt=True,
                     on_progress=lambda *args: (_ for _ in ()).throw(KeyboardInterrupt())) == "saved")
        check(not process.kill.called)
    for failure in (subprocess.TimeoutExpired("wpr", 10), RuntimeError("actual stop failure")):
        process = mock.MagicMock(pid=4321, returncode=1)
        process.__enter__.return_value = process
        process.communicate.side_effect = [failure, ("timeout output", "timeout error")]
        with mock.patch(__name__ + ".subprocess.Popen", return_value=process):
            try:
                invoke(["wpr", "-stop"], preserve_on_interrupt=True)
                check(False)
            except (subprocess.TimeoutExpired, RuntimeError) as exc:
                check(exc is failure and process.kill.call_count == 1)
    calls.clear()
    def interrupted_stop(command, **kwargs):
        calls.append(command)
        if command[1] == "-stop":
            check(kwargs["preserve_on_interrupt"] is True and kwargs["timeout"] == 180)
            raise KeyboardInterrupt()
        return ""
    try:
        record("wpr", "profile", "retained.etl", "private_interrupt", lambda: None, interrupted_stop)
        check(False)
    except KeyboardInterrupt:
        check([c[1] for c in calls] == ["-start", "-stop"])
        check(all(c[-2:] == ["-instancename", "private_interrupt"] for c in calls))
    calls.clear()
    def timed_out_stop(command, **kwargs):
        calls.append(command)
        if command[1] == "-stop":
            raise subprocess.TimeoutExpired(command, 180)
        return ""
    try:
        record("wpr", "profile", "out.etl", "private_timeout", lambda: None, timed_out_stop)
        check(False)
    except subprocess.TimeoutExpired:
        check([c[1] for c in calls] == ["-start", "-stop", "-cancel"])
        check(all(c[-2:] == ["-instancename", "private_timeout"] for c in calls))
    calls.clear()
    try:
        record("wpr", "profile", "out.etl", "private_workload", lambda: (_ for _ in ()).throw(KeyboardInterrupt()), runner)
        check(False)
    except KeyboardInterrupt:
        check([c[1] for c in calls] == ["-start", "-stop"])

    # Exercise the actual smoke plumbing with a fake process/analyzer, not a
    # separately rebuilt command. An optional GPU flight must not pass a CPU-only smoke.
    gpu_smoke = {"eventsLost": 0, "gpuProviderCoverage": {
        "eventsLost": 0, "providerEvents": [{"provider": "Microsoft-Windows-DxgKrnl", "systemWide": 1}],
        "absentProviders": ["Microsoft-Windows-Direct3D11", "Microsoft-Windows-DXGI"],
        "gpuBusyTimeAnalyzed": False}}
    check(validate_gpu_smoke_report(gpu_smoke)["gpu_provider_coverage"]["absentProviders"] ==
          ["Microsoft-Windows-Direct3D11", "Microsoft-Windows-DXGI"])
    for bad in ({}, {"eventsLost": 1, "gpuProviderCoverage": gpu_smoke["gpuProviderCoverage"]},
                {"eventsLost": 0, "gpuProviderCoverage": {"eventsLost": 1, "providerEvents": gpu_smoke["gpuProviderCoverage"]["providerEvents"]}},
                {"eventsLost": 0, "gpuProviderCoverage": {"eventsLost": 0, "providerEvents": []}}):
        try:
            validate_gpu_smoke_report(bad)
            check(False)
        except ValueError:
            check(True)
    with tempfile.TemporaryDirectory() as smoke_tmp:
        smoke_dir = Path(smoke_tmp)
        p_smoke = dict(wpr="wpr", profile="profile", instance="private_smoke", dotnet="dotnet", analyzer="analyzer",
                       logging_mode="file", gpu={"enabled": True}, output=str(smoke_dir / "capture"), smoke_first=True)
        smoke_process = mock.Mock(pid=789, returncode=0)
        smoke_process.communicate.return_value = ("fixture", "")
        def smoke_record(*arguments, **keywords):
            check(keywords["gpu"] and keywords["filemode"])
            check(arguments[3] == "private_smoke_smoke")
            return arguments[4]()
        def smoke_analyze(*arguments, **keywords):
            check(keywords["gpu"])
            check(arguments[2].name == "smoke.etl" and arguments[3] == 789)
            arguments[4].write_text(json.dumps(gpu_smoke), encoding="utf-8")
            return "fixture analyzed"
        with mock.patch(__name__ + ".record", side_effect=smoke_record), \
                mock.patch(__name__ + ".analyze", side_effect=smoke_analyze), \
                mock.patch(__name__ + ".subprocess.Popen", return_value=smoke_process), \
                mock.patch(__name__ + ".validate_smoke_report", return_value={"cpu": "validated"}):
            check(smoke_test_capture(p_smoke, smoke_dir)["gpu_provider_coverage"]["gpuBusyTimeAnalyzed"] is False)
            gpu_smoke = {"eventsLost": 0}  # CPU-only report must abort before waiting for Elite.
            with mock.patch(__name__ + ".plan", return_value=p_smoke), \
                    mock.patch(__name__ + ".ctypes.windll.shell32.IsUserAnAdmin", return_value=True), \
                    mock.patch(__name__ + ".wait_for_game", side_effect=AssertionError("flight armed after CPU-only smoke")):
                try:
                    capture(argparse.Namespace(dry_run=False, smoke_first=True))
                    check(False)
                except ValueError:
                    check(True)
        interrupted_dir = smoke_dir / "interrupted"
        interrupt_plan = {**p_smoke, "output": str(interrupted_dir)}
        with mock.patch(__name__ + ".plan", return_value=interrupt_plan), \
                mock.patch(__name__ + ".ctypes.windll.shell32.IsUserAnAdmin", return_value=True), \
                mock.patch(__name__ + ".smoke_test_capture", side_effect=KeyboardInterrupt()):
            try:
                capture(argparse.Namespace(dry_run=False, smoke_first=True))
                check(False)
            except KeyboardInterrupt:
                failure = json.loads((interrupted_dir / "status.json").read_text(encoding="utf-8"))
                check(failure["error"] == failure["error_type"] == "KeyboardInterrupt")
                check(failure["state"] == "failed" and failure["failed_during"] == "smoke_recording")
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

    commands = wpr_commands("wpr", "profile", "out.etl", "EDVRCPU_x", True, "C:\\cap dir")
    check(commands["start"] == ["wpr", "-start", "profile!EDVRCPU", "-filemode", "-recordtempto", "C:\\cap dir",
                                "-instancename", "EDVRCPU_x"])
    check(commands["stop"] == ["wpr", "-stop", "out.etl", "-instancename", "EDVRCPU_x"])
    check(commands["cancel"] == ["wpr", "-cancel", "-instancename", "EDVRCPU_x"])
    commands = wpr_commands("wpr", "profile", "out.etl", "EDVRCPU_x", False, "C:\\cap dir")
    check("-filemode" not in commands["start"] and "-recordtempto" not in commands["start"])
    check(commands["start"] == ["wpr", "-start", "profile!EDVRCPU", "-instancename", "EDVRCPU_x"])

    gpu_details = "Windows Performance Recorder fixture\nEDVRCPU.Verbose.File\nGPU.Verbose.File\nMicrosoft-Windows-DxgKrnl"
    calls.clear()
    def details_runner(command):
        calls.append(command)
        return gpu_details
    detail = gpu_profile_details("wpr", "profile", details_runner)
    check(calls == [["wpr", "-profiledetails", "profile!EDVRCPU+GPU", "-filemode"]])
    check(detail["enabled"] and detail["profile"] == "GPU" and "not GPU busy time" in detail["qualification"])
    for missing in ("", "EDVRCPU.Verbose.File GPU.Verbose.File", "GPU.Verbose.File Microsoft-Windows-DxgKrnl"):
        try:
            gpu_profile_details("wpr", "profile", lambda _: missing)
            check(False)
        except ValueError:
            check(True)
    try:
        wpr_commands("wpr", "profile", "out.etl", "private", False, "cap", gpu=True)
        check(False)
    except ValueError:
        check(True)
    commands = wpr_commands("wpr", "profile", "out.etl", "private", True, "cap", gpu=True)
    check(commands["start"] == ["wpr", "-start", "profile!EDVRCPU", "-start", "GPU", "-filemode",
                                "-recordtempto", "cap", "-instancename", "private"])
    check(commands["stop"][-2:] == commands["cancel"][-2:] == ["-instancename", "private"])
    for failure in ("start", "callback", "workload", "stop", "callback_stop"):
        calls.clear()
        workload_calls = []
        def gpu_runner(command, **kw):
            calls.append(command)
            if command[1] == "-start" and failure == "start":
                raise RuntimeError("partial GPU start")
            if command[1] == "-stop" and failure in ("stop", "callback_stop"):
                raise RuntimeError("GPU stop")
            return ""
        def gpu_started():
            if failure in ("callback", "callback_stop"):
                raise RuntimeError("GPU callback")
        def gpu_workload():
            workload_calls.append(True)
            if failure == "workload":
                raise RuntimeError("GPU workload")
            return "game_exit"
        try:
            record("wpr", "profile", "out.etl", "private_gpu", gpu_workload, gpu_runner,
                   filemode=True, recordtempto="cap", on_started=gpu_started, gpu=True)
            check(False)
        except RuntimeError:
            check([c[1] for c in calls] == (["-start", "-cancel"] if failure == "start" else
                  ["-start", "-stop", "-cancel"] if failure in ("stop", "callback_stop") else ["-start", "-stop"]))
            check(all(c[-2:] == ["-instancename", "private_gpu"] for c in calls))
            check(bool(workload_calls) == (failure in ("workload", "stop")))
    calls.clear()
    check(record("wpr", "profile", "out.etl", "private_gpu", lambda: "capture_limit", runner,
                 filemode=True, recordtempto="cap", gpu=True) == "capture_limit")
    check([c[1] for c in calls] == ["-start", "-stop"])

    calls.clear()
    started, stopped = [], []
    check(record("wpr", "profile", "out.etl", "EDVRCPU_test4", lambda: 7, runner, filemode=True,
                 recordtempto="C:\\cap", on_started=lambda: started.append(True),
                 on_stopped=lambda: stopped.append(True)) == 7)
    check(calls[0] == ["wpr", "-start", "profile!EDVRCPU", "-filemode", "-recordtempto", "C:\\cap",
                       "-instancename", "EDVRCPU_test4"])
    check(started == [True] and stopped == [True])

    calls.clear()
    started.clear()

    def failing_start_runner(command, **kw):
        calls.append(command)
        if command[1] == "-start":
            raise RuntimeError("fixture")
        return ""

    try:
        record("wpr", "profile", "out.etl", "EDVRCPU_test5", lambda: None, failing_start_runner,
              filemode=True, recordtempto="C:\\cap", on_started=lambda: started.append(True))
        check(False)
    except RuntimeError:
        check(started == [])

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

    check(virtual_key_code("F1") == 0x70)
    check(virtual_key_code("f9") == 0x78)
    check(virtual_key_code("F12") == 0x7B)
    check(virtual_key_code("a") == 0x41)
    check(virtual_key_code("Z") == 0x5A)
    check(virtual_key_code("5") == 0x35)
    for bad in ("F0", "F13", "Ctrl", "@", "", "AB"):
        try:
            virtual_key_code(bad)
            check(False)
        except ValueError:
            check(True)

    current[0] = 0.0
    reader_calls = []

    def fake_reader(vk):
        reader_calls.append(vk)
        return len(reader_calls) >= 3

    check(wait_for_start_trigger(99, None, fake_reader, clock=lambda: current[0], sleep=sleep) == "start_key")
    check(reader_calls == [99, 99, 99] and current[0] == 0.10)

    current[0] = 0.0
    reader_calls.clear()
    check(wait_for_start_trigger(None, 0.1, fake_reader, clock=lambda: current[0], sleep=sleep)
         == "start_after_seconds")
    check(reader_calls == [])

    current[0] = 0.0
    process = mock.Mock()
    process.exited.return_value = False
    reader_calls.clear()

    def stop_reader(vk):
        reader_calls.append(vk)
        return len(reader_calls) >= 2

    check(wait_for_exit(process, 100, clock=lambda: current[0], sleep=sleep, stop_vk=42,
                        reader=stop_reader) == "stop_key")
    check(reader_calls == [42, 42])

    current[0] = 0.0
    check(wait_for_exit(process, 0.12, clock=lambda: current[0], sleep=sleep, stop_vk=42,
                        reader=lambda vk: False) == "capture_limit")

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
        # A subdirectory, never directory itself: directory/status.json is the tool's own
        # output file, and Windows filesystems collide "status.json" with "Status.json".
        status_fixture = directory / "saved_games" / "Status.json"
        status_fixture.parent.mkdir()
        status_fixture.write_text(json.dumps({"timestamp": "fixture"}), encoding="utf-8")

        def snapshot():
            return [(str(path.relative_to(directory)), path.is_dir(),
                     b"" if path.is_dir() else path.read_bytes())
                    for path in sorted(directory.rglob("*"), key=lambda item: str(item))]

        stamp = {"sources": source_hashes(), "analyzer_sha256": hashlib.sha256(analyzer.read_bytes()).hexdigest()}
        (analyzer.parent / "validated-build.json").write_text(json.dumps(stamp), encoding="utf-8")
        before = snapshot()
        args = argparse.Namespace(dry_run=True, target="frontier", expect_build="HEAD",
                                  output=str(directory / "absent"), wait_seconds=10,
                                  max_seconds=10, smoke_first=False, start_key=None, stop_key=None,
                                  start_after_seconds=None, memory_ring=False, status_json=None)
        import install_edvr
        sibling_receipt = target / "edvr_native_receipt.json.pre-fixture-20260918-120000.bak"
        sibling_receipt.write_text("{}", encoding="utf-8")
        with mock.patch.object(install_edvr, "verify_native_receipt", side_effect=lambda path, _: {"files": []}
                               if path == str(sibling_receipt) else (_ for _ in ()).throw(ValueError("stale"))):
            check(installed_receipt(target)[1] == str(sibling_receipt))
        before = snapshot()
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
                            mock.patch(__name__ + ".default_status_json", return_value=None), \
                            mock.patch.dict(os.environ, {"SystemRoot": str(system)}):
                        check(capture(args) == 0)

                        p = plan(args)
                        check(p["logging_mode"] == "file")
                        check("-filemode" in p["start_command"] and "-recordtempto" in p["start_command"])
                        check(p["start_command"][p["start_command"].index("-recordtempto") + 1] == p["output"])
                        check(p["start_command"][-2:] == ["-instancename", p["instance"]])
                        check(p["stop_command"] == [p["wpr"], "-stop", str(Path(p["output"]) / "flight.etl"),
                                                    "-instancename", p["instance"]])
                        check(p["start_vk"] is None and p["stop_vk"] is None and p["start_after_seconds"] is None)
                        check(p["status_json"] is None)
                        check(p["symbols"] == dict(candidate_dirs=[str(ROOT / "build")], dlls=[]))

                        ring_args = argparse.Namespace(**{**vars(args), "memory_ring": True})
                        ring = plan(ring_args)
                        check(ring["logging_mode"] == "memory")
                        check("-filemode" not in ring["start_command"] and
                             "-recordtempto" not in ring["start_command"])
                        check(ring["start_command"] == [ring["wpr"], "-start", ring["profile"] + "!EDVRCPU",
                                                        "-instancename", ring["instance"]])

                        armed_args = argparse.Namespace(**{**vars(args), "start_key": "F9", "stop_key": "f10",
                                                           "start_after_seconds": 30.0,
                                                           "status_json": str(status_fixture)})
                        armed = plan(armed_args)
                        check(armed["start_vk"] == 0x78 and armed["stop_vk"] == 0x79)
                        check(armed["start_after_seconds"] == 30.0)
                        check(armed["status_json"] == str(status_fixture.resolve()))
                        check(capture(armed_args) == 0)

                        gpu_args = argparse.Namespace(**{**vars(args), "gpu": True})
                        def readonly_preflight(command, **kw):
                            check(command == [str(system / "System32" / "wpr.exe"), "-profiledetails",
                                              str(profile) + "!EDVRCPU+GPU", "-filemode"])
                            return gpu_details
                        with mock.patch(__name__ + ".invoke", side_effect=readonly_preflight), \
                                mock.patch(__name__ + ".capture_symbols", side_effect=AssertionError("dry-run copied PDB")), \
                                mock.patch(__name__ + ".record", side_effect=AssertionError("dry-run recorded")), \
                                mock.patch(__name__ + ".wait_for_game", side_effect=AssertionError("dry-run waited for game")):
                            check(capture(gpu_args) == 0)
                            gpu_plan = plan(gpu_args)
                            check(gpu_plan["gpu"]["enabled"] and "GPU" in gpu_plan["start_command"])
                            check(gpu_plan["stop_command"][-1] == gpu_plan["instance"])
                            check(gpu_plan["cancel_command"][-1] == gpu_plan["instance"])
                            with mock.patch(__name__ + ".invoke", return_value="GPU profile absent"):
                                try:
                                    capture(gpu_args)
                                    check(False)
                                except ValueError:
                                    check(True)
                        check("gpu" not in p)
                        check(before == snapshot())
                        try:
                            plan(argparse.Namespace(**{**vars(gpu_args), "memory_ring": True}))
                            check(False)
                        except ValueError:
                            check(True)

                        try:
                            plan(argparse.Namespace(**{**vars(args), "start_key": "Ctrl"}))
                            check(False)
                        except ValueError:
                            check(True)
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

    # codeview_record / pdb_signature / match_symbols / capture_symbols: hand-built
    # PE and MSF fixtures, so the byte-level parse is tested without a real PDB.
    with tempfile.TemporaryDirectory() as tmp3:
        fixture = Path(tmp3)
        e_lfanew, opt_header_size = 0x80, 112 + 16 * 8  # PE32+ optional header + 16 data directories
        coff_off, opt_off = e_lfanew + 4, e_lfanew + 4 + 20
        section_off, section_file_offset, section_rva = opt_off + opt_header_size, 0x400, 0x2000
        guid_bytes = struct.pack("<IHH", 0x12345678, 0x9ABC, 0xDEF0) + bytes(
            [0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF])
        expected_guid = "12345678-9ABC-DEF0-0123-456789ABCDEF"
        check(_format_guid(guid_bytes) == expected_guid)

        def build_pe(debug_rva=section_rva, debug_size=28):
            name = b"C:\\fixture\\build\\d3d11.pdb\x00"
            rsds = b"RSDS" + guid_bytes + struct.pack("<I", 7) + name
            debug_entry = struct.pack("<IIHHIIII", 0, 0, 0, 0, IMAGE_DEBUG_TYPE_CODEVIEW, len(rsds),
                                      section_rva + 28, section_file_offset + 28)
            buf = bytearray(max(section_file_offset + 28 + len(rsds), section_off + 40))
            buf[0:2] = b"MZ"
            struct.pack_into("<I", buf, 0x3C, e_lfanew)
            buf[e_lfanew:e_lfanew + 4] = b"PE\x00\x00"
            struct.pack_into("<HHIIIHH", buf, coff_off, 0x8664, 1, 0, 0, 0, opt_header_size, 0)
            struct.pack_into("<H", buf, opt_off, 0x20B)
            struct.pack_into("<II", buf, opt_off + 112 + 6 * 8, debug_rva, debug_size)
            struct.pack_into("<8sIIII", buf, section_off, b".fixture", 0x1000, section_rva, 0x1000,
                             section_file_offset)
            buf[section_file_offset:section_file_offset + 28] = debug_entry
            buf[section_file_offset + 28:section_file_offset + 28 + len(rsds)] = rsds
            return bytes(buf)

        pe_bytes = build_pe()
        pe_path = fixture / "fixture.dll"
        pe_path.write_bytes(pe_bytes)
        check(codeview_record(pe_path) == dict(pdb_name="d3d11.pdb", guid=expected_guid, age=7))

        not_pe = fixture / "not_pe.dll"
        not_pe.write_bytes(b"not a pe file at all, just filler bytes so it is long enough")
        check(codeview_record(not_pe) is None)

        no_debug = fixture / "no_debug.dll"
        no_debug.write_bytes(build_pe(debug_rva=0, debug_size=0))
        check(codeview_record(no_debug) is None)

        try:
            codeview_record(fixture / "missing.dll")
            check(False)
        except OSError:
            check(True)

        def build_msf(age, guid16):
            block_size = 512
            buf = bytearray(6 * block_size)
            buf[0:32] = MSF_MAGIC
            struct.pack_into("<IIIIII", buf, 32, block_size, 1, 6, 16, 0, 3)
            struct.pack_into("<I", buf, 3 * block_size, 4)              # block map -> stream dir in block 4
            struct.pack_into("<III", buf, 4 * block_size, 2, 0, 28)     # numstreams=2, sizes=[0, 28]
            struct.pack_into("<I", buf, 4 * block_size + 12, 5)         # stream 1's blocks = [5]
            struct.pack_into("<III", buf, 5 * block_size, 20000404, 0, age)  # Version, Signature, Age
            buf[5 * block_size + 12:5 * block_size + 28] = guid16
            return bytes(buf)

        good_pdb = fixture / "d3d11.pdb"
        good_pdb.write_bytes(build_msf(7, guid_bytes))
        check(pdb_signature(good_pdb) == dict(guid=expected_guid, age=7))

        wrong_guid_bytes = struct.pack("<IHH", 0x99999999, 0x1111, 0x2222) + bytes(8)
        wrong_pdb = fixture / "wrong.pdb"
        wrong_pdb.write_bytes(build_msf(99, wrong_guid_bytes))
        check(pdb_signature(wrong_pdb) != pdb_signature(good_pdb))

        not_msf = fixture / "not_msf.pdb"
        not_msf.write_bytes(b"not an MSF container" + b"\x00" * 64)
        check(pdb_signature(not_msf) is None)

        try:
            pdb_signature(fixture / "missing.pdb")
            check(False)
        except OSError:
            check(True)

        build_dir = fixture / "build"
        build_dir.mkdir()
        (build_dir / "d3d11.pdb").write_bytes(build_msf(7, guid_bytes))
        target_dir = fixture / "installed"
        target_dir.mkdir()
        installed_graphics = target_dir / "d3d11.dll"
        installed_graphics.write_bytes(pe_bytes)
        other_source = fixture / "other_source"
        other_source.mkdir()

        installed_files = [
            {"key": "graphics", "target": str(installed_graphics),
             "source": str(build_dir / "edvr_openxr_graphics.dll")},
            {"key": "runtime", "target": str(fixture / "missing_runtime.dll"),
             "source": str(other_source / "edvr_openxr_runtime.dll")},
            {"key": "loader", "target": str(target_dir / "openxr_loader.dll"),
             "source": str(build_dir / "openxr_loader.dll")},
        ]
        matches = match_symbols(installed_files, [build_dir])
        check(len(matches) == 2)  # the loader key is not a SYMBOL_KEYS entry
        graphics = next(m for m in matches if m["key"] == "graphics")
        check(graphics["matched"] is True and graphics["pdb"] == str(build_dir / "d3d11.pdb"))
        check(graphics["guid"] == expected_guid and graphics["age"] == 7)
        runtime = next(m for m in matches if m["key"] == "runtime")
        check(runtime["matched"] is False and runtime["pdb"] is None and runtime["guid"] is None)

        # A same-named PDB with the wrong signature must not count as matched.
        wrong_source_dir = fixture / "wrong_source"
        wrong_source_dir.mkdir()
        (wrong_source_dir / "d3d11.pdb").write_bytes(build_msf(99, wrong_guid_bytes))
        wrong_matches = match_symbols(
            [{"key": "graphics", "target": str(installed_graphics),
              "source": str(wrong_source_dir / "edvr_openxr_graphics.dll")}],
            [fixture / "empty_candidate_dir"])
        check(wrong_matches[0]["matched"] is False and wrong_matches[0]["pdb"] is None)
        check(wrong_matches[0]["pdb_name"] == "d3d11.pdb" and wrong_matches[0]["guid"] == expected_guid)

        # capture_symbols: the copy lands under symbols\, status.json records it, and
        # a capture with nothing matched never creates the symbols\ directory at all.
        capture_dir = fixture / "capture"
        capture_dir.mkdir()
        with mock.patch(__name__ + ".ROOT", build_dir.parent):
            copied = capture_symbols(dict(installed_files=installed_files), capture_dir)
        check(len(copied) == 1 and Path(copied[0]).name == "d3d11.pdb")
        check((capture_dir / "symbols" / "d3d11.pdb").is_file())
        check(Path(copied[0]).read_bytes() == (build_dir / "d3d11.pdb").read_bytes())
        status = json.loads((capture_dir / "status.json").read_text(encoding="utf-8"))
        check(status["symbols"]["copied"] == copied and len(status["symbols"]["matches"]) == 2)

        capture_dir2 = fixture / "capture2"
        capture_dir2.mkdir()
        with mock.patch(__name__ + ".ROOT", fixture / "nonexistent_root"):
            copied2 = capture_symbols(dict(installed_files=[]), capture_dir2)
        check(copied2 == [] and not (capture_dir2 / "symbols").exists())

    with tempfile.TemporaryDirectory() as tmp2:
        saved_games = Path(tmp2)
        check(newest_journal(saved_games) is None)
        check(newest_journal(saved_games / "missing") is None)
        old_journal = saved_games / "Journal.2026-01-01T000000.01.log"
        old_journal.write_text("old", encoding="utf-8")
        new_journal = saved_games / "Journal.2026-01-02T000000.02.log"
        new_journal.write_text("new", encoding="utf-8")
        os.utime(old_journal, (1000000, 1000000))
        os.utime(new_journal, (2000000, 2000000))
        check(newest_journal(saved_games) == str(new_journal))

        with mock.patch.dict(os.environ, {"USERPROFILE": tmp2}):
            check(default_status_json() is None)
            frontier_saves = saved_games / "Saved Games" / "Frontier Developments" / "Elite Dangerous"
            frontier_saves.mkdir(parents=True)
            check(default_status_json() is None)
            (frontier_saves / "Status.json").write_text("{}", encoding="utf-8")
            check(default_status_json() == frontier_saves / "Status.json")

        status_path = saved_games / "live_status.json"
        times = iter(["T1", "T2", "T3", "T4"])
        status_path.write_text(json.dumps({"timestamp": "t0", "Flags": 1, "Latitude": 10.5, "Extra": "ignored"}),
                               encoding="utf-8")
        previous, sample = status_sampler_tick(status_path, None, now=lambda: next(times))
        check(sample == {"utc": "T1", "timestamp": "t0", "Flags": 1, "Latitude": 10.5})
        previous2, sample2 = status_sampler_tick(status_path, previous, now=lambda: next(times))
        check(sample2 is None and previous2 == previous)
        status_path.write_text(json.dumps({"timestamp": "t1", "Flags": 2}), encoding="utf-8")
        previous3, sample3 = status_sampler_tick(status_path, previous2, now=lambda: next(times))
        check(sample3 == {"utc": "T2", "timestamp": "t1", "Flags": 2})
        status_path.write_text('{"timestamp": "t2", "Flags"', encoding="utf-8")  # a partial write mid-rewrite
        previous4, sample4 = status_sampler_tick(status_path, previous3, now=lambda: next(times))
        check(sample4 is None and previous4 == previous3)
        missing_previous, missing_sample = status_sampler_tick(saved_games / "absent.json", previous3,
                                                                now=lambda: next(times))
        check(missing_sample is None and missing_previous == previous3)

        out_path = saved_games / "status_samples.jsonl"
        status_path.write_text(json.dumps({"timestamp": "t0", "Flags": 0}), encoding="utf-8")
        stop_event = threading.Event()
        ticks = [0]
        loop_times = iter(["A", "B", "C", "D"])

        def fake_sleep(interval):
            ticks[0] += 1
            if ticks[0] in (1, 2):
                status_path.write_text(json.dumps({"timestamp": "t1", "Flags": 1}), encoding="utf-8")
            else:
                stop_event.set()

        run_status_sampler(status_path, out_path, stop_event, interval=0.25, sleep=fake_sleep,
                           now=lambda: next(loop_times))
        lines = [json.loads(line) for line in out_path.read_text(encoding="utf-8").splitlines()]
        check(lines == [{"utc": "A", "timestamp": "t0", "Flags": 0}, {"utc": "B", "timestamp": "t1", "Flags": 1}])

    with mock.patch("sys.stderr", io.StringIO()):
        try:
            main(["--capture", "--dry-run", "--max-seconds", "301"])
            check(False)
        except SystemExit as exc:
            check(exc.code == 2)
    with mock.patch("sys.stderr", io.StringIO()):
        try:
            main(["--capture", "--dry-run", "--start-after-seconds", "0"])
            check(False)
        except SystemExit as exc:
            check(exc.code == 2)
    profile = ET.parse(PROFILE).getroot()
    memory = profile.find("./Profiles/Profile[@Id='EDVRCPU.Verbose.Memory']")
    check(memory is not None and memory.attrib.get("LoggingMode") == "Memory")
    file_profile = profile.find("./Profiles/Profile[@Id='EDVRCPU.Verbose.File']")
    check(file_profile is not None and file_profile.attrib.get("LoggingMode") == "File")
    file_buffers = profile.find("./Profiles/SystemCollector[@Id='EDVRCPUFileSystemCollector']/Buffers")
    check(file_buffers is not None and int(file_buffers.attrib["Value"]) >= 256)
    keywords = {element.attrib["Value"] for element in profile.findall("./Profiles/SystemProvider/Keywords/Keyword")}
    stacks = {element.attrib["Value"] for element in profile.findall("./Profiles/SystemProvider/Stacks/Stack")}
    check({"CSwitch", "ReadyThread", "SampledProfile", "Loader", "ProcessThread"}.issubset(keywords))
    check({"CSwitch", "ReadyThread", "SampledProfile"}.issubset(stacks))
    provider = profile.find("./Profiles/EventProvider")
    check(provider is not None and provider.attrib["Name"].upper() == "D3885FA1-0B70-44F1-AF88-63B2012B111E")
    report = dict(coverageComplete=True, analyzedFrameCount=60, eventsLost=0,
                  applicationGpuCompletionCoverage=dict(markerCount=60, explicitCpuMappings=60,
                      legacyCpuFrames=0, unavailableCpuMappings=0, schemaErrors=0, invalidMarkers=0,
                      duplicateGpuSequences=0, ambiguousCpuSequences=0, gpuSequencesWithoutCpuWitness=0,
                      eventsLost=0, cycleStatuses={"valid": 60}), frames=[
        dict(sequence=i, startUs=i * 100000, endUs=(i + 1) * 100000,
             cpuMarkerVersion=2, gpuSequence=100000+i,
             applicationGpu=dict(status="valid", valid=True, producerSequence=100000+i,
                                 source=1, reason=0, ms=9.0+((i-1) % 3)*0.1,
                                 publicationUs=7000000+i, publicationQpc=70000000+i*10),
             runningUs=100000 if i <= 30 else 0, waitingUs=0 if i <= 30 else 100000,
             sampleStackCount=10 if i <= 30 else 0) for i in range(1, 61)])
    check(validate_smoke_report(report)["busy_running_ms"] == 3000)
    check(validate_smoke_report(report)["synthetic_gpu_samples"] == 60)
    for field in ("markerCount", "explicitCpuMappings", "legacyCpuFrames", "unavailableCpuMappings",
                  "schemaErrors", "invalidMarkers", "duplicateGpuSequences", "ambiguousCpuSequences",
                  "gpuSequencesWithoutCpuWitness", "eventsLost"):
        broken = json.loads(json.dumps(report))
        broken["applicationGpuCompletionCoverage"][field] += 1
        try:
            validate_smoke_report(broken)
            check(False)
        except ValueError:
            check(True)
    for field, value in (("cpuMarkerVersion", 1), ("gpuSequence", 1), ("applicationGpu", {})):
        broken = json.loads(json.dumps(report)); broken["frames"][0][field] = value
        try:
            validate_smoke_report(broken)
            check(False)
        except ValueError:
            check(True)
    for field, value in (("producerSequence", 1), ("status", "missing_sequence"), ("valid", False),
                         ("source", 0), ("reason", 5), ("ms", 9.1), ("ms", float("nan")),
                         ("publicationQpc", 0), ("publicationUs", 0)):
        broken = json.loads(json.dumps(report)); broken["frames"][0]["applicationGpu"][field] = value
        try:
            validate_smoke_report(broken)
            check(False)
        except ValueError:
            check(True)
    for field in ("applicationGpuCompletionCoverage", "eventsLost"):
        broken = json.loads(json.dumps(report)); del broken[field]
        try:
            validate_smoke_report(broken)
            check(False)
        except ValueError:
            check(True)
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
    parser.add_argument("--memory-ring", action="store_true",
                        help="use the legacy 512x1MiB memory-ring WPR profile instead of file mode")
    parser.add_argument("--gpu", action="store_true",
                        help="add the built-in GPU profile to the same private file-mode session; "
                             "provider counts are coverage only, not GPU busy time")
    parser.add_argument("--start-key", help="hotkey (F1-F12, or a letter/digit) that arms the WPR start")
    parser.add_argument("--stop-key", help="hotkey (F1-F12, or a letter/digit) that ends the flight leg early")
    parser.add_argument("--start-after-seconds", type=float,
                        help="arm delay in seconds; races --start-key when both are set")
    parser.add_argument("--status-json", help="Status.json to sample at 4 Hz while recording; "
                        "defaults to the Frontier Saved Games copy when it exists")
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
    if not 1 <= args.wait_seconds <= 3600 or not 1 <= args.max_seconds <= 300 or (
            args.start_after_seconds is not None and args.start_after_seconds <= 0):
        parser.error("wait-seconds must be 1..3600, max-seconds must be 1..300, "
                     "and start-after-seconds must be positive")
    try:
        return capture(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print("[edvr] ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
