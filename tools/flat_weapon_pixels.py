#!/usr/bin/env python3
"""Inspect the bounded F10 weapon colour/depth/stencil footprint.

Usage: python tools/flat_weapon_pixels.py CAPTURE_DIR [--output-previews DIR] [--dry-run]
       python tools/flat_weapon_pixels.py --self-test

Byte changes prove only changes in the sampled source ROI at these snapshots.
Neither an unchanged colour pixel nor a stencil mark identifies draw ownership.
"""

import argparse
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile

MAX_MANIFEST = 256 * 1024
MAX_PLANE = 128 * 1024 * 1024
MAX_PIXELS = 8 * 1024 * 1024
MAX_SESSION_BYTES = 384 * 1024 * 1024
HEX64 = re.compile(r"[0-9A-F]{16}\Z")
FRAME = re.compile(r"frame_([0-9]+)\.json\Z")
SAFE_FILE = re.compile(r"[A-Za-z0-9_.-]{1,128}\Z")
STAGES = ("before", "after", "hdr_consumer", "end_frame")
STATES = ("complete", "partial", "failed")
COLOR_BPP = {26: 4, 10: 8, 28: 4, 29: 4, 87: 4}
# R11G11B10_FLOAT, RGBA16_FLOAT, RGBA8_UNORM/(SRGB), BGRA8_UNORM.
DEPTH_FORMAT = 41  # R32_FLOAT mirror of R32G8X24_TYPELESS
STENCIL_FORMAT = 62  # R8_UINT mirror
QUERY_STATES = ("complete", "unavailable", "failed", "timeout")
PIPELINE_FIELDS = ("ia_vertices", "ia_primitives", "vs_invocations", "gs_invocations",
                   "gs_primitives", "c_invocations", "c_primitives", "ps_invocations",
                   "hs_invocations", "ds_invocations", "cs_invocations")
DRAW_KINDS = ("Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced",
              "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect")


class CaptureError(ValueError):
    pass


def integer(v, name, lo=0, hi=2**64 - 1):
    if type(v) is not int or not lo <= v <= hi:
        raise CaptureError(f"{name} must be an integer in [{lo}, {hi}]")
    return v


def ident(v, name):
    if not isinstance(v, str) or not HEX64.fullmatch(v):
        raise CaptureError(f"{name} must be 16 uppercase hexadecimal digits")
    return v


def short_text(v, name):
    if not isinstance(v, str) or len(v) > 512:
        raise CaptureError(f"{name} must be a short string")
    return v


def state(v, name):
    if v not in STATES:
        raise CaptureError(f"{name} must be complete, partial or failed")
    return v


def resource(v, name):
    # Resource identifiers are logged pointer-like tokens, never used as paths.
    if type(v) is int:
        return integer(v, name, 0)
    if isinstance(v, str) and re.fullmatch(r"(?:0x)?[0-9A-Fa-f]{1,16}", v):
        return int(v, 16)
    raise CaptureError(f"{name} must be a hexadecimal resource identity")


def plane_format(v, name):
    if type(v) is int:
        return v
    if isinstance(v, str):
        names = {"R11G11B10_FLOAT": 26, "R16G16B16A16_FLOAT": 10,
                 "R8G8B8A8_UNORM": 28, "R8G8B8A8_UNORM_SRGB": 29,
                 "B8G8R8A8_UNORM": 87, "R32_FLOAT": 41, "R8_UINT": 62}
        names.update({"R32G8X24_TYPELESS": 19, "D32_FLOAT_S8X24_UINT": 20,
                      "R32_FLOAT_X8X24_TYPELESS": 21})
        if v in names:
            return names[v]
    raise CaptureError(f"{name}: unsupported format {v!r}")


def _unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise CaptureError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def _camera_rows(rows, name):
    if not isinstance(rows, list) or len(rows) != 6 or any(
            not isinstance(row, list) or len(row) != 4 or any(
                type(x) not in (int, float) or not math.isfinite(x) for x in row)
            for row in rows):
        raise CaptureError(f"{name} must be six finite float4 rows")


def _finite_number(value, name, low=-1e9, high=1e9):
    if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
        raise CaptureError(f"{name} must be a bounded finite number")
    return value


def _boolean(value, name):
    if type(value) is not bool:
        raise CaptureError(f"{name} must be boolean")
    return value


