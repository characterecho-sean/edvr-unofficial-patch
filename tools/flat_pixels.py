"""Inspect a flat temporal capture without changing the capture directory.

Usage: python tools/flat_pixels.py CAPTURE_DIR [--output PREVIEW_DIR] [--dry-run]
CAPTURE_DIR may be one session or its flat_pixels parent. A normal invocation
prints JSON statistics only. Previews are written only with --output.
--assume-static-scene on|off replays the engine analysis with the 3D main menu's stale-slot
policy forced (the capture's own flag is "static_scene"; captures before it read as off).
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
import tempfile
import zlib
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None


MAX_DIMENSION = 16384
MAX_CAPTURE_BYTES = 384 * 1024 * 1024
TEXTURES = {
    "color": (28, 4, "render"),       # DXGI_FORMAT_R8G8B8A8_UNORM
    "depth": (41, 4, "render"),       # DXGI_FORMAT_R32_FLOAT
    "motion": (34, 4, "render"),      # DXGI_FORMAT_R16G16_FLOAT
    "rejection": (61, 1, "render"),   # DXGI_FORMAT_R8_UNORM
    "raw": (28, 4, "output"),         # DLSS R8G8B8A8_UNORM
    "final": (27, 4, "output"),       # R8G8B8A8_TYPELESS, RGBA8 bytes
}
HDR_TEXTURES = {
    "color": (26, 4, "render"),     # R11G11B10_FLOAT scene H copied before resolve
    "depth": (41, 4, "render"),
    "motion": (34, 4, "render"),
    "rejection": (61, 1, "render"),
    "raw": (10, 8, "output"),      # R16G16B16A16_FLOAT NGX output
    "final": (26, 4, "output"),   # scene H after finish, before tone pass
}
HDR_PROVENANCE = {
    "color_provenance": "private-copy-of-scene-H-before-resolve",
    "raw_provenance": "ngx-fp16-output0",
    "final_provenance": "scene-H-after-finish-before-tonemap",
}
SLOTS = (16, 8, "render")          # DXGI_FORMAT_R32G32_FLOAT
MAX_POOL_BYTES = 64 * 1024 * 1024
MAX_SCENE_BYTES = 64 * 1024


class CaptureError(ValueError):
    pass


def _integer(value, label, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise CaptureError(f"{label} must be an integer in [{minimum}, {maximum}]")
    return value


def _number_pair(value, label):
    if not isinstance(value, list) or len(value) != 2:
        raise CaptureError(f"{label} must contain two numbers")
    for component in value:
        if type(component) not in (int, float) or not math.isfinite(component):
            raise CaptureError(f"{label} must contain two finite numbers")
        if abs(component) > MAX_DIMENSION:
            raise CaptureError(f"{label} component is out of bounds")
    return [float(value[0]), float(value[1])]


def _camera_rows(value, label):
    if not isinstance(value, list) or len(value) != 6:
        raise CaptureError(f"{label} must contain six float4 rows")
    rows = []
    for i, row in enumerate(value):
        if not isinstance(row, list) or len(row) != 4 or any(
                type(x) not in (float, int) or not math.isfinite(x) for x in row):
            raise CaptureError(f"{label}[{i}] must contain four finite numbers")
        rows.append([float(x) for x in row])
    return rows


def _manifest_paths(capture_dir):
    if not capture_dir.is_dir():
        raise CaptureError(f"capture directory does not exist: {capture_dir}")
    direct = sorted(capture_dir.glob("frame_*.json"))
    nested = sorted(p for session in capture_dir.iterdir() if session.is_dir()
                    for p in session.glob("frame_*.json"))
    paths = direct + nested
    if not paths:
        raise CaptureError(f"no frame_*.json manifests in {capture_dir}")
    return paths


def load_manifest(path):
    try:
        if path.stat().st_size > 64 * 1024:
            raise CaptureError(f"{path}: manifest exceeds 64 KiB")
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CaptureError(f"cannot read {path}: {exc}") from exc
    if not isinstance(manifest, dict):
        raise CaptureError(f"{path}: manifest must be a JSON object")
    version = manifest.get("version")
    if type(version) is not int or version not in (1, 2):
        raise CaptureError(f"{path}: unsupported manifest version")
    frame = _integer(manifest.get("frame_id"), "frame_id", 0, 2**64 - 1)
    if path.name != f"frame_{frame}.json":
        raise CaptureError(f"{path}: filename does not match frame_id")
    jitter = _number_pair(manifest.get("jitter"), "jitter")
    previous_jitter = _number_pair(manifest.get("previous_jitter"), "previous_jitter")
    # The raster phase the camera ROWS carry (2026-09-29; the upstream camera injector,
    # flat_camera_phase.h): same unit and sign as `jitter`, present in captures made by a
    # build that writes it. Both or neither; the replay infers it from the rows when absent.
    rows_jitter = previous_rows_jitter = None
    if ("rows_jitter" in manifest) != ("previous_rows_jitter" in manifest):
        raise CaptureError(f"{path}: rows_jitter and previous_rows_jitter come together")
    if "rows_jitter" in manifest:
        rows_jitter = _number_pair(manifest["rows_jitter"], "rows_jitter")
        previous_rows_jitter = _number_pair(manifest["previous_rows_jitter"], "previous_rows_jitter")
    if type(manifest.get("reset")) is not bool:
        raise CaptureError(f"{path}: reset must be a boolean")
    # The 3D main menu's stale-slot policy was on for this frame (2026-09-29, flags.w of the prep
    # shader). Absent in every capture made before the field, which is the policy off.
    static_scene = manifest.get("static_scene", False)
    if type(static_scene) is not bool:
        raise CaptureError(f"{path}: static_scene must be a boolean")
    if manifest.get("mode") not in ("dlss", "dlaa"):
        raise CaptureError(f"{path}: mode must be dlss or dlaa")
    for field in ("binary_version", "binary_compiled"):
        value = manifest.get(field)
        if value is not None and (not isinstance(value, str) or len(value) > 256):
            raise CaptureError(f"{path}: {field} must be a short string")
    preset = manifest.get("configured_dlss_preset")
    if preset is not None:
        _integer(preset, "configured_dlss_preset", 0, 2**32 - 1)
    rw = _integer(manifest.get("render_width"), "render_width", 1, MAX_DIMENSION)
    rh = _integer(manifest.get("render_height"), "render_height", 1, MAX_DIMENSION)
    ow = _integer(manifest.get("output_width"), "output_width", 1, MAX_DIMENSION)
    oh = _integer(manifest.get("output_height"), "output_height", 1, MAX_DIMENSION)
    route = manifest.get("route", "sdr")
    if route not in ("sdr", "hdr") or (route == "hdr" and version != 2):
        raise CaptureError(f"{path}: unsupported capture route")
    layout = HDR_TEXTURES if route == "hdr" else TEXTURES
    roi = None
    if route == "hdr":
        if (ow, oh) != (rw, rh):
            raise CaptureError(f"{path}: HDR output grid must equal render grid")
        for key, expected in HDR_PROVENANCE.items():
            if manifest.get(key) != expected:
                raise CaptureError(f"{path}: unsupported HDR {key}")
        display = (_integer(manifest.get("display_width"), "display_width", 1, rw),
                   _integer(manifest.get("display_height"), "display_height", 1, rh))
        value = manifest.get("capture_roi")
        if not isinstance(value, dict):
            raise CaptureError(f"{path}: HDR capture_roi is required")
        roi = {key: _integer(value.get(key), f"capture_roi.{key}", 0 if key in ("x", "y") else 1, MAX_DIMENSION)
               for key in ("x", "y", "width", "height")}
        if roi["x"] + roi["width"] > rw or roi["y"] + roi["height"] > rh:
            raise CaptureError(f"{path}: HDR capture_roi exceeds source grid")
        if type(manifest.get("refusal_overlay")) is not bool:
            raise CaptureError(f"{path}: HDR refusal_overlay must be a boolean")
    else:
        display = (ow, oh)
    records = manifest.get("textures")
    if not isinstance(records, list) or len(records) not in ((6,) if version == 1 else (6, 7)):
        raise CaptureError(f"{path}: expected six textures and optional slots")
    found = {}
    total_size = 0
    for record in records:
        if not isinstance(record, dict):
            raise CaptureError(f"{path}: texture record must be an object")
        name = record.get("name")
        if not isinstance(name, str) or name not in ({**layout, "slots": SLOTS} if version == 2 else layout) or name in found:
            raise CaptureError(f"{path}: unknown or duplicate texture name {name!r}")
        fmt, bpp, grid = SLOTS if name == "slots" else layout[name]
        width, height = (rw, rh) if grid == "render" else (ow, oh)
        if route == "hdr":
            width, height = roi["width"], roi["height"]
        filename = record.get("filename")
        if not isinstance(filename, str) or not re.fullmatch(r"frame_\d+_[a-z]+\.bin", filename):
            raise CaptureError(f"{path}: unsafe texture filename {filename!r}")
        if filename != f"frame_{frame}_{name}.bin":
            raise CaptureError(f"{path}: unexpected filename for {name}")
        for key, expected in (("dxgi_format", fmt), ("width", width),
                              ("height", height), ("row_stride", width * bpp),
                              ("byte_size", width * height * bpp)):
            if type(record.get(key)) is not int or record[key] != expected:
                raise CaptureError(f"{path}: {name}.{key} must equal {expected}")
        if name == "slots":
            for key, expected in (("srv_format", 16), ("srv_dimension", 4),
                                  ("most_detailed_mip", 0)):
                if record.get(key) != expected:
                    raise CaptureError(f"{path}: slots.{key} must equal {expected}")
            if record.get("mip_levels") not in (1, 2**32 - 1):
                raise CaptureError(f"{path}: slots.mip_levels must describe the single source mip")
        texture_path = path.parent / filename
        # A capture may be supplied by another machine. Never follow a path
        # outside its manifest directory, including through a symlink.
        if texture_path.resolve().parent != path.parent.resolve() or not texture_path.is_file():
            raise CaptureError(f"{path}: missing or unsafe texture {filename}")
        actual = texture_path.stat().st_size
        if actual != record["byte_size"]:
            raise CaptureError(f"{path}: {name} length {actual}, expected {record['byte_size']}")
        found[name] = texture_path
        total_size += actual
        if total_size > MAX_CAPTURE_BYTES:
            raise CaptureError(f"{path}: capture exceeds {MAX_CAPTURE_BYTES} bytes")
    if set(found) != set(layout):
        if set(found) != set(layout) | ({"slots"} if "slots" in found else set()):
            raise CaptureError(f"{path}: missing texture record")
    buffers = {}
    engine = None
    camera = previous_camera = None
    if version == 2:
        camera = _camera_rows(manifest.get("camera"), "camera")
        previous_camera = (None if manifest.get("previous_camera") is None else
                           _camera_rows(manifest["previous_camera"], "previous_camera"))
        if previous_camera is None and not manifest["reset"]:
            raise CaptureError(f"{path}: previous camera absent without reset")
        engine = manifest.get("engine")
        if not isinstance(engine, dict) or type(engine.get("complete")) is not bool:
            raise CaptureError(f"{path}: invalid engine availability")
        names = ("slots", "pool", "scene_now", "scene_previous")
        if any(type(engine.get(f"{n}_present")) is not bool for n in names):
            raise CaptureError(f"{path}: invalid engine presence flags")
        status = "complete" if engine["complete"] else "absent-or-partial"
        if engine.get("status") != status or engine["complete"] != all(
                engine[f"{n}_present"] for n in names):
            raise CaptureError(f"{path}: inconsistent engine status")
        buffer_records = manifest.get("buffers", [])
        if not isinstance(buffer_records, list) or len(buffer_records) > 3:
            raise CaptureError(f"{path}: invalid buffer records")
        for record in buffer_records:
            if not isinstance(record, dict):
                raise CaptureError(f"{path}: invalid buffer record")
            name = record.get("name")
            if name not in names[1:] or name in buffers:
                raise CaptureError(f"{path}: unknown or duplicate buffer {name!r}")
            filename = f"frame_{frame}_{name}.bin"
            if record.get("filename") != filename:
                raise CaptureError(f"{path}: unsafe buffer filename")
            size = _integer(record.get("byte_size"), f"{name}.byte_size", 1,
                            MAX_POOL_BYTES if name == "pool" else MAX_SCENE_BYTES)
            stride = _integer(record.get("stride"), f"{name}.stride", 0, 4096)
            _integer(record.get("misc_flags"), f"{name}.misc_flags", 0, 2**32 - 1)
            if name == "pool":
                first = _integer(record.get("first_element"), "pool.first_element", 0, 2**32 - 1)
                count = _integer(record.get("num_elements"), "pool.num_elements", 1, 2**32 - 1)
                if (stride == 0 or size % stride or (first + count) * stride > size or
                        record.get("srv_format") != 0 or record.get("srv_dimension") != 1):
                    raise CaptureError(f"{path}: unsupported pool view/layout")
            elif stride != 0 or size % 16 or size < 276 * 16:
                raise CaptureError(f"{path}: invalid {name} constant buffer layout")
            # The freshness stamp lives at row 276. Legacy 276-row scenes are
            # accepted only where record interpretation never reads it
            # (rc-since-rc2 review F6).
            elif not manifest["reset"] and size < 277 * 16:
                raise CaptureError(f"{path}: {name} lacks the freshness-stamp row a non-reset capture needs")
            buffer_path = path.parent / filename
            if buffer_path.resolve().parent != path.parent.resolve() or not buffer_path.is_file() or buffer_path.stat().st_size != size:
                raise CaptureError(f"{path}: missing or unsafe buffer {filename}")
            buffers[name] = (buffer_path, record)
            total_size += size
        if total_size > MAX_CAPTURE_BYTES or any(engine[f"{n}_present"] != (n in (found if n == "slots" else buffers)) for n in names):
            raise CaptureError(f"{path}: engine resource presence differs from manifest")
    return {
        "path": path, "version": version, "frame_id": frame, "jitter": jitter,
        "previous_jitter": previous_jitter, "reset": manifest["reset"],
        "rows_jitter": rows_jitter, "previous_rows_jitter": previous_rows_jitter,
        "static_scene": static_scene, "mode": manifest["mode"],
        "binary_version": manifest.get("binary_version"),
        "binary_compiled": manifest.get("binary_compiled"),
        "configured_dlss_preset": preset,
        "render_width": rw, "render_height": rh,
        "output_width": ow, "output_height": oh, "display_width": display[0], "display_height": display[1],
        "route": route, "capture_roi": roi, "layout": layout, "textures": found,
        "refusal_overlay": manifest.get("refusal_overlay", False),
        "total_bytes": total_size, "buffers": buffers, "engine": engine,
        "camera": camera, "previous_camera": previous_camera,
    }


def _open_texture(meta, name):
    width = meta["render_width"] if meta["layout"][name][2] == "render" else meta["output_width"]
    height = meta["render_height"] if meta["layout"][name][2] == "render" else meta["output_height"]
    if meta["capture_roi"] and meta["route"] == "hdr":
        width, height = meta["capture_roi"]["width"], meta["capture_roi"]["height"]
    channels = meta["layout"][name][1]
    shape = (height, width) if channels == 1 else (height, width, channels)
    return np.memmap(meta["textures"][name], mode="r", dtype=np.uint8, shape=shape)


def _mask_geometry(meta):
    # Mirror flat_mono_shader_source.h's finish(): float32 pixel-center UV,
    # jitter in render pixels, floor(... - .5), then four clamped Loads.
    rw, rh = meta["render_width"], meta["render_height"]
    ow, oh = meta["output_width"], meta["output_height"]
    jitter = np.asarray(meta["jitter"], dtype=np.float32)
    x = np.arange(ow, dtype=np.float32)
    uv_x = (x + np.float32(.5)) / np.float32(ow)
    raster_x = uv_x + jitter[0] / np.float32(rw)
    qx = np.floor(raster_x * np.float32(rw) - np.float32(.5)).astype(np.int32)
    x0 = np.clip(qx, 0, rw - 1)
    x1 = np.clip(qx + 1, 0, rw - 1)
    def y_indices(y):
        uv_y = (np.float32(y) + np.float32(.5)) / np.float32(oh)
        raster_y = uv_y + jitter[1] / np.float32(rh)
        qy = int(np.floor(raster_y * np.float32(rh) - np.float32(.5)))
        return min(max(qy, 0), rh - 1), min(max(qy + 1, 0), rh - 1)
    return x0, x1, y_indices


def _mask_row(rejection, geometry, y):
    x0, x1, y_indices = geometry
    y0, y1 = y_indices(y)
    return ((rejection[y0, x0] != 0) | (rejection[y0, x1] != 0) |
            (rejection[y1, x0] != 0) | (rejection[y1, x1] != 0))


def _hdr_rgb(meta, name):
    pixels = _open_texture(meta, name)
    if name == "raw":
        return pixels.view("<f2").reshape((*pixels.shape[:2], 4))[:, :, :3].astype(np.float32)
    packed = pixels.view("<u4").reshape(pixels.shape[:2])
    def channel(shift, mantissa_bits):
        bits = (packed >> shift) & ((1 << (mantissa_bits + 5)) - 1)
        exponent = (bits >> mantissa_bits).astype(np.int32)
        mantissa = (bits & ((1 << mantissa_bits) - 1)).astype(np.float32)
        value = np.ldexp(np.where(exponent == 0, mantissa / (1 << mantissa_bits),
                                  1 + mantissa / (1 << mantissa_bits)),
                         np.where(exponent == 0, -14, exponent - 15))
        return np.where(exponent == 31, np.where(mantissa == 0, np.inf, np.nan), value)
    return np.stack((channel(0, 6), channel(11, 6), channel(22, 5)), axis=2)


def _hdr_mask_row(meta, rejection, y):
    roi = meta["capture_roi"]
    x = np.arange(roi["width"], dtype=np.float32) + roi["x"]
    gy = y + roi["y"]
    jx, jy = meta["jitter"]
    qx = np.floor(x + np.float32(jx)).astype(np.int32)
    qy = int(math.floor(gy + jy))
    valid_x = (qx >= roi["x"]) & (qx + 1 < roi["x"] + roi["width"])
    valid_y = qy >= roi["y"] and qy + 1 < roi["y"] + roi["height"]
    x0 = np.clip(qx - roi["x"], 0, roi["width"] - 1)
    x1 = np.clip(qx + 1 - roi["x"], 0, roi["width"] - 1)
    y0 = min(max(qy - roi["y"], 0), roi["height"] - 1)
    y1 = min(max(qy + 1 - roi["y"], 0), roi["height"] - 1)
    mask = ((rejection[y0, x0] != 0) | (rejection[y0, x1] != 0) |
            (rejection[y1, x0] != 0) | (rejection[y1, x1] != 0))
    return mask, valid_x & valid_y


def _analyze_hdr(meta, rois=None, unjitter=True, static_scene=None):
    rejection = _open_texture(meta, "rejection")
    raw, final = _hdr_rgb(meta, "raw"), _hdr_rgb(meta, "final")
    groups = {label: {"pixels": 0, "different_pixels": 0,
                      "max_channel_difference": 0.0, "sum_channel_difference": 0.0}
              for label in ("rejected", "accepted")}
    excluded = 0
    for y in range(meta["capture_roi"]["height"]):
        mask, valid = _hdr_mask_row(meta, rejection, y)
        excluded += int(np.count_nonzero(~valid))
        diff = np.abs(raw[y] - final[y])
        for label, selector in (("rejected", mask & valid), ("accepted", ~mask & valid)):
            count = int(np.count_nonzero(selector))
            group = groups[label]
            group["pixels"] += count
            if count:
                selected = diff[selector]
                group["different_pixels"] += int(np.count_nonzero(np.any(selected != 0, axis=1)))
                group["max_channel_difference"] = max(group["max_channel_difference"], float(np.nanmax(selected)))
                group["sum_channel_difference"] += float(np.nansum(selected, dtype=np.float64))
    for group in groups.values():
        count = group.pop("sum_channel_difference")
        group["mean_channel_difference"] = count / (group["pixels"] * 3) if group["pixels"] else None
    total = sum(group["pixels"] for group in groups.values())
    from flat_pixels_engine import analyze as analyze_engine
    roi = meta["capture_roi"]
    requested = rois if rois else [("captured", (roi["x"], roi["y"], roi["width"], roi["height"]))]
    try:
        engine_analysis = analyze_engine(meta, requested, unjitter, static_scene)
    except ValueError as exc:
        raise CaptureError(str(exc)) from exc
    return {"manifest": str(meta["path"]), "frame_id": meta["frame_id"], "mode": meta["mode"],
            "route": "hdr", "reset": meta["reset"], "capture_roi": meta["capture_roi"],
            "render_size": [meta["render_width"], meta["render_height"]],
            "display_size": [meta["display_width"], meta["display_height"]],
            "color_provenance": HDR_PROVENANCE["color_provenance"],
            "raw_provenance": HDR_PROVENANCE["raw_provenance"],
            "final_provenance": HDR_PROVENANCE["final_provenance"],
            "binary_version": meta["binary_version"], "binary_compiled": meta["binary_compiled"],
            "configured_dlss_preset": meta["configured_dlss_preset"],
            "jitter": meta["jitter"], "previous_jitter": meta["previous_jitter"],
            "input_rejection_pixels": int(np.count_nonzero(rejection)),
            "output_rejection_pixels": groups["rejected"]["pixels"],
            "output_rejection_percent": 100 * groups["rejected"]["pixels"] / total if total else None,
            "edge_excluded_pixels": excluded,
            "difference_channels": "linear HDR RGB radiance, absolute decoded float difference",
            "difference_note": ("refusal overlay painted into final; raw/final differences are not ordinary finish comparisons"
                                if meta["refusal_overlay"] else
                                "raw fp16 versus finished R11G11B10 includes format quantization; rejected pixels display sampled input rather than backend raw"),
            "refusal_overlay": meta["refusal_overlay"],
            "engine_analysis": engine_analysis,
            "rejected": groups["rejected"], "accepted": groups["accepted"]}


def analyze(meta, rois=None, unjitter=True, static_scene=None):
    """`static_scene` None replays the frame as the capture declares its stale-slot policy;
    True/False forces it (flat_pixels_engine.analyze). The rejection statistics below are
    what the GPU produced and do not change with it."""
    if np is None:
        raise CaptureError("NumPy is required for capture analysis")
    if meta["route"] == "hdr":
        return _analyze_hdr(meta, rois, unjitter, static_scene)
    rejection = _open_texture(meta, "rejection")
    raw = _open_texture(meta, "raw")
    final = _open_texture(meta, "final")
    geometry = _mask_geometry(meta)
    groups = {label: {"pixels": 0, "different_pixels": 0,
                      "max_channel_difference": 0, "sum_channel_difference": 0}
              for label in ("rejected", "accepted")}
    for y in range(meta["output_height"]):
        mask = _mask_row(rejection, geometry, y)
        diff = np.abs(raw[y].astype(np.int16) - final[y].astype(np.int16))
        different = np.any(diff != 0, axis=1)
        for label, selector in (("rejected", mask), ("accepted", ~mask)):
            count = int(np.count_nonzero(selector))
            group = groups[label]
            group["pixels"] += count
            if count:
                selected = diff[selector]
                group["different_pixels"] += int(np.count_nonzero(different[selector]))
                group["max_channel_difference"] = max(group["max_channel_difference"], int(selected.max()))
                group["sum_channel_difference"] += int(selected.sum(dtype=np.int64))
    for group in groups.values():
        count = group.pop("sum_channel_difference")
        group["mean_channel_difference"] = count / (group["pixels"] * 4) if group["pixels"] else None
    rejected = groups["rejected"]["pixels"]
    total = meta["output_width"] * meta["output_height"]
    result = {
        "manifest": str(meta["path"]), "frame_id": meta["frame_id"],
        "mode": meta["mode"], "reset": meta["reset"],
        "binary_version": meta["binary_version"],
        "binary_compiled": meta["binary_compiled"],
        "configured_dlss_preset": meta["configured_dlss_preset"],
        "jitter": meta["jitter"], "previous_jitter": meta["previous_jitter"],
        "render_size": [meta["render_width"], meta["render_height"]],
        "output_size": [meta["output_width"], meta["output_height"]],
        "input_rejection_pixels": int(np.count_nonzero(rejection)),
        "input_rejection_percent": 100 * int(np.count_nonzero(rejection)) / rejection.size,
        "output_rejection_pixels": rejected,
        "output_rejection_percent": 100 * rejected / total,
        "difference_channels": "RGBA8, absolute byte difference",
        "rejected": groups["rejected"], "accepted": groups["accepted"],
    }
    if meta["version"] == 2:
        from flat_pixels_engine import analyze as analyze_engine
        requested = rois if rois else [("full", (0, 0, meta["render_width"], meta["render_height"]))]
        try:
            result["engine_analysis"] = analyze_engine(meta, requested, unjitter, static_scene)
        except ValueError as exc:
            raise CaptureError(str(exc)) from exc
    return result


STABILITY_MAX_GAP = 8      # frames between the two live samples of a pair
STABILITY_VISIBLE = 4      # a change of 4/255 or more in any colour channel is a visible change


def _preview_hdr(rgb):
    # Display aid only; statistics and bin files retain linear radiance above 1.
    linear = np.nan_to_num(rgb.astype(np.float32), nan=0.0, posinf=65024.0)
    mapped = np.maximum(linear, 0) / (1 + np.maximum(linear, 0))
    return np.rint(np.power(mapped, 1 / 2.2) * 255).clip(0, 255).astype(np.uint8)


def _stability_pair(earlier, later):
    """Change between the FINAL images of two live frames, split by the later frame's
    rejection footprint (the pixels the resolver shows as the raw jittered colour)."""
    hdr = later["route"] == "hdr"
    first, second = ((_hdr_rgb(earlier, "final"), _hdr_rgb(later, "final")) if hdr else
                     (_open_texture(earlier, "final"), _open_texture(later, "final")))
    rejection = _open_texture(later, "rejection")
    geometry = None if hdr else _mask_geometry(later)
    groups = {"rejected": [0, 0, 0], "accepted": [0, 0, 0]}   # pixels, sum of change, visible pixels
    linear_change = {"rejected": [0.0, 0.0], "accepted": [0.0, 0.0]}
    height = later["capture_roi"]["height"] if hdr else later["output_height"]
    for y in range(height):
        mask, valid = _hdr_mask_row(later, rejection, y) if hdr else (_mask_row(rejection, geometry, y), None)
        a, b = (_preview_hdr(first[y]), _preview_hdr(second[y])) if hdr else (first[y], second[y])
        change = np.abs(a[:, :3].astype(np.int16) - b[:, :3].astype(np.int16)).max(axis=1)
        radiance_change = np.abs(first[y] - second[y]).max(axis=1) if hdr else None
        for label, selector in (("rejected", mask), ("accepted", ~mask)):
            if valid is not None:
                selector = selector & valid
            count = int(np.count_nonzero(selector))
            if count:
                selected = change[selector]
                group = groups[label]
                group[0] += count
                group[1] += int(selected.sum(dtype=np.int64))
                group[2] += int(np.count_nonzero(selected >= STABILITY_VISIBLE))
                if hdr:
                    changes = radiance_change[selector]
                    linear_change[label][0] += float(np.nansum(changes, dtype=np.float64))
                    linear_change[label][1] = max(linear_change[label][1], float(np.nanmax(changes)))
    total = [sum(group[i] for group in groups.values()) for i in range(3)]
    report = {"frames": [earlier["frame_id"], later["frame_id"]],
              "frame_gap": later["frame_id"] - earlier["frame_id"],
              "route": later["route"]}
    for label, group in (("rejected", groups["rejected"]), ("accepted", groups["accepted"]), ("all", total)):
        report[label] = {"pixels": group[0],
                         "mean_change_of_255": group[1] / group[0] if group[0] else None,
                         "visible_change_percent": 100 * group[2] / group[0] if group[0] else None}
        if hdr:
            rad = ([sum(linear_change[k][0] for k in ("rejected", "accepted")),
                    max(linear_change[k][1] for k in ("rejected", "accepted"))] if label == "all"
                   else linear_change[label])
            report[label]["mean_linear_radiance_change"] = rad[0] / group[0] if group[0] else None
            report[label]["max_linear_radiance_change"] = rad[1]
    if hdr:
        report["preview_metric"] = "Reinhard tone map then gamma 1/2.2, RGB 0..255; linear radiance change also reported"
    return report


def stability(manifests):
    """The shimmer proxy (2026-09-29): how much the displayed image changes between two live
    frames, inside and outside the rejection footprint. Pairs each live (non-reset) frame of a
    session with the next live one at most STABILITY_MAX_GAP frames later, on the same grids.
    Only meaningful for a still scene, the 3D menu: motion inflates it, so compare the same
    ship before and after a change, not across scenes."""
    report = {"pairs": [], "status": "ok",
              "note": ("mean and share of the final image's change between two live frames, "
                       "split by the later frame's rejection footprint; a still scene should "
                       "change little outside it. Motion inflates it: only for the menu, and "
                       "only against the same ship's earlier capture.")}
    sessions = {}
    for meta in manifests:
        if not meta["reset"]:
            sessions.setdefault(meta["path"].parent, []).append(meta)
    for metas in sessions.values():
        metas.sort(key=lambda m: m["frame_id"])
        for earlier, later in zip(metas, metas[1:]):
            gap = later["frame_id"] - earlier["frame_id"]
            same_grids = all(earlier[k] == later[k] for k in
                             ("render_width", "render_height", "output_width", "output_height"))
            same_grids = same_grids and earlier["route"] == later["route"] and earlier["capture_roi"] == later["capture_roi"]
            if 0 < gap <= STABILITY_MAX_GAP and same_grids:
                report["pairs"].append(_stability_pair(earlier, later))
    if not report["pairs"]:
        report["status"] = (f"needs two live frames of one session at most {STABILITY_MAX_GAP} frames "
                            "apart: a reset frame is not one")
    report["consecutive_pair_status"] = ("present" if any(p["frame_gap"] == 1 for p in report["pairs"])
                                         else "absent: no two adjacent live frames with the same capture ROI")
    return report


def _png_chunk(handle, kind, payload):
    handle.write(struct.pack(">I", len(payload)))
    handle.write(kind)
    handle.write(payload)
    handle.write(struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))


def write_png(path, width, height, rows):
    """Write RGBA rows with PNG filter 0; no Pillow dependency."""
    with path.open("wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        _png_chunk(handle, b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        compressor = zlib.compressobj(level=4)
        pending = bytearray()
        for row in rows:
            if len(row) != width * 4:
                raise CaptureError(f"PNG row length differs from {width * 4}")
            pending.extend(compressor.compress(b"\0" + row))
            if len(pending) >= 1024 * 1024:
                _png_chunk(handle, b"IDAT", bytes(pending))
                pending.clear()
        pending.extend(compressor.flush())
        if pending:
            _png_chunk(handle, b"IDAT", bytes(pending))
        _png_chunk(handle, b"IEND", b"")


def write_previews(meta, directory):
    if meta["route"] == "hdr":
        rejection = _open_texture(meta, "rejection")
        decoded = {name: _hdr_rgb(meta, name) for name in ("color", "raw", "final")}
        width, height = meta["capture_roi"]["width"], meta["capture_roi"]["height"]
        for name, rgb in decoded.items():
            def rows(image=rgb):
                for y in range(height):
                    row = np.empty((width, 4), dtype=np.uint8)
                    row[:, :3] = _preview_hdr(image[y])
                    row[:, 3] = 255
                    yield row.tobytes()
            write_png(directory / f"{name}.png", width, height, rows())
        def overlay_rows():
            for y in range(height):
                row = np.empty((width, 4), dtype=np.uint8)
                row[:, :3] = _preview_hdr(decoded["final"][y])
                mask, valid = _hdr_mask_row(meta, rejection, y)
                selected = mask & valid
                row[selected, 0] = ((row[selected, 0].astype(np.uint16) + 255) // 2).astype(np.uint8)
                row[selected, 1:3] //= 2
                row[:, 3] = 255
                yield row.tobytes()
        def difference_rows():
            for y in range(height):
                diff = np.abs(decoded["raw"][y] - decoded["final"][y])
                row = np.empty((width, 4), dtype=np.uint8)
                row[:, :3] = _preview_hdr(diff * 8)
                row[:, 3] = 255
                yield row.tobytes()
        write_png(directory / "rejection_overlay.png", width, height, overlay_rows())
        write_png(directory / "raw_final_diff_x8.png", width, height, difference_rows())
        return
    rejection = _open_texture(meta, "rejection")
    raw = _open_texture(meta, "raw")
    final = _open_texture(meta, "final")
    geometry = _mask_geometry(meta)
    width = meta["output_width"]
    for name in ("color", "raw", "final"):
        texture = _open_texture(meta, name)
        texture_width = meta["render_width"] if name == "color" else width
        texture_height = meta["render_height"] if name == "color" else meta["output_height"]
        def opaque_rows():
            for y in range(texture_height):
                row = np.array(texture[y], copy=True)
                row[:, 3] = 255  # Captured alpha may be undefined; stats keep its bytes.
                yield row.tobytes()
        write_png(directory / f"{name}.png", texture_width, texture_height,
                  opaque_rows())

    def overlay_rows():
        for y in range(meta["output_height"]):
            row = np.array(final[y], copy=True)
            mask = _mask_row(rejection, geometry, y)
            if np.any(mask):
                rgb = row[mask, :3].astype(np.uint16)
                row[mask, 0] = ((rgb[:, 0] + 255) // 2).astype(np.uint8)
                row[mask, 1] = (rgb[:, 1] // 2).astype(np.uint8)
                row[mask, 2] = (rgb[:, 2] // 2).astype(np.uint8)
            row[:, 3] = 255
            yield row.tobytes()

    def difference_rows():
        for y in range(meta["output_height"]):
            diff = np.abs(raw[y].astype(np.int16) - final[y].astype(np.int16))
            rgb = np.maximum(diff[:, :3], diff[:, 3:4])
            row = np.empty((width, 4), dtype=np.uint8)
            row[:, :3] = np.minimum(rgb * 8, 255).astype(np.uint8)
            row[:, 3] = 255
            yield row.tobytes()

    write_png(directory / "rejection_overlay.png", width, meta["output_height"], overlay_rows())
    write_png(directory / "raw_final_diff_x8.png", width, meta["output_height"], difference_rows())


def run(capture_dir, output=None, dry_run=False, rois=None, static_scene=None):
    if np is None:
        raise CaptureError("NumPy is required; use the bundled Codex Python or install NumPy")
    manifests = [load_manifest(p) for p in _manifest_paths(capture_dir)]
    results = [analyze(m, rois, static_scene=static_scene) for m in manifests]
    summary = {"frames": results, "stability": stability(manifests),
               "preview_scale": ("HDR: decoded linear RGB radiance, difference x8 then Reinhard/gamma preview; "
                                 "SDR: absolute RGBA difference x8, alpha difference copied into RGB")}
    if output is not None:
        summary["output"] = str(output)
        summary["dry_run"] = dry_run
        if not dry_run:
            output.mkdir(parents=True, exist_ok=True)
            for meta in manifests:
                # Include session name to avoid collisions in a parent capture.
                frame_dir = output / meta["path"].parent.name / meta["path"].stem
                frame_dir.mkdir(parents=True, exist_ok=True)
                write_previews(meta, frame_dir)
            (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def verify_fixture(capture_dir):
    """Check the real WARP writer's bytes through this reader, without writes."""
    pointer = capture_dir / "current_fixture.txt"
    try:
        if pointer.stat().st_size > 512:
            raise CaptureError("fixture pointer exceeds 512 bytes")
        relative = pointer.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError) as exc:
        raise CaptureError(f"current fixture pointer unavailable: {exc}") from exc
    if not re.fullmatch(r"flat_pixels/[A-Za-z0-9_-]+/frame_7\.json", relative):
        raise CaptureError(f"unsafe or malformed fixture pointer: {relative!r}")
    path = capture_dir.joinpath(*relative.split("/"))
    if path.resolve().parent.parent.parent != capture_dir.resolve():
        raise CaptureError("fixture pointer resolves outside capture root")
    meta = load_manifest(path)
    # A live frame (2026-09-29): the writer's first sample is never a reset frame, and it
    # records the raster phases and the phases the camera rows carry.
    expected_metadata = {
        "frame_id": 7, "mode": "dlss", "reset": False,
        "configured_dlss_preset": 11, "jitter": [0.25, -0.375],
        "previous_jitter": [-0.125, 0.5],
        "rows_jitter": [0.25, -0.375], "previous_rows_jitter": [-0.125, 0.5],
        "render_width": 17, "render_height": 3,
        "output_width": 17, "output_height": 3,
    }
    for field, expected in expected_metadata.items():
        if meta[field] != expected:
            raise CaptureError(f"fixture {field}: {meta[field]!r}, expected {expected!r}")
    if not meta["binary_version"] or not meta["binary_compiled"]:
        raise CaptureError("fixture lacks binary identity metadata")
    x = np.arange(1, 18, dtype=np.uint8)[None, :]
    y = np.arange(1, 4, dtype=np.uint8)[:, None]
    for name in ("color", "final"):
        pixels = _open_texture(meta, name)
        if not (np.all(pixels[:, :, 0] == x) and
                np.all(pixels[:, :, 1] == y) and
                np.all(pixels[:, :, 2] == 0) and
                np.all(pixels[:, :, 3] == 255)):
            raise CaptureError(f"fixture {name} pixels differ from the WARP pattern")
    raw = _open_texture(meta, "raw")
    if not (np.all(raw[:, :, 0] == 0) and np.all(raw[:, :, 1] == 255) and
            np.all(raw[:, :, 2] == 0) and np.all(raw[:, :, 3] == 255)):
        raise CaptureError("fixture raw pixels differ from the WARP pattern")
    rejection = _open_texture(meta, "rejection")
    if not np.all(rejection == 255):
        raise CaptureError("fixture rejection pixels differ from the WARP pattern")
    motion = _open_texture(meta, "motion")
    if not np.all(motion == 0):
        raise CaptureError("fixture motion bytes differ from the WARP pattern")
    depth = np.fromfile(meta["textures"]["depth"], dtype="<f4").reshape((3, 17))
    expected_depth = np.asarray([.01, .02, .03], dtype=np.float32)[:, None]
    if not np.allclose(depth, expected_depth, rtol=0, atol=1e-7):
        raise CaptureError("fixture depth values differ from the WARP pattern")
    if meta["version"] == 2:
        from flat_pixels_engine import record_kind
        if not meta["engine"]["complete"]:
            raise CaptureError("fixture engine inputs are incomplete")
        slots = np.fromfile(meta["textures"]["slots"], dtype="<f4").reshape((3, 17, 2))
        expected_codes = np.asarray([-1 if i % 4 == 0 else 2 * (i % 4) - 1
                                     for i in range(17)], dtype=np.float32)
        if (not np.all(slots[:, :, 0] == expected_codes) or
                not np.allclose(slots[:, :, 1], expected_depth, rtol=0, atol=1e-7)):
            raise CaptureError("fixture slot bytes differ from the WARP pattern")
        pool_path, view = meta["buffers"]["pool"]
        pool = np.fromfile(pool_path, dtype="<u4").reshape((-1, 21, 4))
        scene_now = np.fromfile(meta["buffers"]["scene_now"][0], dtype="<f4").reshape((-1, 4))
        token = np.float32(scene_now[276, 0]).view(np.uint32)
        if (view["first_element"] != 1 or view["num_elements"] != 3 or
                int(pool[0, 0, 0]) != 0x12345678 or
                [record_kind(pool[i], token) for i in (1, 2, 3)] !=
                ["unmarked", "joined", "masked"]):
            raise CaptureError("fixture pool view offset or marker bytes differ")
        for name, rows in (("scene_now", meta["camera"]),
                           ("scene_previous", meta["previous_camera"])):
            scene = np.fromfile(meta["buffers"][name][0], dtype="<f4").reshape((-1, 4))
            if not np.array_equal(scene[270:276], np.asarray(rows, dtype=np.float32)):
                raise CaptureError(f"fixture {name} camera rows differ")
    result = analyze(meta)
    if (result["output_rejection_pixels"] != 51 or
            result["output_rejection_percent"] != 100 or
            result["rejected"]["different_pixels"] != 51 or
            result["rejected"]["max_channel_difference"] != 254 or
            result["rejected"]["mean_channel_difference"] != 65.5 or
            result["accepted"]["pixels"] != 0):
        raise CaptureError("fixture output-mask or raw/final difference statistics drifted")
    result["fixture_verified"] = True
    return result


