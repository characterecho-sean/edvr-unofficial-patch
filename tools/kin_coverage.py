#!/usr/bin/env python3
"""Read out a kinematic-coverage eye burst: what the movers-view mask owned.

    python tools/kin_coverage.py --target frontier
    python tools/kin_coverage.py --target frontier --stamp 160734
    python tools/kin_coverage.py --dir <eyes dir> --stamp 160734
    python tools/kin_coverage.py --self-test

This is the sanctioned replacement for the ad-hoc decode of
eye_<stamp>_KCNear/KCFar/KinSpheres that settled the 2026-09-20 movers-view
question (docs/kinematic-motion-injection-2026-09-19.md, 16:20 entry). One
held-movers-view burst answers, offline:

- did the straddle paint-all branch fire? (a uniform non-sentinel interval
  across ~every texel is its signature -- no projected-rect path produces
  one; the near lane pins the 0.05 m clamp value)
- did the coverage bind flap frame to frame? (decisions.json kin_bound)
- was the veto armed? (kin_veto -- notable, the veto is meant dark)
- which spheres straddle or contain the eye at the paint frame, under each
  camera row set motion.csv carries (the row-provenance question is answered
  by printing all four, not by guessing one)

Formats, both written by writeEyeDecisionArtifacts (temporal_pass.cpp):

  EDVRTEX1: 8-byte magic, nine little-endian uint32 (version, width, height,
  dxgi format, row bytes, frame, 0, 0, 0), then height rows of rowBytes.
  The KC pair is DXGI_FORMAT_R32_UINT (42): near clears to 0 and takes
  InterlockedMax, far clears to 0xFFFFFFFF and takes InterlockedMin; the
  bits are asuint(zraw), zraw = knobs.x + knobs.z / metres (reversed-Z).

  EDVRKSP1: 8-byte magic, four uint32 (count, session, generation lo,
  generation hi), then count x 80-byte KinematicSphereGpu rows (centre 3f,
  radius f, kind u32, reserved 3u32, prevMap 12f).

Depths are reported in metres only under an explicit assumption: knobs.x = 0
and the near lane sat at the straddle clamp (zNearM = 0.05 m), so
knobs.z = near_lane x 0.05 and far metres = knobs.z / far_lane. When the
near lane is not uniform the estimate is suppressed rather than invented.

Read-only by construction, like edvr_log.py: nothing here writes to the
game directory, which is why there is no --dry-run.

Exit 0 when a dump was read, 1 when none was found, 2 on a corrupt or
inconsistent dump.
"""

import argparse
import array
import csv
import json
import math
import os
import struct
import sys
import tempfile
from pathlib import Path

EYE_MAGIC = b"EDVRTEX1"
SPH_MAGIC = b"EDVRKSP1"
R32_UINT = 42
NEAR_EMPTY = 0
FAR_EMPTY = 0xFFFFFFFF
SPHERE_STRIDE = 80
STRADDLE_EPS = 0.05   # the shader's eye-plane epsilon, metres (kinCover)


class CaptureError(Exception):
    pass


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _products_under(root):
    found = []
    products = os.path.join(root, "Products")
    if not os.path.isdir(products):
        return found
    for name in sorted(os.listdir(products)):
        leaf = os.path.join(products, name)
        if os.path.isfile(os.path.join(leaf, "EliteDangerous64.exe")):
            found.append(os.path.normpath(leaf))
    return found


def resolve_game_dir(spec):
    """Same minimal pattern as edvr_log.py: enough to find the eyes dir, and
    a wrong guess costs a message, not an overwritten DLL."""
    if spec not in ("steam", "frontier"):
        p = os.path.abspath(spec)
        if os.path.isdir(p):
            return p
        raise SystemExit("[edvr] no such directory: %s" % p)
    if spec == "frontier":
        local = os.environ.get("LOCALAPPDATA", "")
        found = _products_under(os.path.join(local, "Frontier_Developments")) \
            if local else []
    else:
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Valve\Steam") as k:
                steam = winreg.QueryValueEx(k, "SteamPath")[0]
        except (ImportError, OSError):
            steam = r"C:\Steam"
        found = _products_under(os.path.join(steam, "steamapps", "common",
                                             "Elite Dangerous"))
    if not found:
        raise SystemExit("[edvr] no %s install found; pass a path to "
                         "--target." % spec)
    if len(found) > 1:
        raise SystemExit("[edvr] %d installs found; name one:\n       %s"
                         % (len(found), "\n       ".join(found)))
    return found[0]


