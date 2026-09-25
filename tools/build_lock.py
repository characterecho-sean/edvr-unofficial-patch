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
"""

import argparse
import json
import os
import sys
import tempfile
import time
from pathlib import Path

# Generous past the ~2-4 minutes an observed full build takes, so a machine
# under heavy multi-agent load never reclaims a lock out from under a build
# that is genuinely still running -- Sean's own manual polling used a 20
# minute cap; this is that plus margin. A build.bat killed harder than a
# clean exit (closed terminal, Ctrl+C, a crash) is the only orphan case this
# window exists for.
STALE_SECONDS = 30 * 60


def lock_path():
    base = os.environ.get("TEMP") or os.environ.get("TMP") or tempfile.gettempdir()
    return Path(base) / "edvr_build.lock"


def _read(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _age_seconds(info):
    return time.time() - info.get("started", 0)


def acquire(path, note):
    info = _read(path)
    if info is not None and _age_seconds(info) < STALE_SECONDS:
        minutes = int(_age_seconds(info) // 60)
        print(
            "[edvr] ERROR: another build is already running (%s, started %d "
            "min ago). Wait for it to finish, then run build.bat again -- "
            "two builds at once contend for the same cores, which is how "
            "the vtable_test timing gate has flaked before. Run "
            "\"python tools\\build_lock.py --wait\" to block until it is "
            "free, then retry." % (info.get("note") or "no note", minutes),
            file=sys.stderr)
        return 1
    if info is not None:
        print("[edvr] a build lock from %d min ago looks abandoned (no "
              "release within %d min); reclaiming it." %
              (int(_age_seconds(info) // 60), STALE_SECONDS // 60))
    path.write_text(
        json.dumps({"started": time.time(), "note": note or "", "pid": os.getpid()}),
        encoding="utf-8")
    return 0


def release(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    return 0


def wait(path, timeout):
    deadline = time.time() + timeout if timeout else None
    while True:
        info = _read(path)
        if info is None or _age_seconds(info) >= STALE_SECONDS:
            return 0
        if deadline and time.time() >= deadline:
            print("[edvr] ERROR: still locked after %ds; giving up." % timeout,
                  file=sys.stderr)
            return 1
        time.sleep(5)


def self_test():
    with tempfile.TemporaryDirectory(prefix="edvr-build-lock-") as scratch:
        path = Path(scratch) / "lock.json"

        if acquire(path, "first") != 0:
            raise AssertionError("a free lock must be acquired")
        if not path.exists():
            raise AssertionError("acquire did not write a lock file")
        if acquire(path, "second") != 1:
            raise AssertionError("a live lock must refuse a second acquire")
        if release(path) != 0:
            raise AssertionError("release must succeed")
        if path.exists():
            raise AssertionError("release did not remove the lock file")
        if release(path) != 0:
            raise AssertionError("release of an already-clear lock must be a no-op")

        stale = {"started": time.time() - STALE_SECONDS - 1, "note": "old"}
        path.write_text(json.dumps(stale), encoding="utf-8")
        if acquire(path, "third") != 0:
            raise AssertionError("a lock past the stale ceiling must be reclaimed")
        release(path)

        if wait(path, timeout=1) != 0:
            raise AssertionError("wait() must return once nothing holds the lock")

        fresh = {"started": time.time(), "note": "busy"}
        path.write_text(json.dumps(fresh), encoding="utf-8")
        if wait(path, timeout=1) != 1:
            raise AssertionError("wait() must time out while the lock is still fresh")
    print("build_lock: self-test OK")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--acquire", action="store_true",
                        help="take the lock, or refuse if another build holds it")
    action.add_argument("--release", action="store_true", help="drop the lock")
    action.add_argument("--wait", action="store_true",
                        help="block until the lock is free, then exit 0")
    action.add_argument("--self-test", action="store_true")
    parser.add_argument("--note", default="",
                        help="shown to whoever hits the lock while --acquire holds it")
    parser.add_argument("--timeout", type=int, default=1800,
                        help="seconds --wait blocks before giving up (default 1800)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    path = lock_path()
    if args.acquire:
        return acquire(path, args.note)
    if args.release:
        return release(path)
    return wait(path, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
