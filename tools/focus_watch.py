#!/usr/bin/env python3
"""Catch a process tree opening windows and taking the focus (Windows only).

A build that runs a hundred test exes should never touch the desktop: no
window, no change of the foreground. When one does, whatever the developer is
typing into loses the keystrokes. This finds the exe that did it.

    python tools\\focus_watch.py --run [--log FILE] -- COMMAND [ARGS...]
    python tools\\focus_watch.py --pid N [--seconds S] [--log FILE]
    python tools\\focus_watch.py --run --all -- COMMAND ...     everything on the desktop
    python tools\\focus_watch.py --self-test

--run starts COMMAND and watches it and everything it starts until it exits;
--pid watches the descendants of a running process. Each finding says what
happened, which exe did it, the chain of parents up to the watched root, and
the build job it belongs to: the rig, named by the `--rig <label>` on the
command line of the cmd.exe that tools\\run_jobs.py started for it. --log writes
one JSON object per finding. --all reports every window and foreground change
on the desktop, tree or not.

Three kinds of finding:

  window      a visible top-level window of a process of the tree appeared.
  foreground  the foreground moved to one.
  console     a process of the tree got a console with a window. A process with
              no console to inherit (a GUI program, a DETACHED_PROCESS, a
              CREATE_NEW_CONSOLE child) that runs a console program makes
              Windows open a terminal. With Windows Terminal as the default
              terminal, that is a new Windows Terminal window, owned by
              WindowsTerminal.exe and outside the tree, which takes the
              foreground about 30 ms before the client's own hidden
              PseudoConsoleWindow appears. So the console is what is charged
              to the tree, and a terminal window or a foreground move to one
              within 0.6 s of it is charged with it (its `via` says so). A
              child of a CREATE_NO_WINDOW process inherits that process's
              windowless console and makes none of this.

How it sees. Windows tells a hook about every foreground change and every
window created or shown (SetWinEventHook), the moment it happens, so a console
that flashes for 30 ms is caught; a poll of GetForegroundWindow and
EnumWindows every 50 ms backs the hooks up. The owner of a window is found
from its pid by walking the parents (NtQueryInformationProcess), remembered as
they are met, so an ancestor that has exited is still named. A classic console
window belongs to conhost.exe and is charged to conhost's parent, the process
that made the console; a WerFault.exe crash dialog is charged to the process
named by its `-p` argument.

The same watch runs inside tools\\run_jobs.py as the build's guard (FocusWatch
and run_jobs.run), which fails the build on any finding. Its self-tests put a
real window on the desktop to prove the watch sees one: 1x1, off the desktop,
never activated, titled "focus_watch probe ...". A watch ignores such a window
unless it is the one under test (count_probes), so a build watched from outside
shows none of them; anything else it reports is real.
"""

import argparse
import collections
import ctypes
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from ctypes import wintypes

POLL_SECONDS = 0.05
CORRELATE_SECONDS = 0.6
# The self-tests put a real window on the desktop to prove the watch sees one:
# 1x1, off the desktop, never activated, titled with this. A watch that is not
# itself under test ignores it (count_probes), so a build watched from outside
# reports nothing of the guard's own probes; anything else it reports is real.
PROBE_TITLE = "focus_watch probe"
RIG_OPTION = re.compile(r"--rig\s+([A-Za-z0-9_]+)")
WER_PID = re.compile(r"(?:^|\s)-(?:p|pid)\s+(\d+)", re.IGNORECASE)
CONSOLE_HOSTS = {"conhost.exe", "openconsole.exe"}
WER_IMAGES = {"werfault.exe", "werfaultsecure.exe"}
CONSOLE_CLASSES = {"PseudoConsoleWindow", "ConsoleWindowClass"}
TERMINAL_CLASSES = CONSOLE_CLASSES | {"CASCADIA_HOSTING_WINDOW_CLASS"}

EVENT_SYSTEM_FOREGROUND = 0x0003
EVENT_OBJECT_CREATE = 0x8000
EVENT_OBJECT_SHOW = 0x8002
WINEVENT_OUTOFCONTEXT = 0x0000
WINEVENT_SKIPOWNPROCESS = 0x0002
OBJID_WINDOW = 0
GA_ROOT = 2
GWL_EXSTYLE = -20
WM_QUIT = 0x0012
WM_USER = 0x0400
PM_NOREMOVE = 0
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SYNCHRONIZE = 0x00100000
WAIT_TIMEOUT = 0x102


# --------------------------------------------------------------------------
# The findings, and what the summary and the guard make of them.
# --------------------------------------------------------------------------

