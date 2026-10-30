#!/usr/bin/env python3
"""Which draw of a sight's effects paints what: the footprint each candidate leaves in an eye dump taken with the withhold cycle.

    python tools/sight_candidates.py --target epic-dir-or-steam-or-frontier [--stamp 194734935001] [--kind C] [--json]
    python tools/sight_candidates.py --dir <edvr_logs/eyes> --stamp 194734935001
    python tools/sight_candidates.py --self-test

A MANUAL eye dump (the eye dump key) taken with the sights up leaves one candidate draw of the sight's effects out of each of its sixteen frames, cycling through a list
(src/d3d11/sight_withhold.h): the scope glass' C5A5 draws by shape, the unidentified draws of the same pass, and the gunsight quad as a control. The dump's json
(eye_<stamp>_decisions.json, and eye_<stamp>_withhold.json beside it) names the candidate each frame left out and how many draws of it (eye 0). The first frame of a run, the frame the
key was pressed in, is untouched: it is the run's baseline.

THE SCENE BEFORE DLSS is the raw crop C00..C15: the picture the game hands the upscaler, 1400 px square about the render's centre, before any history, saved by the dump as 24-bit
BMPs. (D00..D15 are the decision maps and DlssBeforeUi the model's output before the UI cleanup: neither is the scene. T and P are after DLSS. FinalCrisp_L00..L15 are the final picture,
with the weapons layer composited in: the one to read for a draw the sharp weapons layer carries, such as the gunsight quad when the crisp reticle is on; --kind FinalCrisp.) When a
withhold run is dumped the alpha plane of each raw crop is saved too (CA00..CA15, grey BMPs; a format with no alpha writes none, and the report says n/a).

The method: the reference is the per-pixel median of all the frames (each candidate is left out of one or two of sixteen, so it holds every draw). A frame's footprint is the pixels
where it differs from the reference by more than --threshold in any channel (default 10 of 255), kept when at least --open of their 3 x 3 neighbours changed too (default 3: a one pixel
line passes, jitter's isolated pixels do not). Per frame the report gives the candidate, the draws left out, the pixel count, the bounding box in the crop and in the render, the mean
colour and alpha change the draw makes (present minus withheld), and the changed pixels before the opening. Frames that left nothing out give the noise floor. A candidate with 0
draws left out in a frame was not drawn in it: a footprint there is motion.

Exit 0 on a report, 2 when the dump has no withhold record or its files are not there.

--self-test builds a run in the temp folder from known rectangles (colour and alpha), checks the footprints, bounding boxes, mean changes, the noise floor, the json fields against
src/d3d11/temporal_pass.cpp's own literals, the BMP reader and the failure modes.
"""
import argparse
import contextlib
import io
import json
import os
import struct
import sys
import tempfile

import numpy as np

KINDS = {"C": "C%02d", "T": "T%02d", "P": "P%02d", "FinalCrisp": "FinalCrisp_L%02d"}
RUN = 16
NONE = "none"


def read_bmp(path):
    """A 24-bit BI_RGB BMP as an (h, w, 3) uint8 RGB array, top row first."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM" or len(data) < 54:
        raise ValueError("%s is not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h, planes, bits, comp = struct.unpack_from("<iiHHI", data, 18)
    if bits != 24 or comp != 0 or planes != 1 or w <= 0 or h == 0:
        raise ValueError("%s is not a 24-bit uncompressed BMP" % path)
    flip = h > 0
    h = abs(h)
    row = (w * 3 + 3) & ~3
    if len(data) < off + row * h:
        raise ValueError("%s is cut short" % path)
    rows = np.frombuffer(data, dtype=np.uint8, count=row * h, offset=off).reshape(h, row)[:, :w * 3].reshape(h, w, 3)
    if flip:
        rows = rows[::-1]
    return rows[:, :, ::-1].copy()   # BGR to RGB


def write_bmp(path, rgb):
    """The inverse, for the self-test: rgb is (h, w, 3) uint8, top row first."""
    h, w, _ = rgb.shape
    row = (w * 3 + 3) & ~3
    out = np.zeros((h, row), dtype=np.uint8)
    out[:, :w * 3] = rgb[::-1, :, ::-1].reshape(h, w * 3)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + row * h, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, row * h, 0, 0, 0, 0))
        f.write(out.tobytes())


def load_run(folder, stamp, kind="C"):
    """The run's record and frames: {candidates, armed_frame, frames: [{index, candidate, draws, frame, path, alpha_path, origin}]}. Raises ValueError with a reason."""
    dec = os.path.join(folder, "eye_%s_decisions.json" % stamp)
    wh = os.path.join(folder, "eye_%s_withhold.json" % stamp)
    doc, src = None, None
    for path in (dec, wh):
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8") as f:
                cand = json.load(f)
            if "sight_withhold" in cand:
                doc, src = cand, path
                break
    if doc is None:
        raise ValueError("eye_%s_decisions.json and eye_%s_withhold.json have no sight_withhold record in %s: the dump was not taken with the sights up by the eye dump key, "
                         "or by a build before C1b" % (stamp, stamp, folder))
    head = doc["sight_withhold"]
    template = KINDS[kind]
    frames = []
    for fr in doc["frames"]:
        k = int(fr["index"])
        name = "eye_%s_%s.bmp" % (stamp, template % k)
        crop = fr.get("decision_crop") or [0, 0, 0, 0]
        alpha = fr.get("alpha_file")
        frames.append({"index": k, "candidate": fr["withheld"], "draws": list(fr["withheld_draws"]), "frame": int(fr["withheld_frame"]), "path": os.path.join(folder, name),
                       "alpha_path": os.path.join(folder, alpha) if alpha and kind == "C" else None, "origin": (int(crop[0]), int(crop[1]))})
    return {"stamp": stamp, "source": src, "kind": kind, "candidates": list(head["candidates"]), "armed_frame": int(head["armed_frame"]), "frames": frames}