def self_test():
    if np is None:
        raise CaptureError("NumPy is required for --self-test")
    from flat_pixels_engine import self_test as engine_self_test
    engine_self_test()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        session = root / "session"
        session.mkdir()
        rw, rh, ow, oh = 3, 2, 5, 3
        contents = {
            "color": bytes(range(rw * rh * 4)),
            "depth": b"\0" * (rw * rh * 4),
            "motion": b"\0" * (rw * rh * 4),
            "rejection": bytes([1, 0, 0, 0, 0, 2]),
            "raw": bytes([17] * (ow * oh * 4)),
            "final": bytes([17] * (ow * oh * 4)),
        }
        records = []
        for name, (fmt, bpp, grid) in TEXTURES.items():
            w, h = (rw, rh) if grid == "render" else (ow, oh)
            filename = f"frame_7_{name}.bin"
            (session / filename).write_bytes(contents[name])
            records.append({"name": name, "filename": filename, "dxgi_format": fmt,
                            "width": w, "height": h, "row_stride": w * bpp,
                            "byte_size": w * h * bpp})
        manifest = {"version": 1, "frame_id": 7, "jitter": [.25, -.25],
                    "previous_jitter": [0, 0], "reset": False, "mode": "dlss",
                    "render_width": rw, "render_height": rh,
                    "output_width": ow, "output_height": oh, "textures": records}
        manifest_path = session / "frame_7.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        meta = load_manifest(manifest_path)
        rejection = _open_texture(meta, "rejection")
        geometry = _mask_geometry(meta)
        # Independent scalar shader formula for mixed grids and fractional jitter.
        expected = []
        for y in range(oh):
            row = []
            for x in range(ow):
                qx = math.floor((((x + .5) / ow) + .25 / rw) * rw - .5)
                qy = math.floor((((y + .5) / oh) - .25 / rh) * rh - .5)
                row.append(any(rejection[min(max(qy + dy, 0), rh - 1),
                                         min(max(qx + dx, 0), rw - 1)] > 0
                               for dy in (0, 1) for dx in (0, 1)))
            expected.append(row)
        actual = [_mask_row(rejection, geometry, y).tolist() for y in range(oh)]
        assert actual == expected and actual[0][0] and actual[-1][-1]
        assert any(not bit for row in actual for bit in row)
        assert analyze(meta)["accepted"]["different_pixels"] == 0
        rejected_index = next(y * ow + x for y, row in enumerate(actual)
                              for x, bit in enumerate(row) if bit)
        accepted_index = next(y * ow + x for y, row in enumerate(actual)
                              for x, bit in enumerate(row) if not bit)
        altered = bytearray(contents["final"])
        altered[rejected_index * 4] = 100
        altered[accepted_index * 4 + 1] = 70
        final_path = session / "frame_7_final.bin"
        final_path.write_bytes(altered)
        statistics = analyze(meta)
        assert statistics["rejected"]["different_pixels"] == 1
        assert statistics["accepted"]["different_pixels"] == 1
        assert statistics["rejected"]["max_channel_difference"] == 83
        assert statistics["accepted"]["max_channel_difference"] == 53
        final_path.write_bytes(contents["final"])
        # Manifest and length errors fail before creating any output.
        broken = json.loads(json.dumps(manifest))
        del broken["jitter"]
        manifest_path.write_text(json.dumps(broken), encoding="utf-8")
        try:
            load_manifest(manifest_path)
            raise AssertionError("missing jitter accepted")
        except CaptureError:
            pass
        broken = json.loads(json.dumps(manifest))
        broken["textures"][0]["filename"] = "../escape.bin"
        manifest_path.write_text(json.dumps(broken), encoding="utf-8")
        try:
            load_manifest(manifest_path)
            raise AssertionError("unsafe path accepted")
        except CaptureError:
            pass
        broken = json.loads(json.dumps(manifest))
        broken["textures"][0]["dxgi_format"] = 87
        manifest_path.write_text(json.dumps(broken), encoding="utf-8")
        try:
            load_manifest(manifest_path)
            raise AssertionError("wrong DXGI format accepted")
        except CaptureError:
            pass
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        color = session / "frame_7_color.bin"
        color.write_bytes(contents["color"][:-1])
        try:
            load_manifest(manifest_path)
            raise AssertionError("truncated texture accepted")
        except CaptureError:
            pass
        color.write_bytes(contents["color"])
        before = {p.name: p.read_bytes() for p in session.iterdir()}
        output = root / "previews"
        run(session, output, dry_run=True)
        assert not output.exists()
        assert before == {p.name: p.read_bytes() for p in session.iterdir()}
        run(session, output)
        png = output / "session" / "frame_7" / "rejection_overlay.png"
        data = png.read_bytes()
        assert data.startswith(b"\x89PNG\r\n\x1a\n")
        offset, compressed = 8, bytearray()
        while offset < len(data):
            length = struct.unpack_from(">I", data, offset)[0]
            kind = data[offset + 4:offset + 8]
            payload = data[offset + 8:offset + 8 + length]
            assert zlib.crc32(kind + payload) & 0xffffffff == struct.unpack_from(">I", data, offset + 8 + length)[0]
            if kind == b"IDAT":
                compressed.extend(payload)
            offset += 12 + length
        assert len(zlib.decompress(compressed)) == oh * (1 + ow * 4)
        raw_png = (output / "session" / "frame_7" / "raw.png").read_bytes()
        offset, compressed = 8, bytearray()
        while offset < len(raw_png):
            length = struct.unpack_from(">I", raw_png, offset)[0]
            if raw_png[offset + 4:offset + 8] == b"IDAT":
                compressed.extend(raw_png[offset + 8:offset + 8 + length])
            offset += 12 + length
        scanlines = zlib.decompress(compressed)
        assert all(scanlines[y * (1 + ow * 4) + 1 + x * 4 + 3] == 255
                   for y in range(oh) for x in range(ow))
        assert before == {p.name: p.read_bytes() for p in session.iterdir()}
        # Version 2 can describe a partial engine snapshot. The prep shader
        # then falls back to the camera term; missing inputs need no dummy files.
        v2_session = root / "v2_session"
        v2_session.mkdir()
        for name, content in contents.items():
            (v2_session / f"frame_7_{name}.bin").write_bytes(content)
        v2 = dict(manifest)
        v2["version"] = 2
        v2["camera"] = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1],
                        [0, 0, .025, 0], [0, 0, 1, 0], [1, 0, 0, 0]]
        v2["previous_camera"] = v2["camera"]
        v2["engine"] = {"complete": False, "status": "absent-or-partial",
                        "slots_present": False, "pool_present": False,
                        "scene_now_present": False, "scene_previous_present": False}
        v2["buffers"] = []
        v2_path = v2_session / "frame_7.json"
        v2_path.write_text(json.dumps(v2), encoding="utf-8")
        v2_meta = load_manifest(v2_path)
        assert v2_meta["version"] == 2
        diagnostics = analyze(v2_meta, [("sample", (0, 0, rw, rh))])["engine_analysis"]
        assert diagnostics["engine_complete"] is False
        assert diagnostics["rois"][0]["sampled_pixels"] == rw * rh
        assert not any(p.suffix == ".png" for p in v2_session.iterdir())
        v2_bad = json.loads(json.dumps(v2))
        v2_bad["engine"]["complete"] = True
        v2_path.write_text(json.dumps(v2_bad), encoding="utf-8")
        try:
            load_manifest(v2_path)
            raise AssertionError("inconsistent engine availability accepted")
        except CaptureError:
            pass
        # A tiny complete capture exercises actual branch selection, view
        # offset, stale depth, malformed slot, and masked record handling.
        from flat_pixels_engine import marker_hash_stamped, record_kind
        complete = json.loads(json.dumps(v2))
        complete["reset"] = False
        complete["engine"] = {"complete": True, "status": "complete",
                              "slots_present": True, "pool_present": True,
                              "scene_now_present": True, "scene_previous_present": True}
        (v2_session / "frame_7_depth.bin").write_bytes(np.full((rh, rw), .5, dtype="<f4").tobytes())
        slots = np.asarray([[[1, .5], [3, .5], [5, .5]],
                            [[2, .5], [3, .4], [-1, .5]]], dtype="<f4")
        (v2_session / "frame_7_slots.bin").write_bytes(slots.tobytes())
        complete["textures"].append({"name": "slots", "filename": "frame_7_slots.bin",
                                     "dxgi_format": 16, "width": rw, "height": rh,
                                     "row_stride": rw * 8, "byte_size": rw * rh * 8,
                                     "srv_format": 16, "srv_dimension": 4,
                                     "most_detailed_mip": 0, "mip_levels": 1})
        # The freshness stamp the prep shader's EN[276].x carries: the markers
        # fold it in and certify only at that frame.
        stamp = 77
        pool = np.zeros((4, 21, 4), dtype="<u4")
        fbits = np.asarray(1, dtype="<f4").view("<u4").item()
        pool[0, 0, 0] = 0x12345678
        for index in (2, 3):
            pool[index, 0, 1] = pool[index, 19, 1] = fbits
            pool[index, 0, 2:4] = pool[index, 19, 2:4] = [0x7FFF7FFF, 0xFFFE7FFF]
            pool[index, 18, 0] = (0x7FC0ED01 if index == 2 else 0x7FC0ED02) ^ marker_hash_stamped(pool[index], stamp)
        assert [record_kind(pool[i], stamp) for i in (1, 2, 3)] == ["unmarked", "joined", "masked"]
        (v2_session / "frame_7_pool.bin").write_bytes(pool.tobytes())
        scene = np.zeros((277, 4), dtype="<f4")
        scene[270:276] = np.asarray(complete["camera"], dtype="<f4")
        scene[276, 0] = np.uint32(stamp).view("<f4")
        for name in ("scene_now", "scene_previous"):
            (v2_session / f"frame_7_{name}.bin").write_bytes(scene.tobytes())
        complete["buffers"] = [
            {"name": "pool", "filename": "frame_7_pool.bin", "byte_size": pool.nbytes,
             "stride": 336, "misc_flags": 64, "srv_format": 0, "srv_dimension": 1,
             "first_element": 1, "num_elements": 3},
            *({"name": n, "filename": f"frame_7_{n}.bin", "byte_size": scene.nbytes,
               "stride": 0, "misc_flags": 0} for n in ("scene_now", "scene_previous"))]
        v2_path.write_text(json.dumps(complete), encoding="utf-8")
        complete_meta = load_manifest(v2_path)
        branches = analyze(complete_meta, [("whole", (0, 0, rw, rh))])["engine_analysis"]["rois"][0]["branch_counts"]
        assert branches["engine_joined"] == 1 and branches["camera_unmarked_record"] == 1
        assert branches["rejected_masked_record"] == 1 and branches["rejected_corrupt_code"] == 1
        assert branches["rejected_stale_or_depth"] == 1 and branches["camera_no_slot"] == 1
        assert "camera_stale_static" not in branches and complete_meta["static_scene"] is False
        # The 3D main menu's stale-slot policy (2026-09-29): the twin of flags.w in the prep shader.
        # The stale pixel (1,1) takes the camera term and nothing else moves; the capture's own flag
        # drives it, a replay override can force it either way, and a capture without the field is off.
        whole = [("whole", (0, 0, rw, rh))]
        static_manifest = json.loads(json.dumps(complete))
        static_manifest["static_scene"] = True
        v2_path.write_text(json.dumps(static_manifest), encoding="utf-8")
        static_meta = load_manifest(v2_path)
        assert static_meta["static_scene"] is True
        static_engine = analyze(static_meta, whole)["engine_analysis"]
        static_branches = static_engine["rois"][0]["branch_counts"]
        assert static_branches.get("rejected_stale_or_depth", 0) == 0 and static_branches["camera_stale_static"] == 1
        for kept in ("engine_joined", "camera_unmarked_record", "rejected_masked_record",
                     "rejected_corrupt_code", "camera_no_slot"):
            assert static_branches[kept] == 1, (kept, static_branches)
        assert static_engine["static_scene"] == {"declared": True, "replayed_as": True}
        # The negative control: forced off, the same capture refuses the stale pixel again.
        forced_off = analyze(static_meta, whole, static_scene=False)["engine_analysis"]
        assert forced_off["rois"][0]["branch_counts"] == branches
        assert forced_off["static_scene"] == {"declared": True, "replayed_as": False}
        # An older capture (no field) asked what the policy would have done.
        assumed = analyze(complete_meta, whole, static_scene=True)["engine_analysis"]
        assert assumed["rois"][0]["branch_counts"]["camera_stale_static"] == 1
        assert assumed["static_scene"] == {"declared": False, "replayed_as": True}
        v2_path.write_text(json.dumps({**complete, "static_scene": "yes"}), encoding="utf-8")
        try:
            load_manifest(v2_path)
            raise AssertionError("a non-boolean static_scene was accepted")
        except CaptureError:
            pass
        # Only the stale-depth refusal is relaxed, in the shader's order: the sky (depth 0) and the
        # out-of-range sentinel refuse first; a stale slot with a malformed even code is stale first.
        edge_slots = np.asarray([[[1, .4], [2, .4], [4294967296.0, .4]],
                                 [[1, .4], [2, .5], [3, .5]]], dtype="<f4")
        edge_depth = np.full((rh, rw), .5, dtype="<f4")
        edge_depth[1, 0] = 0.0
        (v2_session / "frame_7_slots.bin").write_bytes(edge_slots.tobytes())
        (v2_session / "frame_7_depth.bin").write_bytes(edge_depth.tobytes())
        for flag, want in ((False, {"rejected_stale_or_depth": 4, "rejected_corrupt_code": 1, "engine_joined": 1}),
                           (True, {"camera_stale_static": 2, "rejected_stale_or_depth": 2,
                                   "rejected_corrupt_code": 1, "engine_joined": 1})):
            v2_path.write_text(json.dumps({**complete, "static_scene": flag}), encoding="utf-8")
            got = analyze(load_manifest(v2_path), whole)["engine_analysis"]["rois"][0]["branch_counts"]
            assert got == want, (flag, got, want)
        (v2_session / "frame_7_slots.bin").write_bytes(slots.tobytes())
        (v2_session / "frame_7_depth.bin").write_bytes(np.full((rh, rw), .5, dtype="<f4").tobytes())
        v2_path.write_text(json.dumps(complete), encoding="utf-8")
        complete["buffers"][0]["stride"] = 168
        v2_path.write_text(json.dumps(complete), encoding="utf-8")
        stride_meta = load_manifest(v2_path)
        stride_branches = analyze(stride_meta, [("one", (1, 0, 1, 1))])["engine_analysis"]["rois"][0]["branch_counts"]
        assert stride_branches["rejected_pool_stride"] == 1
        # F6 boundary fixtures: the previous 276-row scene layout. A non-reset
        # capture reads the stamp row and must be refused at load; a reset
        # capture never interprets records and stays accepted.
        legacy = json.loads(json.dumps(complete))
        legacy_scene = np.zeros((276, 4), dtype="<f4")
        legacy_scene[270:276] = np.asarray(legacy["camera"], dtype="<f4")
        for name in ("scene_now", "scene_previous"):
            (v2_session / f"frame_7_{name}.bin").write_bytes(legacy_scene.tobytes())
        for record in legacy["buffers"][1:]:
            record["byte_size"] = legacy_scene.nbytes
        v2_path.write_text(json.dumps(legacy), encoding="utf-8")
        try:
            load_manifest(v2_path)
            raise AssertionError("a non-reset capture without the stamp row was accepted")
        except CaptureError:
            pass
        legacy["reset"] = True
        v2_path.write_text(json.dumps(legacy), encoding="utf-8")
        assert load_manifest(v2_path)["reset"] is True
        del rejection, geometry, meta
        # A small copy of the WARP writer's known pixels checks the fixture
        # verifier itself. The full build checks bytes emitted by the real GPU.
        fixture_root = root / "fixture"
        fixture_session = fixture_root / "flat_pixels" / "session"
        fixture_session.mkdir(parents=True)
        fixture_records = []
        for name, (fmt, bpp, grid) in TEXTURES.items():
            pixels = bytearray()
            for fy in range(3):
                for fx in range(17):
                    if name in ("color", "final"):
                        pixels.extend((fx + 1, fy + 1, 0, 255))
                    elif name == "raw":
                        pixels.extend((0, 255, 0, 255))
                    elif name == "rejection":
                        pixels.append(255)
                    elif name == "depth":
                        pixels.extend(struct.pack("<f", .01 + fy * .01))
                    else:
                        pixels.extend(b"\0" * 4)
            filename = f"frame_7_{name}.bin"
            (fixture_session / filename).write_bytes(pixels)
            fixture_records.append({"name": name, "filename": filename, "dxgi_format": fmt,
                                    "width": 17, "height": 3, "row_stride": 17 * bpp,
                                    "byte_size": len(pixels)})
        fixture_manifest = {"version": 1, "frame_id": 7, "mode": "dlss",
                            "configured_dlss_preset": 11, "reset": False,
                            "jitter": [0.25, -0.375], "previous_jitter": [-0.125, 0.5],
                            "rows_jitter": [0.25, -0.375], "previous_rows_jitter": [-0.125, 0.5],
                            "render_width": 17, "render_height": 3,
                            "output_width": 17, "output_height": 3,
                            "binary_version": "fixture", "binary_compiled": "fixture",
                            "textures": fixture_records}
        (fixture_session / "frame_7.json").write_text(json.dumps(fixture_manifest), encoding="utf-8")
        # Match the producer's flat_pixels/<session>/frame_7.json pointer.
        (fixture_root / "current_fixture.txt").write_text(
            "flat_pixels/session/frame_7.json\n", encoding="utf-8")
        assert verify_fixture(fixture_root)["fixture_verified"]
        (fixture_root / "current_fixture.txt").write_text(
            "../session/frame_7.json\n", encoding="utf-8")
        try:
            verify_fixture(fixture_root)
            raise AssertionError("unsafe fixture pointer accepted")
        except CaptureError:
            pass
        _self_test_rows_jitter(root)
        _self_test_stability(root)
        _self_test_hdr(root)
    print("flat_pixels self-test passed")


