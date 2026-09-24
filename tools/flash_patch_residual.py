#!/usr/bin/env python3
"""Residual simulator for the transition-flash render-time patch.

    python tools\\flash_patch_residual.py --file <eyebase_dump.txt>
    python tools\\flash_patch_residual.py --dir <edvr_logs\\flash> [--pattern eyebase_1002*.txt]
    python tools\\flash_patch_residual.py --self-test

Elite composes the VR eye as (camera base 4x4) x (head pose). The base lives
in a consume-and-reset mailbox: on the first frame of a world/ship frame
switch the writer skips one frame, so one frame renders with the identity
base -- head pose alone. That is the transition flash.

A fix being designed patches the eye at render time (R = skip frame N + 2)
with one of two candidate bases:

  held -- cachedM on the skip line: the last refilled mailbox before the skip
  new  -- M on the first refilled mode=2 call after the skip

The right choice depends on which coordinate frame the scene has already
switched to at R. This tool replays the eyebase_*.txt dumps real flights
wrote and, for every un-refilled-mailbox event, prints what each candidate
would have produced:

  patch_held = old_base + P     patch_new = new_base + P

where P is the head-only eye observed at R (scene= of the R summary row).
Residuals are the Euclidean distances of each patch to the rendered eye one
frame before (eye_prev) and one frame after (eye_next) R. Translations only;
the dumps carry no rotations and the ignored rotation term is bounded by
about 0.3 m.

Reading the output: a scene-OLD event (hyperspace exit -- objects still in
the old frame at R, pool step tiny while the camera step is huge) wants
res_held_prev small and res_new_prev huge. A scene-NEW event (drop-in --
objects stepped with the camera, pool ~= cam, both huge) wants res_new_next
small and res_held_next huge. Only dumps with the geom= block can say which
frame the objects were in at R; old-format dumps classify the event
"unclear" and the four residuals speak for themselves (each candidate is
~0.0X m from the eye on its own side of the switch and ~frame-delta from
the other side).

For treat=ACTED events the in-game fix already patched the render with the
held base, so scene(R) IS the held patch: P is recovered as scene(R) -
old_base (approximately 0) and patch_held is reported as observed.

Runs of more than three consecutive un-refilled mode=2 frames are
identity-mode stretches, not transition events: reported with no residual
math. Malformed lines never abort the parse; they are counted and reported.

Read-only: opens dumps for reading and writes nothing. Exit 0 on success,
1 on any failure (bad path, parse trouble, self-test failure).
"""

import argparse
import fnmatch
import math
import os
import re
import sys

# Runs of up to this many consecutive un-refilled mode=2 frames are treated
# as transition events; longer runs are identity-mode stretches.
EVENT_RUN_MAX = 3

# Classification threshold, metres: a "small" residual means the patch
# lands on the eye the scene actually rendered (head + dump rounding); the
# "huge" side of the check is relative to the frame-switch distance itself,
# so a 2.6 m same-origin re-centre and a 3 km drop are both judged fairly.
SMALL_RESIDUAL = 1.0

_VEC = r"\((?P<{n}>[+-]?[0-9.eE+-]+ [+-]?[0-9.eE+-]+ [+-]?[0-9.eE+-]+)\)"

CALL_RE = re.compile(
    r"^\s*f(?P<frame>\d+)\s+seq=\d+\s+t\d+\s+"
    r"mode=(?P<mode>[12])\s+"
    r"(?P<status>refilled|UNREFILLED)\s+treat=(?P<treat>\w+)\s+"
    r"ship=\S+\s+M="
    + _VEC.format(n="m") + r"\s+cachedM="
    + _VEC.format(n="cachedm") + r"\s+"
    r"age=-?\d+\s+F="
    + _VEC.format(n="f") + r"\s+"
    r"dt=\S+\s+dr=\S+\s+wsince=\w+\s+shipchg=\w+\s*$"
)