def load_kc_tex(path):
    """EDVRTEX1 R32_UINT -> (width, height, frame, array('I') of w*h)."""
    data = Path(path).read_bytes()
    if len(data) < 44 or data[:8] != EYE_MAGIC:
        raise CaptureError("%s: not an EDVRTEX1 capture" % path)
    ver, w, h, fmt, row_bytes, frame, _r0, _r1, _r2 = \
        struct.unpack_from("<9I", data, 8)
    if fmt != R32_UINT or row_bytes != w * 4:
        raise CaptureError("%s: expected R32_UINT tightly packed, "
                           "got format %d rowBytes %d" % (path, fmt, row_bytes))
    need = 44 + h * row_bytes
    if len(data) < need:
        raise CaptureError("%s: truncated payload" % path)
    tex = array.array("I")
    tex.frombytes(data[44:need])
    if struct.pack("=I", 1) != struct.pack("<I", 1):   # big-endian host
        tex.byteswap()
    return w, h, frame, tex


def load_spheres(path):
    """EDVRKSP1 -> dict(count, session, generation, spheres=[(x,y,z,r,kind)])."""
    data = Path(path).read_bytes()
    if len(data) < 24 or data[:8] != SPH_MAGIC:
        raise CaptureError("%s: not an EDVRKSP1 capture" % path)
    count, session, gen_lo, gen_hi = struct.unpack_from("<4I", data, 8)
    need = 24 + count * SPHERE_STRIDE
    if len(data) < need:
        raise CaptureError("%s: truncated sphere payload" % path)
    spheres = []
    for i in range(count):
        cx, cy, cz, radius, kind = struct.unpack_from("<4fI", data, 24 + i * SPHERE_STRIDE)
        spheres.append((cx, cy, cz, radius, kind))
    return {"count": count, "session": session,
            "generation": gen_lo | (gen_hi << 32), "spheres": spheres}


def bits_to_float(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def coverage_facts(w, h, frame, tex_near, tex_far):
    texels = w * h
    claimed = 0
    half_written = 0
    pair_counts = {}
    for i in range(texels):
        n = tex_near[i]
        f = tex_far[i]
        is_claimed = (n != NEAR_EMPTY) or (f != FAR_EMPTY)
        if is_claimed and not ((n != NEAR_EMPTY) and (f != FAR_EMPTY)):
            half_written += 1
        if is_claimed:
            claimed += 1
            pair_counts[(n, f)] = pair_counts.get((n, f), 0) + 1
    dom_pair, dom_count = (None, 0)
    if pair_counts:
        dom_pair, dom_count = max(pair_counts.items(), key=lambda kv: kv[1])
    uniform = claimed > 0 and dom_count == claimed
    paint_all = (claimed >= texels * 0.99 and dom_count >= claimed * 0.99)
    return {"frame": frame, "texels": texels, "claimed": claimed,
            "half_written": half_written, "dom_pair": dom_pair,
            "dom_share": (dom_count / claimed) if claimed else 0.0,
            "uniform": uniform, "paint_all": paint_all}


def rowsets_from_motion(row):
    """The 3x4 camera row sets a motion.csv row carries, name -> 3x4 tuple."""
    out = {}
    for name in ("cameraR", "now", "prev", "headR"):
        try:
            vals = [float(row["%s%d" % (name, i)]) for i in range(12)]
        except (KeyError, ValueError):
            continue
        out[name] = (vals[0:4], vals[4:8], vals[8:12])
    return out


def straddle_stats(spheres, rows):
    """Per the shader: behind iff zv + r <= eps; straddle iff zv - r <= eps
    (and not behind); container iff |rel| < r. rows is a 3x4 tuple."""
    eye = (rows[0][3], rows[1][3], rows[2][3])
    fwd = (rows[2][0], rows[2][1], rows[2][2])
    behind = straddle = containers = 0
    straddlers = []
    for idx, (cx, cy, cz, r, kind) in enumerate(spheres):
        if r <= 0.0:
            continue
        rel = (cx - eye[0], cy - eye[1], cz - eye[2])
        zv = rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2]
        if zv + r <= STRADDLE_EPS:
            behind += 1
            continue
        if zv - r <= STRADDLE_EPS:
            straddle += 1
            straddlers.append((idx, r, zv, zv + r))
        if math.sqrt(rel[0] ** 2 + rel[1] ** 2 + rel[2] ** 2) < r:
            containers += 1
    return {"behind": behind, "straddle": straddle, "containers": containers,
            "straddlers": straddlers}


