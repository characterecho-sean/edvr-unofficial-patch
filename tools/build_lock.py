"""EDVR's one-build-at-a-time guard (AGENTS.md "One build at a time").

build.bat wraps its own top-level invocation in --acquire/--release so two
full builds never contend for the same CPU cores on this machine -- the
documented, observed cause of the vtable_test timing gate's flakes under
load. A second invocation is refused with a message naming how long the
first has been running and how to actually wait for it (--wait), rather
than every calling agent hand-rolling its own tasklist-polling loop, which
is exactly the drift this tool exists to end.

Rig children ("build.bat --rig <label>", tools\\run_jobs.py's own pool) run
concurrently by design under the parent's own lock and never call this.

How the lock works (architecture review 2026-09-29, I-6):

  * It is created exclusively (O_CREAT|O_EXCL), so of any number of builds
    starting together exactly one wins.
  * It names its OWNER: this helper's parent process, the cmd.exe running
    build.bat, which is alive for the whole build (the helper itself exits
    at once), with that process's creation time so a reused pid cannot pass
    for it.
  * A lock is STALE, and the next --acquire (or --wait) takes it, when
      - its owner is no longer running: a build that was killed, or whose
        terminal was closed, blocks nobody, however young the lock;
      - its owner is this very shell: cmd runs batch files one at a time, so
        a build starting in the shell that took the lock means the earlier
        build there is over (Ctrl+C then Y skips build.bat's own release and
        leaves the shell open);
      - it has stood for 30 minutes (STALE_SECONDS), whether or not its
        owner still runs. A full build takes minutes and every rig has a
        timeout, so a lock that old is an abandoned one: the ceiling that
        keeps a shell left open from blocking every build on the machine.
    Otherwise it is HELD.
  * A lock with no owner fields (written by an older copy of this tool,
    which other worktrees keep running until they merge main) is judged by
    the age rule alone.
  * A stale lock is taken down with an atomic rename to a tombstone named
    after the lock's own bytes, so however many builds find it stale in the
    same instant, exactly one removes it and none removes a fresh lock.
  * --release removes the lock only for its owner; anyone else gets a
    warning and the file is left alone.
  * The file stays readable by the older copies: "started", "note" and "pid"
    keep their meaning ("pid" is the helper's own, long gone by the time
    anyone reads it); the owner is in the extra fields "owner_pid" and
    "owner_created" (a Windows FILETIME), which older copies ignore.

  python tools\\build_lock.py --acquire [--note TEXT] [--dry-run]
  python tools\\build_lock.py --release [--dry-run]
  python tools\\build_lock.py --wait [--timeout SECONDS]
  python tools\\build_lock.py --self-test

--lock-file names another lock file (the default is %TEMP%\\edvr_build.lock).
--dry-run reports what --acquire or --release would do and writes nothing.
"""

import argparse
import contextlib
import hashlib
import io
import json
import ntpath
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# Generous past the ~2-4 minutes an observed full build takes (every rig now
# times out within 15 minutes), so a machine under heavy multi-agent load
# never reclaims a lock out from under a build that is genuinely still
# running -- Sean's own manual polling used a 20 minute cap; this is that
# plus margin. It is the whole rule for a lock that names no owner (an older
# copy of this tool wrote it, or the owner could not be established), and
# the ceiling for one whose owner is running: past it the lock is abandoned,
# a shell left open by an interrupted build included. A lock whose owner is
# gone, or is the shell asking, is stale however young.
STALE_SECONDS = 30 * 60
# A lock file that is still empty or unparseable this young is being
# written by the acquirer that just created it.
GRACE_SECONDS = 5.0
# How often --wait looks.
POLL_SECONDS = 5.0
# One acquire settles in a handful of steps; this bounds the pathological
# case of a file that keeps changing under it.
MAX_ATTEMPTS = 60
# How long a tombstone is kept (see _take_down for why it is kept at all).
TOMBSTONE_KEEP_SECONDS = 60 * 60

FREE, HELD, STALE = "free", "held", "stale"
ALIVE, DEAD, UNKNOWN = "alive", "dead", "unknown"

# Image names of the things a "python" command can be that start the real
# interpreter as a child and exit with it (a venv's python.exe redirector,
# the py launcher).
LAUNCHER_IMAGES = {"python.exe", "pythonw.exe", "py.exe", "pyw.exe"}

_QUERY_LIMITED_INFORMATION = 0x1000
_SYNCHRONIZE = 0x00100000
_WAIT_OBJECT_0 = 0
_WAIT_TIMEOUT = 0x102
_ERROR_ACCESS_DENIED = 5
_ERROR_INVALID_PARAMETER = 87

_AUTO = object()          # "work the owner out for yourself"; None means "no owner"
_K32 = []
_K32_LOCK = threading.Lock()


def lock_path():
    base = os.environ.get("TEMP") or os.environ.get("TMP") or tempfile.gettempdir()
    return Path(base) / "edvr_build.lock"


# --------------------------------------------------------------------------
# Who is running: process identity on Windows.
# --------------------------------------------------------------------------

def _kernel32():
    """kernel32 with the prototypes this tool uses (a HANDLE is 64 bits wide
    and ctypes' default int return would truncate it), or None off Windows."""
    if sys.platform != "win32":
        return None
    with _K32_LOCK:
        if _K32:
            return _K32[0]
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        kernel.GetProcessTimes.restype = wintypes.BOOL
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype = wintypes.BOOL
        kernel.QueryFullProcessImageNameW.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        _K32.append(kernel)
        return kernel