def _schema2_draw_state(data):
    draw = data.get("draw_state")
    if not isinstance(draw, dict) or draw.get("kind") not in DRAW_KINDS:
        raise CaptureError("schema 2 draw_state has unknown D3D draw kind")
    known = _boolean(draw.get("arguments_known"), "draw_state.arguments_known")
    if known == (draw["kind"] in ("DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect")):
        raise CaptureError("draw kind disagrees with argument availability")
    for key in ("count", "start", "base", "instances", "start_instance"):
        value = draw.get(key)
        if known:
            integer(value, "draw_state." + key, -2**31 if key == "base" else 0,
                    2**31 - 1 if key == "base" else 2**32 - 1)
        elif value is not None:
            raise CaptureError(f"draw_state.{key} must be null for unknown draw arguments")
    integer(draw.get("topology"), "draw_state.topology", 0, 64)
    for key in ("effective_rtv", "effective_dsv"):
        if resource(draw.get(key), "draw_state." + key) != resource(
                data["rtv" if key == "effective_rtv" else "dsv"], key):
            raise CaptureError(f"draw_state.{key} differs from the selected draw target")
    blend = draw.get("blend")
    if not isinstance(blend, dict):
        raise CaptureError("draw_state.blend must be an object")
    for key in ("alpha_to_coverage", "independent"):
        _boolean(blend.get(key), "draw_state.blend." + key)
    integer(blend.get("sample_mask"), "draw_state.blend.sample_mask", 0, 2**32 - 1)
    for key, validator in (("write_masks", lambda v, n: integer(v, n, 0, 15)),
                           ("blend_enable", _boolean)):
        values = blend.get(key)
        if not isinstance(values, list) or len(values) != 8:
            raise CaptureError(f"draw_state.blend.{key} must have eight targets")
        for i, value in enumerate(values):
            validator(value, f"draw_state.blend.{key}[{i}]")
    raster = draw.get("raster")
    if not isinstance(raster, dict):
        raise CaptureError("draw_state.raster must be an object")
    integer(raster.get("cull"), "draw_state.raster.cull", 1, 3)
    _boolean(raster.get("scissor_enable"), "draw_state.raster.scissor_enable")
    scissors = raster.get("scissors")
    if not isinstance(scissors, list) or len(scissors) > 16:
        raise CaptureError("draw_state.raster.scissors must be a bounded list")
    for i, rect in enumerate(scissors):
        if not isinstance(rect, dict):
            raise CaptureError(f"draw_state.raster.scissors[{i}] must be an object")
        edges = {key: integer(rect.get(key), f"scissors[{i}].{key}", -2**31, 2**31 - 1)
                 for key in ("left", "top", "right", "bottom")}
        if edges["right"] < edges["left"] or edges["bottom"] < edges["top"]:
            raise CaptureError("scissor rectangle has negative extent")
    integer(raster.get("depth_bias"), "draw_state.raster.depth_bias", -2**31, 2**31 - 1)
    _finite_number(raster.get("slope_bias"), "draw_state.raster.slope_bias", -1e6, 1e6)
    pred = draw.get("predication")
    if not isinstance(pred, dict):
        raise CaptureError("draw_state.predication must be an object")
    bound = _boolean(pred.get("bound"), "draw_state.predication.bound")
    pointer = resource(pred.get("pointer"), "draw_state.predication.pointer")
    _boolean(pred.get("value"), "draw_state.predication.value")
    if bound != (pointer != 0):
        raise CaptureError("predication bound flag disagrees with pointer")
    indirect = draw.get("indirect_buffer")
    indirect_offset = draw.get("indirect_offset")
    if "Indirect" in draw["kind"]:
        if resource(indirect, "draw_state.indirect_buffer") == 0:
            raise CaptureError("indirect draw lacks arguments buffer")
        integer(indirect_offset, "draw_state.indirect_offset", 0, 2**32 - 1)
    elif indirect is not None or indirect_offset is not None:
        raise CaptureError("direct draw has an indirect arguments buffer")


def _schema2_query(query, name, fields):
    if not isinstance(query, dict) or query.get("status") not in QUERY_STATES:
        raise CaptureError(f"{name} has invalid query status")
    status = query["status"]
    integer(query.get("hr"), name + ".hr", 0, 2**32 - 1)
    reason = short_text(query.get("reason"), name + ".reason")
    if (status == "complete" and reason) or (status != "complete" and not reason):
        raise CaptureError(f"{name} query status/reason disagree")
    for field in fields:
        value = query.get(field)
        if status == "complete":
            integer(value, name + "." + field, 0, 2**64 - 1)
        elif value is not None:
            raise CaptureError(f"{name}.{field} must be null when query is {status}")


def _schema2_visibility(data):
    visibility = data.get("visibility")
    if not isinstance(visibility, dict) or visibility.get("scope") != "original_exact_draw":
        raise CaptureError("schema 2 visibility must cover the original exact draw")
    if integer(visibility.get("draw_seq"), "visibility.draw_seq") != data["draw_seq"]:
        raise CaptureError("visibility draw identity differs from captured draw")
    status = visibility.get("status")
    if status not in QUERY_STATES:
        raise CaptureError("visibility bracket status is invalid")
    reason = short_text(visibility.get("reason"), "visibility.reason")
    if (status == "complete" and reason) or (status != "complete" and not reason):
        raise CaptureError("visibility bracket status/reason disagree")
    for key in ("begin_seq", "end_seq"):
        seq = visibility.get(key)
        if seq is not None and integer(seq, "visibility." + key) != data["draw_seq"]:
            raise CaptureError("visibility bracket sequence differs from captured draw")
        if status == "complete" and seq is None:
            raise CaptureError("complete visibility bracket lacks begin or end")
    _schema2_query(visibility.get("occlusion"), "visibility.occlusion", ("samples_passed",))
    _schema2_query(visibility.get("pipeline_statistics"), "visibility.pipeline_statistics", PIPELINE_FIELDS)
    if status != "complete" and (visibility["occlusion"]["status"] == "complete" or
                                  visibility["pipeline_statistics"]["status"] == "complete"):
        raise CaptureError("incomplete bracket cannot have complete query results")