SUMMARY_RE = re.compile(
    r"^\s*f(?P<frame>\d+)\s+calls=\d+\s+mode1=\d+\s+mode2=\d+\s+scene=(?P<stale>STALE-)?"
    + _VEC.format(n="scene") + r"\s+writerHits=\d+"
    r"(?:\s+controllerCalls=\d+\s+writerEntered=\d+\s+writerWrote=\d+"
    r"\s+offered=\([^)]*\))?"
    r"(?:\s+geom=\(matched=\d+\s+predicted=\d+\s+"
    r"cam=(?P<cam>\S+)\s+pool=(?P<pool>\S+)\s+"
    r"relMed=\S+\s+relP90=\S+\s+predP90=\S+\))?"
    r"(?:\s+decision=(?P<decision>\w+))?\s*$"
)

DATA_LINE_RE = re.compile(r"^\s*f\d+\s")


def parse_vec(text):
    parts = [float(t) for t in text.split()]
    if len(parts) != 3:
        raise ValueError("vec has %d components: %r" % (len(parts), text))
    return tuple(parts)


def vec_ok(v):
    return v is not None and not any(math.isnan(c) for c in v)


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def fmt_vec(v):
    return "(%+.3f,%+.3f,%+.3f)" % v


def fmt_m(v):
    return "n/a" if v is None else "%.3f" % v


class CallLine(object):
    __slots__ = ("frame", "mode", "unrefilled", "treat", "m", "cachedm")

    def __init__(self, mo):
        self.frame = int(mo.group("frame"))
        self.mode = int(mo.group("mode"))
        self.unrefilled = mo.group("status") == "UNREFILLED"
        self.treat = mo.group("treat")
        self.m = parse_vec(mo.group("m"))
        self.cachedm = parse_vec(mo.group("cachedm"))


class SummaryRow(object):
    __slots__ = ("frame", "scene", "stale", "cam", "pool", "decision")

    def __init__(self, mo):
        self.frame = int(mo.group("frame"))
        self.stale = mo.group("stale") is not None
        self.scene = parse_vec(mo.group("scene"))
        self.cam = float(mo.group("cam")) if mo.group("cam") else None
        self.pool = float(mo.group("pool")) if mo.group("pool") else None
        self.decision = mo.group("decision")


class Dump(object):
    def __init__(self, name):
        self.name = name
        self.mode2_calls = []   # CallLine, file order, mode=2 only
        self.summaries = {}     # frame -> SummaryRow (last wins)
        self.skipped = 0        # lines that look like data but failed to parse

    @classmethod
    def parse(cls, lines, name):
        dump = cls(name)
        for line in lines:
            mo = CALL_RE.match(line)
            if mo:
                call = CallLine(mo)
                if call.mode == 2:
                    dump.mode2_calls.append(call)
                continue
            mo = SUMMARY_RE.match(line)
            if mo:
                row = SummaryRow(mo)
                dump.summaries[row.frame] = row
                continue
            if DATA_LINE_RE.match(line):
                dump.skipped += 1
        return dump