def _synthetic_capture(session, frame, rw, rh, ow, oh, *, reset=False, jitter=(0, 0), previous=(0, 0),
                       final=None, rejection=None, depth=None, camera=None, previous_camera=None, extra=None):
    """A small version-2 capture with no engine inputs, written under `session`."""
    session.mkdir(parents=True, exist_ok=True)
    contents = {
        "color": bytes(rw * rh * 4), "motion": bytes(rw * rh * 4),
        "depth": (np.full((rh, rw), .0008 if depth is None else depth, dtype="<f4")).tobytes(),
        "rejection": bytes(rw * rh) if rejection is None else rejection,
        "raw": bytes(ow * oh * 4), "final": bytes(ow * oh * 4) if final is None else final,
    }
    records = []
    for name, (fmt, bpp, grid) in TEXTURES.items():
        w, h = (rw, rh) if grid == "render" else (ow, oh)
        filename = f"frame_{frame}_{name}.bin"
        (session / filename).write_bytes(contents[name])
        records.append({"name": name, "filename": filename, "dxgi_format": fmt, "width": w, "height": h,
                        "row_stride": w * bpp, "byte_size": w * h * bpp})
    still = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1], [0, 0, .025, 0], [0, 0, 1, 0], [1, 0, 0, 0]]
    manifest = {"version": 2, "frame_id": frame, "mode": "dlss", "reset": reset,
                "jitter": list(jitter), "previous_jitter": list(previous),
                "render_width": rw, "render_height": rh, "output_width": ow, "output_height": oh,
                "textures": records, "camera": camera or still, "previous_camera": previous_camera or camera or still,
                "engine": {"complete": False, "status": "absent-or-partial", "slots_present": False,
                           "pool_present": False, "scene_now_present": False, "scene_previous_present": False},
                "buffers": []}
    manifest.update(extra or {})
    path = session / f"frame_{frame}.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def _self_test_hdr(root):
    session = root / "hdr_route"
    session.mkdir()
    rw, rh = 5, 4
    roi = {"x": 1, "y": 1, "width": 3, "height": 2}
    packed_one = ((14 << 5) << 22) | ((16 << 6) << 11) | (15 << 6)
    packed_four = ((14 << 5) << 22) | ((16 << 6) << 11) | (17 << 6)
    camera = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1],
              [0, 0, .025, 0], [0, 0, 1, 0], [1, 0, 0, 0]]
    def make(frame, final_bits):
        contents = {"color": struct.pack("<I", packed_one) * 6,
                    "depth": struct.pack("<f", .01) * 6,
                    "motion": bytes(6 * 4), "rejection": bytes([255] * 6),
                    "raw": struct.pack("<eeee", 1, 2, .5, 1) * 6,
                    "final": struct.pack("<I", final_bits) * 6}
        records = []
        for name, (fmt, bpp, _) in HDR_TEXTURES.items():
            filename = f"frame_{frame}_{name}.bin"
            (session / filename).write_bytes(contents[name])
            records.append({"name": name, "filename": filename, "dxgi_format": fmt,
                            "width": 3, "height": 2, "row_stride": 3 * bpp,
                            "byte_size": 6 * bpp})
        manifest = {"version": 2, "frame_id": frame, "mode": "dlaa", "reset": False,
                    "route": "hdr", "capture_roi": roi, "refusal_overlay": False,
                    **HDR_PROVENANCE, "jitter": [0, 0], "previous_jitter": [0, 0],
                    "render_width": rw, "render_height": rh, "output_width": rw, "output_height": rh,
                    "display_width": rw, "display_height": rh, "textures": records,
                    "camera": camera, "previous_camera": camera,
                    "engine": {"complete": False, "status": "absent-or-partial", "slots_present": False,
                               "pool_present": False, "scene_now_present": False, "scene_previous_present": False},
                    "buffers": []}
        path = session / f"frame_{frame}.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path
    first, second = make(50, packed_one), make(51, packed_four)
    meta = load_manifest(first)
    assert meta["route"] == "hdr" and meta["capture_roi"] == roi
    pixels = _hdr_rgb(meta, "color")
    assert np.allclose(pixels[0, 0], [1, 2, .5])
    result = analyze(meta)
    assert result["route"] == "hdr" and result["output_rejection_pixels"] == 2
    assert result["edge_excluded_pixels"] == 4 and result["rejected"]["pixels"] == 2
    assert result["engine_analysis"]["rois"][0]["xywh"] == [1, 1, 3, 2]
    pair = stability([meta, load_manifest(second)])
    assert pair["consecutive_pair_status"] == "present" and pair["pairs"][0]["route"] == "hdr"
    assert pair["pairs"][0]["all"]["mean_linear_radiance_change"] > 1
    previews = root / "hdr_previews"
    run(session, previews, dry_run=True)
    assert not previews.exists()
    run(session, previews)
    assert (previews / session.name / "frame_50" / "final.png").is_file()
    malformed = json.loads(second.read_text(encoding="utf-8"))
    malformed["textures"][0]["dxgi_format"] = 28
    second.write_text(json.dumps(malformed), encoding="utf-8")
    try:
        load_manifest(second)
        raise AssertionError("HDR color accepted as SDR format")
    except CaptureError:
        pass
    # R11/G11/B10 channel boundaries, including smallest subnormals and
    # largest finite values, are decoded in linear radiance without clipping.
    for bits, expected in ((1 | (1 << 11) | (1 << 22), [2**-20, 2**-20, 2**-19]),
                           ((30 << 6 | 63) | ((30 << 6 | 63) << 11) |
                            ((30 << 5 | 31) << 22), [65024, 65024, 64512])):
        (session / "frame_50_color.bin").write_bytes(struct.pack("<I", bits) * 6)
        assert np.allclose(_hdr_rgb(meta, "color")[0, 0], expected, rtol=0, atol=1e-6)
    inf_bits = (31 << 6) | ((31 << 6 | 1) << 11)
    (session / "frame_50_color.bin").write_bytes(struct.pack("<I", inf_bits) * 6)
    decoded = _hdr_rgb(meta, "color")[0, 0]
    assert np.isinf(decoded[0]) and np.isnan(decoded[1])


