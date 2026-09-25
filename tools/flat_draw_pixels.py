#!/usr/bin/env python3
"""Read a bounded F10 flat draw-window capture.

Each draw's before/after copies are 16x16 native pixels. A byte change proves
only that this draw changed the sampled render-target window. It does not prove
that the draw owns the final visible pixel, or that its motion is correct.

Usage: python tools/flat_draw_pixels.py CAPTURE_DIR [--output SUMMARY.json] [--dry-run]
       python tools/flat_draw_pixels.py --self-test
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile


WINDOW = 16
MAX_DRAWS = 512
MAX_POINTS = 8
MAX_MANIFEST = 16 * 1024 * 1024
MAX_BLOB = 384 * 1024 * 1024
MAX_REASON = 512
HEX = re.compile(r"(?:0x)?[0-9a-fA-F]{1,16}\Z")
FRAME = re.compile(r"frame_([0-9]+)\.json\Z")
FIXTURE = re.compile(r"flat_draw_pixels/[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9]+_[0-9]+\Z")
STATES = {"complete", "partial", "failed"}
# Native resource formats accepted by the bounded GPU copy. Values are
# DXGI_FORMAT numeric IDs; view formats can differ for typeless resources.
NATIVE_BPP = {9: 8, 10: 8, 11: 8, 15: 8, 16: 8,
              23: 4, 24: 4, 26: 4, 27: 4, 28: 4, 29: 4,
              39: 4, 41: 4, 87: 4, 90: 4, 91: 4}


class CaptureError(ValueError):
    pass


def integer(value, name, low=0, high=2**64 - 1):
    if type(value) is not int or not low <= value <= high:
        raise CaptureError(f"{name} must be an integer in [{low}, {high}]")
    return value


def short_text(value, name):
    if not isinstance(value, str) or len(value) > MAX_REASON:
        raise CaptureError(f"{name} must be a short string")
    return value


def hex_token(value, name):
    if not isinstance(value, str) or not HEX.fullmatch(value):
        raise CaptureError(f"{name} must be a hexadecimal token")
    return value


def read_blob(path):
    try:
        size = path.stat().st_size
        if size > MAX_BLOB:
            raise CaptureError(f"{path}: blob exceeds {MAX_BLOB} bytes")
        return path.read_bytes()
    except OSError as exc:
        raise CaptureError(f"cannot read {path}: {exc}") from exc


def blob_slice(data, offset, length, name):
    integer(offset, name + ".offset", 0, len(data))
    integer(length, name + ".length", 0, len(data))
    if offset + length > len(data):
        raise CaptureError(f"{name}: payload lies outside blob")
    return data[offset:offset + length]


def check_nonoverlap(ranges, size, name):
    cursor = 0
    for offset, length in sorted(ranges):
        if offset != cursor or offset + length > size:
            raise CaptureError(f"{name}: overlapping, gapped or out-of-bounds payloads")
        cursor = offset + length
    if cursor != size:
        raise CaptureError(f"{name}: unreferenced packed bytes")


def load_frame(path):
    match = FRAME.fullmatch(path.name)
    if not match:
        raise CaptureError(f"expected frame_N.json: {path}")
    try:
        if path.stat().st_size > MAX_MANIFEST:
            raise CaptureError(f"{path}: manifest exceeds {MAX_MANIFEST} bytes")
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CaptureError(f"cannot read {path}: {exc}") from exc
    if not isinstance(manifest, dict):
        raise CaptureError(f"{path}: manifest must be an object")
    if manifest.get("schema") != 1 or type(manifest.get("schema")) is not int:
        raise CaptureError(f"{path}: unsupported schema")
    frame = integer(manifest.get("frame"), "frame")
    if int(match.group(1)) != frame:
        raise CaptureError(f"{path}: filename and frame disagree")
    status = manifest.get("status")
    if status not in STATES:
        raise CaptureError(f"{path}: invalid status")
    reason = short_text(manifest.get("reason"), "reason")
    qualified = manifest.get("qualified")
    identity_match = manifest.get("identity_match")
    if type(qualified) is not bool or type(identity_match) is not bool:
        raise CaptureError("qualified and identity_match must be booleans")
    refusals = integer(manifest.get("refusals"), "refusals", 0, 1_000_000)
    width = integer(manifest.get("render_width"), "render_width", WINDOW, 16384)
    height = integer(manifest.get("render_height"), "render_height", WINDOW, 16384)
    for key in ("prior_depth", "selected_depth", "selected_hdr"):
        hex_token(manifest.get(key), key)
    if status == "complete" and (not qualified or not identity_match or refusals or
                                  int(manifest["prior_depth"], 16) == 0 or
                                  int(manifest["selected_hdr"], 16) == 0 or
                                  int(manifest["prior_depth"], 16) != int(manifest["selected_depth"], 16)):
        raise CaptureError("complete frame lacks matching qualified depth or has refusals")
    cap = integer(manifest.get("draw_cap"), "draw_cap", 1, MAX_DRAWS)
    seen = integer(manifest.get("draws_seen"), "draws_seen", 0, 1_000_000)
    recorded = integer(manifest.get("draws_recorded"), "draws_recorded", 0, cap)
    overflow = manifest.get("overflow")
    if type(overflow) not in (bool, int) or (type(overflow) is int and overflow < 0):
        raise CaptureError("overflow must be a boolean or nonnegative count")
    if status == "complete" and overflow:
        raise CaptureError("complete frame cannot overflow the draw cap")
    points = manifest.get("points")
    if not isinstance(points, list) or not 1 <= len(points) <= MAX_POINTS:
        raise CaptureError("points must contain one to eight windows")
    for index, point in enumerate(points):
        if not isinstance(point, dict):
            raise CaptureError(f"point {index} must be an object")
        for key in ("u", "v"):
            value = point.get(key)
            if type(value) not in (int, float) or not 0 <= value <= 1:
                raise CaptureError(f"point {index}.{key} must lie in [0,1]")
        integer(point.get("x"), f"point {index}.x", 0, width - 1)
        integer(point.get("y"), f"point {index}.y", 0, height - 1)
        if point["x"] != min(int(point["u"] * width), width - 1) or point["y"] != min(int(point["v"] * height), height - 1):
            raise CaptureError(f"point {index} center disagrees with normalized coordinate")
    draws = manifest.get("draws")
    if not isinstance(draws, list) or len(draws) != recorded:
        raise CaptureError("draw list disagrees with draws_recorded")
    if recorded > seen:
        raise CaptureError("draws_recorded exceeds draws_seen")
    if status == "complete" and recorded != seen:
        raise CaptureError("complete frame has unrecorded matching draws")
    pixels_path = path.with_name(f"frame_{frame}_pixels.bin")
    cb_path = path.with_name(f"frame_{frame}_cb.bin")
    if manifest.get("pixels_file") != pixels_path.name or manifest.get("cb_file") != cb_path.name:
        raise CaptureError("unsafe or incorrect packed blob filenames")
    pixel_data = read_blob(pixels_path) if pixels_path.exists() else b""
    cb_data = read_blob(cb_path) if cb_path.exists() else b""
    if (integer(manifest.get("pixels_bytes"), "pixels_bytes", 0, MAX_BLOB) != len(pixel_data) or
            integer(manifest.get("cb_bytes"), "cb_bytes", 0, MAX_BLOB) != len(cb_data)):
        raise CaptureError("packed blob byte count disagrees with manifest")
    pixel_ranges, cb_ranges = [], []
    previous_q = -1
    changes = {}
    unknown = {}
    cb_info = {}
    for index, draw in enumerate(draws):
        if not isinstance(draw, dict):
            raise CaptureError(f"draw {index} must be an object")
        q = integer(draw.get("q"), f"draw {index}.q", 0, 1_000_000)
        if q <= previous_q:
            raise CaptureError("draw sequences must increase strictly")
        previous_q = q
        vs, ps = hex_token(draw.get("vs"), f"draw {index}.vs"), hex_token(draw.get("ps"), f"draw {index}.ps")
        kind = short_text(draw.get("kind"), "draw.kind")
        if kind not in ("D", "I", "N", "X", "?"):
            raise CaptureError("draw.kind must be D, I, N, X or unknown")
        count = integer(draw.get("count"), "draw.count", 0, 2**32 - 1)
        start = integer(draw.get("start"), "draw.start", 0, 2**32 - 1)
        base = integer(draw.get("base"), "draw.base", -(2**31), 2**31 - 1)
        start_instance = integer(draw.get("start_instance"), "draw.start_instance", 0, 2**32 - 1)
        if type(draw.get("args_available")) is not bool:
            raise CaptureError("draw.args_available must be a boolean")
        if draw["args_available"] != (kind != "?"):
            raise CaptureError("draw args availability disagrees with kind")
        instances = integer(draw.get("instances"), "draw.instances", 0, 2**32 - 1)
        topology = integer(draw.get("topology"), "draw.topology", 0, 200)
        hex_token(draw.get("vs_object"), "draw.vs_object")
        hex_token(draw.get("ps_object"), "draw.ps_object")
        depth_resource = hex_token(draw.get("depth_resource"), "draw.depth_resource")
        integer(draw.get("depth_format"), "draw.depth_format", 0, 200)
        integer(draw.get("depth_view_format"), "draw.depth_view_format", 0, 200)
        shader_bytes = draw.get("shader_bytes")
        if not isinstance(shader_bytes, dict) or any(type(shader_bytes.get(s)) is not bool for s in ("vs", "ps")):
            raise CaptureError("draw.shader_bytes must report VS and PS availability")
        rt = draw.get("rt")
        if not isinstance(rt, list) or len(rt) > 4:
            raise CaptureError(f"draw {index}.rt must be a list of at most four targets")
        rt_indices = set()
        matching_rt_indices = set()
        for target in rt:
            if not isinstance(target, dict):
                raise CaptureError(f"draw {index}.rt target must be an object")
            slot = integer(target.get("index"), "rt.index", 0, 3)
            if slot in rt_indices:
                raise CaptureError(f"draw {index}.rt has duplicate index")
            rt_indices.add(slot)
            hex_token(target.get("resource"), "rt.resource")
            integer(target.get("format"), "rt.format", 0, 200)
            integer(target.get("view_format"), "rt.view_format", 0, 200)
            tw = integer(target.get("width"), "rt.width", 0, 16384)
            th = integer(target.get("height"), "rt.height", 0, 16384)
            if type(target.get("valid")) is not bool:
                raise CaptureError("rt.valid must be a boolean")
            if target["valid"]:
                if (tw, th) != (width, height) or int(target["resource"], 16) == 0:
                    raise CaptureError("valid rt must be the selected nonnull render size")
            if (tw, th) == (width, height) and int(target["resource"], 16) != 0:
                matching_rt_indices.add(slot)
        for key in ("dsv",):
            hex_token(draw.get(key), f"draw {index}.{key}")
        for key in ("depth_state", "raster", "viewport", "ia"):
            if not isinstance(draw.get(key), dict):
                raise CaptureError(f"draw {index}.{key} must be an object")
        cbs = draw.get("cb")
        if not isinstance(cbs, list) or len(cbs) > 3:
            raise CaptureError(f"draw {index}.cb must be a list of at most three buffers")
        seen_slots = set()
        summaries = []
        for cb in cbs:
            if not isinstance(cb, dict):
                raise CaptureError("cb entry must be an object")
            slot = integer(cb.get("slot"), "cb.slot", 0, 2)
            if slot in seen_slots:
                raise CaptureError("duplicate cb slot")
            seen_slots.add(slot)
            hex_token(cb.get("resource"), "cb.resource")
            byte_width = integer(cb.get("byte_width"), "cb.byte_width", 0, 1 << 20)
            first_constant = integer(cb.get("first_constant"), "cb.first_constant", 0, 4096)
            constant_count = integer(cb.get("constant_count"), "cb.constant_count", 0, 4096)
            cb_status = short_text(cb.get("status"), "cb.status")
            short_text(cb.get("reason", ""), "cb.reason")
            if cb_status == "ok":
                offset = integer(cb.get("offset"), "cb.offset", 0, len(cb_data))
                length = integer(cb.get("length"), "cb.length", 0, byte_width)
                if length != byte_width or length % 16:
                    raise CaptureError("complete cb must contain its full 16-byte aligned buffer")
                payload = blob_slice(cb_data, offset, length, "cb")
                cb_ranges.append((offset, length))
                summaries.append({"slot": slot, "resource": cb["resource"],
                                  "status": cb_status, "offset": offset, "bytes": length,
                                  "first_constant": first_constant, "constant_count": constant_count,
                                  "sha256": hashlib.sha256(payload).hexdigest()})
            elif cb_status in ("unavailable", "timeout", "gpu_error", "skipped"):
                if status == "complete" and (int(cb["resource"], 16) != 0 or byte_width):
                    raise CaptureError("complete frame lacks a bound cb snapshot")
                summaries.append({"slot": slot, "resource": cb["resource"],
                                  "status": cb_status, "bytes": 0,
                                  "first_constant": first_constant, "constant_count": constant_count,
                                  "reason": cb.get("reason", "")})
            else:
                raise CaptureError("unknown cb status")
        cb_info[q] = summaries
        copies = draw.get("copies")
        if not isinstance(copies, list) or len(copies) > len(points) * 4 * 2:
            raise CaptureError(f"draw {index}.copies has invalid count")
        pairs = {}
        for copy in copies:
            if not isinstance(copy, dict):
                raise CaptureError("copy entry must be an object")
            target = integer(copy.get("rt_index"), "copy.rt_index", 0, 3)
            point = integer(copy.get("point"), "copy.point", 0, len(points)-1)
            phase = copy.get("phase")
            if phase not in ("before", "after"):
                raise CaptureError("copy.phase must be before or after")
            key = (target, point)
            if target not in matching_rt_indices:
                raise CaptureError("copy names a render target outside the selected extent")
            if phase in pairs.setdefault(key, {}):
                raise CaptureError("duplicate copy phase")
            copy_status = short_text(copy.get("status"), "copy.status")
            short_text(copy.get("reason", ""), "copy.reason")
            fmt = integer(copy.get("format"), "copy.format", 0, 200)
            bpp = integer(copy.get("bpp"), "copy.bpp", 0, 16)
            if copy_status == "ok":
                source_format = next(r["format"] for r in rt if r["index"] == target)
                if fmt != source_format or NATIVE_BPP.get(fmt) != bpp:
                    raise CaptureError("ok copy has unsupported or mismatched native format/bpp")
                x0 = integer(copy.get("x0"), "copy.x0", 0, width - WINDOW)
                y0 = integer(copy.get("y0"), "copy.y0", 0, height - WINDOW)
                center = points[point]
                if not (x0 <= center["x"] < x0 + WINDOW and y0 <= center["y"] < y0 + WINDOW):
                    raise CaptureError("copy window does not contain its sampled center")
                offset = integer(copy.get("offset"), "copy.offset", 0, len(pixel_data))
                length = integer(copy.get("length"), "copy.length", 0, WINDOW * WINDOW * bpp)
                if length != WINDOW * WINDOW * bpp:
                    raise CaptureError("complete copy has wrong native byte count")
                pairs[key][phase] = (blob_slice(pixel_data, offset, length, "copy"), fmt, bpp, x0, y0)
                pixel_ranges.append((offset, length))
            elif copy_status in ("unavailable", "timeout", "gpu_error", "skipped"):
                pairs[key][phase] = None
            else:
                raise CaptureError("unknown copy status")
        expected_pairs = {(slot, point) for slot in matching_rt_indices for point in range(len(points))}
        for target, point in sorted(expected_pairs):
            samples = pairs.get((target, point), {})
            name = f"rt{target}/point{point}"
            if "before" not in samples or "after" not in samples or any(v is None for v in samples.values()):
                unknown[name] = unknown.get(name, 0) + 1
                continue
            before, before_fmt, before_bpp, before_x, before_y = samples["before"]
            after, after_fmt, after_bpp, after_x, after_y = samples["after"]
            if (before_fmt, before_bpp, before_x, before_y) != (after_fmt, after_bpp, after_x, after_y):
                raise CaptureError("before and after copy layouts differ")
            bpp = before_bpp
            changed_pixels = sum(before[n:n+bpp] != after[n:n+bpp]
                                 for n in range(0, len(before), bpp))
            if not changed_pixels:
                continue
            row = {"q": q, "vs": vs, "ps": ps, "kind": kind, "count": count,
                   "instances": instances, "topology": topology,
                   "draw_args": {"start": start, "base": base, "start_instance": start_instance,
                                 "available": draw["args_available"]},
                   "changed_pixels": changed_pixels,
                   "changed_bytes": sum(a != b for a, b in zip(before, after)),
                   "max_byte_delta": max(abs(a-b) for a, b in zip(before, after)),
                   "rt_resource": next(r["resource"] for r in rt if r["index"] == target),
                   "dsv": draw["dsv"], "depth_resource": depth_resource,
                   "depth_format": draw["depth_format"],
                   "depth_view_format": draw["depth_view_format"],
                   "depth_state": draw["depth_state"],
                   "viewport": draw["viewport"], "ia": draw["ia"],
                   "shader_bytes": shader_bytes, "cb": summaries}
            changes.setdefault(name, []).append(row)
        if status == "complete":
            if set(pairs) != expected_pairs or any(set(phases) != {"before", "after"} for phases in pairs.values()):
                raise CaptureError("complete frame omits an eligible RT/point before-after pair")
    check_nonoverlap(pixel_ranges, len(pixel_data), "pixels")
    check_nonoverlap(cb_ranges, len(cb_data), "cb")
    if status == "complete" and unknown:
        raise CaptureError("complete frame has unpaired or unavailable window copies")
    return {"frame": frame, "status": status, "reason": reason,
            "qualified": qualified, "identity_match": identity_match,
            "refusals": refusals,
            "render_size": [width, height], "points": points,
            "draws_seen": seen, "draws_recorded": recorded, "overflow": overflow,
            "changes": changes, "unknown_pairs": unknown,
            "interpretation": "Changes are native render-target bytes before and after a draw; final visible owner and cross-frame draw identity are not inferred."}


def run(capture_dir, output=None, dry_run=False):
    if not capture_dir.is_dir():
        raise CaptureError(f"capture directory does not exist: {capture_dir}")
    paths = sorted(capture_dir.glob("frame_*.json"))
    if not paths:
        raise CaptureError("no frame_N.json manifests")
    frames = [load_frame(path) for path in paths]
    ids = [frame["frame"] for frame in frames]
    if len(set(ids)) != len(ids):
        raise CaptureError("duplicate frame ids")
    report = {"frames": frames, "consecutive": len(ids) == 2 and ids[1] == ids[0] + 1,
              "cross_frame_matching": "unproven; draw ordinals and resource pointers are not stable object identity"}
    if output is not None and not dry_run:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def verify_fixture(root):
    """Read the current WARP writer fixture, never an older matching glob."""
    pointer = root / "flat_draw_current_fixture.txt"
    try:
        if pointer.stat().st_size > 512:
            raise CaptureError("fixture pointer exceeds 512 bytes")
        relative = pointer.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError) as exc:
        raise CaptureError(f"current fixture pointer unavailable: {exc}") from exc
    if not FIXTURE.fullmatch(relative):
        raise CaptureError("unsafe or malformed fixture pointer")
    directory = root.joinpath(*relative.split("/"))
    if directory.resolve().parent.parent != root.resolve():
        raise CaptureError("fixture pointer resolves outside capture root")
    report = run(directory)
    frames = report["frames"]
    if [f["frame"] for f in frames] != [101, 102] or not report["consecutive"]:
        raise CaptureError("fixture lacks two consecutive captured frames")
    changed_counts = []
    for frame in frames:
        if frame["status"] != "complete" or frame["draws_recorded"] != 1 or frame["render_size"] != [64, 64]:
            raise CaptureError("fixture frame is not the expected qualified WARP draw")
        number = frame["frame"]
        cb_bytes = (directory / f"frame_{number}_cb.bin").read_bytes()
        expected = bytes(range(7 if number == 101 else 41,
                               (7 if number == 101 else 41) + 64))
        if cb_bytes != expected * 3:
            raise CaptureError("fixture draw-time CB bytes differ from the GPU producer expectation")
        pixel_bytes = (directory / f"frame_{number}_pixels.bin").read_bytes()
        if len(pixel_bytes) != 8 * 2 * WINDOW * WINDOW * 4:
            raise CaptureError("fixture does not contain all eight native before/after windows")
        expected_changes = {"rt0/point0": (256, 512), "rt0/point5": (6, 12)}
        actual = {key: [(item["q"], item["changed_pixels"], item["changed_bytes"])
                        for item in values] for key, values in frame["changes"].items()}
        expected = {key: [(1, pixels, byte_count)] for key, (pixels, byte_count) in expected_changes.items()}
        if actual != expected:
            raise CaptureError(f"fixture native changed-pixel counts differ: {actual}")
        changed_counts.append({key: pixels for key, (pixels, _) in expected_changes.items()})
    return {"fixture_verified": True, "frames": [101, 102],
            "changed_window_counts": changed_counts,
            "source": str(directory)}


def self_test():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        pixels = bytes([0] * 1024 + [0] * 1020 + [7, 6, 5, 4])
        (root / "frame_7_pixels.bin").write_bytes(pixels)
        cb = bytes(range(16))
        (root / "frame_7_cb.bin").write_bytes(cb)
        draw = {"q": 12, "vs": "1111", "ps": "2222", "kind": "X", "count": 6,
                "instances": 1, "topology": 4, "vs_object": "0xA", "ps_object": "0xB",
                "start": 0, "base": 0, "start_instance": 0, "args_available": True,
                "rt": [{"index": 0, "resource": "AA", "format": 28, "view_format": 28,
                        "width": 32, "height": 32, "valid": True}],
                "dsv": "BB", "depth_resource": "AB", "depth_format": 45,
                "depth_view_format": 45, "shader_bytes": {"vs": True, "ps": True},
                "depth_state": {}, "raster": {}, "viewport": {}, "ia": {},
                "cb": [{"slot": 0, "resource": "CC", "byte_width": 16,
                        "first_constant": 0, "constant_count": 4096,
                        "status": "ok", "reason": "", "offset": 0, "length": 16}],
                "copies": [{"rt_index": 0, "point": 0, "phase": "before", "status": "ok",
                            "reason": "", "format": 28, "bpp": 4, "x0": 8, "y0": 8,
                            "offset": 0, "length": 1024},
                           {"rt_index": 0, "point": 0, "phase": "after", "status": "ok",
                            "reason": "", "format": 28, "bpp": 4, "x0": 8, "y0": 8,
                            "offset": 1024, "length": 1024}]}
        manifest = {"schema": 1, "status": "complete", "reason": "", "frame": 7,
                    "qualified": True, "identity_match": True, "refusals": 0,
                    "render_width": 32, "render_height": 32,
                    "prior_depth": "1", "selected_depth": "1", "selected_hdr": "2",
                    "draw_cap": 512, "draws_seen": 1, "draws_recorded": 1, "overflow": False,
                    "pixels_file": "frame_7_pixels.bin", "pixels_bytes": len(pixels),
                    "cb_file": "frame_7_cb.bin", "cb_bytes": len(cb),
                    "points": [{"u": .5, "v": .5, "x": 16, "y": 16}], "draws": [draw]}
        path = root / "frame_7.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        report = run(root)
        change = report["frames"][0]["changes"]["rt0/point0"][0]
        assert change["changed_pixels"] == 1 and change["changed_bytes"] == 4
        assert change["max_byte_delta"] == 7
        out = root / "new" / "summary.json"
        run(root, out, dry_run=True)
        assert not out.exists() and not out.parent.exists()
        run(root, out)
        assert out.exists()
        (root / "frame_7_pixels.bin").write_bytes(bytes(2048))
        assert run(root)["frames"][0]["changes"] == {}
        (root / "frame_7_pixels.bin").write_bytes(pixels)
        draw["copies"][1]["status"] = "unavailable"
        draw["copies"][1]["bpp"] = 0
        manifest["status"] = "partial"
        (root / "frame_7_pixels.bin").write_bytes(pixels[:1024])
        manifest["pixels_bytes"] = 1024
        path.write_text(json.dumps(manifest), encoding="utf-8")
        assert run(root)["frames"][0]["unknown_pairs"] == {"rt0/point0": 1}
        manifest["status"] = "complete"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            run(root)
            raise AssertionError("complete frame with unavailable copy accepted")
        except CaptureError:
            pass
        manifest["status"] = "partial"
        draw["copies"][1]["status"] = "ok"
        draw["copies"][1]["bpp"] = 4
        (root / "frame_7_pixels.bin").write_bytes(pixels)
        manifest["pixels_bytes"] = len(pixels)
        draw["copies"][1]["offset"] = len(pixels) - 3
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            run(root)
            raise AssertionError("out-of-bounds copy accepted")
        except CaptureError:
            pass
        draw["copies"][1]["offset"] = 1024
        draw["copies"][1]["bpp"] = 8
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            run(root)
            raise AssertionError("bad native layout accepted")
        except CaptureError:
            pass
        draw["copies"][1]["bpp"] = 4
        manifest["pixels_file"] = "../outside.bin"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            run(root)
            raise AssertionError("unsafe packed blob path accepted")
        except CaptureError:
            pass
        manifest["pixels_file"] = "frame_7_pixels.bin"
        manifest["frame"] = 8
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            run(root)
            raise AssertionError("filename mismatch accepted")
        except CaptureError:
            pass
        try:
            run(root / "missing")
            raise AssertionError("missing capture path accepted")
        except CaptureError:
            pass
    print("flat_draw_pixels self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", nargs="?", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verify-fixture", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            self_test()
            return 0
        if args.capture_dir is None:
            parser.error("capture_dir is required unless --self-test is used")
        if args.verify_fixture:
            if args.output is not None:
                parser.error("--verify-fixture does not take --output")
            print(json.dumps(verify_fixture(args.capture_dir), indent=2))
            return 0
        print(json.dumps(run(args.capture_dir, args.output, args.dry_run), indent=2))
        return 0
    except (CaptureError, OSError) as exc:
        print(f"flat_draw_pixels: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