def _read_manifest(path):
    if path.stat().st_size > MAX_MANIFEST:
        raise CaptureError(f"{path}: manifest exceeds {MAX_MANIFEST} bytes")
    try:
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object,
                          parse_constant=lambda x: (_ for _ in ()).throw(CaptureError(f"nonfinite JSON value: {x}")))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CaptureError(f"cannot read {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema") not in (1, 2) or type(data.get("schema")) is not int:
        raise CaptureError(f"{path}: unsupported manifest schema")
    frame = integer(data.get("frame"), "frame")
    match = FRAME.fullmatch(path.name)
    if not match or int(match.group(1)) != frame:
        raise CaptureError(f"{path}: filename/frame mismatch")
    state(data.get("status"), "status")
    short_text(data.get("reason"), "reason")
    if not short_text(data.get("build"), "build"):
        raise CaptureError("build identity is empty")
    if data["status"] == "complete" and data["reason"]:
        raise CaptureError("complete capture has a failure reason")
    for key, wanted in (("vs", "025B4B9FF54622ED"), ("ps", "46F92DC71BF8DFA5")):
        if ident(data.get(key), key) != wanted:
            raise CaptureError(f"unexpected {key}: capture is not the exact weapon pair")
    ident(data.get("camera_hash"), "camera_hash")
    ident(data.get("reference_camera_hash"), "reference_camera_hash")
    _camera_rows(data.get("camera_rows"), "camera_rows")
    _camera_rows(data.get("reference_camera_rows"), "reference_camera_rows")
    integer(data.get("draw_seq"), "draw_seq", 1)
    integer(data.get("draw_count_exact"), "draw_count_exact", 1, 1000000)
    first_bad = integer(data.get("first_bad_seq"), "first_bad_seq")
    short_text(data.get("first_bad_cause"), "first_bad_cause")
    if first_bad > data["draw_seq"]:
        raise CaptureError("first bad sequence follows selected draw")
    if type(data.get("hdr_conflict")) is not bool or not data["hdr_conflict"]:
        raise CaptureError("capture must identify an HDR conflict")
    ids = {k: resource(data.get(k), k) for k in
           ("color_resource", "depth_resource", "rtv", "dsv")}
    if any(ids[k] == 0 for k in ids):
        raise CaptureError("resource identities must be nonzero")
    width = integer(data.get("source_width"), "source_width", 1, 16384)
    height = integer(data.get("source_height"), "source_height", 1, 16384)
    color_fmt = plane_format(data.get("color_format"), "color_format")
    if color_fmt not in COLOR_BPP:
        raise CaptureError("unsupported colour source format")
    if plane_format(data.get("depth_format"), "depth_format") != 19:
        raise CaptureError("source depth must be R32G8X24_TYPELESS")
    if plane_format(data.get("depth_view_format"), "depth_view_format") != 20:
        raise CaptureError("depth_view_format must be D32_FLOAT_S8X24_UINT DSV")
    integer(data.get("stencil_ref"), "stencil_ref", 0, 255)
    integer(data.get("stencil_read_mask"), "stencil_read_mask", 0, 255)
    integer(data.get("stencil_write_mask"), "stencil_write_mask", 0, 255)
    if (data["stencil_ref"] & 4) != 4 or (data["stencil_write_mask"] & 4) != 4:
        raise CaptureError("selected pass does not write stencil bit 0x04")
    for key in ("depth_write", "depth_enable", "stencil_enable"):
        if type(data.get(key)) is not bool:
            raise CaptureError(f"{key} must be boolean")
    integer(data.get("depth_func"), "depth_func", 1, 8)
    integer(data.get("dsv_flags"), "dsv_flags", 0, 3)
    for face_name in ("front_stencil", "back_stencil"):
        face = data.get(face_name)
        if not isinstance(face, dict):
            raise CaptureError(f"{face_name} must be an object")
        for key in ("func", "pass", "fail", "depth_fail"):
            integer(face.get(key), face_name + "." + key, 1, 8)
    viewport_count = integer(data.get("viewport_count"), "viewport_count", 0, 16)
    viewport = data.get("viewport")
    if not isinstance(viewport, list) or len(viewport) != 6 or any(
            type(v) not in (int, float) or not math.isfinite(v) for v in viewport):
        raise CaptureError("viewport must be six finite numbers")
    if viewport_count and (viewport[2] <= 0 or viewport[3] <= 0 or
                           viewport[4] < 0 or viewport[5] > 1 or viewport[4] > viewport[5]):
        raise CaptureError("active viewport has invalid extent or depth range")
    consumer = data.get("consumer")
    if not isinstance(consumer, dict) or type(consumer.get("found")) is not bool:
        raise CaptureError("consumer must identify whether the HDR source consumer was found")
    consumer_seq = integer(consumer.get("draw_seq"), "consumer.draw_seq")
    if resource(consumer.get("source_resource"), "consumer.source_resource") != ids["color_resource"]:
        raise CaptureError("consumer source is not the captured HDR resource")
    if consumer.get("phase") != "before_draw":
        raise CaptureError("consumer phase must be before_draw")
    ident(consumer.get("vs"), "consumer.vs")
    ident(consumer.get("ps"), "consumer.ps")
    integer(consumer.get("srv_slot"), "consumer.srv_slot", 0, 2**32 - 1)
    short_text(consumer.get("verdict"), "consumer.verdict")
    if integer(consumer.get("first_bad_seq"), "consumer.first_bad_seq") != first_bad:
        raise CaptureError("consumer and selected draw disagree on first bad sequence")
    if consumer["found"] and consumer_seq < data["draw_seq"]:
        raise CaptureError("consumer precedes the selected draw")
    if data["status"] == "complete" and not consumer["found"]:
        raise CaptureError("complete capture lacks the HDR consumer")
    clears = data.get("clears")
    if not isinstance(clears, list) or len(clears) > 64:
        raise CaptureError("clears must be a bounded list")
    for clear in clears:
        if not isinstance(clear, dict):
            raise CaptureError("clear must be an object")
        integer(clear.get("draw_seq"), "clear.draw_seq")
        integer(clear.get("flags"), "clear.flags", 0, 255)
        integer(clear.get("stencil"), "clear.stencil", 0, 255)
        resource(clear.get("view"), "clear.view")
    integer(data.get("clear_overflow"), "clear_overflow", 0, 1000000)
    roi = data.get("roi")
    if not isinstance(roi, dict):
        raise CaptureError("roi must be an object")
    x = integer(roi.get("x"), "roi.x", 0, width - 1)
    y = integer(roi.get("y"), "roi.y", 0, height - 1)
    w = integer(roi.get("width"), "roi.width", 1, width - x)
    h = integer(roi.get("height"), "roi.height", 1, height - y)
    if w * h > MAX_PIXELS:
        raise CaptureError("ROI exceeds pixel cap")
    stages = data.get("stages")
    if not isinstance(stages, list) or len(stages) != len(STAGES) or any(
            not isinstance(s, dict) for s in stages) or [s.get("name") for s in stages] != list(STAGES):
        raise CaptureError("stages must be before, after, hdr_consumer, end_frame in order")
    used = set()
    images = {}
    for stage in stages:
        name = stage["name"]
        if integer(stage.get("frame"), name + ".frame") != frame:
            raise CaptureError(f"{name}: wrong frame")
        seq = integer(stage.get("draw_seq"), name + ".draw_seq")
        if name in ("before", "after") and seq != data["draw_seq"]:
            raise CaptureError(f"{name}: wrong selected draw")
        if name in ("before", "after") and stage.get("phase") != name + "_draw":
            raise CaptureError(f"{name}: wrong draw phase")
        if name == "hdr_consumer" and stage.get("phase") != "before_draw":
            raise CaptureError("hdr_consumer must be captured before the consumer draw")
        if name == "end_frame" and stage.get("phase") != "before_present":
            raise CaptureError("end_frame must be captured before Present")
        for key in ("color_resource", "depth_resource"):
            if resource(stage.get(key), name + "." + key) != ids[key]:
                raise CaptureError(f"{name}: resource identity changed")
        st = state(stage.get("status"), name + ".status")
        reason = short_text(stage.get("reason"), name + ".reason")
        if st == "complete" and reason:
            raise CaptureError(f"{name}: complete stage has reason")
        if name == "hdr_consumer" and st == "complete" and (not consumer["found"] or seq != consumer_seq):
            raise CaptureError("HDR consumer stage disagrees with consumer identity")
        if name == "end_frame" and st == "complete" and consumer["found"] and seq < consumer_seq:
            raise CaptureError("end-frame stage precedes HDR consumer")
        planes = stage.get("planes")
        if not isinstance(planes, dict) or set(planes) != {"color", "depth", "stencil"}:
            raise CaptureError(f"{name}: expected three planes")
        images[name] = {}
        for plane_name, spec in planes.items():
            if not isinstance(spec, dict):
                raise CaptureError(f"{name}.{plane_name}: invalid plane")
            ps = state(spec.get("status"), name + "." + plane_name + ".status")
            plane_reason = short_text(spec.get("reason"), name + "." + plane_name + ".reason")
            if ps == "complete" and plane_reason:
                raise CaptureError(f"{name}.{plane_name}: complete plane has reason")
            fmt = plane_format(spec.get("format"), name + "." + plane_name + ".format")
            expected_fmt = {"color": color_fmt, "depth": DEPTH_FORMAT,
                            "stencil": STENCIL_FORMAT}[plane_name]
            if fmt != expected_fmt and not (ps != "complete" and fmt == 0):
                raise CaptureError(f"{name}.{plane_name}: wrong format")
            bpp = COLOR_BPP[color_fmt] if plane_name == "color" else (4 if plane_name == "depth" else 1)
            expected_row = w * bpp
            row_bytes = integer(spec.get("row_bytes"), name + "." + plane_name + ".row_bytes")
            if row_bytes != expected_row and not (ps != "complete" and row_bytes == 0):
                raise CaptureError(f"{name}.{plane_name}: row stride mismatch")
            byte_count = integer(spec.get("bytes"), name + "." + plane_name + ".bytes", 0, MAX_PLANE)
            filename = spec.get("file")
            if ps == "complete":
                if st != "complete" or byte_count != expected_row * h or not isinstance(filename, str) or not SAFE_FILE.fullmatch(filename) or filename in used:
                    raise CaptureError(f"{name}.{plane_name}: invalid completed payload")
                used.add(filename)
                target = path.parent / filename
                if target.is_symlink() or not target.is_file() or target.stat().st_size != byte_count:
                    raise CaptureError(f"{name}.{plane_name}: missing or wrong-size payload")
                images[name][plane_name] = target.read_bytes()
            else:
                if byte_count not in (0, expected_row * h):
                    raise CaptureError(f"{name}.{plane_name}: invalid incomplete size")
                if filename and (not isinstance(filename, str) or not SAFE_FILE.fullmatch(filename)):
                    raise CaptureError(f"{name}.{plane_name}: invalid incomplete payload name")
        if st == "complete" and len(images[name]) != 3:
            raise CaptureError(f"{name}: complete stage lacks a plane")
    if data["status"] == "complete" and any(s["status"] != "complete" for s in stages):
        raise CaptureError("complete capture has an incomplete stage")
    if data["schema"] == 2:
        _schema2_draw_state(data)
        _schema2_visibility(data)
    return data, images