def _self_test_rows_jitter(root):
    """The camera injector's rows, end to end through the loader and the replay: a still scene
    whose rows carry the raster phase. The replay this tool made before (rows as captured)
    reports a motion error of exactly |current phase - previous phase| on every pixel; with the
    phase removed as the shader does it is float error."""
    from flat_pixels_engine import jitter_rows, rows_ndc, synthetic_camera
    rw, rh = 16, 9
    jn, jo = [-0.125, -0.27777779], [0.125, 0.27777779]
    base = synthetic_camera(rw, rh)
    now_rows = jitter_rows(base, rows_ndc(jn, rw, rh))
    old_rows = jitter_rows(base, rows_ndc(jo, rw, rh))
    expected = math.hypot(jn[0] - jo[0], jn[1] - jo[1])
    for label, extra, source in (("inferred", None, "inferred-carried"),
                                 ("declared", {"rows_jitter": jn, "previous_rows_jitter": jo}, "declared")):
        path = _synthetic_capture(root / f"rows_{label}", 9, rw, rh, rw, rh, jitter=jn, previous=jo,
                                  camera=now_rows.tolist(), previous_camera=old_rows.tolist(), extra=extra)
        meta = load_manifest(path)
        window = [("all", (0, 0, rw, rh))]
        stock = analyze(meta, window, unjitter=False)["engine_analysis"]["rois"][0]["motion_comparison"]["camera"]
        fixed_analysis = analyze(meta, window)["engine_analysis"]
        fixed = fixed_analysis["rois"][0]["motion_comparison"]["camera"]
        # The stock replay's error IS the phase difference: the assertion that pins the tool defect.
        assert abs(stock["median_error_px"] - expected) < 2e-3, (label, stock, expected)
        assert fixed["max_error_px"] < 1e-3, (label, fixed)
        assert fixed_analysis["rows_jitter"]["source"] == source, (label, fixed_analysis["rows_jitter"])
    # Legacy captures (rows unjittered, no field) are left exactly as they were.
    plain = _synthetic_capture(root / "rows_plain", 9, rw, rh, rw, rh, jitter=jn, previous=jo,
                               camera=base.tolist(), previous_camera=base.tolist())
    analysis = analyze(load_manifest(plain), [("all", (0, 0, rw, rh))])["engine_analysis"]
    assert analysis["rows_jitter"]["source"] == "unjittered"
    # A capture that declares one of the two phases is malformed.
    lopsided = _synthetic_capture(root / "rows_lopsided", 9, rw, rh, rw, rh, extra={"rows_jitter": jn})
    try:
        load_manifest(lopsided)
        raise AssertionError("a capture declaring only one rows phase was accepted")
    except CaptureError:
        pass