class Event(object):
    def __init__(self, kind, run_frames, treats):
        self.kind = kind            # "event" or "stretch"
        self.n = run_frames[0]      # first frame of the un-refilled run
        self.run_len = len(run_frames)
        self.last = run_frames[-1]
        self.treats = sorted(set(treats))
        self.r = self.n + 2         # bad render frame (pipeline offset)
        self.old_base = None
        self.new_base = None
        self.p = None
        self.eye_prev = None
        self.eye_next = None
        self.res = {}               # held_prev/held_next/new_prev/new_next
        self.cam = None
        self.pool = None
        self.decision = None
        self.cls = "unclear"        # scene-new / scene-old / unclear
        self.preferred = "-"
        self.match = None           # preferred patch meets expectation

    def compute(self, dump):
        if self.kind == "stretch":
            return
        # old_base: cachedM on the skip line; new_base: M on the first
        # refilled mode=2 call after the run.
        for i, call in enumerate(dump.mode2_calls):
            if call.frame != self.n or not call.unrefilled:
                continue
            self.old_base = call.cachedm
            for later in dump.mode2_calls[i + 1:]:
                if not later.unrefilled:
                    self.new_base = later.m
                    break
            break
        row_r = dump.summaries.get(self.r)
        if row_r is not None:
            self.cam, self.pool, self.decision = row_r.cam, row_r.pool, row_r.decision
        acted = "ACTED" in self.treats
        scene_r = row_r.scene if (row_r is not None and not row_r.stale) else None
        if not acted:
            self.p = scene_r if vec_ok(scene_r) else None
        else:
            # scene(R) is the held patch already applied; recover P.
            if vec_ok(scene_r) and self.old_base is not None:
                self.p = tuple(s - o for s, o in zip(scene_r, self.old_base))
        row_prev = dump.summaries.get(self.r - 1)
        row_next = dump.summaries.get(self.r + 1)
        if (row_prev is not None and not row_prev.stale
                and vec_ok(row_prev.scene)):
            self.eye_prev = row_prev.scene
        if (row_next is not None and not row_next.stale
                and vec_ok(row_next.scene)):
            self.eye_next = row_next.scene
        if self.p is None:
            return
        patch_held = (tuple(o + p for o, p in zip(self.old_base, self.p))
                      if self.old_base is not None else None)
        patch_new = (tuple(n_ + p for n_, p in zip(self.new_base, self.p))
                     if self.new_base is not None else None)
        if patch_held is not None and self.eye_prev is not None:
            self.res["held_prev"] = dist(patch_held, self.eye_prev)
        if patch_held is not None and self.eye_next is not None:
            self.res["held_next"] = dist(patch_held, self.eye_next)
        if patch_new is not None and self.eye_prev is not None:
            self.res["new_prev"] = dist(patch_new, self.eye_prev)
        if patch_new is not None and self.eye_next is not None:
            self.res["new_next"] = dist(patch_new, self.eye_next)
        self._classify()
        self._prefer()

    def _classify(self):
        # Only the geom evidence can say which frame the objects were in at
        # R. pool ~= cam (both large) means the objects stepped with the
        # camera -- scene already in the NEW frame. pool tiny while cam is
        # huge means ordinary object motion around a camera that already
        # jumped -- scene still in the OLD frame.
        if self.decision == "cameraReset":
            self.cls = "scene-new"
            return
        if self.cam is not None and self.pool is not None:
            if self.cam > 10.0 * max(self.pool, 0.001) and self.cam > 100.0:
                self.cls = "scene-old"
                return
            if self.pool > 0.8 * self.cam and self.cam > 100.0:
                self.cls = "scene-new"
                return
        # No geom (old dumps): the class is genuinely unknowable from the
        # camera alone. A watched event renders head-only at R, which sits
        # near the origin and is therefore always closer to whichever base
        # happens to be shorter -- geometry, not evidence. An ACTED event's
        # scene(R) is the held patch by construction, so the same comparison
        # always answers scene-old. Report "unclear" and let the four
        # residuals speak: each candidate is ~0.0X m from the eye on its own
        # side of the switch and ~frame-delta from the other side.
        self.cls = "unclear"

    def _prefer(self):
        g = self.res.get
        delta = (dist(self.old_base, self.new_base)
                 if self.old_base is not None and self.new_base is not None
                 else None)
        # The wrong patch must miss by at least half the frame-switch
        # distance; the right one must land within head/rounding error.
        small = max(SMALL_RESIDUAL, 0.1 * delta) if delta is not None else None
        huge = 0.5 * delta if delta is not None else None
        if self.cls == "scene-new":
            self.preferred = "new"
            self.match = (g("new_next") is not None and small is not None
                          and g("new_next") < small
                          and g("held_next") is not None
                          and g("held_next") >= huge)
        elif self.cls == "scene-old":
            self.preferred = "held"
            self.match = (g("held_prev") is not None and small is not None
                          and g("held_prev") < small
                          and g("new_prev") is not None
                          and g("new_prev") >= huge)
        else:
            self.preferred = "n/a"
            self.match = None

    def reset_follows(self, dump):
        for f in range(self.last + 1, self.last + 7):
            row = dump.summaries.get(f)
            if row is not None and row.decision == "cameraReset":
                return f
        return None


def find_events(dump):
    events = []
    run = []

    def flush():
        if not run:
            return
        if len(run) <= EVENT_RUN_MAX:
            ev = Event("event", [c.frame for c in run], [c.treat for c in run])
        else:
            ev = Event("stretch", [c.frame for c in run], [c.treat for c in run])
        ev.compute(dump)
        events.append(ev)
        del run[:]

    for call in dump.mode2_calls:
        if call.unrefilled:
            run.append(call)
        else:
            flush()
    flush()
    return events