def _mask_counts(before, after, bpp, pixels, consumer=None, end=None):
    bc, bd, bs = before["color"], before["depth"], before["stencil"]
    ac, ad, ast = after["color"], after["depth"], after["stencil"]
    cc = consumer["stencil"] if consumer else None
    ec = end["stencil"] if end else None
    counts = {k: 0 for k in ("color_changed", "depth_changed", "stencil_changed",
                            "bit4_before", "bit4_after", "bit4_added", "bit4_removed",
                            "bit4_before_outside_color_change", "bit4_after_outside_color_change",
                            "bit4_added_outside_color_change", "color_changed_with_preexisting_bit4",
                            "color_changed_with_bit4_after", "other_stencil_bits_changed")}
    if cc is not None:
        counts.update(added_bit4_at_consumer=0, color_changed_bit4_at_consumer=0)
    if ec is not None:
        counts.update(added_bit4_at_end_frame=0, color_changed_bit4_at_end_frame=0)
    for i in range(pixels):
        color = bc[i*bpp:(i+1)*bpp] != ac[i*bpp:(i+1)*bpp]
        depth = bd[i*4:(i+1)*4] != ad[i*4:(i+1)*4]
        stencil = bs[i] != ast[i]
        prior = bool(bs[i] & 4)
        marked = bool(ast[i] & 4)
        added = marked and not prior
        counts["color_changed"] += color
        counts["depth_changed"] += depth
        counts["stencil_changed"] += stencil
        counts["bit4_before"] += prior
        counts["bit4_after"] += marked
        counts["bit4_added"] += added
        counts["bit4_removed"] += prior and not marked
        counts["bit4_before_outside_color_change"] += prior and not color
        counts["bit4_after_outside_color_change"] += marked and not color
        counts["bit4_added_outside_color_change"] += added and not color
        counts["color_changed_with_preexisting_bit4"] += color and prior
        counts["color_changed_with_bit4_after"] += color and marked
        if cc is not None:
            counts["added_bit4_at_consumer"] += added and bool(cc[i] & 4)
            counts["color_changed_bit4_at_consumer"] += color and marked and bool(cc[i] & 4)
        if ec is not None:
            counts["added_bit4_at_end_frame"] += added and bool(ec[i] & 4)
            counts["color_changed_bit4_at_end_frame"] += color and marked and bool(ec[i] & 4)
        counts["other_stencil_bits_changed"] += bool((bs[i] ^ ast[i]) & ~4)
    return counts