def _self_test_stability(root):
    """The stability report: change between two live frames' final images, split by the later
    frame's rejection footprint; a reset frame or a wide gap is not a pair."""
    rw, rh, ow, oh = 4, 3, 8, 6
    # Everything rejected: a 40-level change is 100% visible and all of it lands in the footprint.
    for frame, level in ((10, 100), (12, 140)):
        _synthetic_capture(root / "stab_rejected", frame, rw, rh, ow, oh, final=bytes([level]) * (ow * oh * 4),
                           rejection=bytes([255]) * (rw * rh))
    report = stability([load_manifest(p) for p in _manifest_paths(root / "stab_rejected")])
    assert len(report["pairs"]) == 1 and report["pairs"][0]["frames"] == [10, 12]
    pair = report["pairs"][0]
    assert pair["rejected"]["pixels"] == ow * oh and pair["rejected"]["mean_change_of_255"] == 40
    assert pair["rejected"]["visible_change_percent"] == 100 and pair["accepted"]["pixels"] == 0
    assert pair["accepted"]["mean_change_of_255"] is None
    # Nothing rejected: a 2-level change is below the visible threshold and lands outside the footprint.
    _synthetic_capture(root / "stab_accepted", 20, rw, rh, ow, oh, final=bytes([100]) * (ow * oh * 4))
    _synthetic_capture(root / "stab_accepted", 21, rw, rh, ow, oh, final=bytes([102]) * (ow * oh * 4))
    pair = stability([load_manifest(p) for p in _manifest_paths(root / "stab_accepted")])["pairs"][0]
    assert pair["accepted"]["pixels"] == ow * oh and pair["accepted"]["mean_change_of_255"] == 2
    assert pair["accepted"]["visible_change_percent"] == 0 and pair["rejected"]["pixels"] == 0
    # Not pairs: a reset frame, a gap past the limit, another grid.
    _synthetic_capture(root / "stab_none", 30, rw, rh, ow, oh, reset=True)
    _synthetic_capture(root / "stab_none", 32, rw, rh, ow, oh)
    _synthetic_capture(root / "stab_none", 32 + STABILITY_MAX_GAP + 1, rw, rh, ow, oh)
    report = stability([load_manifest(p) for p in _manifest_paths(root / "stab_none")])
    assert report["pairs"] == [] and "reset frame is not one" in report["status"]
    _synthetic_capture(root / "stab_grid", 40, rw, rh, ow, oh)
    _synthetic_capture(root / "stab_grid", 41, rw, rh, ow * 2, oh * 2)
    assert stability([load_manifest(p) for p in _manifest_paths(root / "stab_grid")])["pairs"] == []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", nargs="?", type=Path)
    parser.add_argument("--output", type=Path, help="write PNG previews and summary to this directory")
    parser.add_argument("--dry-run", action="store_true", help="analyze and show intended output without writing")
    parser.add_argument("--roi", action="append", nargs=4, metavar=("X", "Y", "W", "H"), type=int,
                        help="repeatable ROI in render/input pixels; default samples the full frame")
    parser.add_argument("--verify-fixture", action="store_true", help="verify the real WARP writer fixture")
    parser.add_argument("--assume-static-scene", choices=("on", "off"),
                        help="replay every frame as if the 3D main menu's stale-slot policy were on (or off) "
                             "instead of as the capture declares it: what would it have done on a capture "
                             "made before it existed. The rejection statistics stay what the GPU produced.")
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
        rois = []
        for index, values in enumerate(args.roi or []):
            x, y, width, height = values
            if min(x, y) < 0 or min(width, height) < 1:
                raise CaptureError("ROI coordinates must be nonnegative and dimensions positive")
            rois.append((f"roi_{index + 1}", (x, y, width, height)))
        assumed = None if args.assume_static_scene is None else args.assume_static_scene == "on"
        print(json.dumps(run(args.capture_dir, args.output, args.dry_run, rois, assumed), indent=2))
        return 0
    except (CaptureError, OSError) as exc:
        print(f"flat_pixels: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