def median_reference(stack):
    """The per-pixel median over the first axis, the upper middle of an even count (uint8 in, uint8 out)."""
    return np.sort(stack, axis=0)[stack.shape[0] // 2]


def open_mask(mask, need):
    """mask pixels with at least `need` changed pixels in their 3 x 3 neighbourhood (themselves included)."""
    if need <= 1:
        return mask.copy()
    p = np.pad(mask.astype(np.uint8), 1)
    h, w = mask.shape
    count = np.zeros(mask.shape, dtype=np.uint8)
    for dy in range(3):
        for dx in range(3):
            count += p[dy:dy + h, dx:dx + w]
    return mask & (count >= need)


def footprint(frame, ref, threshold, need, alpha=None, ref_alpha=None):
    """The footprint of one frame against the reference: dict of the changed pixel count before the opening, the pixels kept, the bbox (x0, y0, x1, y1 inclusive, or None) and the
    mean change the draw makes (present minus withheld, so the reference minus the frame) in each colour channel and in alpha (None without the planes)."""
    d = np.abs(frame.astype(np.int16) - ref.astype(np.int16)).max(axis=2)
    changed = d > threshold
    mask = open_mask(changed, need)
    n = int(mask.sum())
    out = {"changed": int(changed.sum()), "pixels": n, "bbox": None, "colour": None, "luma": None, "alpha": None}
    if n:
        ys, xs = np.nonzero(mask)
        out["bbox"] = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
        delta = (ref[mask].astype(np.float64) - frame[mask].astype(np.float64)).mean(axis=0)
        out["colour"] = [float(v) for v in delta]
        out["luma"] = float(0.299 * delta[0] + 0.587 * delta[1] + 0.114 * delta[2])
        if alpha is not None and ref_alpha is not None:
            out["alpha"] = float((ref_alpha[mask].astype(np.float64) - alpha[mask].astype(np.float64)).mean())
    return out


def analyse(run, threshold=10, need=3):
    """Per-frame results (the run's frames in order, each with its footprint), the per-candidate summary and the noise floor (the frames that left nothing out)."""
    frames = run["frames"]
    if not frames:
        raise ValueError("the run has no frames")
    imgs = [read_bmp(f["path"]) for f in frames]
    shape = imgs[0].shape
    if any(i.shape != shape for i in imgs):
        raise ValueError("the frames are not all the same size")
    ref = median_reference(np.stack(imgs))
    alphas = None
    if all(f["alpha_path"] and os.path.isfile(f["alpha_path"]) for f in frames):
        alphas = [read_bmp(f["alpha_path"])[:, :, 0] for f in frames]
        ref_alpha = median_reference(np.stack(alphas))
    results = []
    for i, f in enumerate(frames):
        fp = footprint(imgs[i], ref, threshold, need, alphas[i] if alphas else None, ref_alpha if alphas else None)
        row = {"index": f["index"], "candidate": f["candidate"], "draws": f["draws"], "frame": f["frame"], "origin": f["origin"]}
        row.update(fp)
        if fp["bbox"]:
            x0, y0, x1, y1 = fp["bbox"]
            row["bbox_render"] = (x0 + f["origin"][0], y0 + f["origin"][1], x1 + f["origin"][0], y1 + f["origin"][1])
        else:
            row["bbox_render"] = None
        results.append(row)
    summary = {}
    for r in results:
        if r["candidate"] == NONE:
            continue
        s = summary.setdefault(r["candidate"], {"frames": 0, "draws": 0, "pixels": [], "alpha": None})
        s["frames"] += 1
        s["draws"] += r["draws"][0]
        s["pixels"].append(r["pixels"])
    noise = [r["pixels"] for r in results if r["candidate"] == NONE]
    return {"results": results, "summary": summary, "noise": noise, "alpha_planes": alphas is not None, "size": (shape[1], shape[0]), "threshold": threshold, "open": need}


def report(run, a):
    w, h = a["size"]
    lines = ["sight candidates: run %s (%s), %s crops %dx%d, threshold %d, opening %d; candidates in the cycle: %s; the cycle started after game frame %d%s"
             % (run["stamp"], os.path.basename(run["source"]), run["kind"], w, h, a["threshold"], a["open"], ", ".join(run["candidates"]), run["armed_frame"],
                "" if a["alpha_planes"] else "; no alpha planes (alpha change n/a)")]
    lines.append("per frame (mean change = the draw present minus withheld, RGB / luma / alpha):")
    for r in a["results"]:
        if r["bbox"]:
            box = "bbox crop (%d,%d)-(%d,%d) render (%d,%d)-(%d,%d)" % (r["bbox"] + r["bbox_render"])
            col = "colour %+.1f %+.1f %+.1f luma %+.1f alpha %s" % (r["colour"][0], r["colour"][1], r["colour"][2], r["luma"], "n/a" if r["alpha"] is None else "%+.1f" % r["alpha"])
        else:
            box, col = "no footprint", ""
        note = ""
        if r["candidate"] != NONE and r["draws"][0] == 0:
            note = "  [no draw of it was left out: it was not drawn in this frame, so any footprint is motion]"
        lines.append("  frame %2d (game frame %d) %-12s draws left out L %d R %d: %6d px (%d before the opening); %s %s%s"
                     % (r["index"], r["frame"], r["candidate"], r["draws"][0], r["draws"][1], r["pixels"], r["changed"], box, col, note))
    if a["noise"]:
        lines.append("noise floor (frames that left nothing out): %s px" % ", ".join(str(n) for n in a["noise"]))
        if max(a["noise"]) > 0.01 * w * h:
            lines.append("WARNING: a frame that left nothing out already differs from the median on more than 1% of the crop: the scene moved (the head, the weapon or the world); "
                         "hold the sights still through the dump, or the footprints below are motion")
    else:
        lines.append("noise floor: no frame left nothing out (the key was pressed before the cycle began, or the sights were down)")
    named = [r for r in a["results"] if r["candidate"] != NONE]
    if not named:
        lines.append("WARNING: no frame names a candidate: the sights were down for the whole run, so nothing was left out")
    elif not any(r["draws"][0] for r in named):
        lines.append("WARNING: no draw of any candidate was left out in eye 0 in any frame: none of the candidates was drawn while the cycle ran (the sights were not up in the scene, or the shader pairs "
                     "are not this build's); the footprints below are motion")
    lines.append("per candidate:")
    for name in run["candidates"]:
        s = a["summary"].get(name)
        if not s:
            lines.append("  %-12s not withheld in any frame" % name)
            continue
        lines.append("  %-12s withheld in %d frame(s), %d draw(s) left out in eye 0: %s px" % (name, s["frames"], s["draws"], ", ".join(str(p) for p in s["pixels"])))
    return "\n".join(lines)


def to_json(run, a):
    return json.dumps({"stamp": run["stamp"], "kind": run["kind"], "size": a["size"], "threshold": a["threshold"], "open": a["open"], "armed_frame": run["armed_frame"],
                       "candidates": run["candidates"], "noise": a["noise"], "frames": a["results"], "summary": a["summary"]}, indent=1)


def newest_stamp(folder):
    best = None
    for name in os.listdir(folder):
        if name.startswith("eye_") and (name.endswith("_decisions.json") or name.endswith("_withhold.json")):
            path = os.path.join(folder, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    if "sight_withhold" not in json.load(f):
                        continue
            except (OSError, ValueError):
                continue
            stamp = name.split("_")[1]
            m = os.path.getmtime(path)
            if best is None or m > best[0]:
                best = (m, stamp)
    return best[1] if best else None


def eyes_folder(target):
    """edvr_logs/eyes for a target (steam, frontier or a path), or the folder itself when it holds the dump."""
    if target and os.path.isdir(target) and any(n.startswith("eye_") for n in os.listdir(target)):
        return target
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import edvr_log
        game = edvr_log.resolve_target(target)
    except SystemExit as e:
        raise ValueError(str(e))
    folder = os.path.join(game, "edvr_logs", "eyes")
    if not os.path.isdir(folder):
        raise ValueError("no %s: no eye dump was taken there" % folder)
    return folder


def main(argv=None):
    ap = argparse.ArgumentParser(description="Per-candidate footprints of a sight withhold eye dump.")
    ap.add_argument("--target", help="steam, frontier, or the game's directory (its edvr_logs/eyes holds the dump)")
    ap.add_argument("--dir", help="the folder holding the dump's files")
    ap.add_argument("--stamp", help="the dump's stamp (default: the newest dump with a withhold record)")
    ap.add_argument("--kind", choices=sorted(KINDS), default="C", help="which per-frame image: C raw crop (the scene before DLSS, default), T treated, P before UI, FinalCrisp final")
    ap.add_argument("--threshold", type=int, default=10, help="a pixel changed when a channel differs from the median by more than this (of 255)")
    ap.add_argument("--open", type=int, default=3, dest="need", help="keep a changed pixel with at least this many changed in its 3x3 neighbourhood, itself included")
    ap.add_argument("--json", action="store_true", help="print the result as json")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return 0 if self_test() else 1
    try:
        folder = args.dir or eyes_folder(args.target or "steam")
        stamp = args.stamp or newest_stamp(folder)
        if not stamp:
            raise ValueError("no dump with a sight_withhold record in %s" % folder)
        run = load_run(folder, stamp, args.kind)
        for f in run["frames"]:
            if not os.path.isfile(f["path"]):
                raise ValueError("%s is not there" % f["path"])
        a = analyse(run, args.threshold, args.need)
    except (ValueError, OSError, KeyError) as e:
        print("sight_candidates: %s" % e)
        return 2
    print(to_json(run, a) if args.json else report(run, a))
    return 0


# ---- the self-test ----------------------------------------------------------------------------------------------------------------------------------------------------------------

def self_test():
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("FAIL: %s" % what)
            ok = False

    def quiet(argv):
        """main() with its output held back; its exit code and its text."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    names = ["C5A5_n18_i1", "C5A5_n6_i5", "C5A5_n6_i2", "PS_66B08F89", "QUAD_46F9"]
    # the candidates' rectangles (x0, y0, x1, y1 inclusive), their colour effect (added when present) and alpha effect
    rects = {"C5A5_n18_i1": ((20, 18, 59, 41), (60, 0, 0), 0), "C5A5_n6_i5": ((10, 50, 29, 61), (0, 50, 0), 40),
             "C5A5_n6_i2": ((70, 10, 79, 69), (0, 0, 70), 0), "PS_66B08F89": ((40, 44, 41, 44), (-30, -30, -30), 0), "QUAD_46F9": ((48, 30, 51, 33), (80, 80, 0), 100)}
    w, h = 96, 80
    rng = np.random.RandomState(7)
    base = np.zeros((h, w, 3), dtype=np.uint8)
    base[:, :, 0] = np.linspace(40, 140, w).astype(np.uint8)[None, :]
    base[:, :, 1] = np.linspace(60, 120, h).astype(np.uint8)[:, None]
    base[:, :, 2] = 90
    with tempfile.TemporaryDirectory() as tmp:
        stamp = "123456789001"
        order = [NONE] + [names[(k - 1) % len(names)] for k in range(1, RUN)]   # the first frame is the key press's, untouched; then the list in order
        frames = []
        for k in range(RUN):
            img = base.astype(np.int16) + rng.randint(-2, 3, size=base.shape)   # noise under the threshold
            al = np.full((h, w), 100, dtype=np.int16)
            drew = 0 if order[k] == NONE else 3
            for name in names:
                (x0, y0, x1, y1), dc, da = rects[name]
                if name == order[k]:
                    continue   # withheld: the rectangle gets no addition
                img[y0:y1 + 1, x0:x1 + 1] += np.array(dc, dtype=np.int16)
                al[y0:y1 + 1, x0:x1 + 1] += da
            write_bmp(os.path.join(tmp, "eye_%s_C%02d.bmp" % (stamp, k)), np.clip(img, 0, 255).astype(np.uint8))
            write_bmp(os.path.join(tmp, "eye_%s_CA%02d.bmp" % (stamp, k)), np.repeat(np.clip(al, 0, 255).astype(np.uint8)[:, :, None], 3, axis=2))
            frames.append({"index": k, "frame": 500 + k, "decision_crop": [308, 308, w, h], "raw_file": "eye_%s_C%02d.bmp" % (stamp, k), "withheld": order[k],
                           "withheld_draws": [drew, drew], "withheld_frame": 500 + k, "alpha_file": "eye_%s_CA%02d.bmp" % (stamp, k)})
        frames[6]["withheld_draws"] = [0, 0]   # a frame told as having left no draw out, whose rectangle is missing all the same: flagged as motion by the report
        doc = {"schema": 1, "stamp": stamp, "requested": RUN, "sight_withhold": {"armed_frame": 499, "candidates": names}, "frames": frames}
        with open(os.path.join(tmp, "eye_%s_decisions.json" % stamp), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        # the BMP round trip, rows and channels
        probe = np.arange(5 * 3 * 3, dtype=np.uint8).reshape(5, 3, 3)
        write_bmp(os.path.join(tmp, "probe.bmp"), probe)
        check(np.array_equal(read_bmp(os.path.join(tmp, "probe.bmp")), probe), "a 24-bit BMP round-trips with a row width that is not a multiple of four")
        run = load_run(tmp, stamp)
        check(run["candidates"] == names and run["armed_frame"] == 499 and len(run["frames"]) == RUN and run["frames"][3]["candidate"] == names[2], "the run's record is read from the decisions json")
        a = analyse(run)
        res = a["results"]
        check(a["noise"] == [0] and res[0]["pixels"] == 0 and res[0]["candidate"] == NONE, "the frame of the key press is the noise floor and has no footprint (pixels %s)" % a["noise"])
        # each frame names its candidate's rectangle exactly
        for r in res[1:]:
            name = r["candidate"]
            (x0, y0, x1, y1), dc, da = rects[name]
            area = (x1 - x0 + 1) * (y1 - y0 + 1)
            if name == "PS_66B08F89":   # a two pixel line: kept by the opening (each pixel has its neighbour and itself: 2 of 3 needed)
                check(r["pixels"] == 0, "a two pixel line is dropped by the default opening (pixels %d)" % r["pixels"])
                continue
            check(r["pixels"] == area and r["bbox"] == (x0, y0, x1, y1), "frame %d %s: footprint %d px bbox %s (wanted %d px %s)" % (r["index"], name, r["pixels"], r["bbox"], area, (x0, y0, x1, y1)))
            want = list(dc)   # present minus withheld: the reference has the draw, the frame does not
            check(all(abs(c - v) <= 1.6 for c, v in zip(r["colour"], want)), "frame %d %s: mean colour change %s (wanted about %s)" % (r["index"], name, r["colour"], want))
            check(r["alpha"] is not None and abs(r["alpha"] - da) <= 1.0, "frame %d %s: mean alpha change %s (wanted %d)" % (r["index"], name, r["alpha"], da))
            check(r["bbox_render"] == (x0 + 308, y0 + 308, x1 + 308, y1 + 308), "frame %d %s: the render-space box adds the crop's origin" % (r["index"], name))
        # a candidate withheld in several frames reports each
        s = a["summary"]
        check(all(s[n]["frames"] == 3 for n in names), "the cycle repeats: 15 withheld frames over five names, three each (%s)" % {n: v["frames"] for n, v in s.items()})
        check(len(set(s["C5A5_n18_i1"]["pixels"])) == 1, "the same candidate leaves the same footprint in each of its frames")
        text = report(run, a)
        for name in names:
            check(name in text, "the report names %s" % name)
        check("noise floor (frames that left nothing out): 0 px" in text and "render (328,326)-(367,349)" in text and "WARNING" not in text, "the report states the noise floor and the render-space box")
        check("[no draw of it was left out" in text, "a frame whose candidate had no draw left out is flagged as motion if it shows a footprint")
        j = json.loads(to_json(run, a))
        check(j["stamp"] == stamp and len(j["frames"]) == RUN and j["noise"] == [0], "the json carries the frames and the noise floor")
        nothing = report(run, dict(a, results=[dict(r, draws=[0, 0]) for r in a["results"]]))
        check("WARNING: no draw of any candidate was left out" in nothing, "a run in which no draw was left out in any frame is warned about")
        check("WARNING: no frame names a candidate" in report(run, dict(a, results=[dict(r, candidate=NONE) for r in a["results"]])), "a run in which every frame is none is warned about")
        moved = dict(a)
        moved["noise"] = [int(0.02 * w * h)]
        check("WARNING: a frame that left nothing out" in report(run, moved), "a noise floor above 1% of the crop is warned about as motion")
        # a stricter opening drops the thin line and keeps the rectangles; a lower threshold counts the line
        thin = analyse(run, 10, 1)
        line_frames = [r for r in thin["results"] if r["candidate"] == "PS_66B08F89"]
        check(line_frames and all(r["pixels"] == 2 for r in line_frames), "with no opening (--open 1) the two pixel line is counted: %s" % [r["pixels"] for r in line_frames])
        # no alpha planes: n/a, not zero
        for k in range(RUN):
            os.remove(os.path.join(tmp, "eye_%s_CA%02d.bmp" % (stamp, k)))
        na = analyse(load_run(tmp, stamp))
        check(not na["alpha_planes"] and all(r["alpha"] is None for r in na["results"]) and "alpha n/a" in report(run, na), "without the alpha planes the alpha change is n/a")
        # the failure modes
        os.remove(os.path.join(tmp, "eye_%s_decisions.json" % stamp))
        try:
            load_run(tmp, stamp)
            check(False, "a dump with no json is refused")
        except ValueError as e:
            check("no sight_withhold record" in str(e), "a dump with no withhold record is refused with the reason")
        bare = dict(doc)
        del bare["sight_withhold"]
        with open(os.path.join(tmp, "eye_%s_decisions.json" % stamp), "w", encoding="utf-8") as f:
            json.dump(bare, f)
        try:
            load_run(tmp, stamp)
            check(False, "a decisions json without the record is refused")
        except ValueError:
            check(True, "")
        rc, out = quiet(["--dir", tmp, "--stamp", stamp])
        check(rc == 2 and "no sight_withhold record" in out, "the command exits 2 for a dump with no record and says why")
        with open(os.path.join(tmp, "eye_%s_decisions.json" % stamp), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        rc, out = quiet(["--dir", tmp, "--stamp", stamp, "--kind", "C"])
        rcj, outj = quiet(["--dir", tmp, "--stamp", stamp, "--json"])
        check(rc == 0 and "per candidate:" in out and rcj == 0 and json.loads(outj)["stamp"] == stamp, "the command exits 0 on a good run, as text and as json")
        os.remove(os.path.join(tmp, "eye_%s_C03.bmp" % stamp))
        rc, out = quiet(["--dir", tmp, "--stamp", stamp])
        check(rc == 2 and "is not there" in out, "the command exits 2 when a frame's file is missing")
        check(newest_stamp(tmp) == stamp, "the newest dump with a record is found")
    # the json the DLL writes: every key this reader uses is a literal of temporal_pass.cpp's writers, and the candidate names are sight_withhold.h's
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        with open(os.path.join(here, "..", "src", "d3d11", "temporal_pass.cpp"), "r", encoding="utf-8", errors="replace") as f:
            cpp = f.read()
        with open(os.path.join(here, "..", "src", "d3d11", "sight_withhold.h"), "r", encoding="utf-8", errors="replace") as f:
            hdr = f.read()
    except OSError as e:
        print("FAIL: the DLL sources the json is checked against are not where they look (%s)" % e)
        return False
    for key in ("sight_withhold", "armed_frame", "candidates", "withheld", "withheld_draws", "withheld_frame", "alpha_file", "raw_file", "decision_crop"):
        check('\\"%s\\"' % key in cpp, "temporal_pass.cpp writes the json key %s the reader uses" % key)
    check("eye_%ls_CA%02d.bmp" in cpp and "_withhold.json" in cpp, "temporal_pass.cpp writes the alpha planes and the withhold json under the names the reader opens")
    for name in ("C5A5_n18_i1", "C5A5_n6_i5", "C5A5_n6_i2", "C5A5_n9_i1", "C5A5_n12_i2", "PS_0925AA1B", "PS_66B08F89", "PS_AFED1D4B", "PS_912477AE", "QUAD_46F9"):
        check('"%s"' % name in hdr, "sight_withhold.h lists the candidate %s" % name)
    print("sight_candidates self-test: %s" % ("ok" if ok else "FAILED"))
    return ok


if __name__ == "__main__":
    sys.exit(main())