def _transition_counts(a, b, bpp, pixels):
    return {
        "color_changed": sum(a["color"][i*bpp:(i+1)*bpp] != b["color"][i*bpp:(i+1)*bpp]
                             for i in range(pixels)),
        "depth_changed": sum(a["depth"][i*4:(i+1)*4] != b["depth"][i*4:(i+1)*4]
                             for i in range(pixels)),
        "stencil_changed": sum(a["stencil"][i] != b["stencil"][i] for i in range(pixels)),
        "bit4_lost": sum(bool(a["stencil"][i] & 4) and not bool(b["stencil"][i] & 4)
                         for i in range(pixels)),
        "bit4_gained": sum(not bool(a["stencil"][i] & 4) and bool(b["stencil"][i] & 4)
                           for i in range(pixels)),
    }


def _linear_difference(before, after, fmt, pixels):
    finite = nonfinite = changed = over_one_percent = 0
    total = maximum = 0.0
    for i in range(pixels):
        a, b = _linear_rgb(before, fmt, i), _linear_rgb(after, fmt, i)
        if not all(math.isfinite(x) for x in a+b):
            nonfinite += 1
            continue
        delta = max(abs(x-y) for x, y in zip(a, b))
        finite += 1
        changed += delta != 0
        over_one_percent += delta >= 0.01
        total += sum(abs(x-y) for x, y in zip(a, b)) / 3
        maximum = max(maximum, delta)
    return {"finite_pixels": finite, "nonfinite_pixels": nonfinite,
            "decoded_rgb_changed_pixels": changed,
            "decoded_rgb_max_channel_delta_at_least_0_01": over_one_percent,
            "mean_absolute_linear_rgb_per_finite_pixel": total / finite if finite else None,
            "max_absolute_linear_rgb_channel_delta": maximum if finite else None}


def analyze(path):
    manifest, images = _read_manifest(path)
    roi = manifest["roi"]
    result = {"schema": manifest["schema"], "build": manifest["build"], "frame": manifest["frame"], "status": manifest["status"],
              "reason": manifest["reason"], "draw_seq": manifest["draw_seq"],
              "draw_count_exact": manifest["draw_count_exact"],
              "shader_pair": {"vs": manifest["vs"], "ps": manifest["ps"]},
              "camera_hash": manifest["camera_hash"],
              "reference_camera_hash": manifest["reference_camera_hash"],
              "first_bad_seq": manifest["first_bad_seq"],
              "first_bad_cause": manifest["first_bad_cause"],
              "resource_identities": {key: manifest[key] for key in
                                      ("color_resource", "depth_resource", "rtv", "dsv")},
              "state": {key: manifest[key] for key in ("color_format", "depth_format", "depth_view_format",
                                                      "stencil_ref", "stencil_read_mask", "stencil_write_mask",
                                                      "depth_write", "depth_enable", "depth_func", "stencil_enable",
                                                      "dsv_flags", "front_stencil", "back_stencil", "viewport_count",
                                                      "viewport", "hdr_conflict")},
              "consumer": manifest["consumer"], "stencil_clears": manifest["clears"],
              "stencil_clear_overflow": manifest["clear_overflow"],
              "source": {"width": manifest["source_width"], "height": manifest["source_height"],
                         "roi": roi,
                         "roi_coverage_fraction": (roi["width"] * roi["height"] /
                                                   (manifest["source_width"] * manifest["source_height"]))},
               "stages_complete": [s for s in STAGES if len(images[s]) == 3]}
    if manifest["schema"] == 2:
        result["draw_state"] = manifest["draw_state"]
        result["visibility"] = manifest["visibility"]
    if all(len(images[s]) == 3 for s in STAGES[:2]):
        fmt = plane_format(manifest["color_format"], "color_format")
        bpp = COLOR_BPP[fmt]
        pixels = roi["width"] * roi["height"]
        result["counts"] = _mask_counts(
            images["before"], images["after"], bpp, pixels,
            images["hdr_consumer"] if len(images["hdr_consumer"]) == 3 else None,
            images["end_frame"] if len(images["end_frame"]) == 3 else None)
        result["linear_color_before_after"] = _linear_difference(
            images["before"]["color"], images["after"]["color"], fmt,
            pixels)
        result["pixels_sampled"] = pixels
        result["transitions"] = {"before_to_after": _transition_counts(images["before"], images["after"], bpp, pixels)}
        if len(images["hdr_consumer"]) == 3:
            result["transitions"]["after_to_hdr_consumer"] = _transition_counts(images["after"], images["hdr_consumer"], bpp, pixels)
        if len(images["hdr_consumer"]) == 3 and len(images["end_frame"]) == 3:
            result["transitions"]["hdr_consumer_to_end_frame"] = _transition_counts(images["hdr_consumer"], images["end_frame"], bpp, pixels)
    result["limits"] = [
        "A raw color change proves only a sampled render-target value changed; unchanged color does not prove the draw had no footprint.",
        "Stencil bit 0x04 is used by other game passes. A set bit is not exclusive weapon ownership.",
        "Survival counts are observations at two later snapshots; intervening clear, overwrite, or re-mark cannot be attributed from endpoints.",
        "The end_frame snapshot is taken before Present from a retained resource; it is not proof of displayed pixel ownership.",
        "ROI covers only the reported source coordinates; changes and marks outside it were not sampled.",
        "One selected draw per frame is captured; draw_count_exact may exceed one. Final visible ownership and temporal history safety are unproven."]
    if manifest["schema"] == 2:
        result["limits"].extend([
            "Occlusion samples count the whole draw, not the sampled ROI or final color ownership.",
            "PS invocations prove shader execution, not a surviving color output; unavailable or timed-out queries are unknown, not zero."])
    return result, manifest, images


def _r11(v, mantissa_bits):
    m = (1 << mantissa_bits) - 1
    mantissa = v & m
    exponent = (v >> mantissa_bits) & 31
    if exponent == 0:
        return mantissa * (2.0 ** (1 - 15 - mantissa_bits))
    if exponent == 31:
        return math.inf
    return (1 + mantissa / (m + 1)) * (2.0 ** (exponent - 15))


