#!/usr/bin/env python3
"""Export bounded identity-fusion replay fixtures from eye draw captures.

The replay intentionally borrows complete geometry from the first selected
frame.  Later frames in the source capture retain draw-time constants and
mesh-pool snapshots, but not geometry payloads.  Matching therefore uses the
stable draw description and occurrence order; the emitted geometry index is
the authoritative cross-frame group.
"""

import argparse
from collections import Counter, defaultdict
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile

import eye_draw_snapshot


MAGIC = b"EDVRIFR1"
VERSION = 1
FAMILIES = (
    0x7B0DC42D383F694C,
    0x8B589D25B2A0ADDC,
    0x114AF608F86D9ED8,
    0xAACFDCF2FB9AD809,
    0x174E8D76363BE337,
)
MAX_FRAMES = 4
MAX_DRAWS = 64
MAX_GEOMETRIES = 64
MAX_RESOURCE = 16 * 1024 * 1024
MAX_TOTAL = 256 * 1024 * 1024
MAX_CONSTANT = 8192
MAX_VERTEX = 262144
MISSING = 0xFFFFFFFF


class ExportError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def edvr_hash(data):
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def eligible(draw):
    return (draw.get("ordinal") == 0xFFFFFFFE and
            draw.get("vs") in FAMILIES and
            draw.get("instances") == 1 and
            0 < draw.get("count", 0) <= 131072 and
            draw["count"] % 3 == 0)


def layout_key(layout):
    return tuple((e["semantic"], e["index"], e["format"], e["slot"],
                  e["offset"], e["classification"], e["step"])
                 for e in layout)


def stable_key(draw):
    return (draw["vs"], layout_key(draw["layout"]), draw["count"],
            draw["start"], draw["base"])


def full_geometry_key(draw):
    streams = draw["streams"]
    return stable_key(draw) + tuple((s["offset"], s["stride"], s["whole"])
                                    for s in streams[1:3])


def _buffer(capture, ref, stride, frame, draw_index, require_full=True):
    buffers = capture.get("mesh_buffers", [])
    if ref == MISSING or ref < 0 or ref >= len(buffers):
        raise ExportError("unknown_resource")
    buf = buffers[ref]
    if (buf.get("stride") != stride or buf.get("frame") != frame or
            buf.get("first_draw", draw_index + 1) > draw_index):
        raise ExportError("resource_descriptor")
    data = buf.get("data", b"")
    whole = buf.get("whole", 0)
    if not 0 < whole <= MAX_RESOURCE:
        raise ExportError("resource_too_large")
    if whole % stride:
        raise ExportError("resource_descriptor")
    if require_full and (len(data) != whole or buf.get("size", len(data)) != whole):
        raise ExportError("resource_incomplete")
    return buf


def draw_payload(capture, draw, draw_index, instance_binding=None):
    refs = draw.get("mesh", [MISSING] * 3)
    if len(refs) != 3:
        raise ExportError("unknown_resource")
    pool = _buffer(capture, refs[0], 336, draw["frame"], draw_index)
    bones = _buffer(capture, refs[1], 48, draw["frame"], draw_index)
    instances = _buffer(capture, refs[2], 8, draw["frame"], draw_index)
    if instance_binding is None:
        streams = draw.get("streams", [])
        if not streams or streams[0].get("stride") != 8:
            raise ExportError("instance_descriptor")
        instance_binding = (streams[0].get("offset", 0), streams[0]["stride"])
    instance_offset, instance_stride = instance_binding
    if instance_stride != 8:
        raise ExportError("instance_descriptor")
    begin = instance_offset + draw["start_instance"] * instance_stride
    if begin < 0 or begin + 8 > len(instances["data"]):
        raise ExportError("instance_out_of_bounds")
    buffers = draw.get("buffers", [])
    if len(buffers) < 3:
        raise ExportError("constant_missing")
    constants = []
    for slot in (1, 2):
        item = buffers[slot]
        data = item.get("data", b"")
        whole = item.get("whole", 0)
        if not data or len(data) != whole or whole > MAX_CONSTANT or whole % 16:
            raise ExportError("constant_incomplete")
        constants.append(data)
    return refs[0], refs[1], instances["data"][begin:begin + 8], constants


