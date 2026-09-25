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
import math
from pathlib import Path
import re
import struct
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
NATIVE_BPP = {9: 8, 10: 8, 11: 8, 15: 8, 16: 8, 19: 8,
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


def producer_center(normalized, extent):
    """Match the producer's float32 literal and float32 u * extent."""
    f32 = lambda value: struct.unpack("<f", struct.pack("<f", value))[0]
    return min(int(f32(f32(normalized) * extent)), extent - 1)


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
    if type(manifest.get("schema")) is not int or manifest["schema"] not in (1, 2):
        raise CaptureError(f"{path}: unsupported schema")
    schema = manifest["schema"]
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
    motion_refusals = integer(manifest.get("motion_refusals"), "motion_refusals", 0, 1_000_000) if schema == 2 else 0
    motion_draws = integer(manifest.get("motion_draws"), "motion_draws", 0, MAX_DRAWS) if schema == 2 else 0
    motion_complete = manifest.get("motion_complete", False) if schema == 2 else False
    if type(motion_complete) is not bool or (motion_complete and
       (not motion_draws or motion_refusals or not qualified or not identity_match)):
        raise CaptureError("invalid motion completeness fields")
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
        if point["x"] != producer_center(point["u"], width) or point["y"] != producer_center(point["v"], height):
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
    pool_path = path.with_name(f"frame_{frame}_pool.bin")
    if schema == 2 and manifest.get("pool_file") != pool_path.name:
        raise CaptureError("unsafe or incorrect pool blob filename")
    pool_data = read_blob(pool_path) if schema == 2 and pool_path.exists() else b""
    if (integer(manifest.get("pixels_bytes"), "pixels_bytes", 0, MAX_BLOB) != len(pixel_data) or
            integer(manifest.get("cb_bytes"), "cb_bytes", 0, MAX_BLOB) != len(cb_data)):
        raise CaptureError("packed blob byte count disagrees with manifest")
    if schema == 2 and integer(manifest.get("pool_bytes"), "pool_bytes", 0, MAX_BLOB) != len(pool_data):
        raise CaptureError("pool blob byte count disagrees with manifest")
    pixel_ranges, cb_ranges, pool_ranges = [], [], []
    previous_q = -1
    changes = {}
    unknown = {}
    motion_windows = []
    cb_info = {}
    candidate_count = 0
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
        motion = draw.get("motion") if schema == 2 else None
        aux = {}
        pool_payload = None
        pool_summary = {"status": "not-captured"}
        if schema == 2:
            if not isinstance(motion, dict) or type(motion.get("expected")) is not bool or type(motion.get("candidate")) is not bool:
                raise CaptureError("schema2 draw.motion requires candidate and expected booleans")
            if motion["candidate"]:
                candidate_count += 1
            if motion["expected"] and not motion["candidate"]:
                raise CaptureError("expected motion requires a source candidate")
            motion_status = short_text(motion.get("status"), "motion.status")
            if motion_status not in ("not-requested", "producer-declined", "unavailable", "captured",
                                     "depth-mirror-unavailable", "view-or-mirror-changed"):
                raise CaptureError("invalid motion status")
            if ((motion_status == "not-requested") != (not motion["expected"] and not motion["candidate"]) or
                    (motion_status == "producer-declined") != (motion["candidate"] and not motion["expected"])):
                raise CaptureError("motion expectation/status mismatch")
            hex_token(motion.get("pool_resource"), "motion.pool_resource")
            hex_token(motion.get("pool_view"), "motion.pool_view")
            for target_id, name in ((6, "slot"), (7, "depth")):
                target = motion.get(name)
                if not isinstance(target, dict):
                    raise CaptureError(f"motion.{name} must be an object")
                hex_token(target.get("resource"), f"motion.{name}.resource")
                hex_token(target.get("view"), f"motion.{name}.view")
                fmt = integer(target.get("format"), f"motion.{name}.format", 0, 200)
                vf = integer(target.get("view_format"), f"motion.{name}.view_format", 0, 200)
                tw = integer(target.get("width"), f"motion.{name}.width", 0, 16384)
                th = integer(target.get("height"), f"motion.{name}.height", 0, 16384)
                if type(target.get("valid")) is not bool:
                    raise CaptureError(f"motion.{name}.valid must be boolean")
                if target["valid"]:
                    if not motion["expected"] or (tw, th) != (width, height) or int(target["resource"], 16) == 0:
                        raise CaptureError("valid motion target lacks expected nonnull extent")
                    allowed = (fmt in (15, 16) and vf == 16) if name == "slot" else (
                        (fmt, vf) in ((19, 20), (39, 40)))
                    if not allowed:
                        raise CaptureError("motion target has wrong native/view format")
                    matching_rt_indices.add(target_id)
                aux[target_id] = target
            if motion_status == "captured" and not (aux[6]["valid"] and aux[7]["valid"]):
                raise CaptureError("captured motion needs valid slot and depth")
            if motion["candidate"] and not (aux[6]["valid"] and aux[7]["valid"]):
                for point in range(len(points)):
                    unknown[f"motion/point{point}"] = unknown.get(f"motion/point{point}", 0) + 1
            pool = motion.get("pool")
            if not isinstance(pool, dict):
                raise CaptureError("motion.pool must be an object")
            pool_status = short_text(pool.get("status"), "pool.status")
            if pool_status not in ("not-requested", "ok", "unavailable", "timeout", "gpu_error", "skipped"):
                raise CaptureError("invalid pool status")
            short_text(pool.get("reason", ""), "pool.reason")
            pool_resource = hex_token(pool.get("resource"), "pool.resource")
            pool_view = hex_token(pool.get("view"), "pool.view")
            pool_width = integer(pool.get("byte_width"), "pool.byte_width", 0, 2**32 - 1)
            stride = integer(pool.get("stride"), "pool.stride", 0, 2**32 - 1)
            first = integer(pool.get("first_element"), "pool.first_element", 0, 2**32 - 1)
            elements = integer(pool.get("num_elements"), "pool.num_elements", 0, 2**32 - 1)
            if pool_status == "ok":
                if not motion["expected"] or not pool_width or pool_width > 4 * 1024 * 1024 or stride != 336 or not elements or (
                        first + elements) * stride > pool_width or pool_resource != motion["pool_resource"] or (
                        pool_view != motion["pool_view"]):
                    raise CaptureError("ok pool snapshot lacks matching bound SRV range")
                offset = integer(pool.get("offset"), "pool.offset", 0, len(pool_data))
                length = integer(pool.get("length"), "pool.length", 0, pool_width)
                if length != pool_width:
                    raise CaptureError("ok pool snapshot must contain full buffer")
                pool_payload = blob_slice(pool_data, offset, length, "pool")
                pool_ranges.append((offset, length))
            elif motion_complete and pool_status != "not-requested":
                raise CaptureError("motion-complete frame has unavailable requested pool snapshot")
            pool_summary = {"status": pool_status, "resource": pool_resource, "view": pool_view,
                            "stride": stride, "first_element": first, "num_elements": elements,
                            "bytes": pool_width, "reason": pool.get("reason", "")}
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
        if not isinstance(copies, list) or len(copies) > len(points) * (6 if schema == 2 else 4) * 2:
            raise CaptureError(f"draw {index}.copies has invalid count")
        pairs = {}
        for copy in copies:
            if not isinstance(copy, dict):
                raise CaptureError("copy entry must be an object")
            target = integer(copy.get("rt_index"), "copy.rt_index", 0, 7 if schema == 2 else 3)
            if target not in (0, 1, 2, 3, 6, 7):
                raise CaptureError("copy names unsupported target")
            point = integer(copy.get("point"), "copy.point", 0, len(points)-1)
            phase = copy.get("phase")
            if phase not in ("before", "after"):
                raise CaptureError("copy.phase must be before or after")
            key = (target, point)
            if target not in matching_rt_indices:
                raise CaptureError("copy names a target outside the selected extent")
            if phase in pairs.setdefault(key, {}):
                raise CaptureError("duplicate copy phase")
            copy_status = short_text(copy.get("status"), "copy.status")
            short_text(copy.get("reason", ""), "copy.reason")
            fmt = integer(copy.get("format"), "copy.format", 0, 200)
            bpp = integer(copy.get("bpp"), "copy.bpp", 0, 16)
            if copy_status == "ok":
                source_format = (aux[target]["format"] if target in aux else
                                 next(r["format"] for r in rt if r["index"] == target))
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
            name = f"{('slot' if target == 6 else 'depth') if target in (6, 7) else 'rt' + str(target)}/point{point}"
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
                   "rt_resource": (aux[target]["resource"] if target in aux else
                                   next(r["resource"] for r in rt if r["index"] == target)),
                   "dsv": draw["dsv"], "depth_resource": depth_resource,
                   "depth_format": draw["depth_format"],
                   "depth_view_format": draw["depth_view_format"],
                   "depth_state": draw["depth_state"],
                   "viewport": draw["viewport"], "ia": draw["ia"],
                   "shader_bytes": shader_bytes, "cb": summaries,
                   "pool": pool_summary}
            changes.setdefault(name, []).append(row)
        if schema == 2 and motion["expected"]:
            for point in range(len(points)):
                slot_pair = pairs.get((6, point), {})
                depth_pair = pairs.get((7, point), {})
                if any(phase not in slot_pair or slot_pair[phase] is None or
                       phase not in depth_pair or depth_pair[phase] is None
                       for phase in ("before", "after")):
                    continue
                sb, _, _, sx, sy = slot_pair["before"]
                sa, _, _, ax, ay = slot_pair["after"]
                db, _, _, dx, dy = depth_pair["before"]
                da, _, _, ex, ey = depth_pair["after"]
                if (sx, sy) != (ax, ay) or (sx, sy) != (dx, dy) or (sx, sy) != (ex, ey):
                    raise CaptureError("motion slot/depth windows do not align")
                depth_bpp = NATIVE_BPP[aux[7]["format"]]
                before_codes, after_codes = {}, {}
                before_exact = after_exact = slot_changed = depth_changed = 0
                after_depth_deltas = []
                seen_records = set()
                for pixel in range(WINDOW * WINDOW):
                    soff = pixel * 8
                    doff = pixel * depth_bpp
                    cb = struct.unpack_from("<f", sb, soff)[0]
                    ca = struct.unpack_from("<f", sa, soff)[0]
                    for code, counts in ((cb, before_codes), (ca, after_codes)):
                        key = str(int(code)) if math.isfinite(code) and code >= 1 and code.is_integer() else "invalid"
                        counts[key] = counts.get(key, 0) + 1
                        if key != "invalid" and int(code) & 1:
                            seen_records.add(int(code) >> 1)
                    before_exact += sb[soff+4:soff+8] == db[doff:doff+4]
                    after_exact += sa[soff+4:soff+8] == da[doff:doff+4]
                    slot_changed += sb[soff:soff+8] != sa[soff:soff+8]
                    depth_changed += db[doff:doff+depth_bpp] != da[doff:doff+depth_bpp]
                    if sa[soff+4:soff+8] != da[doff:doff+4]:
                        after_depth_deltas.append(struct.unpack_from("<f", da, doff)[0] -
                                                  struct.unpack_from("<f", sa, soff+4)[0])
                record_hashes = {}
                unresolved_records = []
                if pool_payload is not None:
                    for record in sorted(seen_records):
                        if record >= elements:
                            unresolved_records.append(record)
                            continue
                        start_byte = (first + record) * stride
                        record_hashes[str(record)] = hashlib.sha256(
                            pool_payload[start_byte:start_byte+stride]).hexdigest()
                motion_windows.append({"q": q, "point": point, "xy": [sx, sy],
                                       "vs": vs, "ps": ps, "slot_resource": aux[6]["resource"],
                                       "depth_resource": aux[7]["resource"],
                                       "pool_resource": motion["pool_resource"],
                                       "before_codes": before_codes, "after_codes": after_codes,
                                       "before_exact_depth": before_exact,
                                       "after_exact_depth": after_exact,
                                       "slot_changed_pixels": slot_changed,
                                       "dsv_changed_pixels": depth_changed,
                                       "after_dsv_minus_slot_range": ([min(after_depth_deltas), max(after_depth_deltas)]
                                                                       if after_depth_deltas else None),
                                       "pool_record_sha256": record_hashes,
                                       "pool_record_out_of_range": unresolved_records,
                                       "pool_snapshot_status": pool_summary["status"]})
        if status == "complete":
            color_expected = {key for key in expected_pairs if key[0] < 4}
            if not color_expected.issubset(pairs) or any(set(pairs[key]) != {"before", "after"}
                                                         for key in color_expected):
                raise CaptureError("complete frame omits an eligible color RT/point pair")
    check_nonoverlap(pixel_ranges, len(pixel_data), "pixels")
    check_nonoverlap(cb_ranges, len(cb_data), "cb")
    if schema == 2:
        check_nonoverlap(pool_ranges, len(pool_data), "pool")
    if status == "complete" and any(key.startswith("rt") for key in unknown):
        raise CaptureError("complete frame has unpaired or unavailable color copies")
    if schema == 2:
        if candidate_count != motion_draws:
            raise CaptureError("motion_draws disagrees with candidate draws")
        if motion_complete and (any(key.startswith(("slot/", "depth/", "motion/")) for key in unknown) or
                                any(d["motion"]["candidate"] and (d["motion"]["status"] != "captured" or
                                    d["motion"]["pool"]["status"] != "ok") for d in draws)):
            raise CaptureError("motion_complete claimed with missing slot/depth or pool")
    return {"frame": frame, "status": status, "reason": reason,
            "qualified": qualified, "identity_match": identity_match,
            "refusals": refusals, "motion_draws": motion_draws,
            "motion_refusals": motion_refusals, "motion_complete": motion_complete,
            "render_size": [width, height], "points": points,
            "draws_seen": seen, "draws_recorded": recorded, "overflow": overflow,
            "changes": changes, "motion_windows": motion_windows, "unknown_pairs": unknown,
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
    """Read both fresh WARP writer fixtures, never an older matching glob."""
    def pointed_directory(name):
        pointer = root / name
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
        return directory
    directory = pointed_directory("flat_draw_current_fixture.txt")
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
    motion_directory = pointed_directory("flat_draw_motion_current_fixture.txt")
    motion_report = run(motion_directory)
    motion_frames = motion_report["frames"]
    if [f["frame"] for f in motion_frames] != [501, 502] or not motion_report["consecutive"]:
        raise CaptureError("motion fixture lacks two consecutive WARP frames")
    motion_counts = []
    for frame in motion_frames:
        if frame["status"] != "complete" or not frame["motion_complete"] or frame["motion_draws"] != 2:
            raise CaptureError("motion fixture lacks complete underlay/decal captures")
        rows = {row["q"]: row for row in frame["motion_windows"] if row["point"] == 0}
        if set(rows) != {1, 2}:
            raise CaptureError("motion fixture lacks point0 underlay/decal chronology")
        underlay, decal = rows[1], rows[2]
        if (underlay["before_codes"] != {"invalid": 256} or
                underlay["after_codes"] != {"7": 256} or
                underlay["after_exact_depth"] != 256 or
                underlay["slot_changed_pixels"] != 256 or
                underlay["dsv_changed_pixels"] != 256 or
                decal["before_codes"] != {"7": 256} or
                decal["after_codes"] != {"7": 256} or
                decal["before_exact_depth"] != 256 or
                decal["after_exact_depth"] != 0 or
                decal["slot_changed_pixels"] != 256 or
                decal["dsv_changed_pixels"] != 0):
            raise CaptureError("motion fixture slot/depth transitions differ from GPU expectation")
        first_hash = underlay["pool_record_sha256"].get("3")
        if (not first_hash or first_hash != decal["pool_record_sha256"].get("3") or
                underlay["pool_resource"] != decal["pool_resource"]):
            raise CaptureError("motion fixture does not prove the same unchanged t33 record")
        motion_counts.append({"underlay_slot_changed": underlay["slot_changed_pixels"],
                              "decal_slot_changed": decal["slot_changed_pixels"],
                              "decal_dsv_changed": decal["dsv_changed_pixels"]})
    return {"fixture_verified": True, "frames": [101, 102],
            "changed_window_counts": changed_counts,
            "motion_frames": [501, 502], "motion_counts": motion_counts,
            "source": str(directory), "motion_source": str(motion_directory)}


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
        assert producer_center(.70, 1440) == 1008
        manifest["render_width"], manifest["render_height"] = 2560, 1440
        manifest["points"] = [{"u": .08, "v": .70, "x": 204, "y": 1008}]
        draw["rt"][0]["width"], draw["rt"][0]["height"] = 2560, 1440
        for copy in draw["copies"]:
            copy["x0"], copy["y0"] = 196, 1000
        path.write_text(json.dumps(manifest), encoding="utf-8")
        assert run(root)["frames"][0]["render_size"] == [2560, 1440]
        manifest["points"][0]["y"] = 1007
        path.write_text(json.dumps(manifest), encoding="utf-8")
        try:
            run(root)
            raise AssertionError("malformed 2560x1440 point accepted")
        except CaptureError:
            pass
        manifest["render_width"], manifest["render_height"] = 32, 32
        manifest["points"] = [{"u": .5, "v": .5, "x": 16, "y": 16}]
        draw["rt"][0]["width"], draw["rt"][0]["height"] = 32, 32
        for copy in draw["copies"]:
            copy["x0"], copy["y0"] = 8, 8
        path.write_text(json.dumps(manifest), encoding="utf-8")
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
        # Schema 2: a depth-write-off overlay replaces the slot's projected
        # depth and leaves the DSV unchanged. The raw record stays identifiable.
        second = root / "motion"
        second.mkdir()
        pool_bytes = bytes((i * 13 + 7) % 256 for i in range(4 * 336))
        (second / "frame_9_pool.bin").write_bytes(pool_bytes)
        (second / "frame_9_cb.bin").write_bytes(cb)
        slot_before = struct.pack("<ff", 7.0, .5) * (WINDOW * WINDOW)
        slot_after = struct.pack("<ff", 7.0, .25) * (WINDOW * WINDOW)
        depth_native = struct.pack("<fI", .5, 0) * (WINDOW * WINDOW)
        packed = pixels + slot_before + depth_native + slot_after + depth_native
        (second / "frame_9_pixels.bin").write_bytes(packed)
        newer_draw = json.loads(json.dumps(draw))
        newer_draw["q"] = 1
        newer_draw["motion"] = {
            "candidate": True, "expected": True, "status": "captured",
            "pool_resource": "0xE", "pool_view": "0xF",
            "slot": {"resource": "0xA1", "view": "0xA2", "format": 16,
                     "view_format": 16, "width": 32, "height": 32, "valid": True},
            "depth": {"resource": "0x1", "view": "0xB", "format": 19,
                      "view_format": 20, "width": 32, "height": 32, "valid": True},
            "pool": {"status": "ok", "reason": "", "resource": "0xE", "view": "0xF",
                     "byte_width": len(pool_bytes), "stride": 336,
                     "first_element": 0, "num_elements": 4,
                     "offset": 0, "length": len(pool_bytes)}}
        for target, phase, offset, fmt in ((6, "before", 2048, 16), (7, "before", 4096, 19),
                                           (6, "after", 6144, 16), (7, "after", 8192, 19)):
            newer_draw["copies"].append({"rt_index": target, "point": 0, "phase": phase,
                                          "status": "ok", "format": fmt, "bpp": 8,
                                          "x0": 8, "y0": 8, "offset": offset, "length": 2048})
        newer = {"schema": 2, "status": "complete", "reason": "qualified", "frame": 9,
                 "qualified": True, "identity_match": True, "refusals": 0,
                 "motion_draws": 1, "motion_refusals": 0, "motion_complete": True,
                 "render_width": 32, "render_height": 32,
                 "prior_depth": "1", "selected_depth": "1", "selected_hdr": "2",
                 "draw_cap": 512, "draws_seen": 1, "draws_recorded": 1, "overflow": 0,
                 "pixels_file": "frame_9_pixels.bin", "pixels_bytes": len(packed),
                 "cb_file": "frame_9_cb.bin", "cb_bytes": len(cb),
                 "pool_file": "frame_9_pool.bin", "pool_bytes": len(pool_bytes),
                 "points": [{"u": .5, "v": .5, "x": 16, "y": 16}], "draws": [newer_draw]}
        second_path = second / "frame_9.json"
        second_path.write_text(json.dumps(newer), encoding="utf-8")
        motion_frame = load_frame(second_path)
        row = motion_frame["motion_windows"][0]
        assert motion_frame["motion_complete"] and row["before_exact_depth"] == 256
        assert row["after_exact_depth"] == 0 and row["slot_changed_pixels"] == 256
        assert row["dsv_changed_pixels"] == 0 and row["before_codes"] == {"7": 256}
        expected_hash = hashlib.sha256(pool_bytes[3*336:4*336]).hexdigest()
        assert row["pool_record_sha256"] == {"3": expected_hash}
        newer_draw["motion"]["pool"]["offset"] = len(pool_bytes) - 1
        second_path.write_text(json.dumps(newer), encoding="utf-8")
        try:
            load_frame(second_path)
            raise AssertionError("out-of-bounds raw pool snapshot accepted")
        except CaptureError:
            pass
        newer_draw["motion"]["pool"]["offset"] = 0
        newer_draw["motion"]["depth"]["format"] = 39
        second_path.write_text(json.dumps(newer), encoding="utf-8")
        try:
            load_frame(second_path)
            raise AssertionError("mismatched native depth layout accepted")
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