def report(dump, events, out):
    out.append("== %s" % dump.name)
    for ev in events:
        if ev.kind == "stretch":
            note = ""
            rf = ev.reset_follows(dump)
            if rf is not None:
                note = "; scene-judged reset follows at f%d" % rf
            out.append(
                "  STRETCH f%d..f%d (%d frames, treat=%s)%s"
                % (ev.n, ev.last, ev.run_len, "/".join(ev.treats), note))
            continue
        out.append(
            "  EVENT N=%d R=%d treat=%s run=%d"
            % (ev.n, ev.r, "/".join(ev.treats), ev.run_len))
        out.append("    old_base=%s new_base=%s P=%s"
                   % (fmt_vec(ev.old_base) if ev.old_base else "n/a",
                      fmt_vec(ev.new_base) if ev.new_base else "n/a",
                      fmt_vec(ev.p) if ev.p is not None else "n/a"))
        out.append("    eye_prev=%s eye_next=%s"
                   % (fmt_vec(ev.eye_prev) if ev.eye_prev else "n/a",
                      fmt_vec(ev.eye_next) if ev.eye_next else "n/a"))
        out.append("    res_held_prev=%s res_held_next=%s "
                   "res_new_prev=%s res_new_next=%s (m)"
                   % (fmt_m(ev.res.get("held_prev")), fmt_m(ev.res.get("held_next")),
                      fmt_m(ev.res.get("new_prev")), fmt_m(ev.res.get("new_next"))))
        evidence = "n/a"
        if ev.cam is not None:
            evidence = "cam=%.3f pool=%.3f decision=%s" % (
                ev.cam, ev.pool, ev.decision or "-")
        match = ("yes" if ev.match else "no") if ev.match is not None else "n/a"
        note = ""
        if ev.cls == "unclear" and ev.old_base is not None \
                and ev.new_base is not None:
            note = ("; frame-delta=%.3f m (each patch ~0 on its own side, "
                    "~frame-delta on the other)" % dist(ev.old_base, ev.new_base))
        out.append("    evidence: %s -> %s; preferred=%s; expected-match=%s%s"
                   % (evidence, ev.cls, ev.preferred, match, note))
    counts = {"scene-new": 0, "scene-old": 0, "stretch": 0, "unclear": 0}
    matched = classified = 0
    for ev in events:
        if ev.kind == "stretch":
            counts["stretch"] += 1
            continue
        counts[ev.cls] += 1
        if ev.match is not None:
            classified += 1
            matched += 1 if ev.match else 0
    out.append(
        "  SUMMARY %s: events=%d scene-new=%d scene-old=%d stretch=%d "
        "unclear=%d; preferred==expected %d/%d; skipped_lines=%d"
        % (dump.name, len(events), counts["scene-new"], counts["scene-old"],
           counts["stretch"], counts["unclear"], matched, classified,
           dump.skipped))


def analyze_file(path, out):
    name = os.path.basename(path)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError as exc:
        out.append("== %s: ERROR: %s" % (name, exc))
        return 1
    dump = Dump.parse(lines, name)
    report(dump, find_events(dump), out)
    return 0


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

def _call(frame, seq, status, treat, m, cachedm):
    return ("  f%-6d seq=%-8d t100  mode=2   %-10s treat=%-7s ship=0xABC "
            "M=(%s) cachedM=(%s) age=1    F=(+0.000 +0.000 +0.000) "
            "dt=0.0000 dr=0.000000 wsince=no shipchg=no"
            % (frame, seq, status, treat, m, cachedm))


def _summary(frame, scene, geom=None, decision=None):
    line = ("  f%-6d calls=2   mode1=1 mode2=1          scene=(%s) writerHits=1"
            % (frame, scene))
    if geom is not None:
        cam, pool = geom
        line += (" controllerCalls=1 writerEntered=1 writerWrote=1 "
                 "offered=(+nan +nan +nan) geom=(matched=10 predicted=10 "
                 "cam=%.3f pool=%.3f relMed=0.000 relP90=0.000 predP90=0.000)"
                 % (cam, pool))
    if decision:
        line += " decision=" + decision
    return line