def normalize_geometry(draw):
    streams = draw.get("streams", [])
    if len(streams) < 3:
        raise ExportError("geometry_descriptor_missing")
    vb, ib = streams[1], streams[2]
    stride = vb.get("stride", 0)
    index_size = ib.get("stride", 0)
    if not 0 < stride <= 256 or index_size not in (2, 4):
        raise ExportError("geometry_descriptor_invalid")
    count = draw["count"]
    ib_first = ib.get("offset", 0) + draw["start"] * index_size
    ib_capture = ib.get("capture_offset", ib.get("offset", 0))
    ib_data = ib.get("data", b"")
    ib_rel = ib_first - ib_capture
    ib_bytes = count * index_size
    if ib_rel < 0 or ib_rel + ib_bytes > len(ib_data):
        raise ExportError("index_window_incomplete")
    raw = ib_data[ib_rel:ib_rel + ib_bytes]
    code = "H" if index_size == 2 else "I"
    indices = struct.unpack("<%d%s" % (count, code), raw)
    vertices = [draw["base"] + value for value in indices]
    if not vertices or min(vertices) < 0:
        raise ExportError("negative_vertex_index")
    low, high = min(vertices), max(vertices)
    vb_first = vb.get("offset", 0) + low * stride
    vb_last = vb.get("offset", 0) + (high + 1) * stride
    vb_capture = vb.get("capture_offset", vb.get("offset", 0))
    vb_data = vb.get("data", b"")
    vb_rel = vb_first - vb_capture
    if vb_rel < 0 or vb_last - vb_capture > len(vb_data):
        raise ExportError("vertex_window_incomplete")
    compact = vb_data[vb_rel:vb_last - vb_capture]
    if len(compact) > MAX_VERTEX:
        raise ExportError("vertex_window_too_large")
    normalized = struct.pack("<%dI" % count, *(value - low for value in vertices))
    layout = draw.get("layout", [])
    if not 0 < len(layout) <= 32:
        raise ExportError("layout_invalid")
    clean_layout = []
    for element in layout:
        semantic = element.get("semantic", "")
        try:
            encoded = semantic.encode("ascii")
        except UnicodeEncodeError:
            raise ExportError("layout_invalid")
        if not 0 < len(encoded) <= 63:
            raise ExportError("layout_invalid")
        clean_layout.append(dict(element))
    return {
        "shader_hash": draw["vs"], "index_count": count,
        "vb_stride": stride, "layout": clean_layout,
        "vb": compact, "ib": normalized,
        "source_key": full_geometry_key(draw),
        "source_index_size": index_size,
        "source_vertex_first": low,
        "source_streams": [
            {name: stream.get(name, 0) for name in ("offset", "stride", "whole", "capture_offset")}
            for stream in streams
        ],
    }