class Finding:
    """One thing a process of the tree did to the desktop."""
    __slots__ = ("kind", "at", "hwnd", "pid", "exe", "cls", "title", "rect", "culprit", "chain", "job", "via")

    def __init__(self, kind, at, window, culprit, chain, job, via=None):
        self.kind, self.at, self.job, self.via = kind, at, job, via
        self.hwnd, self.pid, self.exe = window["hwnd"], window["pid"], window["exe"]
        self.cls, self.title, self.rect = window["cls"], window["title"], window["rect"]
        self.culprit = culprit                       # (pid, exe) to blame
        self.chain = chain                           # ["exe(pid)", ...] from the owner up to the root

    def as_dict(self):
        return {"kind": self.kind, "at": round(self.at, 3), "hwnd": self.hwnd, "pid": self.pid, "exe": self.exe,
                "class": self.cls, "title": self.title, "rect": self.rect, "culprit": list(self.culprit),
                "chain": self.chain, "job": self.job, "via": self.via}

    def line(self):
        return "+%.2fs %s %s(%d) owner=%s class=%s job=%s%s title=%r chain: %s" % (
            self.at, self.kind, self.culprit[1], self.culprit[0], self.exe, self.cls, self.job or "(main flow)",
            "" if not self.via else " via " + self.via, self.title[:50], " <- ".join(self.chain[:6]))


def summarize(findings, seconds=None):
    """The report: one line per (culprit exe, job, kind, window class) with how
    often and when it first happened."""
    groups = collections.OrderedDict()
    for finding in findings:
        key = (finding.culprit[1], finding.job, finding.kind, finding.cls)
        groups.setdefault(key, []).append(finding)
    lines = ["focus_watch: %d finding(s) in %d group(s)%s" % (
        len(findings), len(groups), "" if seconds is None else " over %.0f s" % seconds)]
    for index, ((exe, job, kind, cls), items) in enumerate(groups.items(), 1):
        first = items[0]
        lines.append("  %d. %s by %s (pid %d) in job %s, class %s, x%d, first at +%.1f s%s: %r" % (
            index, kind, exe, first.culprit[0], job or "(main flow)", cls, len(items), first.at,
            "" if not first.via else " via " + first.via, first.title[:50]))
        lines.append("       chain: %s" % " <- ".join(first.chain[:7]))
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Processes: who is a window's owner, and whose child is it.
# --------------------------------------------------------------------------

class Proc:
    __slots__ = ("pid", "ppid", "name", "created", "cmd")

    def __init__(self, pid, ppid, name, created, cmd):
        self.pid, self.ppid, self.name, self.created, self.cmd = pid, ppid, name, created, cmd

    def label(self):
        return "%s(%d)" % (self.name, self.pid)


class Processes:
    """Processes as they are met, kept after they exit: `info` reads a live
    process once and remembers it, so `chain` can still name an ancestor that
    is gone. A pid that is reused is noticed by its creation time. The OS is
    read by `_read` and `_created`, which the self-test replaces."""

    def __init__(self):
        self._known = {}          # pid -> Proc, the latest of that pid
        self._lock = threading.Lock()

    def info(self, pid):
        if not pid:
            return None
        with self._lock:
            known = self._known.get(pid)
        if known is not None:
            created = self._created(pid)
            if created is None or created == known.created:
                return known
        fresh = self._read(pid)
        if fresh is None:
            return known
        with self._lock:
            self._known[pid] = fresh
        return fresh

    def chain(self, pid, limit=24):
        """[Proc, ...] from `pid` up towards the top: a parent is followed only
        if it was created before the child, else its pid was reused."""
        out, seen, current = [], set(), pid
        while current and current not in seen and len(out) < limit:
            proc = self.info(current)
            if proc is None:
                break
            out.append(proc)
            seen.add(current)
            parent = self.info(proc.ppid) if proc.ppid else None
            if parent is None or (parent.created and proc.created and parent.created > proc.created):
                break
            current = proc.ppid
        return out

    def _created(self, pid):
        return _bind().creation_time(pid)

    def _read(self, pid):
        return _bind().read_process(pid)


def job_of(chain):
    """The rig a process belongs to: the label on the `--rig` of the nearest
    cmd.exe among its ancestors; None for the build's own main flow."""
    for proc in chain:
        if proc.name.lower() == "cmd.exe" and proc.cmd:
            match = RIG_OPTION.search(proc.cmd)
            if match:
                return match.group(1)
    return None


def attribute(window, procs, root):
    """(culprit Proc, chain [Proc...]) when `window` (a dict from the
    inspector) belongs to the tree under `root`, else None. `root` None means
    everything counts."""
    owner = procs.info(window["pid"])
    if owner is None:
        if root is not None:
            return None
        stub = Proc(window["pid"], 0, window.get("exe", "?"), None, "")
        return stub, [stub]
    chain = procs.chain(owner.pid)
    culprit, name = owner, owner.name.lower()
    if name in CONSOLE_HOSTS and len(chain) > 1:
        culprit = chain[1]                     # the console's client, not its host
    elif name in WER_IMAGES and owner.cmd:
        match = WER_PID.search(owner.cmd)
        faulted = procs.info(int(match.group(1))) if match else None
        if faulted is not None:
            culprit, chain = faulted, procs.chain(faulted.pid)
    if root is None:
        return culprit, chain
    for depth, proc in enumerate(chain):
        if proc.pid == root:
            return culprit, chain[:depth + 1]
    return None


# --------------------------------------------------------------------------
# The watch.
# --------------------------------------------------------------------------