FIXTURE = "\n".join([
    "transition flash eye base: dump, reasons=(self-test), requested trigger frames 98..305",
    "writer table: 1 of 32 unique writer(s)",
    # Scene-NEW watched event: N=100, R=102. Old frame base (1000,2000,3000),
    # new frame base (1010,2000,3000), head P=(0,0,0) exactly.
    _call(99, 4, "refilled", "none", "1000.000 2000.000 3000.000",
          "1000.000 2000.000 3000.000"),
    _call(100, 6, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "1000.000 2000.000 3000.000"),
    _call(101, 8, "refilled", "none", "1010.000 2000.000 3000.000",
          "1010.000 2000.000 3000.000"),
    _call(102, 10, "refilled", "none", "1010.000 2000.000 3000.000",
          "1010.000 2000.000 3000.000"),
    "per-frame rows:",
    _summary(99, "1000.00 2000.00 3000.00"),
    _summary(100, "1000.00 2000.00 3000.00"),
    _summary(101, "1000.00 2000.00 3000.00"),
    _summary(102, "0.00 0.00 0.00", geom=(5000.0, 5100.0),
             decision="cameraReset"),
    _summary(103, "1010.00 2000.00 3000.00"),
    "per-frame rows done, 5 entries",
    # Scene-OLD ACTED event in the OLD summary format (no geom): N=200,
    # R=202. Held base (0,0,4000), new base (0,0,10).
    _call(199, 12, "refilled", "none", "0.000 0.000 4000.000",
          "0.000 0.000 4000.000"),
    _call(200, 14, "UNREFILLED", "ACTED", "+0.000 +0.000 +0.000",
          "0.000 0.000 4000.000"),
    _call(201, 16, "refilled", "none", "0.000 0.000 10.000",
          "0.000 0.000 10.000"),
    "per-frame rows:",
    # Scene-old residual pattern (the ACTED held patch continues the old
    # eye; the new patch would sit ~frame-delta off it). The old summary
    # format carries no geom, so the class must be "unclear": the camera
    # alone cannot say which frame the objects were in at R.
    _summary(199, "0.00 0.00 4000.00"),
    _summary(200, "0.00 0.00 4000.00"),
    _summary(201, "0.00 0.00 4000.00"),
    _summary(202, "0.00 0.00 4000.00"),   # R: held patch, observed
    _summary(203, "0.00 0.00 10.00"),     # eye_next, already in new frame
    "per-frame rows done, 5 entries",
    # Identity-mode stretch, six frames, mailbox never held anything.
    _call(300, 20, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "+0.000 +0.000 +0.000"),
    _call(301, 22, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "+0.000 +0.000 +0.000"),
    _call(302, 24, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "+0.000 +0.000 +0.000"),
    _call(303, 26, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "+0.000 +0.000 +0.000"),
    _call(304, 28, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "+0.000 +0.000 +0.000"),
    _call(305, 30, "UNREFILLED", "watched", "+0.000 +0.000 +0.000",
          "+0.000 +0.000 +0.000"),
    # A STALE scene row: parsed, flagged, and excluded from residual math.
    "  f306   calls=2   mode1=1 mode2=1          scene=STALE-(+0.00 +0.00 +0.00) writerHits=0",
    # A line that looks like data but is malformed: must be counted, not fatal.
    "  f400   seq=broken garbage that matches no field layout",
    "transition flash eye base: dump done, 12 entries",
])