def extract(capture, shader_blobs, frame_limit=2, verify_shader_hash=True):
    if not 1 <= frame_limit <= MAX_FRAMES:
        raise ExportError("frame count must be between 1 and 4")
    candidates = [(i, d) for i, d in enumerate(capture.get("draws", [])) if eligible(d)]
    all_frames = sorted({d["frame"] for _, d in candidates})
    selected = all_frames[:frame_limit]
    if len(selected) != frame_limit:
        raise ExportError("capture has fewer than %d eligible frames" % frame_limit)
    if any(frame != selected[0] + offset for offset, frame in enumerate(selected)):
        raise ExportError("selected frames are not consecutive")
    frame_set = set(selected)
    by_frame = {frame: [(i, d) for i, d in candidates if d["frame"] == frame]
                for frame in selected}
    if any(len(items) > MAX_DRAWS for items in by_frame.values()):
        raise ExportError("eligible draw count exceeds per-frame bound")
    skips = Counter()
    per_frame_skips = {str(frame): Counter() for frame in selected}
    geometries = []
    geometry_by_identity = {}
    source_slots = defaultdict(list)
    frames = []
    resources = []
    resource_by_ref = {}
    resource_provenance = []

    def reject(frame, reason):
        skips[reason] += 1
        per_frame_skips[str(frame)][reason] += 1

    def resource(ref, expected_stride):
        if ref in resource_by_ref:
            index = resource_by_ref[ref]
            if resources[index]["stride"] != expected_stride:
                raise ExportError("resource_descriptor")
            return index
        buf = capture["mesh_buffers"][ref]
        if len(resources) >= MAX_FRAMES * 2:
            raise ExportError("resource count exceeds bound")
        index = len(resources)
        resource_by_ref[ref] = index
        resources.append({"stride": expected_stride, "data": buf["data"]})
        resource_provenance.append({
            "resource_index": index, "capture_ref": ref, "frame": buf["frame"],
            "first_draw": buf["first_draw"], "stride": buf["stride"],
            "bytes": len(buf["data"]), "sha256": sha256(buf["data"]),
        })
        return index

    first = selected[0]
    first_draws = []
    dimensions = None
    normalization = Counter()
    for draw_index, draw in by_frame[first]:
        key = stable_key(draw)
        geometry_index = None
        try:
            geometry = normalize_geometry(draw)
            instance_binding = (draw["streams"][0].get("offset", 0),
                                draw["streams"][0].get("stride", 0))
            pool_ref, bone_ref, instance, constants = draw_payload(
                capture, draw, draw_index, instance_binding)
            dims = (draw["width"], draw["height"])
            if not dims[0] or not dims[1]:
                raise ExportError("dimensions_invalid")
            if dimensions is None:
                dimensions = dims
            elif dims != dimensions:
                raise ExportError("dimensions_mismatch")
            identity = (geometry["source_key"], geometry["vb"], geometry["ib"])
            geometry_index = geometry_by_identity.get(identity)
            if geometry_index is None:
                if len(geometries) >= MAX_GEOMETRIES:
                    raise ExportError("geometry_count_exceeded")
                geometry_index = len(geometries)
                geometry_by_identity[identity] = geometry_index
                geometry["source_draws"] = [draw_index]
                geometry["borrow_signature"] = identity
                geometries.append(geometry)
            else:
                geometries[geometry_index]["source_draws"].append(draw_index)
            pool_index = resource(pool_ref, 336)
            bone_index = resource(bone_ref, 48)
            first_draws.append({
                "geometry": geometry_index, "pool": pool_index, "bones": bone_index,
                "original_draw": draw_index, "instance": instance,
                "cb1": constants[0], "cb2": constants[1],
            })
            normalization["draws"] += 1
            normalization["source_indices"] += draw["count"]
            normalization["normalized_index_bytes"] += draw["count"] * 4
            normalization["compact_vertex_bytes"] += len(geometry["vb"])
            normalization["source_index_%d_bit" % (geometry["source_index_size"] * 8)] += 1
        except ExportError as exc:
            reject(first, str(exc))
        source_slots[key].append(None if geometry_index is None else
                                 (geometry_index, instance_binding))
    frames.append({"original_frame": first, "draws": first_draws})

    for frame in selected[1:]:
        occurrence = Counter()
        output_draws = []
        ambiguous = {
            key for key, slots in source_slots.items()
            if len({geometries[slot[0]]["borrow_signature"] for slot in slots if slot is not None}) > 1
        }
        for draw_index, draw in by_frame[frame]:
            key = stable_key(draw)
            slot = occurrence[key]
            occurrence[key] += 1
            if key in ambiguous:
                reject(frame, "ambiguous_geometry_key")
                continue
            choices = source_slots.get(key, [])
            if slot >= len(choices) or choices[slot] is None:
                reject(frame, "geometry_unavailable")
                continue
            choice = choices[slot]
            if choice is None:
                reject(frame, "geometry_unavailable")
                continue
            geometry_index, instance_binding = choice
            try:
                pool_ref, bone_ref, instance, constants = draw_payload(
                    capture, draw, draw_index, instance_binding)
                if dimensions != (draw["width"], draw["height"]):
                    raise ExportError("dimensions_mismatch")
                pool_index = resource(pool_ref, 336)
                bone_index = resource(bone_ref, 48)
                output_draws.append({
                    "geometry": geometry_index, "pool": pool_index, "bones": bone_index,
                    "original_draw": draw_index, "instance": instance,
                    "cb1": constants[0], "cb2": constants[1],
                })
            except ExportError as exc:
                reject(frame, str(exc))
        frames.append({"original_frame": frame, "draws": output_draws})

    if not geometries or dimensions is None:
        raise ExportError("no complete first-frame geometry")
    used_families = [family for family in FAMILIES
                     if any(geometry["shader_hash"] == family for geometry in geometries)]
    shader_index = {family: index for index, family in enumerate(used_families)}
    for family in used_families:
        blob = shader_blobs.get(family)
        if not blob:
            raise ExportError("missing shader %016X" % family)
        if verify_shader_hash and edvr_hash(blob) != family:
            raise ExportError("shader bytecode hash mismatch for %016X" % family)
    for geometry in geometries:
        geometry["shader_index"] = shader_index[geometry["shader_hash"]]
    fixture = {
        "width": dimensions[0], "height": dimensions[1],
        "shaders": [{"hash": family, "data": shader_blobs[family]} for family in used_families],
        "geometries": geometries, "resources": resources, "frames": frames,
    }
    manifest = {
        "format": "EDVRIFR1", "version": VERSION,
        "settings": {
            "frame_limit": frame_limit, "selected_frames": selected,
            "frame_boundaries": {"first": selected[0], "last": selected[-1]},
            "source_ordinal": "FFFFFFFE", "instances": 1,
            "triangle_index_count": {"multiple": 3, "maximum": 131072},
            "families": ["%016X" % family for family in FAMILIES],
        },
        "coverage": {
            "candidate_draws": {str(frame): len(by_frame[frame]) for frame in selected},
            "accepted_draws": {str(f["original_frame"]): len(f["draws"]) for f in frames},
            "skipped_draws": dict(skips),
            "skipped_by_frame": {frame: dict(counts) for frame, counts in per_frame_skips.items()},
            "geometry_count": len(geometries), "resource_count": len(resources),
            "accepted_families": sorted({"%016X" % geometries[d["geometry"]]["shader_hash"]
                                         for f in frames for d in f["draws"]}),
        },
        "normalization": dict(normalization),
        "resources": resource_provenance,
        "geometries": [{
            "geometry_index": index, "source_draws": geometry["source_draws"],
            "shader": "%016X" % geometry["shader_hash"],
            "index_count": geometry["index_count"],
            "source_index_bits": geometry["source_index_size"] * 8,
            "source_vertex_first": geometry["source_vertex_first"],
            "source_streams": geometry["source_streams"],
            "compact_vertex_bytes": len(geometry["vb"]),
            "compact_vertex_sha256": sha256(geometry["vb"]),
            "normalized_index_bytes": len(geometry["ib"]),
            "normalized_index_sha256": sha256(geometry["ib"]),
        } for index, geometry in enumerate(geometries)],
        "assumptions": {
            "historical_geometry_reuse": "later-frame draws borrow first-frame geometry by stable key and occurrence",
            "binding_metadata_reuse": "later-frame stream descriptors are zero; VB/IB and instance binding descriptors are borrowed from the unambiguous first-frame match",
            "shared_snapshot": "pool and bone buffers are full frame-local copies; first_draw may precede the replayed draw",
            "depth_stencil": "per-draw depth/stencil state was not captured; the benchmark reconstructs selected-mesh depth per frame",
            "image_export": False,
        },
    }
    return fixture, manifest