class FocusWatch:
    """Watches the descendants of `root` (a pid; None: the whole desktop) and
    records a Finding for every new visible top-level window, every new console
    window and every move of the foreground to a window of the tree.
    start() ... stop() -> findings."""

    def __init__(self, root, poll=POLL_SECONDS, procs=None, api=None, count_probes=False):
        self.root, self.poll = root, poll
        self.procs = procs or Processes()
        self.api = api
        self.count_probes = count_probes
        self.findings = []
        self.errors = []
        self.events = collections.Counter()
        self.hooked = False
        self.on_finding = None
        self._lock = threading.Lock()
        self._reported = set()
        self._baseline = set()          # (hwnd, pid) of the visible windows at the start
        self._polled = set()            # (hwnd, pid) of the visible windows the poll has looked at closely
        self._known = set()             # every hwnd at the start and since
        self._foreground = None
        self._marks = []                # (at, Finding) of each console charged to the tree
        self._pending = []              # (at, kind, window) terminal windows not yet charged to anyone
        self._stop = threading.Event()
        self._threads = []
        self._hook_thread_id = None
        self._started = time.monotonic()
        self._ended = None

    # --- life cycle ------------------------------------------------------------
    def start(self):
        self.api = self.api or _bind()
        self._started = time.monotonic()
        self._baseline = {(w["hwnd"], w["pid"]) for w in self.api.visible_windows()}
        self._known = set(self.api.all_windows())
        self._foreground = self.api.foreground()
        ready = threading.Event()
        hook = threading.Thread(target=self._hook_loop, args=(ready,), name="focus-hooks", daemon=True)
        poll = threading.Thread(target=self._poll_loop, name="focus-poll", daemon=True)
        self._threads = [hook, poll]
        hook.start()
        ready.wait(2.0)
        poll.start()
        return self

    def stop(self):
        """Stop watching; the findings, in order."""
        self._stop.set()
        if self._hook_thread_id:
            self.api.post_quit(self._hook_thread_id)
        for thread in self._threads:
            thread.join(3.0)
        self._ended = self._ended or time.monotonic()
        return list(self.findings)

    @property
    def seconds(self):
        return (self._ended or time.monotonic()) - self._started

    def report(self, findings=None):
        """What a run prints once the watch is over: the summary, then what the
        hooks saw, so a watch that saw nothing because it could not see is
        told from one that saw nothing because there was nothing."""
        findings = self.findings if findings is None else findings
        saw = "hooks saw %d show, %d console and %d foreground event(s)%s" % (
            self.events["show"], self.events["console"], self.events["foreground"],
            "" if self.hooked else " (hooks unavailable: poll only)")
        if findings:
            lines = [summarize(findings, self.seconds), "focus_watch: " + saw]
        else:
            lines = ["focus_watch: no window was shown, no console opened and the foreground never moved to a "
                     "window of the tree in %.0f s (%s)" % (self.seconds, saw)]
        if self.errors:
            lines.append("focus_watch: %d internal error(s), first: %s" % (len(self.errors), self.errors[0]))
        return "\n".join(lines)

    def finish(self):
        """Stop watching: (the findings, the text to print about them)."""
        findings = self.stop()
        return findings, self.report(findings)

    # --- the two ways of seeing ---------------------------------------------
    def _hook_loop(self, ready):
        try:
            self._hook_thread_id = self.api.thread_id()
            self.hooked = self.api.run_hooks(self._on_event, ready, self._stop)
        except Exception as error:                      # the poll still works without hooks
            self.errors.append("hooks: %r" % (error,))
            ready.set()

    def _on_event(self, event, hwnd):
        """From the hook, on its own thread: look at the window now, at once,
        because it may not be there in a millisecond."""
        try:
            if event == EVENT_SYSTEM_FOREGROUND:
                self.events["foreground"] += 1
                self._note("foreground", self.api.inspect(hwnd))
            elif event in (EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW) and self.api.is_top_level(hwnd):
                window = self.api.inspect(hwnd)
                if window and window["cls"] in CONSOLE_CLASSES:
                    self.events["console"] += 1
                    self._known.add(window["hwnd"])
                    self._note("console", window)
                elif window and event == EVENT_OBJECT_SHOW:
                    self.events["show"] += 1
                    self._note("window", window)
        except Exception as error:
            self.errors.append("event: %r" % (error,))

    def _poll_loop(self):
        while not self._stop.wait(self.poll):
            try:
                foreground = self.api.foreground()
                if foreground != self._foreground:
                    self._foreground = foreground
                    self._note("foreground", self.api.inspect(foreground))
                for window in self.api.visible_windows(self._baseline | self._polled):
                    self._polled.add((window["hwnd"], window["pid"]))
                    self._note("console" if window["cls"] in CONSOLE_CLASSES else "window", window)
                for hwnd in self.api.all_windows():          # a console window may never be visible
                    if hwnd not in self._known:
                        self._known.add(hwnd)
                        window = self.api.inspect(hwnd)
                        if window and window["cls"] in CONSOLE_CLASSES:
                            self._note("console", window)
            except Exception as error:
                self.errors.append("poll: %r" % (error,))

    # --- what a sighting is worth ---------------------------------------------
    def _note(self, kind, window, at=None):
        """Charge a sighting to the tree if it is the tree's; else hold a
        terminal window a little while in case a console of the tree explains
        it (the terminal window is there first)."""
        if window is None or (window["title"].startswith(PROBE_TITLE) and not self.count_probes):
            return
        at = time.monotonic() - self._started if at is None else at
        key = (kind, window["hwnd"], window["pid"])
        with self._lock:
            if key in self._reported or (kind != "foreground" and key[1:] in self._baseline):
                return
            self._reported.add(key)
        owned = attribute(window, self.procs, self.root)
        if owned is not None:
            culprit, chain = owned
            self._record(Finding(kind, at, window, (culprit.pid, culprit.name), [p.label() for p in chain],
                                 job_of(chain)))
            return
        if window["cls"] not in TERMINAL_CLASSES:
            return
        with self._lock:
            mark = next((m for m in reversed(self._marks) if abs(m[0] - at) <= CORRELATE_SECONDS), None)
            if mark is None:
                self._pending = [p for p in self._pending if at - p[0] <= 2 * CORRELATE_SECONDS] + [(at, kind, window)]
                return
        self._charge(mark[1], at, kind, window)

    def _record(self, finding):
        with self._lock:
            self.findings.append(finding)
            claimed = []
            if finding.kind == "console":
                self._marks.append((finding.at, finding))
                claimed = [p for p in self._pending if abs(p[0] - finding.at) <= CORRELATE_SECONDS]
                self._pending = [p for p in self._pending if p not in claimed]
        if self.on_finding:
            self.on_finding(finding)
        for at, kind, window in claimed:
            self._charge(finding, at, kind, window)

    def _charge(self, console, at, kind, window):
        via = "the console of %s(%d)" % (console.culprit[1], console.culprit[0])
        finding = Finding(kind, at, window, console.culprit, console.chain, console.job, via)
        with self._lock:
            self.findings.append(finding)
        if self.on_finding:
            self.on_finding(finding)