def _linear_rgb(data, fmt, i):
    if fmt == 26:
        bits = struct.unpack_from("<I", data, i*4)[0]
        return (_r11(bits & 2047, 6), _r11((bits >> 11) & 2047, 6),
                _r11((bits >> 22) & 1023, 5))
    if fmt == 10:
        return struct.unpack_from("<eee", data, i*8)
    channels = data[i*4:i*4+3]
    if fmt == 87:
        channels = channels[::-1]
    rgb = tuple(v/255 for v in channels)
    if fmt == 29:
        return tuple(v/12.92 if v <= 0.04045 else ((v+0.055)/1.055)**2.4 for v in rgb)
    return rgb


def _linear_pfm(data, fmt, width, height):
    out = bytearray(f"PF\n{width} {height}\n-1.0\n".encode("ascii"))
    for y in range(height - 1, -1, -1):
        for x in range(width):
            rgb = _linear_rgb(data, fmt, y*width + x)
            out.extend(struct.pack("<fff", *(v if math.isfinite(v) else 0.0 for v in rgb)))
    return bytes(out)


def _preview(data, fmt, width, height):
    out = bytearray(f"P6\n# display preview: fixed exposure=1, Reinhard, sRGB\n{width} {height}\n255\n".encode("ascii"))
    for i in range(width * height):
        rgb = _linear_rgb(data, fmt, i)
        for x in rgb:
            x = 0.0 if not math.isfinite(x) or x < 0 else x
            x = x / (1 + x)
            x = x * 12.92 if x <= 0.0031308 else 1.055 * (x ** (1/2.4)) - 0.055
            out.append(min(255, max(0, round(x*255))))
    return bytes(out)


def manifests(directory):
    if directory.is_file():
        return [directory]
    if not directory.is_dir():
        raise CaptureError(f"capture path is not a directory: {directory}")
    paths = sorted(directory.glob("frame_*.json"))
    if not paths:
        paths = sorted(p for child in directory.iterdir() if child.is_dir()
                       for p in child.glob("frame_*.json"))
    if not paths or len(paths) > 16:
        raise CaptureError("expected one to sixteen frame manifests")
    return paths


def _verify_wide_fixture(report, manifest):
    cases = {
        "visible": (501, 16, 20, 16, 1, 16, True, "complete"),
        "depth_rejected": (601, 0, 4, 0, 1, 0, False, "complete"),
        "color_disabled": (701, 0, 20, 16, 1, 16, True, "complete"),
        "zero_count": (801, 0, 4, 0, 0, 0, False, "complete"),
        "query_unavailable": (901, 16, 20, 16, None, None, False, "unavailable"),
        "query_timeout": (1001, 16, 20, 16, None, None, False, "timeout"),
    }
    case = manifest.get("fixture_case")
    if manifest.get("fixture_kind") != "flat_weapon_gpu_v2" or case not in cases:
        raise CaptureError("unknown wide GPU fixture identity")
    frame, color, bit4_after, added, primitives, samples, ps_required, query_status = cases[case]
    counts = report.get("counts", {})
    source = report["source"]
    roi = source["roi"]
    visibility = report.get("visibility", {})
    occlusion = visibility.get("occlusion", {})
    pipeline = visibility.get("pipeline_statistics", {})
    draw = report.get("draw_state", {})
    ps = pipeline.get("ps_invocations")
    if (report["schema"] != 2 or report["frame"] != frame or
            source["width"] != 2304 or source["height"] != 64 or
            (roi["x"], roi["y"], roi["width"], roi["height"]) != (0, 0, 2304, 64) or
            report.get("pixels_sampled") != 2304 * 64 or
            counts.get("color_changed") != color or counts.get("depth_changed") != 0 or
            counts.get("bit4_before") != 4 or counts.get("bit4_after") != bit4_after or
            counts.get("bit4_added") != added or
            counts.get("bit4_before_outside_color_change") != 4 or
            visibility.get("status") != "complete" or
            occlusion.get("status") != query_status or occlusion.get("samples_passed") != samples or
            pipeline.get("status") != query_status or pipeline.get("ia_primitives") != primitives or
            pipeline.get("cs_invocations") != (0 if query_status == "complete" else None) or
            (ps_required and (type(ps) is not int or ps <= 0)) or
            (case == "zero_count" and ps != 0) or
            (case == "zero_count" and draw.get("count") != 0) or
            (case == "color_disabled" and draw.get("blend", {}).get("write_masks", [None])[0] != 0) or
            (case != "color_disabled" and draw.get("blend", {}).get("write_masks", [None])[0] == 0)):
        raise CaptureError(f"wide GPU fixture {case} disagrees with known draw/visibility footprint")