def encode(fixture):
    frames = fixture["frames"]
    shaders = fixture["shaders"]
    geometries = fixture["geometries"]
    resources = fixture["resources"]
    if not 1 <= len(frames) <= MAX_FRAMES or len(shaders) > 5 or len(geometries) > MAX_GEOMETRIES:
        raise ExportError("fixture count exceeds format bound")
    if any(len(frame["draws"]) > MAX_DRAWS for frame in frames):
        raise ExportError("fixture draw count exceeds format bound")
    out = io.BytesIO()

    def put(fmt, *values):
        out.write(struct.pack(fmt, *values))

    def blob(data, maximum=MAX_TOTAL):
        if len(data) > maximum:
            raise ExportError("blob exceeds format bound")
        put("<I", len(data)); out.write(data)

    out.write(MAGIC)
    put("<7I", VERSION, fixture["width"], fixture["height"], len(frames),
        len(shaders), len(geometries), len(resources))
    for shader in shaders:
        put("<Q", shader["hash"]); blob(shader["data"], MAX_RESOURCE)
    for resource in resources:
        put("<I", resource["stride"]); blob(resource["data"], MAX_RESOURCE)
    for geometry in geometries:
        put("<4I", geometry["shader_index"], geometry["index_count"],
            geometry["vb_stride"], len(geometry["layout"]))
        for element in geometry["layout"]:
            blob(element["semantic"].encode("ascii"), 63)
            put("<6I", element["index"], element["format"], element["slot"],
                element["offset"], element["classification"], element["step"])
        blob(geometry["vb"], MAX_VERTEX)
        blob(geometry["ib"], 131072 * 4)
    for frame in frames:
        put("<2I", frame["original_frame"], len(frame["draws"]))
        for draw in frame["draws"]:
            put("<4I", draw["geometry"], draw["pool"], draw["bones"], draw["original_draw"])
            if len(draw["instance"]) != 8:
                raise ExportError("instance pair is not 8 bytes")
            out.write(draw["instance"])
            blob(draw["cb1"], MAX_CONSTANT); blob(draw["cb2"], MAX_CONSTANT)
    data = out.getvalue()
    if len(data) > MAX_TOTAL:
        raise ExportError("fixture exceeds total size bound")
    return data