def probe_process(pid):
    """(state, created) for process `pid`: ALIVE, DEAD or UNKNOWN, and its
    creation time as a FILETIME integer (100 ns ticks since 1601) when it
    could be read. A process that has exited is DEAD even while something
    still holds a handle to it, which keeps its pid opening fine."""
    kernel = _kernel32()
    if kernel is None or not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return UNKNOWN, None
    import ctypes
    from ctypes import wintypes
    can_wait = True
    handle = kernel.OpenProcess(_QUERY_LIMITED_INFORMATION | _SYNCHRONIZE, False, pid)
    if not handle and ctypes.get_last_error() == _ERROR_ACCESS_DENIED:
        can_wait = False
        handle = kernel.OpenProcess(_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        # ERROR_INVALID_PARAMETER is "no such process"; anything else
        # (access denied to a protected process, say) is "cannot tell".
        return (DEAD if ctypes.get_last_error() == _ERROR_INVALID_PARAMETER else UNKNOWN), None
    try:
        created = None
        times = [wintypes.FILETIME() for _ in range(4)]
        if kernel.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        if can_wait:
            status = kernel.WaitForSingleObject(handle, 0)
            state = ALIVE if status == _WAIT_TIMEOUT else DEAD if status == _WAIT_OBJECT_0 else UNKNOWN
        else:
            code = wintypes.DWORD()
            if kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
                state = ALIVE if code.value == 259 else DEAD      # STILL_ACTIVE
            else:
                state = UNKNOWN
        return state, created
    finally:
        kernel.CloseHandle(handle)


def owner_state(pid, created):
    """Is the process a lock names as its owner still that process? DEAD
    when nothing runs under the pid or when what runs under it was created
    at another time (the pid was reused)."""
    state, actual = probe_process(pid)
    if state != ALIVE:
        return state
    if actual is None:
        return UNKNOWN
    return ALIVE if actual == created else DEAD


def image_path(pid):
    """The full path of the executable process `pid` runs, or None."""
    kernel = _kernel32()
    if kernel is None:
        return None
    import ctypes
    from ctypes import wintypes
    handle = kernel.OpenProcess(_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        size = wintypes.DWORD(1024)
        buffer = ctypes.create_unicode_buffer(size.value)
        if kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return buffer.value
        return None
    finally:
        kernel.CloseHandle(handle)


def is_launcher_shim(parent_image, own_image):
    """True when the parent process is a Python launcher standing between the
    shell and the real interpreter (a venv's python.exe redirector, the py
    launcher): it exits the moment we do, so it cannot own a build. It is
    told from a python that legitimately runs this tool by running a
    different executable than ours."""
    if not parent_image:
        return False
    if ntpath.basename(parent_image).lower() not in LAUNCHER_IMAGES:
        return False
    return ntpath.normcase(ntpath.normpath(parent_image)) != \
        ntpath.normcase(ntpath.normpath(own_image or ""))


def find_owner():
    """(owner, why_not): owner is (pid, creation time) of this helper's
    parent -- the cmd.exe running build.bat -- or None, with the reason,
    when that cannot be established: not Windows, the parent already gone
    (its pid may belong to a stranger by now), or a parent that is only a
    launcher and exits with us. The lock is then written without owner
    fields and the 30 minute rule applies to it."""
    if _kernel32() is None:
        return None, "process identity needs Windows"
    ppid = os.getppid()
    state, created = probe_process(ppid)
    if state != ALIVE or created is None:
        return None, "the parent process (pid %d) is not running" % ppid
    _, own_created = probe_process(os.getpid())
    if own_created is not None and created > own_created:
        return None, "pid %d was reused after our parent exited" % ppid
    if is_launcher_shim(image_path(ppid), image_path(os.getpid())):
        return None, "the parent process (pid %d) is a Python launcher that exits with us" % ppid
    return (ppid, created), None


def _resolve_owner(owner, say=True):
    """`owner` as given, or (_AUTO) this helper's parent; None when it cannot
    be established, said on stderr unless `say` is off."""
    if owner is not _AUTO:
        return owner
    owner, why_not = find_owner()
    if owner is None and say:
        print("[edvr] NOTE: the build lock cannot track its owner here (%s); a killed "
              "build will hold it for up to %d min." % (why_not, STALE_SECONDS // 60),
              file=sys.stderr)
    return owner


# --------------------------------------------------------------------------
# The lock file.
# --------------------------------------------------------------------------

class _Lock:
    """One reading of the lock file: its bytes, the JSON object they parse to
    (None when they do not parse to one), and its modification time."""
    __slots__ = ("raw", "info", "mtime", "mtime_ns")

    def __init__(self, raw, info, mtime, mtime_ns):
        self.raw, self.info, self.mtime, self.mtime_ns = raw, info, mtime, mtime_ns


def _retry(operation, attempts=40, delay=0.025):
    """Run `operation`, retrying briefly on PermissionError: on Windows a file
    another process has open for a moment (this tool's own readers, which do
    not share it for deletion) cannot be renamed or removed."""
    for attempt in range(attempts):
        try:
            return operation()
        except PermissionError:
            if attempt == attempts - 1:
                raise
            time.sleep(delay)


def _read(path):
    """The lock file as a _Lock, or None when there is none."""
    def load():
        with open(str(path), "rb") as stream:
            return stream.read(), os.fstat(stream.fileno())
    try:
        raw, stat = _retry(load)
    except FileNotFoundError:
        return None
    try:
        info = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        info = None
    if not isinstance(info, dict):
        info = None
    return _Lock(raw, info, stat.st_mtime, stat.st_mtime_ns)


def _owner_of(info):
    """(pid, creation time) a lock names, or None for one without valid owner
    fields (an older copy of this tool wrote it, or it is unreadable)."""
    if not info:
        return None
    pid, created = info.get("owner_pid"), info.get("owner_created")
    if all(isinstance(value, int) and not isinstance(value, bool) for value in (pid, created)):
        return pid, created
    return None


def _started(info):
    value = (info or {}).get("started")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    return 0.0


def _minutes(info, now=None):
    started = _started(info)
    return max(0, int(((time.time() if now is None else now) - started) // 60)) if started else 0


def assess(lock, now=None, caller=None, alive=None):
    """(FREE | HELD | STALE, why) for a lock as _read returned it (None: no
    file). HELD is the one verdict that makes an acquirer stop.

    `caller` is the (pid, creation time) of the shell asking, None when it is
    not known. `alive` says whether an owner still runs, (pid, creation time)
    -> ALIVE | DEAD | UNKNOWN; it defaults to owner_state. With `now`, these
    are what the self-test drives it through without a clock or a process."""
    now = time.time() if now is None else now
    alive = owner_state if alive is None else alive
    if lock is None:
        return FREE, "there is no lock"
    if lock.info is None:
        if now - lock.mtime < GRACE_SECONDS:
            return HELD, "the lock file is still being written"
        return STALE, "the lock file is empty or unreadable"
    age = now - _started(lock.info)
    ceiling = "%d min" % (STALE_SECONDS // 60)
    owner = _owner_of(lock.info)
    if owner is None:
        if age < STALE_SECONDS:
            return HELD, "it names no owner, so it stands until %s after it was taken" % ceiling
        return STALE, "no release within %s" % ceiling
    if owner == caller:
        # cmd runs one batch file at a time and this shell is now running
        # another: the build that took the lock here is over, and Ctrl+C
        # then Y (which skips build.bat's release) is how it ended.
        return STALE, "the previous build in this shell must have been interrupted"
    state = alive(*owner)
    if state == DEAD:
        return STALE, "owner pid %d is no longer running" % owner[0]
    what = ("owner pid %d is running" % owner[0] if state == ALIVE
            else "it cannot be told whether owner pid %d is running" % owner[0])
    if age < STALE_SECONDS:
        # UNKNOWN is judged by the same age, the safe rule.
        return HELD, what + ("" if state == ALIVE else ", so it stands until %s after it was taken" % ceiling)
    return STALE, "%s, but the lock is over %s old: no build runs that long, so it was abandoned" % (what, ceiling)


def _record(note, owner):
    record = {"started": time.time(), "note": note or "", "pid": os.getpid()}
    if owner is not None:
        record["owner_pid"], record["owner_created"] = owner
    return record


def _create(path, record):
    """Create the lock file holding `record` -- only if there is none: of any
    number of processes calling this at once exactly one gets True. The
    bytes go down in one write, so a reader sees nothing or all of it (an
    empty file a reader does catch is the GRACE_SECONDS case in assess)."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    try:
        descriptor = os.open(str(path), flags, 0o644)
    except FileExistsError:
        return False
    except PermissionError:
        # A file another process is deleting reads as "access denied" until
        # its last handle closes; it is not ours to create yet.
        time.sleep(0.02)
        return False
    try:
        os.write(descriptor, json.dumps(record).encode("utf-8"))
    except BaseException:
        os.close(descriptor)
        with contextlib.suppress(OSError):
            os.unlink(str(path))
        raise
    os.close(descriptor)
    return True


def _tombstone_path(path, lock):
    """The name a stale lock is taken down to. It is a function of the lock's
    own bytes and modification time, not of who takes it down."""
    tag = hashlib.sha256(lock.raw + b"|" + str(lock.mtime_ns).encode("ascii")).hexdigest()[:16]
    return path.with_name("%s.stale-%s" % (path.name, tag))


def _take_down(path, lock):
    """Remove the stale lock `lock` (as _read returned it), so it goes exactly
    once however many acquirers found it stale at the same moment. True when
    this call removed it.

    Removal is a rename to a tombstone, atomic on one volume, and the
    tombstone's name comes from the lock's own bytes (_tombstone_path). Every
    acquirer that read the same stale lock therefore aims at the same name,
    and Windows refuses a rename onto an existing name: the first wins, the
    others are told "already taken down" even when a fresh lock has been
    created at `path` in the meantime, which is the case a rename to a
    unique name would get wrong (it would move the fresh lock aside). The
    tombstone is kept (TOMBSTONE_KEEP_SECONDS) for that reason.

    Should `path` hold something else by the time the rename lands (the
    stale lock was removed some other way, and its name is not taken yet),
    the bytes are compared after the fact and the file is put back."""
    tomb = _tombstone_path(path, lock)
    try:
        os.rename(str(path), str(tomb))
    except (FileNotFoundError, FileExistsError):
        return False
    except PermissionError:
        time.sleep(0.02)          # a reader has it open for a moment; look again
        return False
    with contextlib.suppress(OSError):
        os.utime(str(tomb), None)     # the tombstone's age counts from now
    moved = _read(tomb)
    if moved is not None and moved.raw == lock.raw:
        return True
    with contextlib.suppress(OSError):
        os.rename(str(tomb), str(path))     # not what we judged: put it back
    return False


def _sweep_tombstones(path, now=None):
    """Delete tombstones older than TOMBSTONE_KEEP_SECONDS. Run once the lock
    is ours, when no take-down of a lock we could have seen is in flight."""
    cutoff = (time.time() if now is None else now) - TOMBSTONE_KEEP_SECONDS
    try:
        tombs = list(path.parent.glob(path.name + ".stale-*"))
    except OSError:
        return
    for tomb in tombs:
        with contextlib.suppress(OSError):
            if tomb.stat().st_mtime < cutoff:
                tomb.unlink()


# --------------------------------------------------------------------------
# The three operations.
# --------------------------------------------------------------------------

def _refuse(path, lock, why):
    info = lock.info or {}
    note = info.get("note") if isinstance(info.get("note"), str) and info.get("note") else "no note"
    print(
        "[edvr] ERROR: another build is already running (%s, started %d min ago; %s). "
        "Wait for it to finish, then run build.bat again -- two builds at once "
        "contend for the same cores, which is how the vtable_test timing gate has "
        "flaked before. Run \"python tools\\build_lock.py --wait\" to block until "
        "it is free, then retry. The lock frees itself when that build's shell "
        "exits, when a build starts in the shell that took it (an interrupted build "
        "there is over), and %d min after it was taken. (Lock file: %s)"
        % (note, _minutes(info), why, STALE_SECONDS // 60, path),
        file=sys.stderr)


def acquire(path, note, owner=_AUTO, dry_run=False, alive=None):
    """Take the lock, or refuse (return 1) when a build holds it. Takes over a
    stale one, an interrupted build's in this very shell included. With
    dry_run, says which of those would happen and writes nothing -- no lock,
    no tombstone. `alive` is assess's (the self-test's seam)."""
    owner = _resolve_owner(owner)
    record = _record(note, owner)
    took_over = None
    for attempt in range(MAX_ATTEMPTS):
        if not dry_run and _create(path, record):
            if took_over:
                lock, why = took_over
                print("[edvr] took over a stale build lock (%s; %s, started %d min ago)." %
                      (why, (lock.info or {}).get("note") or "no note", _minutes(lock.info)))
            _sweep_tombstones(path)
            return 0
        lock = _read(path)
        if lock is None:
            if dry_run:
                print("[edvr] dry run: the build lock is free; --acquire would take it.")
                return 0
            continue              # released between our create and our read
        verdict, why = assess(lock, caller=owner, alive=alive)
        if verdict == HELD:
            _refuse(path, lock, why)
            return 1
        if dry_run:
            print("[edvr] dry run: the build lock is stale (%s); --acquire would take it over." % why)
            return 0
        if _take_down(path, lock):
            took_over = (lock, why)
        else:
            time.sleep(min(0.01 * (attempt + 1), 0.1))
    print("[edvr] ERROR: could not settle the build lock %s after %d tries; run again." %
          (path, MAX_ATTEMPTS), file=sys.stderr)
    return 1


def release(path, owner=_AUTO, dry_run=False):
    """Drop the lock, if it is this build's. Someone else's is left alone with
    a warning. Always returns 0: build.bat calls this on the way out and a
    lock that is not ours is not a reason to fail the build."""
    owner = _resolve_owner(owner)
    lock = _read(path)
    if lock is None:
        return 0
    if _owner_of(lock.info) != owner:
        info = lock.info or {}
        print("[edvr] WARNING: not releasing the build lock %s: it belongs to another build "
              "(%s, started %d min ago), not to this one; leaving it." %
              (path, info.get("note") or "no note", _minutes(info)), file=sys.stderr)
        return 0
    if dry_run:
        print("[edvr] dry run: --release would remove the build lock.")
        return 0
    try:
        _retry(lambda: os.unlink(str(path)))
    except FileNotFoundError:
        pass
    except PermissionError as error:
        print("[edvr] WARNING: could not remove the build lock %s: %s" % (path, error),
              file=sys.stderr)
    return 0


def wait(path, timeout, poll=None, owner=_AUTO, alive=None, clock=time.time, sleep=time.sleep):
    """Block until the lock is free or stale -- a lock this very shell took
    is stale, so waiting on your own interrupted build ends at once -- then
    return 0; 1 after `timeout` seconds (0: no limit). `clock` and `sleep`
    are the self-test's seam, as `alive` is assess's: it drives half an hour
    of polling without waiting for any of it."""
    poll = POLL_SECONDS if poll is None else poll
    caller = _resolve_owner(owner, say=False)
    deadline = clock() + timeout if timeout else None
    while True:
        if assess(_read(path), now=clock(), caller=caller, alive=alive)[0] != HELD:
            return 0
        pause = poll
        if deadline is not None:
            remaining = deadline - clock()
            if remaining <= 0:
                print("[edvr] ERROR: still locked after %ds; giving up." % timeout,
                      file=sys.stderr)
                return 1
            pause = min(poll, remaining)
        sleep(pause)


# --------------------------------------------------------------------------
# Self-test.
# --------------------------------------------------------------------------

# Owner ids no real process has. The thread races give every thread one of its
# own, and an `alive` that reads them as running, so each thread is a separate
# shell (a lock the very shell asking took counts as abandoned: see assess).
_FAKE_PID = 1 << 30

# A stand-in for one cmd.exe: a process that stays up and answers, per request
# line on stdin (a JSON object), "<op> <exit code>":
#   acquire | release   runs this tool on the request's lock as its child, as
#                       cmd.exe runs build.bat's helper, so the lock's owner is
#                       the stand-in
#   race                calls acquire itself, as its own owner, once the clock
#                       passes "start", so every stand-in starts together
_SHELL = (
    "import contextlib, io, json, os, subprocess, sys, time\n"
    "sys.path.insert(0, sys.argv[1])\n"
    "import build_lock\n"
    "from pathlib import Path\n"
    "me = (os.getpid(), build_lock.probe_process(os.getpid())[1])\n"
    "print('ready', os.getpid(), flush=True)\n"
    "for line in sys.stdin:\n"
    "    request = json.loads(line)\n"
    "    op, lock = request['op'], request['lock']\n"
    "    if op == 'race':\n"
    "        while time.time() < request['start']:\n"
    "            pass\n"
    "        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):\n"
    "            rc = build_lock.acquire(Path(lock), 'racer', owner=me)\n"
    "    else:\n"
    "        rc = subprocess.run([sys.executable, build_lock.__file__, '--' + op, '--lock-file', lock,\n"
    "                             '--note', 'stand-in'], stdout=subprocess.DEVNULL,\n"
    "                            stderr=subprocess.DEVNULL).returncode\n"
    "    print(op, rc, flush=True)\n")


class _StandIn:
    """One stand-in cmd.exe (see _SHELL)."""

    def __init__(self, tools_dir):
        self.process = subprocess.Popen(
            [sys.executable, "-c", _SHELL, tools_dir], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, text=True, bufsize=1)
        self.pid = self.process.pid       # until it says: a venv's launcher is not the interpreter
        self.started = False

    def ready(self):
        """Block until it has imported this module and is reading requests;
        `pid` is then the pid of the process that will own the locks."""
        if not self.started:
            words = self.process.stdout.readline().split()
            if len(words) == 2 and words[0] == "ready":
                self.pid = int(words[1])
            self.started = True

    def send(self, op, lock, **more):
        self.ready()
        self.process.stdin.write(json.dumps(dict(more, op=op, lock=str(lock))) + "\n")
        self.process.stdin.flush()

    def reply(self):
        """(op, exit code) of its next answer; (None, None) if it is gone."""
        words = self.process.stdout.readline().split()
        return (words[0], int(words[1])) if len(words) == 2 else (None, None)

    def ask(self, op, lock):
        self.send(op, lock)
        return self.reply()

    def close(self):
        """End it -- as a build's shell ends, without a release -- and wait."""
        with contextlib.suppress(OSError, ValueError):
            self.process.stdin.close()
        with contextlib.suppress(subprocess.TimeoutExpired):
            self.process.wait(30)
        with contextlib.suppress(OSError, ValueError):
            self.process.stdout.close()


def self_test():
    failures = []

    def check(condition, why):
        if not condition:
            failures.append(why)

    tool = Path(__file__).resolve()
    tools_dir = str(tool.parent)
    windows = _kernel32() is not None

    # The stand-in shells need a moment to start (each imports this module).
    # Starting them first lets the in-process cases below cover it.
    shells = [_StandIn(tools_dir) for _ in range(4)] if windows else []

    def stop_hung_shells():
        for shell in shells:
            with contextlib.suppress(OSError):
                shell.process.kill()

    watchdog = threading.Timer(90, stop_hung_shells)      # a hang fails the test, not the build
    watchdog.daemon = True
    watchdog.start()

    def quiet(function, *args, **kwargs):
        """(result, stdout, stderr) of a call: the tool's messages are its
        output, not the test's."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            result = function(*args, **kwargs)
        return result, out.getvalue(), err.getvalue()

    def tick(seconds=0.0):
        return time.time() - seconds

    def put(path, record, age=0.0):
        path.write_text(json.dumps(record), encoding="utf-8")
        if age:
            os.utime(str(path), (tick(age), tick(age)))

    def listing(path):
        return sorted(entry.name for entry in path.parent.iterdir())

    def clocked():
        """(clock, sleep, naps): a clock that starts at the real time and
        moves only when `sleep` is called, so wait() can be taken through half
        an hour of polling without any of it being waited for."""
        now, naps = [time.time()], []

        def sleep(seconds):
            naps.append(seconds)
            now[0] += seconds

        return (lambda: now[0]), sleep, naps

    with tempfile.TemporaryDirectory(prefix="edvr-build-lock-") as scratch:
        counter = [0]

        def new_lock():
            counter[0] += 1
            folder = Path(scratch) / ("case%d" % counter[0])
            folder.mkdir()
            return folder / "edvr_build.lock"

        def tombstones(path):
            return [name for name in listing(path) if ".stale-" in name]

        def waited(path, timeout, **more):
            """(exit code, seconds) of a wait() on a clock of its own: the
            seconds are those it would have spent polling."""
            clock, sleep, naps = clocked()
            rc = quiet(wait, path, timeout, 5, clock=clock, sleep=sleep, **more)[0]
            return rc, sum(naps)

        # --- the plain cycle, on a lock with no owner tracking ---------------
        path = new_lock()
        rc, _, _ = quiet(acquire, path, "first", owner=None)
        check(rc == 0 and path.is_file(), "a free lock must be acquired")
        record = json.loads(path.read_text(encoding="utf-8"))
        check(isinstance(record.get("started"), float) and record.get("note") == "first"
              and record.get("pid") == os.getpid(),
              "the fields older copies read (started, note, pid) are written as before: %r" % record)
        before = path.read_bytes()
        rc, _, err = quiet(acquire, path, "second", owner=None)
        check(rc == 1 and "another build is already running" in err
              and "build_lock.py --wait" in err and "first" in err,
              "a held lock refuses a second acquire and says how to wait: %r" % err)
        check("frees itself when that build's shell exits" in err and "interrupted build" in err
              and "and %d min after it was taken" % (STALE_SECONDS // 60) in err,
              "the refusal says when a lock frees itself: %r" % err)
        check(path.read_bytes() == before, "a refused acquire leaves the lock alone")
        check(quiet(release, path, owner=None)[0] == 0 and not path.exists(),
              "release removes the lock")
        check(quiet(release, path, owner=None)[0] == 0, "releasing a clear lock is a no-op")
        check(waited(path, 60, owner=None) == (0, 0), "wait returns at once when nothing holds the lock")

        # --- locks written by an older copy of this tool ---------------------
        old_fresh = {"started": tick(60), "note": "old build", "pid": 4242}
        old_stale = {"started": tick(STALE_SECONDS + 60), "note": "old build", "pid": 4242}
        path = new_lock()
        put(path, old_fresh)
        before = path.read_bytes()
        rc, _, err = quiet(acquire, path, "new", owner=None)
        check(rc == 1 and "old build" in err and path.read_bytes() == before,
              "a fresh old-format lock is still refused: %r" % err)
        rc, seconds = waited(path, 60, owner=None)
        check(rc == 1 and 59 <= seconds <= 61,
              "wait keeps waiting on a fresh old-format lock, then gives up: %r %r" % (rc, seconds))
        put(path, dict(old_fresh, started=tick(STALE_SECONDS - 120)))
        rc, seconds = waited(path, 600, owner=None)
        check(rc == 0 and 100 <= seconds <= 130,
              "wait returns when the 30 minute rule frees an old-format lock: %r %r" % (rc, seconds))
        path = new_lock()
        put(path, old_stale)
        rc, out, _ = quiet(acquire, path, "new", owner=None)
        check(rc == 0 and json.loads(path.read_text(encoding="utf-8"))["note"] == "new"
              and "took over" in out and len(tombstones(path)) == 1,
              "a stale old-format lock (past 30 min) is taken over: %r %r" % (rc, out))
        for label, odd in (("no started field", {"note": "x"}),
                           ("a started that is not a number", {"started": "soon"}),
                           ("an empty object", {})):
            path = new_lock()
            put(path, odd)
            check(quiet(acquire, path, "new", owner=None)[0] == 0,
                  "a lock with %s reads as abandoned, as it always did" % label)
        path = new_lock()
        put(path, dict(old_fresh, owner_pid="12", owner_created="soon"))
        check(quiet(acquire, path, "new", owner=None)[0] == 1,
              "owner fields of the wrong type are ignored: the age rule applies (fresh: refused)")
        put(path, dict(old_stale, owner_pid="12", owner_created="soon"))
        check(quiet(acquire, path, "new", owner=None)[0] == 0,
              "owner fields of the wrong type are ignored: the age rule applies (old: taken over)")
        path = new_lock()
        put(path, dict(old_fresh, future_field=[1, 2, 3], other={"a": 1}))
        check(quiet(acquire, path, "new", owner=None)[0] == 1,
              "fields this version has never heard of are tolerated")

        # --- an unreadable lock file ------------------------------------------
        path = new_lock()
        path.write_bytes(b"")
        rc, _, err = quiet(acquire, path, "new", owner=None)
        check(rc == 1 and "being written" in err, "a young empty lock file is an acquirer mid-write: %r" % err)
        os.utime(str(path), (tick(60), tick(60)))
        check(quiet(acquire, path, "new", owner=None)[0] == 0, "an old empty lock file is stale")
        path = new_lock()
        path.write_bytes(b"{not json")
        os.utime(str(path), (tick(60), tick(60)))
        check(quiet(acquire, path, "new", owner=None)[0] == 0, "an old garbled lock file is stale")

        # --- dry run writes nothing --------------------------------------------
        path = new_lock()
        rc, out, _ = quiet(acquire, path, "x", owner=None, dry_run=True)
        check(rc == 0 and listing(path) == [] and "would take it" in out,
              "a dry-run acquire of a free lock writes nothing: %r %r" % (rc, listing(path)))
        put(path, old_stale)
        seen = (listing(path), path.read_bytes(), path.stat().st_mtime_ns)
        rc, out, _ = quiet(acquire, path, "x", owner=None, dry_run=True)
        check(rc == 0 and (listing(path), path.read_bytes(), path.stat().st_mtime_ns) == seen
              and "would take it over" in out,
              "a dry-run acquire of a stale lock removes and writes nothing (no tombstone): %r" % listing(path))
        put(path, old_fresh)
        seen = (listing(path), path.read_bytes())
        rc, _, _ = quiet(acquire, path, "x", owner=None, dry_run=True)
        check(rc == 1 and (listing(path), path.read_bytes()) == seen, "a dry-run acquire of a held lock refuses, writes nothing")
        rc, out, _ = quiet(release, path, owner=None, dry_run=True)
        check(rc == 0 and (listing(path), path.read_bytes()) == seen and "would remove" in out,
              "a dry-run release removes nothing")
        rc, _, err = quiet(main, ["--acquire", "--dry-run", "--lock-file", str(path)])
        check(rc == 1 and (listing(path), path.read_bytes()) == seen,
              "--dry-run reaches acquire through the command line")

        # --- how an owner is judged, with no processes behind it ----------------
        lock = _Lock(b"", {"started": 1000.0, "note": "n", "pid": 1, "owner_pid": 7, "owner_created": 9},
                     1000.0, 0)
        def says(state):
            return lambda pid, created: state

        up, down, unsure = says(ALIVE), says(DEAD), says(UNKNOWN)
        shell = (7, 9)
        elsewhere = (8, 9)
        check(assess(lock, now=1000.0 + STALE_SECONDS - 1, caller=elsewhere, alive=up)[0] == HELD
              and assess(lock, now=1000.0 + 5, caller=elsewhere, alive=up)[0] == HELD,
              "a lock whose owner is running is held for STALE_SECONDS")
        verdict, why = assess(lock, now=1000.0 + STALE_SECONDS + 1, caller=elsewhere, alive=up)
        check(verdict == STALE and "abandoned" in why and "30 min" in why and "pid 7 is running" in why,
              "and stale after it, though the owner still runs: %r %r" % (verdict, why))
        check(assess(lock, now=1000.0 + STALE_SECONDS * 50, caller=None, alive=up)[0] == STALE,
              "however long it has been held, and whether or not the caller is known")
        check(assess(lock, now=1000.0 + 5, caller=elsewhere, alive=down)[0] == STALE
              and assess(lock, now=1000.0 + STALE_SECONDS * 2, caller=elsewhere, alive=down)[0] == STALE,
              "a lock whose owner is gone is stale at any age")
        verdict, why = assess(lock, now=1000.0 + 5, caller=shell, alive=up)
        check(verdict == STALE and "previous build in this shell must have been interrupted" in why,
              "a lock the caller's own shell took is stale, however young: %r %r" % (verdict, why))
        check(assess(lock, now=1000.0 + 5, caller=shell, alive=down)[0] == STALE,
              "... and however its owner reads")
        check(assess(lock, now=1000.0 + 5, caller=(7, 10), alive=up)[0] == HELD
              and assess(lock, now=1000.0 + 5, caller=(6, 9), alive=up)[0] == HELD,
              "the shell is the pid AND the creation time: a pid reused since is another shell")
        check(assess(lock, now=1000.0 + 5, caller=elsewhere, alive=unsure)[0] == HELD
              and assess(lock, now=1000.0 + STALE_SECONDS + 1, caller=elsewhere, alive=unsure)[0] == STALE,
              "an owner that cannot be read is judged by age alone")
        check(assess(None)[0] == FREE, "no lock is free")

        # --- owners (Windows) ------------------------------------------------------
        if windows:
            state, mine = probe_process(os.getpid())
            check(state == ALIVE and isinstance(mine, int) and mine > 0,
                  "this process reads as alive with a creation time: %r %r" % (state, mine))
            me = (os.getpid(), mine)
            stranger = (99999999, 1)
            check(owner_state(*me) == ALIVE and owner_state(me[0], me[1] + 1) == DEAD,
                  "a live pid with another creation time is a reused pid, so dead")
            check(probe_process(99999999)[0] == DEAD, "a pid nothing runs under is dead")
            # A child that has exited while this process still holds its handle:
            # its pid stays valid, and it reads as dead.
            exited = subprocess.Popen([sys.executable, "-c", "pass"])
            exited.wait(30)
            state, created = probe_process(exited.pid)
            check(state == DEAD and created is not None,
                  "a child that has exited reads as dead, its creation time still readable: %r %r"
                  % (state, created))
            gone = (exited.pid, created)
            check(owner_state(*gone) == DEAD, "owner_state agrees")
            shells[0].ready()
            state, created = probe_process(shells[0].pid)
            check(state == ALIVE and created is not None and owner_state(shells[0].pid, created) == ALIVE,
                  "a running child reads as alive: %r %r" % (state, created))

            # a running owner refuses, whoever asks; the record carries the owner
            path = new_lock()
            check(quiet(acquire, path, "mine", owner=me)[0] == 0, "acquire with an owner")
            record = json.loads(path.read_text(encoding="utf-8"))
            check(record.get("owner_pid") == me[0] and record.get("owner_created") == me[1]
                  and record.get("pid") == os.getpid(),
                  "the owner is recorded beside the legacy fields: %r" % record)
            before = path.read_bytes()
            rc, _, err = quiet(acquire, path, "theirs", owner=stranger)
            check(rc == 1 and ("owner pid %d is running" % me[0]) in err and path.read_bytes() == before,
                  "a lock whose owner is running is refused: %r" % err)
            rc, _, err = quiet(acquire, path, "theirs", owner=(me[0], me[1] + 1))
            check(rc == 1 and path.read_bytes() == before,
                  "a shell with the owner's pid and another creation time is another shell: refused")
            # ... for up to STALE_SECONDS: past that it is abandoned, owner or no
            put(path, dict(record, started=tick(STALE_SECONDS - 60)))
            check(quiet(acquire, path, "theirs", owner=stranger)[0] == 1,
                  "a lock 29 minutes old whose owner is running is still held")
            put(path, dict(record, started=tick(STALE_SECONDS + 60)))
            rc, out, _ = quiet(acquire, path, "theirs", owner=stranger)
            check(rc == 0 and "took over" in out and "abandoned" in out and "mine" in out
                  and len(tombstones(path)) == 1
                  and json.loads(path.read_text(encoding="utf-8"))["note"] == "theirs",
                  "a lock 31 minutes old is taken over though its owner is running: %r %r" % (rc, out))
            # release belongs to the owner alone
            path = new_lock()
            quiet(acquire, path, "mine", owner=me)
            rc, _, err = quiet(release, path, owner=stranger)
            check(rc == 0 and path.exists() and "not releasing" in err,
                  "a non-owner's release warns and leaves the lock: %r" % err)
            check(quiet(release, path, owner=None)[0] == 0 and path.exists(),
                  "a release that cannot name its owner leaves an owned lock")
            check(quiet(release, path, owner=(me[0], me[1] + 1))[0] == 0 and path.exists(),
                  "the right pid with another creation time is not the owner")
            check(quiet(release, path, owner=me)[0] == 0 and not path.exists(),
                  "the owner's release removes it")

            # --- the same shell: an interrupted build ---------------------------------
            # cmd runs one batch file at a time, so a build that starts in the
            # shell whose earlier build left the lock behind (Ctrl+C, then Y,
            # skips build.bat's release) is the only build there is.
            path = new_lock()
            quiet(acquire, path, "interrupted build", owner=me)
            before = path.read_bytes()
            rc, out, _ = quiet(acquire, path, "x", owner=me, dry_run=True)
            check(rc == 0 and path.read_bytes() == before and tombstones(path) == []
                  and "would take it over" in out and "this shell" in out,
                  "a dry run over this shell's own lock writes and removes nothing: %r" % out)
            rc, out, err = quiet(acquire, path, "the next build", owner=me)
            check(rc == 0 and err == "" and out.count("\n") == 1 and out.startswith("[edvr] took over")
                  and "the previous build in this shell must have been interrupted" in out
                  and "interrupted build" in out and len(tombstones(path)) == 1
                  and json.loads(path.read_text(encoding="utf-8"))["note"] == "the next build",
                  "this shell's own lock is taken over at once, in one line: %r %r" % (rc, out))
            before = path.read_bytes()
            check(quiet(acquire, path, "another shell", owner=stranger)[0] == 1 and path.read_bytes() == before,
                  "and it is this shell's again: another shell is refused")
            rc, seconds = waited(path, 600, owner=stranger)
            check(rc == 1 and 599 <= seconds <= 601,
                  "wait keeps waiting on a running owner's lock for another shell: %r %r" % (rc, seconds))
            check(waited(path, 600, owner=me) == (0, 0),
                  "wait ends at once for the shell whose own lock it is")
            put(path, dict(json.loads(path.read_text(encoding="utf-8")), started=tick(STALE_SECONDS - 120)))
            rc, seconds = waited(path, 600, owner=stranger)
            check(rc == 0 and 100 <= seconds <= 130,
                  "wait ends when a running owner's lock reaches the ceiling: %r %r" % (rc, seconds))

            # a dead owner: taken over at once, whatever the age; so is a reused pid
            for label, dead in (("has exited", gone), ("pid was reused (creation time differs)", (me[0], me[1] + 10))):
                path = new_lock()
                put(path, {"started": tick(), "note": "killed build", "pid": 1,
                           "owner_pid": dead[0], "owner_created": dead[1]})
                started = time.monotonic()
                rc, out, _ = quiet(acquire, path, "next", owner=me)
                check(rc == 0 and time.monotonic() - started < 3 and "took over" in out
                      and "killed build" in out and len(tombstones(path)) == 1
                      and json.loads(path.read_text(encoding="utf-8"))["note"] == "next",
                      "a lock whose owner %s is taken over at once: %r %r" % (label, rc, out))
            path = new_lock()
            put(path, {"started": tick(), "note": "killed build", "pid": 1,
                       "owner_pid": gone[0], "owner_created": gone[1]})
            check(waited(path, 600, owner=stranger) == (0, 0), "wait returns as soon as the owner is dead")
            seen = (listing(path), path.read_bytes())
            rc, out, _ = quiet(acquire, path, "next", owner=me, dry_run=True)
            check(rc == 0 and (listing(path), path.read_bytes()) == seen and "would take it over" in out,
                  "a dry run over a dead owner's lock writes and removes nothing")

            # old tombstones are swept once the lock is ours; fresh ones stay
            path = new_lock()
            old_tomb = path.with_name(path.name + ".stale-0000000000000001")
            new_tomb = path.with_name(path.name + ".stale-0000000000000002")
            old_tomb.write_text("x", encoding="utf-8")
            new_tomb.write_text("x", encoding="utf-8")
            os.utime(str(old_tomb), (tick(TOMBSTONE_KEEP_SECONDS + 60),) * 2)
            check(quiet(acquire, path, "sweeper", owner=me)[0] == 0
                  and not old_tomb.exists() and new_tomb.exists(),
                  "an old tombstone is swept, a fresh one is kept: %r" % listing(path))

            # --- taking a stale lock down without a race ----------------------------
            # The case a thread race almost never hits: an acquirer read the stale
            # lock, then slept while another took it down and created a fresh one.
            # Woken, it must not move the fresh lock (it would, if the tombstone's
            # name were its own; here the name is the stale lock's, and taken).
            path = new_lock()
            dead_record = {"started": tick(), "note": "killed build", "pid": 1,
                           "owner_pid": gone[0], "owner_created": gone[1]}
            put(path, dead_record)
            slow = _read(path)                  # what the slow acquirer saw
            check(_take_down(path, slow) and not path.exists(), "the quick acquirer takes the stale lock down")
            check(_create(path, _record("fresh", me)), "and creates its own")
            fresh = path.read_bytes()
            moves = []
            real_rename = os.rename

            def spy(source, destination):
                result = real_rename(source, destination)
                moves.append((str(source), str(destination)))
                return result

            os.rename = spy
            try:
                late = _take_down(path, slow)   # the slow acquirer wakes
            finally:
                os.rename = real_rename
            check(late is False and moves == [] and path.read_bytes() == fresh,
                  "a late acquirer never moves a fresh lock: %r %r" % (late, moves))
            # ... and when the stale lock left some other way (an older copy's
            # overwrite, say) and its name is not taken, the bytes are compared and
            # the file put back.
            path = new_lock()
            put(path, dead_record)
            slow = _read(path)
            put(path, {"started": tick(), "note": "someone else", "pid": 2,
                       "owner_pid": me[0], "owner_created": me[1]})
            other = path.read_bytes()
            check(_take_down(path, slow) is False and path.read_bytes() == other
                  and tombstones(path) == [],
                  "a lock that is not the one judged stale is put back: %r" % listing(path))

            # --- the O_EXCL race: many at once, exactly one wins -------------------
            # Threads, each its own shell (an owner of its own, read as running):
            # the exclusivity is the file system's, and threads reach it as
            # processes do.
            def fake_alive(pid, created):
                return ALIVE if pid >= _FAKE_PID else owner_state(pid, created)

            def dead_lock(path):
                put(path, {"started": tick(), "note": "killed build", "pid": 1,
                           "owner_pid": gone[0], "owner_created": gone[1]})

            def race(threads, prepare=None):
                path = new_lock()
                if prepare:
                    prepare(path)
                gate = threading.Barrier(threads)
                results = [None] * threads

                def runner(index):
                    gate.wait()
                    results[index] = acquire(path, "thread-%d" % index,
                                             owner=(_FAKE_PID + index, 1), alive=fake_alive)

                pool = [threading.Thread(target=runner, args=(i,)) for i in range(threads)]
                out, err = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    for thread in pool:
                        thread.start()
                    for thread in pool:
                        thread.join(60)
                return path, results

            for round_number in range(4):
                path, results = race(16)
                winner = [i for i, rc in enumerate(results) if rc == 0]
                check(len(winner) == 1 and results.count(1) == 15
                      and json.loads(path.read_text(encoding="utf-8"))["note"] == "thread-%d" % winner[0],
                      "16 threads acquiring a free lock: exactly one wins, and the file is its: %r" % (results,))
                path, results = race(16, dead_lock)
                winner = [i for i, rc in enumerate(results) if rc == 0]
                check(len(winner) == 1 and results.count(1) == 15
                      and json.loads(path.read_text(encoding="utf-8"))["note"] == "thread-%d" % winner[0]
                      and len(tombstones(path)) == 1,
                      "16 threads taking over one dead owner's lock: exactly one wins, one tombstone: %r %r"
                      % (results, tombstones(path)))

            # --- processes: stand-in shells, started once and reused ----------------
            def shell_race(path):
                """Every stand-in acquires `path` at the same instant, as the
                shell of a build each: their exit codes."""
                for shell in shells:
                    shell.ready()
                start = time.time() + 0.05
                for shell in shells:
                    shell.send("race", path, start=start)
                return [shell.reply()[1] for shell in shells]

            path = new_lock()
            codes = shell_race(path)
            check(codes.count(0) == 1 and codes.count(1) == len(shells) - 1 and path.is_file(),
                  "%d processes acquiring a free lock: exactly one wins: %r" % (len(shells), codes))
            path = new_lock()
            dead_lock(path)
            codes = shell_race(path)
            check(codes.count(0) == 1 and codes.count(1) == len(shells) - 1 and len(tombstones(path)) == 1,
                  "%d processes taking over one dead lock: exactly one wins: %r %r"
                  % (len(shells), codes, tombstones(path)))

            # --- end to end through the command line: the owner is the helper's parent
            holder, other_shell, third_shell = shells[0], shells[1], shells[2]
            state, holder_created = probe_process(holder.pid)
            path = new_lock()
            check(holder.ask("acquire", path) == ("acquire", 0) and path.is_file(),
                  "the stand-in build acquires")
            info = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
            owned = "owner_pid" in info
            if not owned:
                # A venv's python.exe or the py launcher stands between the
                # stand-in and the helper and exits with it (see find_owner).
                print("build_lock: NOTE: no owner recorded (launcher parent?); "
                      "the end-to-end owner checks are skipped")
            else:
                check(info["owner_pid"] == holder.pid and info.get("pid") != holder.pid,
                      "the owner is the helper's parent, the stand-in build, and the legacy pid the helper's own: %r"
                      % (info,))
                before = path.read_bytes()
                other_shell.send("acquire", path)
                third_shell.send("release", path)
                check(other_shell.reply() == ("acquire", 1),
                      "while the stand-in build lives, another shell's acquire is refused")
                check(third_shell.reply() == ("release", 0) and path.read_bytes() == before,
                      "a release from another shell leaves the lock")
                check(holder.ask("release", path) == ("release", 0) and not path.exists(),
                      "the build's own release removes it")
                # the build is interrupted: it acquired and never released, and
                # the next build starts in the same shell
                check(holder.ask("acquire", path) == ("acquire", 0) and path.is_file(),
                      "acquire again, then that build is interrupted without a release")
                check(holder.ask("acquire", path) == ("acquire", 0) and len(tombstones(path)) == 1
                      and json.loads(path.read_text(encoding="utf-8")).get("owner_pid") == holder.pid,
                      "the next build in the same shell takes the lock over at once")
            # the shell itself ends with the lock held (killed, or its window closed)
            holder.close()
            check(owner_state(holder.pid, holder_created) == DEAD,
                  "a shell that has exited reads as dead")
            if owned:
                started = time.monotonic()
                rc, out, _ = quiet(acquire, path, "after the kill", owner=me)
                check(rc == 0 and time.monotonic() - started < 3 and "took over" in out,
                      "once the build that held the lock has exited, the next acquire takes over at once: %r" % out)
        else:
            print("build_lock: NOTE: not Windows; the owner cases are skipped")

    watchdog.cancel()
    for shell in shells:
        shell.close()

    # --- which parent is a launcher --------------------------------------------
    py = r"C:\Python312\python.exe"
    venv = r"C:\repo\.venv\Scripts\python.exe"
    check(is_launcher_shim(venv, py) and is_launcher_shim(r"C:\Windows\py.exe", py)
          and is_launcher_shim(r"c:\repo\.venv\scripts\PYTHON.EXE", py),
          "a python.exe or py.exe that is not our own executable is a launcher")
    check(not is_launcher_shim(py, py.upper()) and not is_launcher_shim(r"C:\Windows\System32\cmd.exe", py)
          and not is_launcher_shim(None, py) and not is_launcher_shim(r"C:\Windows\explorer.exe", py),
          "cmd.exe, our own executable and an unknown parent are not launchers")

    if failures:
        for why in failures:
            print("FAIL build_lock: %s" % why)
        return 1
    print("build_lock: self-test OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--acquire", action="store_true",
                        help="take the lock, or refuse if another build holds it "
                             "(a stale lock is taken over)")
    action.add_argument("--release", action="store_true",
                        help="drop the lock, if it is this build's")
    action.add_argument("--wait", action="store_true",
                        help="block until the lock is free or stale, then exit 0")
    action.add_argument("--self-test", action="store_true")
    parser.add_argument("--note", default="",
                        help="shown to whoever hits the lock while --acquire holds it")
    parser.add_argument("--timeout", type=int, default=1800,
                        help="seconds --wait blocks before giving up (default 1800)")
    parser.add_argument("--lock-file", type=Path, default=None, metavar="PATH",
                        help="the lock file (default: %%TEMP%%\\edvr_build.lock)")
    parser.add_argument("--dry-run", action="store_true",
                        help="with --acquire or --release: say what would happen, write nothing")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    path = args.lock_file or lock_path()
    if args.acquire:
        return acquire(path, args.note, dry_run=args.dry_run)
    if args.release:
        return release(path, dry_run=args.dry_run)
    return wait(path, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