def self_test():
    assert _linear_rgb(bytes((0, 0, 255, 255)), 87, 0) == (1.0, 0.0, 0.0)
    assert abs(_linear_rgb(bytes((128, 0, 0, 255)), 29, 0)[0] - 0.21586) < 0.0001
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        w, h = 3, 2
        color0 = bytes(4*w*h)
        color1 = bytearray(color0); color1[0] = 1; color1[4] = 2
        stencil0 = bytes((0, 4, 0, 4, 0, 0))
        stencil1 = bytes((4, 4, 4, 4, 0, 0))
        snapshots = ((color0, stencil0), (bytes(color1), stencil1),
                     (bytes(color1), bytes((4, 4, 0, 4, 0, 0))),
                     (bytes(color1), bytes((0, 4, 0, 4, 0, 0))))
        data = {"schema": 1, "build": "fixture-test", "frame": 7, "status": "complete", "reason": "",
                "vs": "025B4B9FF54622ED", "ps": "46F92DC71BF8DFA5",
                "camera_hash": "1234567890ABCDEF", "reference_camera_hash": "0000000000001234",
                "camera_rows": [[0.0]*4 for _ in range(6)],
                "reference_camera_rows": [[0.0]*4 for _ in range(6)],
                "first_bad_seq": 19, "first_bad_cause": "test-hdr-camera-conflict",
                "draw_seq": 19, "draw_count_exact": 2, "hdr_conflict": True,
                "color_resource": "0xA", "depth_resource": "0xB", "rtv": "0xC", "dsv": "0xD",
                "source_width": 10, "source_height": 10, "color_format": 26,
                "depth_format": 19, "depth_view_format": 20, "stencil_ref": 4,
                "stencil_read_mask": 255, "stencil_write_mask": 4, "depth_write": False,
                "depth_enable": True, "depth_func": 4, "stencil_enable": True, "dsv_flags": 0,
                "front_stencil": {"func": 8, "pass": 3, "fail": 1, "depth_fail": 1},
                "back_stencil": {"func": 8, "pass": 3, "fail": 1, "depth_fail": 1},
                "viewport_count": 1, "viewport": [0, 0, 10, 10, 0, 1],
                "roi": {"x": 2, "y": 3, "width": w, "height": h},
                "consumer": {"draw_seq": 19, "source_resource": "0xA", "found": True,
                             "phase": "before_draw", "vs": "0000000000000000",
                             "ps": "0000000000000000", "srv_slot": 0,
                             "verdict": "test", "first_bad_seq": 19},
                "clears": [], "clear_overflow": 0,
                "stages": []}
        for name, (color, stencil) in zip(STAGES, snapshots):
            planes = {}
            for plane, payload, fmt, stride in (("color", color, 26, 4*w),
                                                ("depth", bytes(4*w*h), 41, 4*w),
                                                ("stencil", stencil, 62, w)):
                filename = f"{name}_{plane}.bin"
                (root / filename).write_bytes(payload)
                planes[plane] = {"file": filename, "format": fmt, "row_bytes": stride,
                                 "bytes": len(payload), "status": "complete", "reason": ""}
            phase = {"before": "before_draw", "after": "after_draw",
                     "hdr_consumer": "before_draw", "end_frame": "before_present"}[name]
            data["stages"].append({"name": name, "frame": 7, "draw_seq": 19,
                                   "color_resource": "0xA", "depth_resource": "0xB",
                                   "status": "complete", "reason": "", "planes": planes,
                                   "phase": phase})
        path = root / "frame_7.json"
        def write():
            path.write_text(json.dumps(data), encoding="utf-8")
        write()
        result, _, _ = analyze(path)
        assert result["counts"]["color_changed"] == 2
        assert result["counts"]["bit4_added"] == 2
        assert result["counts"]["bit4_before_outside_color_change"] == 1
        assert result["counts"]["color_changed_with_preexisting_bit4"] == 1
        assert result["counts"]["added_bit4_at_consumer"] == 1
        assert result["counts"]["added_bit4_at_end_frame"] == 0
        depth_path = root / "after_depth.bin"
        original_depth = depth_path.read_bytes()
        changed_depth = bytearray(original_depth)
        struct.pack_into("<f", changed_depth, 4*5, 0.25)
        depth_path.write_bytes(changed_depth)
        assert analyze(path)[0]["counts"]["depth_changed"] == 1
        depth_path.write_bytes(original_depth)
        before = json.loads(json.dumps(data))
        partial = json.loads(json.dumps(before))
        partial.update(status="partial", reason="frame-end-not-observed")
        partial["stages"][3].update(status="partial", reason="not-reached")
        for plane in partial["stages"][3]["planes"].values():
            plane.update(status="partial", reason="not-reached", format=0,
                         row_bytes=0, bytes=0, file="")
        data = partial; write()
        partial_report = analyze(path)[0]
        assert partial_report["counts"]["color_changed"] == 2
        assert "added_bit4_at_end_frame" not in partial_report["counts"]
        mutants = [lambda d: d.update(schema=2),
                   lambda d: d["roi"].update(width=9),
                   lambda d: d["stages"][1].update(frame=8),
                   lambda d: d["stages"][2].update(depth_resource="0xE"),
                   lambda d: d["stages"][2].update(phase="after_draw"),
                   lambda d: d["stages"][0]["planes"]["color"].update(file="../escape.bin"),
                   lambda d: d["stages"][0]["planes"]["color"].update(row_bytes=5),
                   lambda d: d["stages"][0]["planes"]["stencil"].update(format=61),
                   lambda d: d["stages"][0]["planes"]["color"].update(bytes=1),
                   lambda d: d.update(ps="0000000000000000"),
                   lambda d: d.update(stencil_write_mask=0)]
        for mutate in mutants:
            data = json.loads(json.dumps(before)); mutate(data); write()
            try:
                analyze(path)
            except CaptureError:
                pass
            else:
                raise AssertionError(f"accepted malformed capture: {mutate}")
        v2 = json.loads(json.dumps(before))
        v2["schema"] = 2
        v2["draw_state"] = {
            "kind": "Draw", "arguments_known": True, "count": 3, "start": 0,
            "base": 0, "instances": 1, "start_instance": 0, "topology": 4,
            "indirect_buffer": None, "indirect_offset": None,
            "effective_rtv": "0xC", "effective_dsv": "0xD",
            "blend": {"alpha_to_coverage": False, "independent": False,
                      "sample_mask": 0xffffffff, "write_masks": [15] * 8,
                      "blend_enable": [False] * 8},
            "raster": {"cull": 1, "scissor_enable": True,
                       "scissors": [{"left": 0, "top": 0, "right": 3, "bottom": 2}],
                       "depth_bias": 0, "slope_bias": 0.0},
            "predication": {"bound": False, "pointer": "0x0", "value": False}}
        v2["visibility"] = {
            "scope": "original_exact_draw", "status": "complete", "reason": "",
            "draw_seq": 19, "begin_seq": 19, "end_seq": 19,
            "occlusion": {"status": "complete", "samples_passed": 2, "hr": 0, "reason": ""},
            "pipeline_statistics": {"status": "complete", "hr": 0, "reason": "",
                                    **{key: (2 if key == "ps_invocations" else 0)
                                       for key in PIPELINE_FIELDS}}}
        data = v2; write()
        assert analyze(path)[0]["visibility"]["occlusion"]["samples_passed"] == 2
        v2_mutants = [lambda d: d["visibility"].update(begin_seq=18),
                      lambda d: d["visibility"]["occlusion"].update(status="timeout", reason="timeout"),
                      lambda d: d["visibility"].update(status="unavailable", reason="no-bracket"),
                      lambda d: d["draw_state"]["blend"]["write_masks"].__setitem__(0, 16),
                      lambda d: d["draw_state"]["raster"]["scissors"][0].update(right=-1),
                      lambda d: d["draw_state"]["predication"].update(pointer="0x1")]
        for mutate in v2_mutants:
            data = json.loads(json.dumps(v2)); mutate(data); write()
            try:
                analyze(path)
            except CaptureError:
                pass
            else:
                raise AssertionError(f"accepted malformed schema 2 capture: {mutate}")
        data = json.loads(json.dumps(v2))
        data["visibility"]["occlusion"].update(status="timeout", samples_passed=None,
                                                hr=1, reason="readback-timeout")
        write()
        timed_out = analyze(path)[0]["visibility"]["occlusion"]
        assert timed_out["samples_passed"] is None and timed_out["status"] == "timeout"
        data = json.loads(json.dumps(v2))
        data["draw_state"]["count"] = 0
        data["visibility"]["occlusion"]["samples_passed"] = 0
        for key in PIPELINE_FIELDS:
            data["visibility"]["pipeline_statistics"][key] = 0
        write()
        zero = analyze(path)[0]["visibility"]
        assert zero["occlusion"]["samples_passed"] == 0
        assert zero["pipeline_statistics"]["ia_primitives"] == 0
        names_before = set(root.iterdir())
        results = run(root, output_previews=root / "previews", dry_run=True)
        assert len(results) == 1 and names_before == set(root.iterdir())
    print("flat weapon pixel analyzer self-test passed")