def inspect_binary(data):
    """Bounded format reader used by self-test; returns the parsed counts."""
    if len(data) > MAX_TOTAL:
        raise ExportError("fixture exceeds total size bound")
    stream = io.BytesIO(data)

    def take(size):
        value = stream.read(size)
        if len(value) != size:
            raise ExportError("truncated fixture")
        return value

    def get(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))

    def blob(maximum, minimum=0):
        size, = get("<I")
        if not minimum <= size <= maximum:
            raise ExportError("invalid blob size")
        return take(size)

    if take(8) != MAGIC:
        raise ExportError("invalid fixture magic")
    version, width, height, nf, ns, ng, nr = get("<7I")
    if (version != VERSION or not width or not height or not 1 <= nf <= MAX_FRAMES or
            ns > 5 or ng > MAX_GEOMETRIES or nr > MAX_FRAMES * 2):
        raise ExportError("invalid fixture header")
    for _ in range(ns):
        get("<Q"); blob(MAX_RESOURCE, 1)
    for _ in range(nr):
        stride, = get("<I")
        if stride not in (336, 48):
            raise ExportError("invalid resource stride")
        blob(MAX_RESOURCE, 1)
    layouts = []
    for _ in range(ng):
        shader, count, stride, ne = get("<4I")
        if shader >= ns or not 0 < count <= 131072 or not 0 < stride <= 256 or ne > 32:
            raise ExportError("invalid geometry descriptor")
        for _ in range(ne):
            semantic = blob(63, 1)
            try:
                semantic.decode("ascii")
            except UnicodeDecodeError:
                raise ExportError("invalid semantic")
            get("<6I")
        blob(MAX_VERTEX, 1)
        ib = blob(131072 * 4, 1)
        if len(ib) != count * 4:
            raise ExportError("invalid normalized index size")
        layouts.append(ne)
    draw_counts = []
    for _ in range(nf):
        _, nd = get("<2I")
        if nd > MAX_DRAWS:
            raise ExportError("invalid frame draw count")
        draw_counts.append(nd)
        for _ in range(nd):
            geometry, pool, bones, _ = get("<4I")
            if geometry >= ng or pool >= nr or bones >= nr:
                raise ExportError("invalid draw reference")
            take(8); blob(MAX_CONSTANT, 1); blob(MAX_CONSTANT, 1)
    if stream.read(1):
        raise ExportError("trailing fixture bytes")
    return {"frames": nf, "shaders": ns, "geometries": ng,
            "resources": nr, "draws": draw_counts, "layouts": layouts}


def load_shaders(directory):
    result = {}
    for family in FAMILIES:
        path = directory / ("vs_%016X.dxbc" % family)
        if path.is_file():
            result[family] = path.read_bytes()
    return result


def write_output(output_path, binary, manifest, dry_run=False):
    output_path = Path(output_path)
    manifest_path = output_path.with_suffix(".json")
    if not dry_run:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(binary)
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    return manifest_path