def pick_motion_row(rows, frame, eye):
    """Exact frame+eye first, then nearest frame for that eye."""
    best = None
    best_dist = None
    for r in rows:
        try:
            f = int(r.get("frame", ""))
            e = int(r.get("eye", ""))
        except ValueError:
            continue
        if e != eye:
            continue
        dist = abs(f - frame)
        if f == frame:
            return r
        if best is None or dist < best_dist:
            best, best_dist = r, dist
    return best


def analyze(eyes_dir, stamp):
    """Read one burst. Returns a facts dict; raises CaptureError on corruption.
    Read-only: every file is opened for reading and nothing else."""
    eyes = Path(eyes_dir)
    manifest_path = eyes / ("eye_%s_decisions.json" % stamp)
    if not manifest_path.is_file():
        raise CaptureError("no decisions.json for stamp %s in %s" % (stamp, eyes))
    root = json.loads(manifest_path.read_text(encoding="utf-8"))
    facts = {"stamp": stamp, "schema": root.get("schema"),
             "kinematic": root.get("kinematic"), "frames": []}
    for fr in root.get("frames", []):
        facts["frames"].append({"frame": fr.get("frame"),
                                "kin_bound": fr.get("kin_bound"),
                                "kin_veto": fr.get("kin_veto")})
    kin = facts["kinematic"]
    if not kin:
        return facts   # pre-stage-B manifest: the frame table is all there is

    for eye in (0, 1):
        for lane, key in (("kc_near", "near"), ("kc_far", "far")):
            names = kin.get(lane)
            if not names or not names[eye]:
                continue
            facts.setdefault("files", {})["%s%d" % (key, eye)] = str(eyes / names[eye])

    cov = {}
    near_facts = far_facts = None
    files = facts.get("files", {})
    if "near0" in files and "far0" in files:
        for eye in (0, 1):
            nk, fk = "near%d" % eye, "far%d" % eye
            if nk not in files or fk not in files:
                continue
            w, h, fn, tn = load_kc_tex(files[nk])
            w2, h2, ff, tf = load_kc_tex(files[fk])
            if (w, h) != (w2, h2):
                raise CaptureError("near/far size mismatch on eye %d" % eye)
            near_facts = coverage_facts(w, h, fn, tn, tf)
            cov[eye] = near_facts
    facts["coverage"] = cov

    if kin.get("spheres_file"):
        sph = load_spheres(eyes / kin["spheres_file"])
        sph["gen_matches_manifest"] = (sph["generation"] == kin.get("generation")
                                       and sph["count"] == kin.get("count")
                                       and sph["session"] == kin.get("session"))
        facts["spheres"] = sph

    motion_path = eyes / ("eye_%s_motion.csv" % stamp)
    if motion_path.is_file() and "spheres" in facts:
        with motion_path.open(newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        paint = kin.get("kc_paint_frame") or [kin.get("kc_frame", 0)] * 2
        per_rows = {}
        for eye in (0, 1):
            row = pick_motion_row(rows, int(paint[eye] or kin.get("kc_frame", 0)), eye)
            if row:
                per_rows[eye] = {"frame": int(row["frame"]),
                                 "rowsets": rowsets_from_motion(row)}
        facts["motion"] = per_rows
        sphere_sets = facts["spheres"]["spheres"]
        strat = {}
        for eye, pack in per_rows.items():
            for name, rs in pack["rowsets"].items():
                strat["eye%d/%s" % (eye, name)] = straddle_stats(sphere_sets, rs)
        facts["straddle"] = strat
    return facts


def report(facts):
    out = []
    stamp = facts["stamp"]
    out.append("burst %s (schema %s)" % (stamp, facts["schema"]))
    kin = facts.get("kinematic")
    if not kin:
        out.append("  no kinematic block -- a pre-stage-B manifest; "
                   "the frame table below is all it carries")
    else:
        out.append("  kinematic: session %s, generation %s, count %s, "
                   "kc_frame %s, kc_paint_frame %s, kc_size %s"
                   % (kin.get("session"), kin.get("generation"), kin.get("count"),
                      kin.get("kc_frame"), kin.get("kc_paint_frame"),
                      kin.get("kc_size")))
    frames = facts["frames"]
    if frames:
        bounds = [bool(f["kin_bound"]) for f in frames]
        vetoes = [bool(f["kin_veto"]) for f in frames]
        flapped = [frames[i]["frame"] for i in range(1, len(frames))
                   if bounds[i] != bounds[i - 1]]
        if all(bounds) and not flapped:
            out.append("  kin_bound: solid true on all %d frames "
                       "(bind flapping refuted)" % len(frames))
        elif not any(bounds):
            out.append("  kin_bound: false on all %d frames -- never bound "
                       "(stage B stood down?)" % len(frames))
        else:
            out.append("  kin_bound: FLAPPED -- transitions at frames %s"
                       % ", ".join(str(f) for f in flapped))
        if any(vetoes):
            out.append("  kin_veto: ARMED on frames %s -- the veto was live"
                       % ", ".join(str(f["frame"]) for f in frames if f["kin_veto"]))
        else:
            out.append("  kin_veto: false on all frames (veto dark, as designed)")
    for eye, c in sorted(facts.get("coverage", {}).items()):
        pct = 100.0 * c["claimed"] / c["texels"] if c["texels"] else 0.0
        out.append("  eye%d coverage: %d/%d texels claimed (%.1f%%), "
                   "half-written %d" % (eye, c["claimed"], c["texels"], pct,
                                        c["half_written"]))
        if c["dom_pair"]:
            n, f = c["dom_pair"]
            nf, ff = bits_to_float(n), bits_to_float(f)
            line = ("    dominant pair: near %.6g far %.6g, share %.1f%% of claimed"
                    % (nf, ff, 100.0 * c["dom_share"]))
            if c["uniform"] and nf > 0.0 and ff > 0.0:
                knobs_z = nf * STRADDLE_EPS   # assumes knobs.x = 0, clamp lane
                metres = knobs_z / ff
                line += "  (~%.0f m far side, assuming knobs.x=0 and the " \
                        "0.05 m clamp lane)" % metres
            out.append(line)
        if c["paint_all"]:
            out.append("    PAINT-ALL SIGNATURE: ~every texel carries one "
                       "interval -- the straddle branch fired this frame")
    sph = facts.get("spheres")
    if sph:
        out.append("  spheres: count %d, session %d, generation %d%s"
                   % (sph["count"], sph["session"], sph["generation"],
                      "" if sph["gen_matches_manifest"]
                      else "  (MISMATCH with the manifest -- the tracker "
                           "re-published during the burst)"))
    for key, s in sorted(facts.get("straddle", {}).items()):
        out.append("  straddle under %s rows (frame %s): %d straddling, "
                   "%d containing the eye, %d wholly behind"
                   % (key, facts["motion"][int(key[3])]["frame"],
                      s["straddle"], s["containers"], s["behind"]))
        for idx, r, zv, zvr in s["straddlers"][:6]:
            out.append("    sphere[%d] r=%.1f m zv=%.2f zv+r=%.2f"
                       % (idx, r, zv, zvr))
    return "\n".join(out)


def latest_stamp(eyes_dir):
    stamps = []
    for p in Path(eyes_dir).glob("eye_*_decisions.json"):
        stamps.append((p.stat().st_mtime, p.name[len("eye_"):-len("_decisions.json")]))
    if not stamps:
        return None
    stamps.sort()
    return stamps[-1][1]


def self_test():
    """Synthetic burst in a temp dir; asserts the decode and every verdict.
    Writes only inside a TemporaryDirectory -- the build gate runs this."""
    checks = [0, 0]

    def check(cond, name):
        checks[0] += 1
        if cond:
            print("  ok    %s" % name)
        else:
            checks[1] += 1
            print("  FAIL  %s" % name)

    def write_tex(path, w, h, frame, fill):
        payload = array.array("I", fill)
        if struct.pack("=I", 1) != struct.pack("<I", 1):
            payload.byteswap()
        header = struct.pack("<9I", 1, w, h, R32_UINT, w * 4, frame, 0, 0, 0)
        Path(path).write_bytes(EYE_MAGIC + header + payload.tobytes())

    def write_spheres(path, spheres, session, gen):
        header = struct.pack("<4I", len(spheres), session,
                             gen & 0xFFFFFFFF, (gen >> 32) & 0xFFFFFFFF)
        body = b"".join(struct.pack("<4fI3I12f", x, y, z, r, kind,
                                    0, 0, 0, *([0.0] * 12))
                        for (x, y, z, r, kind) in spheres)
        Path(path).write_bytes(SPH_MAGIC + header + body)

    near_bits = struct.unpack("<I", struct.pack("<f", 0.5))[0]
    far_bits = struct.unpack("<I", struct.pack("<f", 3.7287016e-05))[0]
    spheres = [(0.0, 0.0, 10.0, 1.0, 0),     # in front, no straddle
               (0.0, 0.0, 0.03, 0.05, 0),    # straddles the eye plane
               (0.0, 0.0, -5.0, 1.0, 0)]     # wholly behind
    ident = [1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0]

    def make_burst(eyes, stamp, bounds):
        for eye in (0, 1):
            for lane, fill_bit in (("KCNear", near_bits), ("KCFar", far_bits)):
                other = FAR_EMPTY if lane == "KCFar" else NEAR_EMPTY
                if eye == 0:
                    write_tex(eyes / ("eye_%s_%s%d.bin" % (stamp, lane, eye)),
                              4, 3, 100, [fill_bit] * 12)
                else:
                    write_tex(eyes / ("eye_%s_%s%d.bin" % (stamp, lane, eye)),
                              4, 3, 100, [other] * 12)
        write_spheres(eyes / ("eye_%s_KinSpheres.bin" % stamp), spheres, 1, 7)
        header = "frame,eye," + ",".join("cameraR%d" % i for i in range(12)) + "\n"
        row = "100,0," + ",".join("%.9g" % v for v in ident) + "\n"
        row += "100,1," + ",".join("%.9g" % v for v in ident) + "\n"
        (eyes / ("eye_%s_motion.csv" % stamp)).write_text(header + row,
                                                          encoding="utf-8")
        frames = [{"index": i, "frame": 98 + i, "kin_bound": b, "kin_veto": False}
                  for i, b in enumerate(bounds)]
        manifest = {"schema": 2, "stamp": stamp, "requested": len(bounds),
                    "kinematic": {"session": 1, "generation": 7, "count": 3,
                                  "kc_frame": 100, "kc_paint_frame": [100, 100],
                                  "kc_size": [4, 3],
                                  "spheres_file": "eye_%s_KinSpheres.bin" % stamp,
                                  "kc_near": ["eye_%s_KCNear0.bin" % stamp,
                                              "eye_%s_KCNear1.bin" % stamp],
                                  "kc_far": ["eye_%s_KCFar0.bin" % stamp,
                                             "eye_%s_KCFar1.bin" % stamp]},
                    "frames": frames}
        (eyes / ("eye_%s_decisions.json" % stamp)).write_text(
            json.dumps(manifest), encoding="utf-8")

    with tempfile.TemporaryDirectory() as tmp:
        eyes = Path(tmp)
        make_burst(eyes, "111111", [True, True, True])
        make_burst(eyes, "222222", [True, False, True])

        facts = analyze(eyes, "111111")
        check(facts["schema"] == 2, "schema read")
        kin = facts["kinematic"]
        check(kin["generation"] == 7 and kin["count"] == 3, "kinematic block")
        c0 = facts["coverage"][0]
        check(c0["claimed"] == 12 and c0["texels"] == 12, "eye0 fully claimed")
        check(c0["uniform"] and c0["paint_all"], "paint-all signature detected")
        check(c0["half_written"] == 0, "no half-written texels")
        c1 = facts["coverage"][1]
        check(c1["claimed"] == 0 and not c1["paint_all"], "eye1 sentinel-only")
        sph = facts["spheres"]
        check(sph["gen_matches_manifest"], "sphere header matches manifest")
        strat = facts["straddle"]["eye0/cameraR"]
        check(strat["straddle"] == 1 and strat["behind"] == 1
              and strat["containers"] == 1, "straddle/behind/container counts")
        check(strat["straddlers"][0][0] == 1, "the straddler is sphere[1]")
        text = report(facts)
        check("PAINT-ALL SIGNATURE" in text, "paint-all verdict printed")
        check("bind flapping refuted" in text, "solid-bind verdict printed")
        check("far side" in text and "670 m" in text, "far-side metres estimated")
        check("veto dark" in text, "veto-dark verdict printed")

        flap = report(analyze(eyes, "222222"))
        check("FLAPPED" in flap and "100" in flap, "bind flap detected at the frame")

        pre = {"schema": 1, "stamp": "333333", "requested": 1, "frames": []}
        (eyes / "eye_333333_decisions.json").write_text(json.dumps(pre),
                                                        encoding="utf-8")
        old = report(analyze(eyes, "333333"))
        check("no kinematic block" in old, "pre-stage-B manifest tolerated")

        try:
            analyze(eyes, "999999")
            check(False, "missing stamp raises")
        except CaptureError:
            check(True, "missing stamp raises")

        check(latest_stamp(eyes) == "333333", "latest stamp by mtime order")

        root = eyes / "fd"
        leaf = root / "Products" / "elite-dangerous-odyssey-64"
        leaf.mkdir(parents=True)
        (leaf / "EliteDangerous64.exe").write_bytes(b"")
        check(_products_under(str(root)) == [os.path.normpath(str(leaf))],
              "products resolution joins Products itself")

    print("kin_coverage: %d checks, %d failures" % (checks[0], checks[1]))
    return 1 if checks[1] else 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Read out a kinematic-coverage eye burst (movers view).")
    ap.add_argument("--target", default="frontier",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--dir", default=None,
                    help="this eyes directory directly, ignoring --target")
    ap.add_argument("--stamp", default=None,
                    help="the burst stamp (eye_<stamp>_decisions.json); "
                         "default is the newest")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()

    eyes = Path(args.dir) if args.dir else \
        Path(resolve_game_dir(args.target)) / "edvr_logs" / "eyes"
    if not eyes.is_dir():
        print("[edvr] no eyes directory at %s -- no burst captured yet?" % eyes)
        return 1
    stamp = args.stamp or latest_stamp(eyes)
    if not stamp:
        print("[edvr] no eye_*_decisions.json in %s" % eyes)
        return 1
    try:
        facts = analyze(eyes, stamp)
    except CaptureError as exc:
        print("[edvr] %s" % exc)
        return 2
    print(report(facts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