def run(capture, output_previews=None, dry_run=False, verify_fixture=False,
        verify_partial_fixture=False):
    paths = manifests(capture)
    if (verify_fixture or verify_partial_fixture) and len(paths) != 1:
        raise CaptureError("GPU fixture must contain exactly one frame")
    for directory in {path.parent for path in paths}:
        payloads = [p for p in directory.iterdir() if p.suffix in (".bin", ".json")]
        if len(payloads) > 128 or sum(p.stat().st_size for p in payloads) > MAX_SESSION_BYTES:
            raise CaptureError(f"{directory}: capture session exceeds file or byte budget")
    results = []
    for path in paths:
        report, manifest, images = analyze(path)
        if verify_fixture and (report["status"] != "complete" or
                               report["stages_complete"] != list(STAGES) or
                               not report.get("counts")):
            raise CaptureError("GPU fixture lacks a complete four-stage capture")
        if verify_fixture:
            if manifest.get("fixture_kind") == "flat_weapon_gpu_v2":
                _verify_wide_fixture(report, manifest)
            else:
                expected = {"color_changed": 16, "depth_changed": 0,
                            "bit4_before": 4, "bit4_after": 19, "bit4_added": 15,
                            "bit4_before_outside_color_change": 3,
                            "color_changed_with_preexisting_bit4": 1,
                            "added_bit4_at_consumer": 12, "added_bit4_at_end_frame": 0}
                if (manifest.get("fixture_kind") != "flat_weapon_gpu_v1" or
                        report["pixels_sampled"] != 4096 or
                        any(report["counts"].get(k) != v for k, v in expected.items()) or
                        report["transitions"]["after_to_hdr_consumer"]["depth_changed"] != 4 or
                        report["transitions"]["hdr_consumer_to_end_frame"]["depth_changed"] != 0 or
                        report["transitions"]["hdr_consumer_to_end_frame"]["bit4_lost"] != 15):
                    raise CaptureError("GPU fixture does not match the known color/depth/stencil footprint")
        if verify_partial_fixture:
            expected = {"color_changed": 16, "depth_changed": 0,
                        "bit4_before": 4, "bit4_after": 19, "bit4_added": 15,
                        "bit4_before_outside_color_change": 3,
                        "color_changed_with_preexisting_bit4": 1}
            if (manifest.get("fixture_kind") != "flat_weapon_gpu_v1" or
                    report["frame"] != 201 or report.get("pixels_sampled") != 4096 or
                    report["status"] != "partial" or report["reason"] != "consumer-not-observed" or
                    report["consumer"]["found"] or
                    report["stages_complete"] != ["before", "after", "end_frame"] or
                    any(report.get("counts", {}).get(k) != v for k, v in expected.items()) or
                    "added_bit4_at_consumer" in report.get("counts", {})):
                raise CaptureError("GPU partial fixture does not preserve before/after evidence and missing-consumer status")
        results.append(report)
        if output_previews and any("color" in images[s] for s in STAGES):
            fmt = plane_format(manifest["color_format"], "color_format")
            roi = manifest["roi"]
            for name in STAGES:
                if "color" not in images[name]:
                    continue
                payload = images[name]["color"]
                for suffix, encoder in (("linear_rgb.pfm", _linear_pfm),
                                        ("display_preview.ppm", _preview)):
                    target = output_previews / f"frame_{manifest['frame']}_{name}_{suffix}"
                    if not dry_run:
                        output_previews.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(encoder(payload, fmt, roi["width"], roi["height"]))
                    report.setdefault("preview_files", []).append(str(target))
            report["preview_note"] = "PFM contains decoded linear RGB (nonfinite channels replaced by zero); PPM applies exposure=1, Reinhard and sRGB for display."
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=Path)
    parser.add_argument("--output-previews", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--verify-fixture", action="store_true")
    parser.add_argument("--verify-partial-fixture", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            if args.capture or args.output_previews or args.verify_fixture or args.verify_partial_fixture:
                parser.error("--self-test takes no capture or output")
            self_test()
            return 0
        if not args.capture:
            parser.error("capture path required")
        if args.verify_fixture and args.verify_partial_fixture:
            parser.error("choose one fixture verification mode")
        print(json.dumps(run(args.capture, args.output_previews, args.dry_run,
                             args.verify_fixture, args.verify_partial_fixture), indent=2))
        return 0
    except (CaptureError, OSError, AssertionError) as exc:
        print(f"flat weapon pixels: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