def export_capture(capture_path, output_path=None, frame_limit=2, dry_run=False):
    capture_path = Path(capture_path)
    capture_bytes = capture_path.read_bytes()
    capture = eye_draw_snapshot.read(capture_path)
    shaders = load_shaders(capture_path.parent)
    fixture, manifest = extract(capture, shaders, frame_limit)
    binary = encode(fixture)
    parsed = inspect_binary(binary)
    manifest["source"] = {"path": str(capture_path), "bytes": len(capture_bytes),
                          "sha256": sha256(capture_bytes)}
    manifest["shaders"] = [{"hash": "%016X" % item["hash"],
                            "bytes": len(item["data"]), "sha256": sha256(item["data"])}
                           for item in fixture["shaders"]]
    manifest["fixture"] = {"bytes": len(binary), "sha256": sha256(binary),
                           "roundtrip": parsed}
    if output_path is not None:
        output_path = Path(output_path)
        manifest_path = output_path.with_suffix(".json")
        manifest["fixture"]["path"] = str(output_path)
        manifest["fixture"]["manifest_path"] = str(manifest_path)
        write_output(output_path, binary, manifest, dry_run)
    summary = {
        "capture": str(capture_path), "dry_run": dry_run,
        "output": str(output_path) if output_path is not None else None,
        "frames": manifest["settings"]["selected_frames"],
        "candidate_draws": manifest["coverage"]["candidate_draws"],
        "accepted_draws": manifest["coverage"]["accepted_draws"],
        "skipped_draws": manifest["coverage"]["skipped_draws"],
        "geometries": len(fixture["geometries"]), "resources": len(fixture["resources"]),
        "fixture_bytes": len(binary), "fixture_sha256": sha256(binary),
    }
    return summary, manifest


def _synthetic():
    shader_blobs = {family: b"DXBC" + struct.pack("<Q", family) for family in FAMILIES}
    mesh_buffers = []
    draws = []
    for frame_no in (10, 11):
        pool_ref = len(mesh_buffers)
        mesh_buffers.append({"frame": frame_no, "first_draw": len(draws), "whole": 672,
                             "stride": 336, "size": 672, "data": bytes([frame_no]) * 672})
        bone_ref = len(mesh_buffers)
        mesh_buffers.append({"frame": frame_no, "first_draw": len(draws), "whole": 96,
                             "stride": 48, "size": 96, "data": bytes([frame_no + 1]) * 96})
        inst_ref = len(mesh_buffers)
        mesh_buffers.append({"frame": frame_no, "first_draw": len(draws), "whole": 32,
                             "stride": 8, "size": 32, "data": bytes(range(32))})
        for occurrence in range(2):
            vb = bytes(range(12))
            indices = struct.pack("<3H", 2, 4, 3)
            streams = [dict(offset=0, stride=8, whole=32, capture_offset=0, data=b""),
                       dict(offset=4, stride=4, whole=128, capture_offset=8, data=vb),
                       dict(offset=0, stride=2, whole=64, capture_offset=6, data=indices)]
            if frame_no != 10:
                streams = [dict(offset=0, stride=0, whole=0, capture_offset=0, data=b"") for _ in range(3)]
            draws.append({
                "vs": FAMILIES[0], "frame": frame_no, "ordinal": 0xFFFFFFFE,
                "instances": 1, "count": 3, "start": 3, "base": -1,
                "start_instance": occurrence + 1, "width": 20, "height": 10,
                "mesh": [pool_ref, bone_ref, inst_ref], "streams": streams,
                "layout": [{"semantic": "POSITION", "index": 0, "format": 2,
                            "slot": 1, "offset": 0, "classification": 0, "step": 0}],
                "buffers": [{"whole": 0, "data": b""},
                            {"whole": 16, "data": bytes([1 + occurrence]) * 16},
                            {"whole": 16, "data": bytes([3 + occurrence]) * 16},
                            {"whole": 0, "data": b""}],
            })
    return {"draws": draws, "mesh_buffers": mesh_buffers}, shader_blobs