# --------------------------------------------------------------------------
# Windows itself.
# --------------------------------------------------------------------------

class _Api:
    """The few Win32 calls the watch needs, with their prototypes. Everything
    that touches a window or a process is here, so the logic above can be
    tested without either."""

    def __init__(self):
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.ntdll = ctypes.WinDLL("ntdll")
        u, k, n = self.user32, self.kernel32, self.ntdll
        self.ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        self.EVENTPROC = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.DWORD, wintypes.HWND, wintypes.LONG,
                                            wintypes.LONG, wintypes.DWORD, wintypes.DWORD)
        u.EnumWindows.argtypes, u.EnumWindows.restype = [self.ENUMPROC, wintypes.LPARAM], wintypes.BOOL
        u.GetForegroundWindow.argtypes, u.GetForegroundWindow.restype = [], wintypes.HWND
        u.IsWindowVisible.argtypes, u.IsWindowVisible.restype = [wintypes.HWND], wintypes.BOOL
        u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        u.GetWindowThreadProcessId.restype = wintypes.DWORD
        u.GetWindowTextW.argtypes, u.GetWindowTextW.restype = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int
        u.GetClassNameW.argtypes, u.GetClassNameW.restype = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int
        u.GetWindowRect.argtypes, u.GetWindowRect.restype = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL
        u.GetAncestor.argtypes, u.GetAncestor.restype = [wintypes.HWND, wintypes.UINT], wintypes.HWND
        u.SetWinEventHook.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.HMODULE, self.EVENTPROC,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
        u.SetWinEventHook.restype = wintypes.HANDLE
        u.UnhookWinEvent.argtypes, u.UnhookWinEvent.restype = [wintypes.HANDLE], wintypes.BOOL
        u.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
        u.GetMessageW.restype = ctypes.c_int
        u.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT,
                                   wintypes.UINT]
        u.PeekMessageW.restype = wintypes.BOOL
        u.PostThreadMessageW.argtypes = [wintypes.DWORD, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        u.PostThreadMessageW.restype = wintypes.BOOL
        k.OpenProcess.argtypes, k.OpenProcess.restype = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE
        k.CloseHandle.argtypes, k.CloseHandle.restype = [wintypes.HANDLE], wintypes.BOOL
        k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                 ctypes.POINTER(wintypes.DWORD)]
        k.QueryFullProcessImageNameW.restype = wintypes.BOOL
        k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        k.GetProcessTimes.restype = wintypes.BOOL
        k.WaitForSingleObject.argtypes, k.WaitForSingleObject.restype = [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD
        k.GetCurrentThreadId.argtypes, k.GetCurrentThreadId.restype = [], wintypes.DWORD
        n.NtQueryInformationProcess.argtypes = [wintypes.HANDLE, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG,
                                                ctypes.POINTER(wintypes.ULONG)]
        n.NtQueryInformationProcess.restype = ctypes.c_long

    # --- windows ---------------------------------------------------------------
    def foreground(self):
        return self.user32.GetForegroundWindow() or 0

    def is_top_level(self, hwnd):
        return bool(hwnd) and self.user32.GetAncestor(hwnd, GA_ROOT) == hwnd

    def inspect(self, hwnd):
        """A dict about a window, or None when it is gone (or not a window)."""
        if not hwnd:
            return None
        u = self.user32
        pid = wintypes.DWORD(0)
        if not u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid)) or not pid.value:
            return None
        text, cls = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(128)
        u.GetWindowTextW(hwnd, text, 256)
        u.GetClassNameW(hwnd, cls, 128)
        rect = wintypes.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(rect))
        return {"hwnd": int(hwnd), "pid": pid.value, "exe": self._image_name(pid.value), "cls": cls.value,
                "title": text.value, "rect": [rect.left, rect.top, rect.right, rect.bottom]}

    def all_windows(self):
        """Every top-level window handle, hidden ones included."""
        found = []

        def each(hwnd, _):
            found.append(int(hwnd))
            return True

        self.user32.EnumWindows(self.ENUMPROC(each), 0)
        return found

    def visible_windows(self, skip=()):
        """Every visible top-level window, inspected, but for those whose
        (hwnd, pid) is in `skip`: a poll every 50 ms need only look closely at
        what is new."""
        found = []
        pid = wintypes.DWORD(0)

        def each(hwnd, _):
            if self.user32.IsWindowVisible(hwnd):
                self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if (int(hwnd), pid.value) not in skip:
                    window = self.inspect(hwnd)
                    if window:
                        found.append(window)
            return True

        self.user32.EnumWindows(self.ENUMPROC(each), 0)
        return found

    def thread_id(self):
        return self.kernel32.GetCurrentThreadId()

    def post_quit(self, thread_id):
        self.user32.PostThreadMessageW(thread_id, WM_QUIT, 0, 0)

    def run_hooks(self, on_event, ready, stop):
        """Hook foreground changes and window creations and shows, and pump
        messages until WM_QUIT. False when Windows refuses the hooks (no
        desktop)."""
        u = self.user32

        def callback(hook, event, hwnd, object_id, child_id, thread, when):
            if object_id == OBJID_WINDOW and child_id == 0:
                on_event(event, hwnd)

        keep = self.EVENTPROC(callback)
        flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        hooks = [u.SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, None, keep, 0, 0, flags),
                 u.SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, None, keep, 0, 0, flags)]
        ok = all(hooks)
        message = wintypes.MSG()
        u.PeekMessageW(ctypes.byref(message), None, WM_USER, WM_USER, PM_NOREMOVE)   # makes the thread's queue
        ready.set()
        while not stop.is_set() and u.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
            pass
        for hook in hooks:
            if hook:
                u.UnhookWinEvent(hook)
        return ok

    # --- processes -------------------------------------------------------------
    def wait_zero(self, pid):
        """True while process `pid` is running."""
        handle = self.kernel32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            return False
        try:
            return self.kernel32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT
        finally:
            self.kernel32.CloseHandle(handle)

    def _image_name(self, pid, handle=None):
        opened = handle or self.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not opened:
            return "pid%d" % pid
        try:
            buffer, size = ctypes.create_unicode_buffer(1024), wintypes.DWORD(1024)
            if self.kernel32.QueryFullProcessImageNameW(opened, 0, buffer, ctypes.byref(size)):
                return buffer.value.rsplit("\\", 1)[-1]
            return "pid%d" % pid
        finally:
            if not handle:
                self.kernel32.CloseHandle(opened)

    def _creation_time(self, handle):
        times = [wintypes.FILETIME() for _ in range(4)]
        if self.kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return None

    def creation_time(self, pid):
        handle = self.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return None
        try:
            return self._creation_time(handle)
        finally:
            self.kernel32.CloseHandle(handle)

    def read_process(self, pid):
        """A Proc for a process that can be opened, or None."""
        handle = self.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return None
        try:
            class Basic(ctypes.Structure):
                _fields_ = [("ExitStatus", ctypes.c_long), ("Peb", ctypes.c_void_p), ("Affinity", ctypes.c_size_t),
                            ("BasePriority", ctypes.c_long), ("UniquePid", ctypes.c_size_t),
                            ("ParentPid", ctypes.c_size_t)]

            basic = Basic()
            if self.ntdll.NtQueryInformationProcess(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic), None) != 0:
                return None
            return Proc(pid, int(basic.ParentPid), self._image_name(pid, handle), self._creation_time(handle),
                        self._command_line(handle))
        finally:
            self.kernel32.CloseHandle(handle)

    def _command_line(self, handle):
        """The command line, via ProcessCommandLineInformation (60): a
        UNICODE_STRING whose text follows it in the same buffer."""
        needed = wintypes.ULONG(0)
        self.ntdll.NtQueryInformationProcess(handle, 60, None, 0, ctypes.byref(needed))
        if not needed.value:
            return ""
        buffer = ctypes.create_string_buffer(needed.value + 16)
        if self.ntdll.NtQueryInformationProcess(handle, 60, buffer, len(buffer), ctypes.byref(needed)) != 0:
            return ""
        length = ctypes.c_ushort.from_buffer(buffer, 0).value
        pointer = ctypes.c_void_p.from_buffer(buffer, 8).value
        return ctypes.wstring_at(pointer, length // 2) if pointer else ""


_API = []


def _bind():
    if not _API:
        if sys.platform != "win32":
            raise OSError("focus_watch needs Windows")
        _API.append(_Api())
    return _API[0]


# --------------------------------------------------------------------------
# Command line.
# --------------------------------------------------------------------------

def _watching(watch, log_path, quiet):
    stream = open(log_path, "w", encoding="utf-8") if log_path else None

    def note(finding):
        if stream:
            stream.write(json.dumps(finding.as_dict()) + "\n")
            stream.flush()
        if not quiet:
            print("[focus] " + finding.line(), flush=True)

    watch.on_finding = note
    return stream


def _report(watch, findings, stream):
    if stream:
        stream.close()
    print(watch.report(findings), flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--run", action="store_true", help="run COMMAND (after --) and watch what it starts")
    action.add_argument("--pid", type=int, help="watch the descendants of this process")
    action.add_argument("--self-test", action="store_true")
    parser.add_argument("--seconds", type=float, default=0, help="with --pid: stop after this long (0: until it exits)")
    parser.add_argument("--all", action="store_true", help="report every window and focus change on the desktop")
    parser.add_argument("--log", metavar="FILE", help="write each finding as one JSON line")
    parser.add_argument("--quiet", action="store_true", help="print only the summary")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.run:
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        if not command:
            parser.error("--run needs a command after --")
        process = subprocess.Popen(command)
        watch = FocusWatch(None if args.all else process.pid).start()
        stream = _watching(watch, args.log, args.quiet)
        code = process.wait()
    else:
        if args.pid <= 0:
            parser.error("--pid needs a process id")
        watch = FocusWatch(None if args.all else args.pid).start()
        stream = _watching(watch, args.log, args.quiet)
        deadline = time.monotonic() + args.seconds if args.seconds else None
        try:
            while (deadline is None or time.monotonic() < deadline) and watch.api.wait_zero(args.pid):
                time.sleep(0.2)
        except KeyboardInterrupt:
            pass
        code = 0
    findings = watch.stop()
    _report(watch, findings, stream)
    return code


# --------------------------------------------------------------------------
# Self-test. Nothing here takes the focus or puts anything on screen: the live
# case uses a window that is visible to Windows but 1x1, off the desktop, a
# tool window that never activates. A console or a move of the foreground
# cannot be made without opening a terminal or stealing the focus, so those
# paths are checked on made-up windows only (and were calibrated by hand
# against a detached process, a CREATE_NEW_CONSOLE child and a CREATE_NO_WINDOW
# one).
# --------------------------------------------------------------------------

_CHILD_WINDOW = r"""
import ctypes, sys, time
from ctypes import wintypes
u = ctypes.WinDLL('user32', use_last_error=True)
u.CreateWindowExW.restype = wintypes.HWND
u.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                              wintypes.HINSTANCE, wintypes.LPVOID]
u.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
# WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP, 1x1 at (-32000, -32000). Created hidden and, for the visible
# one, shown with SW_SHOWNOACTIVATE: a window created with WS_VISIBLE is activated whatever its extended style.
hwnd = u.CreateWindowExW(0x08000000 | 0x80, 'STATIC', 'focus_watch probe ' + sys.argv[1], 0x80000000,
                         -32000, -32000, 1, 1, None, None, None, None)
if sys.argv[1] == 'visible':
    u.ShowWindow(hwnd, 4)
print(hwnd or 0, flush=True)
time.sleep(float(sys.argv[2]))
"""


def _fake_processes(table):
    """A Processes whose OS side is `table`: {pid: (ppid, name, cmd)}; a
    process is created at the time its pid says, so parents are older."""
    class Fake(Processes):
        def _created(self, pid):
            return pid if pid in table else None

        def _read(self, pid):
            entry = table.get(pid)
            return Proc(pid, entry[0], entry[1], pid, entry[2]) if entry else None

    return Fake()


def self_test():
    failures = []

    def check(condition, why):
        if not condition:
            failures.append(why)

    # --- who owns what, on a made-up tree ----------------------------------------
    table = {
        100: (1, "python.exe", "python tools\\run_jobs.py --script build.bat"),
        200: (100, "cmd.exe", 'cmd.exe /d /c ""C:\\x y\\build.bat" --rig alpha_test"'),
        300: (200, "cl.exe", "cl.exe /nologo x.cpp"),
        310: (300, "test.exe", "test.exe --self-test"),
        320: (310, "conhost.exe", "\\??\\C:\\Windows\\system32\\conhost.exe 0xffffffff"),
        400: (100, "cmd.exe", "cmd.exe /d /c ping -n 3 127.0.0.1"),
        500: (1, "services.exe", ""),
        510: (500, "WerFault.exe", "C:\\Windows\\system32\\WerFault.exe -u -p 310 -s 4444"),
        800: (1, "WindowsTerminal.exe", "WindowsTerminal.exe"),
        900: (1, "notepad.exe", "notepad.exe"),
    }
    procs = _fake_processes(table)
    chain = [p.pid for p in procs.chain(310)]
    check(chain == [310, 300, 200, 100], "the chain runs from the owner up through the rig's cmd.exe to the runner: %r" % chain)
    check(job_of(procs.chain(310)) == "alpha_test" and job_of(procs.chain(400)) is None,
          "the job is the rig named by --rig on the nearest cmd.exe; the runner's own child has none")

    def owned(pid, root=100):
        result = attribute({"pid": pid, "exe": "?"}, procs, root)
        return None if result is None else (result[0].pid, [p.pid for p in result[1]])

    check(owned(310) == (310, [310, 300, 200, 100]), "a window of a test exe is the tree's, chain cut at the root: %r" % (owned(310),))
    check(owned(320) == (310, [320, 310, 300, 200, 100]),
          "a console window is charged to the client of its conhost, not to conhost: %r" % (owned(320),))
    check(owned(510) == (310, [310, 300, 200, 100]),
          "a crash dialog is charged to the process its -p names, though WerFault has other parents: %r" % (owned(510),))
    check(owned(900) is None and owned(1234) is None, "a window of an unrelated or unreadable process is not the tree's")
    check(owned(900, root=None) == (900, [900]) and owned(1234, root=None) == (1234, [1234]),
          "with no root, everything is, an unreadable owner included")
    check(owned(310, root=999) is None, "another root does not own it")
    reused = _fake_processes({100: (1, "python.exe", ""), 700: (100, "a.exe", ""), 5: (700, "b.exe", "")})
    check([p.pid for p in reused.chain(5)] == [5],
          "a parent created after its child is a reused pid and is not followed")

    def window(hwnd, pid, cls="Static", title="w"):
        return {"hwnd": hwnd, "pid": pid, "exe": table.get(pid, (0, "?"))[1], "cls": cls, "title": title,
                "rect": [0, 0, 10, 10]}

    # windows: each once per kind; the tree's and nobody else's; not what was there at the start
    watch = FocusWatch(100, procs=procs)
    watch._baseline = {(79, 310)}
    watch._note("window", window(77, 310), 1.0)
    watch._note("window", window(77, 310), 1.1)                  # the same window twice: one finding
    watch._note("foreground", window(77, 310), 1.2)              # ... and it took the foreground: another
    watch._note("window", window(78, 900), 1.3)                  # someone else's: none
    watch._note("window", window(79, 310), 1.4)                  # already there when the watch began: none
    got = [(f.kind, f.culprit, f.job) for f in watch.findings]
    check(got == [("window", (310, "test.exe"), "alpha_test"), ("foreground", (310, "test.exe"), "alpha_test")],
          "each window is reported once per kind, with culprit and job: %r" % (got,))

    # the self-tests' own probe windows: ignored, unless the watch is the one under test
    probe = window(70, 310, title=PROBE_TITLE + " visible")
    ignoring = FocusWatch(100, procs=procs)
    ignoring._note("window", probe, 1.0)
    ignoring._note("foreground", probe, 1.1)
    counting = FocusWatch(100, procs=procs, count_probes=True)
    counting._note("window", probe, 1.0)
    check(ignoring.findings == [] and [f.kind for f in counting.findings] == ["window"],
          "a watch not under test ignores the self-tests' probe windows; one that is counts them: %r %r"
          % ([f.line() for f in ignoring.findings], [f.line() for f in counting.findings]))

    # a console with a window for a process of the tree, and the terminal window that opens for it, first
    watch = FocusWatch(100, procs=procs)
    terminal = window(50, 800, "CASCADIA_HOSTING_WINDOW_CLASS", "Windows Terminal")
    watch._note("foreground", terminal, 5.000)                   # Windows Terminal takes the foreground ...
    check(watch.findings == [], "a terminal window alone is nobody's: it is only held for a moment")
    watch._note("console", window(60, 310, "PseudoConsoleWindow", ""), 5.034)      # ... 34 ms before the client's console
    got = [(f.kind, f.cls, f.culprit, f.job, f.via) for f in watch.findings]
    check(got == [("console", "PseudoConsoleWindow", (310, "test.exe"), "alpha_test", None),
                  ("foreground", "CASCADIA_HOSTING_WINDOW_CLASS", (310, "test.exe"), "alpha_test",
                   "the console of test.exe(310)")],
          "the console is charged to the client, and the foreground move to the terminal is charged with it: %r" % (got,))
    watch._note("window", window(51, 800, "CASCADIA_HOSTING_WINDOW_CLASS", "Windows Terminal"), 5.2)   # after the mark
    check(len(watch.findings) == 3 and watch.findings[-1].via, "a terminal window after the console is charged too")
    watch._note("foreground", window(52, 800, "CASCADIA_HOSTING_WINDOW_CLASS", "Windows Terminal"), 9.0)   # much later
    watch._note("foreground", window(53, 900, "Notepad", "n"), 5.1)                                       # not a terminal
    check(len(watch.findings) == 3, "a terminal window with no console of the tree near it is not the tree's")
    lonely = FocusWatch(100, procs=procs)
    lonely._note("foreground", terminal, 1.0)
    lonely._note("console", window(61, 900, "PseudoConsoleWindow", ""), 1.1)        # a console of someone else
    check(lonely.findings == [], "another process's console does not charge a terminal to the tree")

    report = summarize(watch.findings, 12.0)
    check("3 finding(s) in 3 group(s)" in report and "job alpha_test" in report and "via the console of test.exe(310)" in report,
          "the summary groups by exe, job, kind and class, and says what a terminal window was charged through: %r" % report)
    check(json.loads(json.dumps(watch.findings[0].as_dict()))["job"] == "alpha_test", "a finding serializes")

    # what a run prints: the findings, or a plain statement that there were none, and what the hooks saw
    nothing = FocusWatch(100, procs=procs)
    nothing.hooked, nothing._ended = True, nothing._started + 12.0
    nothing.events.update(show=3, foreground=7)
    text = nothing.report()
    check("no window was shown" in text and "in 12 s" in text and "hooks saw 3 show, 0 console and 7 foreground" in text
          and "unavailable" not in text and "internal error" not in text and "finding" not in text,
          "a watch that found nothing says so, and what the hooks saw: %r" % text)
    nothing.hooked = False
    nothing.errors.append("poll: boom")
    text = nothing.report()
    check("hooks unavailable: poll only" in text and "1 internal error(s), first: poll: boom" in text,
          "a watch that could not use its hooks or hit an error says that too: %r" % text)
    watch._ended = watch._started + 12.0
    found, text = watch.finish()
    check(found == watch.findings and "3 finding(s) in 3 group(s) over 12 s" in text and "hooks saw" in text
          and "no window was shown" not in text,
          "finish() hands back the findings and a report that names them: %r" % text)

    # the runner names a rig by `--rig <label>` on its cmd.exe; that string is run_jobs's, so read it from there
    import run_jobs
    shell = run_jobs.child_command(os.path.join("C:\\", "a b", "build.bat"), "beta_test")
    check(job_of([Proc(1, 0, "cmd.exe", 1, shell)]) == "beta_test",
          "the command line run_jobs gives a rig names its job: %r" % shell)

    # --- a real desktop, without touching it -----------------------------------------
    if sys.platform != "win32":
        print("focus_watch: NOTE: not Windows; the live case is skipped")
    else:
        started = time.monotonic()
        live = FocusWatch(os.getpid(), count_probes=True)
        ignoring = FocusWatch(os.getpid())              # a watch that is not under test
        try:
            live.start()
            ignoring.start()
        except OSError as error:
            print("focus_watch: NOTE: no usable desktop (%s); the live case is skipped" % error)
            live = None
        if live is not None:
            flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            children = {label: subprocess.Popen([sys.executable, "-c", _CHILD_WINDOW, label, "2.0"],
                                                stdout=subprocess.PIPE, text=True, creationflags=flags)
                        for label in ("visible", "hidden")}
            handles = {label: child.stdout.readline().strip() for label, child in children.items()}
            time.sleep(0.8)
            seen, unseen = live.stop(), ignoring.stop()
            for child in children.values():
                child.wait(10)
            mine = [f for f in seen if f.title.startswith(PROBE_TITLE)]
            check(all(handle not in ("", "0") for handle in handles.values()),
                  "the two child windows were created: %r" % handles)
            check([f.kind for f in mine] == ["window"] and mine[0].title.endswith("visible")
                  and mine[0].pid == children["visible"].pid and mine[0].culprit[0] == children["visible"].pid
                  and mine[0].job is None and mine[0].chain[-1].endswith("(%d)" % os.getpid()),
                  "the visible child window is found once, as a window and not as the foreground (this test must "
                  "not take the focus), owned by its process, with the watch as the top of its chain, and the "
                  "hidden one is not: %r" % [f.line() for f in mine])
            check(not [f for f in seen if f.kind != "window" or not f.title.startswith(PROBE_TITLE)],
                  "the test children, started with CREATE_NO_WINDOW, opened no console: %r" % [f.line() for f in seen])
            check(unseen == [], "a watch that is not under test reports none of the probe windows, so a build "
                                "watched from outside shows nothing of them: %r" % [f.line() for f in unseen])
            check(PROBE_TITLE in _CHILD_WINDOW, "the probe window is titled the way watches ignore")
            if live.hooked:
                check(live.events["show"] >= 1,
                      "the window hook was told of the show, not only the poll: %r" % dict(live.events))
            else:
                print("focus_watch: NOTE: the window hooks were not available here; only the poll was tested")
            check(not live.errors, "the watch ran without an internal error: %r" % live.errors[:2])
            check(time.monotonic() - started < 30, "the live case stays quick")

    if failures:
        for why in failures:
            print("FAIL focus_watch: %s" % why)
        return 1
    print("focus_watch: self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