def self_test():
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))

    dump = Dump.parse(FIXTURE.splitlines(), "fixture")
    events = find_events(dump)
    check("event count", len(events) == 3,
          "got %d: %r" % (len(events), [(e.kind, e.n) for e in events]))
    by_n = {e.n: e for e in events}

    ev1 = by_n.get(100)
    check("event1 parsed", ev1 is not None and ev1.kind == "event")
    if ev1 is not None:
        check("R = N + 2", ev1.r == 102, "R=%r" % ev1.r)
        check("old_base", ev1.old_base == (1000.0, 2000.0, 3000.0),
              repr(ev1.old_base))
        check("new_base", ev1.new_base == (1010.0, 2000.0, 3000.0),
              repr(ev1.new_base))
        check("P", ev1.p == (0.0, 0.0, 0.0), repr(ev1.p))
        check("residual arithmetic",
              ev1.res.get("held_prev") == 0.0
              and ev1.res.get("new_next") == 0.0
              and ev1.res.get("held_next") == 10.0
              and ev1.res.get("new_prev") == 10.0,
              repr(ev1.res))
        check("geom evidence", ev1.cam == 5000.0 and ev1.pool == 5100.0
              and ev1.decision == "cameraReset",
              "cam=%r pool=%r decision=%r" % (ev1.cam, ev1.pool, ev1.decision))
        check("scene-new class + preferred new",
              ev1.cls == "scene-new" and ev1.preferred == "new"
              and ev1.match is True,
              "%s %s %s" % (ev1.cls, ev1.preferred, ev1.match))

    ev2 = by_n.get(200)
    check("event2 parsed (old format)", ev2 is not None and ev2.kind == "event")
    if ev2 is not None:
        check("R = N + 2", ev2.r == 202, "R=%r" % ev2.r)
        check("treat ACTED", ev2.treats == ["ACTED"], repr(ev2.treats))
        check("P recovered ~0", ev2.p == (0.0, 0.0, 0.0), repr(ev2.p))
        check("old-format geom absent",
              ev2.cam is None and ev2.pool is None and ev2.decision is None,
              "cam=%r pool=%r decision=%r" % (ev2.cam, ev2.pool, ev2.decision))
        check("scene-old residual pattern",
              ev2.res.get("held_prev") == 0.0
              and ev2.res.get("new_prev") == 3990.0
              and ev2.res.get("held_next") == 3990.0
              and ev2.res.get("new_next") == 0.0,
              repr(ev2.res))
        check("old-format class is unclear (no geom evidence)",
              ev2.cls == "unclear" and ev2.preferred == "n/a"
              and ev2.match is None,
              "%s %s %s" % (ev2.cls, ev2.preferred, ev2.match))

    ev3 = by_n.get(300)
    check("stretch classified", ev3 is not None and ev3.kind == "stretch"
          and ev3.run_len == 6, repr(ev3 and (ev3.kind, ev3.run_len)))
    if ev3 is not None:
        check("stretch has no residual math",
              not ev3.res and ev3.old_base is None and ev3.new_base is None,
              repr(ev3.res))

    row306 = dump.summaries.get(306)
    check("STALE scene row parsed and flagged",
          row306 is not None and row306.stale is True,
          repr(row306 and row306.stale))

    check("skipped malformed lines", dump.skipped == 1, "skipped=%r" % dump.skipped)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        if not ok:
            print("FAIL: %s %s" % (name, detail))
    if failed:
        print("SELF-TEST FAILED: %d/%d checks failed" % (len(failed), len(checks)))
        return 1
    print("SELF-TEST PASS: %d checks (fixture: scene-new watched event, "
          "scene-old ACTED event in old format, stretch, malformed line)"
          % len(checks))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Residual simulator for the transition-flash "
                    "render-time patch: replays eyebase_*.txt dumps and "
                    "computes what each candidate camera base would have "
                    "produced. Read-only, stdout only.")
    parser.add_argument("--file", help="one eyebase dump to analyze")
    parser.add_argument("--dir", help="directory of eyebase dumps")
    parser.add_argument("--pattern", default="eyebase_*.txt",
                        help="glob for --dir (default eyebase_*.txt)")
    parser.add_argument("--self-test", action="store_true",
                        help="run the embedded fixture self-test and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if bool(args.file) == bool(args.dir):
        parser.error("exactly one of --file or --dir is required")

    out = []
    rc = 0
    if args.file:
        rc = analyze_file(args.file, out)
    else:
        try:
            names = sorted(n for n in os.listdir(args.dir)
                           if fnmatch.fnmatch(n, args.pattern))
        except OSError as exc:
            print("ERROR: %s: %s" % (args.dir, exc))
            return 1
        if not names:
            print("no files matching %s in %s" % (args.pattern, args.dir))
            return 1
        for name in names:
            rc |= analyze_file(os.path.join(args.dir, name), out)
    print("\n".join(out))
    return rc


if __name__ == "__main__":
    sys.exit(main())