def self_test():
    capture, shaders = _synthetic()
    fixture, manifest = extract(capture, shaders, 2, verify_shader_hash=False)
    assert [len(frame["draws"]) for frame in fixture["frames"]] == [2, 2]
    assert len(fixture["geometries"]) == 1
    assert [d["geometry"] for d in fixture["frames"][1]["draws"]] == [0, 0]
    assert fixture["frames"][0]["draws"][0]["instance"] != fixture["frames"][0]["draws"][1]["instance"]
    assert struct.unpack("<3I", fixture["geometries"][0]["ib"]) == (0, 2, 1)
    assert fixture["geometries"][0]["vb"] == bytes(range(12))
    assert manifest["normalization"]["source_index_16_bit"] == 2
    assert edvr_hash(b"") == 1469598103934665603
    assert edvr_hash(b"x") == ((1469598103934665603 ^ ord("x")) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    data = encode(fixture)
    assert inspect_binary(data)["draws"] == [2, 2]
    for bad in (data[:-1], data + b"x", b"BADMAGIC" + data[8:]):
        try:
            inspect_binary(bad)
            raise AssertionError("malformed replay fixture accepted")
        except ExportError:
            pass
    bad_frames = bytearray(data)
    struct.pack_into("<I", bad_frames, 20, MAX_FRAMES + 1)
    try:
        inspect_binary(bytes(bad_frames))
        raise AssertionError("out-of-range frame count accepted")
    except ExportError:
        pass

    def rejected(mutator, reason):
        sample, sample_shaders = _synthetic()
        mutator(sample)
        try:
            result, report = extract(sample, sample_shaders, 2, verify_shader_hash=False)
        except ExportError:
            return
        assert report["coverage"]["skipped_draws"].get(reason), report
        assert len(result["frames"][0]["draws"]) < 2

    rejected(lambda c: c["draws"][0].__setitem__("mesh", [999, 1, 2]), "unknown_resource")
    rejected(lambda c: c["mesh_buffers"][0].__setitem__("data", b"x"), "resource_incomplete")
    rejected(lambda c: c["draws"][0]["streams"][1].__setitem__("data", bytes(11)),
             "vertex_window_incomplete")
    rejected(lambda c: c["draws"][0]["streams"][2].__setitem__("data", bytes(5)),
             "index_window_incomplete")
    rejected(lambda c: c["draws"][0].__setitem__("base", -3), "negative_vertex_index")

    ambiguous_capture, ambiguous_shaders = _synthetic()
    ambiguous_capture["draws"][1]["streams"][1]["data"] = bytes([99]) + bytes(range(1, 12))
    ambiguous_fixture, ambiguous_manifest = extract(
        ambiguous_capture, ambiguous_shaders, 2, verify_shader_hash=False)
    assert len(ambiguous_fixture["frames"][0]["draws"]) == 2
    assert not ambiguous_fixture["frames"][1]["draws"]
    assert ambiguous_manifest["coverage"]["skipped_draws"]["ambiguous_geometry_key"] == 2

    gap_capture, gap_shaders = _synthetic()
    for draw in gap_capture["draws"][2:]:
        draw["frame"] = 12
    for resource in gap_capture["mesh_buffers"][3:]:
        resource["frame"] = 12
    try:
        extract(gap_capture, gap_shaders, 2, verify_shader_hash=False)
        raise AssertionError("non-consecutive replay frames accepted")
    except ExportError as exc:
        assert str(exc) == "selected frames are not consecutive"

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        output = root / "written" / "fixture.bin"
        write_output(output, data, manifest)
        assert output.read_bytes() == data
        assert json.loads(output.with_suffix(".json").read_text(encoding="utf-8"))["format"] == "EDVRIFR1"
        dry_target = root / "dry" / "fixture.bin"
        assert not dry_target.parent.exists()
        write_output(dry_target, data, manifest, dry_run=True)
        assert not dry_target.parent.exists()
    print("identity_fusion_capture: self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", help="drawstate capture to export")
    parser.add_argument("--output", help="write EDVRIFR1 binary here (manifest uses .json)")
    parser.add_argument("--frames", type=int, default=2, help="adjacent eligible frames (default: 2)")
    parser.add_argument("--dry-run", action="store_true", help="validate and summarize without writing anything")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        if args.capture or args.output or args.dry_run or args.frames != 2:
            parser.error("--self-test must be used alone")
        self_test(); return 0
    if not args.capture:
        parser.error("capture is required unless --self-test is used")
    try:
        summary, _ = export_capture(args.capture, args.output, args.frames, args.dry_run)
    except (OSError, ValueError, struct.error) as exc:
        parser.exit(1, "identity_fusion_capture: %s\n" % exc)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
