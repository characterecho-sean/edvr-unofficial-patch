#!/usr/bin/env python3
"""Validate and analyse an EDVR object-classification capture.

The input is ``classification_<stamp>.json``.  Its named binary sibling is
read without modification.  Visible ownership is reported only for a valid
MeshCoverage record whose stored depth exactly equals SceneZ and whose exact-
frame Mesh bytes agree with the draw-generation ID and t33 snapshots.
"""

import argparse
from collections import Counter, defaultdict
import copy
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile


SCHEMA = "edvr_object_classification_v1"
MESH_STRIDE = 240
POOL_STRIDE = 336
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_BINARY_BYTES = 256 * 1024 * 1024
MAX_ITEMS = 1_000_000
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
KIND_NAMES = {1: "map", 3: "update", 4: "copy_resource", 5: "copy_region", 6: "unknown"}
ROLE_NAMES = {"t33_pool", "ia_ids", "provenance_source", "copy_source", "staged_input"}
BUFFER_STATUSES = {
    "available", "unsafe_uav_or_stream_output", "not_buffer", "buffer_size_cap",
    "buffer_budget", "staging_create_failed", "bin_open_failed", "readback_failed",
    "bin_write_failed", "mapped_write_open",
}
TEXTURE_STATUSES = {
    "available", "resource_cap", "absent", "not_texture2d", "texture_shape",
    "unsupported_format", "texture_budget", "staging_create_failed",
    "bin_open_failed", "readback_failed", "mapped_row_pitch", "bin_write_failed",
}
PIXEL_BYTES = {
    1: 16, 2: 16, 3: 16, 4: 16, 10: 8, 11: 8, 12: 8, 13: 8,
    16: 8, 17: 8, 18: 8, 19: 8, 20: 8,
    24: 4, 25: 4, 26: 4, 27: 4, 28: 4, 29: 4, 30: 4,
    34: 4, 35: 4, 36: 4, 37: 4, 38: 4, 39: 4, 40: 4,
    41: 4, 42: 4, 43: 4, 44: 4, 45: 4, 46: 4, 47: 4,
    48: 2, 49: 2, 50: 2, 51: 2, 52: 2, 53: 2, 54: 2, 55: 2,
    60: 1, 61: 1, 62: 1, 63: 1, 64: 1, 65: 1,
}
DEPTH_FORMATS = {19, 20, 39, 40, 41, 44, 45, 53, 55}
HEX_ID = re.compile(r"^0x[0-9a-fA-F]+$")
HEX_RVA = re.compile(r"^0x[0-9a-fA-F]+$")


class CaptureError(ValueError):
    pass


def _object_no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise CaptureError("duplicate JSON member %r" % key)
        result[key] = value
    return result


def _dict(value, name):
    if not isinstance(value, dict):
        raise CaptureError("%s must be an object" % name)
    return value


def _list(value, name, maximum=MAX_ITEMS):
    if not isinstance(value, list) or len(value) > maximum:
        raise CaptureError("%s must be a bounded array" % name)
    return value


def _uint(value, name, maximum=UINT64_MAX):
    if type(value) is not int or value < 0 or value > maximum:
        raise CaptureError("%s must be an unsigned integer" % name)
    return value


def _u32(value, name):
    return _uint(value, name, UINT32_MAX)


def _boolean(value, name):
    if type(value) is not bool:
        raise CaptureError("%s must be boolean" % name)
    return value


def _string(value, name, maximum=256):
    if not isinstance(value, str) or len(value) > maximum:
        raise CaptureError("%s must be a bounded string" % name)
    return value


def _nullable_ref(value, name, count):
    if value is None:
        return None
    result = _u32(value, name)
    if result >= count:
        raise CaptureError("%s references missing item %u" % (name, result))
    return result


def _sequential_id(item, index, name):
    if _u32(item.get("id"), "%s[%u].id" % (name, index)) != index:
        raise CaptureError("%s IDs must be dense and ordered" % name)


def _read_json(path):
    path = Path(path)
    try:
        size = path.stat().st_size
        if size > MAX_JSON_BYTES:
            raise CaptureError("%s: JSON exceeds %u-byte bound" % (path, MAX_JSON_BYTES))
        text = path.read_text(encoding="utf-8")
        value = json.loads(text, object_pairs_hook=_object_no_duplicates)
    except CaptureError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CaptureError("%s: invalid capture JSON: %s" % (path, exc)) from exc
    return _dict(value, "capture root")


def _validate_resources(root):
    resources = _list(root.get("resources"), "resources", 64)
    for i, item in enumerate(resources):
        item = _dict(item, "resources[%u]" % i)
        _sequential_id(item, i, "resources")
        identity = _string(item.get("identity"), "resources[%u].identity" % i, 34)
        if not HEX_ID.fullmatch(identity):
            raise CaptureError("resources[%u].identity is not hexadecimal" % i)
        dimension = _u32(item.get("dimension"), "resources[%u].dimension" % i)
        roles = _list(item.get("roles"), "resources[%u].roles" % i, 5)
        if any(role not in ROLE_NAMES for role in roles) or len(set(roles)) != len(roles):
            raise CaptureError("resources[%u].roles is invalid" % i)
        for field in ("safe_association", "unobserved_prior_write", "unmatched"):
            _boolean(item.get(field), "resources[%u].%s" % (i, field))
        _uint(item.get("generation"), "resources[%u].generation" % i)
        if dimension == 1:
            b = _dict(item.get("buffer"), "resources[%u].buffer" % i)
            for field in ("bytes", "usage", "bind_flags", "cpu_access", "misc_flags", "stride"):
                _u32(b.get(field), "resources[%u].buffer.%s" % (i, field))
            if not b["bytes"]:
                raise CaptureError("resources[%u] has a zero-sized buffer" % i)
            if "t33_pool" in roles and (b["stride"] != POOL_STRIDE or b["bytes"] % POOL_STRIDE):
                raise CaptureError("resources[%u] t33 pool has incompatible size/stride" % i)
        elif dimension == 3:
            t = _dict(item.get("texture2d"), "resources[%u].texture2d" % i)
            for field in ("width", "height", "mips", "array", "format", "sample_count", "bind_flags"):
                _u32(t.get(field), "resources[%u].texture2d.%s" % (i, field))
            if not t["width"] or not t["height"] or not t["mips"] or not t["array"] or not t["sample_count"]:
                raise CaptureError("resources[%u] has invalid texture dimensions" % i)
        else:
            if "buffer" in item or "texture2d" in item:
                raise CaptureError("resources[%u] descriptor disagrees with dimension" % i)
    return resources


def _validate_stacks(root, image_size):
    stacks = _list(root.get("stacks"), "stacks", 128)
    for i, item in enumerate(stacks):
        item = _dict(item, "stacks[%u]" % i)
        _sequential_id(item, i, "stacks")
        captured = _u32(item.get("captured"), "stacks[%u].captured" % i)
        game_frames = _u32(item.get("game_frames"), "stacks[%u].game_frames" % i)
        if captured > 32 or game_frames > captured:
            raise CaptureError("stacks[%u] frame counts are invalid" % i)
        rvas = _string(item.get("rvas"), "stacks[%u].rvas" % i, 511)
        parts = [] if not rvas else rvas.split("/")
        if len(parts) != game_frames or any(not HEX_RVA.fullmatch(part) for part in parts):
            raise CaptureError("stacks[%u].rvas disagrees with game_frames" % i)
        if any(int(part, 16) >= image_size for part in parts) and image_size:
            raise CaptureError("stacks[%u] RVA lies outside executable image" % i)
    return stacks


def _validate_events(root, resources, stacks):
    events = _list(root.get("events"), "events", 512)
    seen = set()
    last_generation = defaultdict(int)
    for i, item in enumerate(events):
        item = _dict(item, "events[%u]" % i)
        _sequential_id(item, i, "events")
        resource = _nullable_ref(item.get("resource"), "events[%u].resource" % i, len(resources))
        if resource is None:
            raise CaptureError("events[%u].resource cannot be null" % i)
        kind = _u32(item.get("kind_id"), "events[%u].kind_id" % i)
        if kind not in KIND_NAMES or item.get("kind") != KIND_NAMES[kind]:
            raise CaptureError("events[%u] has inconsistent write kind" % i)
        generation = _uint(item.get("generation"), "events[%u].generation" % i)
        if not generation or generation > resources[resource]["generation"]:
            raise CaptureError("events[%u] generation is outside its resource" % i)
        if generation <= last_generation[resource] or (resource, generation) in seen:
            raise CaptureError("resource event generations must increase without duplicates")
        seen.add((resource, generation))
        last_generation[resource] = generation
        _u32(item.get("mesh_frame"), "events[%u].mesh_frame" % i)
        context = _string(item.get("context"), "events[%u].context" % i, 34)
        source_identity = _string(item.get("source_identity"), "events[%u].source_identity" % i, 34)
        if not HEX_ID.fullmatch(context) or not HEX_ID.fullmatch(source_identity):
            raise CaptureError("events[%u] has invalid context/source identity" % i)
        source = _nullable_ref(item.get("source_resource"), "events[%u].source_resource" % i, len(resources))
        source_generation = _uint(item.get("source_generation"), "events[%u].source_generation" % i)
        source_matched = _boolean(item.get("source_matched"), "events[%u].source_matched" % i)
        if source is None:
            if source_generation or source_matched:
                raise CaptureError("events[%u] matches an absent source" % i)
        elif source_generation > resources[source]["generation"]:
            raise CaptureError("events[%u] source generation is outside resource" % i)
        if source is not None and source_identity != resources[source]["identity"]:
            raise CaptureError("events[%u] source identity disagrees with source resource" % i)
        first = _uint(item.get("first"), "events[%u].first" % i)
        end = _uint(item.get("end"), "events[%u].end" % i)
        if first > end:
            raise CaptureError("events[%u] has reversed write range" % i)
        _u32(item.get("subresource"), "events[%u].subresource" % i)
        map_type = _u32(item.get("map_type"), "events[%u].map_type" % i)
        if (kind == 1 and map_type not in (2, 3, 4, 5)) or (kind != 1 and map_type):
            raise CaptureError("events[%u].map_type disagrees with write kind" % i)
        completion_status = _string(item.get("completion_status"), "events[%u].completion_status" % i, 32)
        completion_values = {"open", "matched", "generation_changed", "duplicate_map",
                             "pending_map_cap", "unmatched_unmap", "not_applicable"}
        if completion_status not in completion_values:
            raise CaptureError("events[%u].completion_status is unknown" % i)
        completion_frame = _u32(item.get("completion_frame"), "events[%u].completion_frame" % i)
        completion_stack = _nullable_ref(item.get("completion_stack"), "events[%u].completion_stack" % i, len(stacks))
        if kind == 1:
            if completion_status == "not_applicable":
                raise CaptureError("events[%u] Map lacks completion status" % i)
            if completion_status == "matched" and completion_stack is None:
                raise CaptureError("events[%u] matched Map lacks completion caller" % i)
        elif completion_status != "not_applicable" or completion_frame or completion_stack is not None:
            raise CaptureError("events[%u] non-Map has Map completion metadata" % i)
        _boolean(item.get("mapped"), "events[%u].mapped" % i)
        complete = _boolean(item.get("complete"), "events[%u].complete" % i)
        if kind == 1 and complete != (completion_status == "matched"):
            raise CaptureError("events[%u] Map completion flag/status disagree" % i)
        _nullable_ref(item.get("stack"), "events[%u].stack" % i, len(stacks))
    by_version = {(e["resource"], e["generation"]): e for e in events}
    for i, event in enumerate(events):
        if event["source_matched"] and (event["source_resource"], event["source_generation"]) not in by_version:
            raise CaptureError("events[%u] claims a source version with no stored event" % i)
    return events, by_version


def _validate_blobs(root, resources):
    blobs = _list(root.get("blobs"), "blobs", 1024)
    spans = []
    for i, item in enumerate(blobs):
        item = _dict(item, "blobs[%u]" % i)
        _sequential_id(item, i, "blobs")
        _string(item.get("name"), "blobs[%u].name" % i, 64)
        status = _string(item.get("status"), "blobs[%u].status" % i, 64)
        texture = _boolean(item.get("texture"), "blobs[%u].texture" % i)
        if status not in (TEXTURE_STATUSES if texture else BUFFER_STATUSES):
            raise CaptureError("blobs[%u] has unknown capture status %r" % (i, status))
        source = _nullable_ref(item.get("source_resource"), "blobs[%u].source_resource" % i, len(resources))
        generation = _uint(item.get("generation"), "blobs[%u].generation" % i)
        _uint(item.get("foreign_epoch"), "blobs[%u].foreign_epoch" % i)
        if source is not None and generation > resources[source]["generation"]:
            raise CaptureError("blobs[%u] generation is outside source resource" % i)
        for field in ("mesh_frame", "subresource_or_record_count", "format", "width", "height", "row_bytes"):
            _u32(item.get(field), "blobs[%u].%s" % (i, field))
        offset = _uint(item.get("offset"), "blobs[%u].offset" % i)
        size = _uint(item.get("bytes"), "blobs[%u].bytes" % i)
        if offset > MAX_BINARY_BYTES or size > MAX_BINARY_BYTES or offset + size > MAX_BINARY_BYTES:
            raise CaptureError("blobs[%u] binary span exceeds safety bound" % i)
        if status == "available":
            if source is None or not size:
                raise CaptureError("blobs[%u] available payload lacks source or bytes" % i)
            spans.append((offset, offset + size, i))
        elif size:
            raise CaptureError("blobs[%u] missing payload declares bytes" % i)
        if texture:
            if status == "available":
                if not item["width"] or not item["height"] or item["format"] not in PIXEL_BYTES:
                    raise CaptureError("blobs[%u] has invalid texture metadata" % i)
                expected_row = item["width"] * PIXEL_BYTES[item["format"]]
                if item["row_bytes"] != expected_row or size != expected_row * item["height"]:
                    raise CaptureError("blobs[%u] texture extent disagrees with payload" % i)
                desc = resources[source].get("texture2d")
                if not desc or any(desc[k] != item[k] for k in ("width", "height", "format")):
                    raise CaptureError("blobs[%u] texture metadata disagrees with resource" % i)
                if desc["array"] != 1 or desc["sample_count"] != 1:
                    raise CaptureError("blobs[%u] source texture is not a single non-MSAA image" % i)
        elif status == "available":
            if any(item[field] for field in ("format", "width", "height", "row_bytes")):
                raise CaptureError("blobs[%u] buffer carries texture metadata" % i)
            desc = resources[source].get("buffer")
            if not desc or size != desc["bytes"]:
                raise CaptureError("blobs[%u] payload size disagrees with buffer" % i)
    spans.sort()
    cursor = 0
    for begin, end, index in spans:
        if begin != cursor:
            reason = "overlaps" if begin < cursor else "leaves an unaccounted gap"
            raise CaptureError("blobs[%u] %s in packed binary" % (index, reason))
        cursor = end
    return blobs, cursor


def _validate_snapshots(root, resources, blobs):
    snapshots = _list(root.get("snapshots"), "snapshots", 1024)
    versions = set()
    for i, item in enumerate(snapshots):
        item = _dict(item, "snapshots[%u]" % i)
        _sequential_id(item, i, "snapshots")
        resource = _nullable_ref(item.get("resource"), "snapshots[%u].resource" % i, len(resources))
        blob = _nullable_ref(item.get("blob"), "snapshots[%u].blob" % i, len(blobs))
        if resource is None or blob is None:
            raise CaptureError("snapshots[%u] has a null resource/blob" % i)
        generation = _uint(item.get("generation"), "snapshots[%u].generation" % i)
        foreign_epoch = _uint(item.get("foreign_epoch"), "snapshots[%u].foreign_epoch" % i)
        frame = _u32(item.get("mesh_frame"), "snapshots[%u].mesh_frame" % i)
        if generation > resources[resource]["generation"]:
            raise CaptureError("snapshots[%u] generation is outside resource" % i)
        source_blob = blobs[blob]
        if (source_blob["source_resource"] != resource or source_blob["generation"] != generation or
                source_blob["foreign_epoch"] != foreign_epoch or source_blob["mesh_frame"] != frame):
            raise CaptureError("snapshots[%u] metadata disagrees with its blob" % i)
        version = (resource, generation, foreign_epoch, frame)
        if version in versions:
            raise CaptureError("duplicate resource-generation-frame snapshot")
        versions.add(version)
    return snapshots


def _validate_draws(root, resources, snapshots, selected_frame):
    draws = _list(root.get("draws"), "draws", 512)
    ranges = []
    for i, item in enumerate(draws):
        item = _dict(item, "draws[%u]" % i)
        _sequential_id(item, i, "draws")
        frame = _u32(item.get("mesh_frame"), "draws[%u].mesh_frame" % i)
        eye = _u32(item.get("eye"), "draws[%u].eye" % i)
        first = _u32(item.get("first_record"), "draws[%u].first_record" % i)
        instances = _u32(item.get("instances"), "draws[%u].instances" % i)
        offset = _u32(item.get("id_byte_offset"), "draws[%u].id_byte_offset" % i)
        if frame != selected_frame or eye != 0 or not instances or first + instances > UINT32_MAX or offset % 8:
            raise CaptureError("draws[%u] has invalid selected-frame range/eye/ID alignment" % i)
        ranges.append((first, first + instances, i))
        present = _boolean(item.get("key_present"), "draws[%u].key_present" % i)
        key = _list(item.get("key16"), "draws[%u].key16" % i, 16)
        if len(key) != 16:
            raise CaptureError("draws[%u].key16 must contain 16 words" % i)
        for k, value in enumerate(key):
            _u32(value, "draws[%u].key16[%u]" % (i, k))
        if not present and any(key):
            raise CaptureError("draws[%u] has key words while key_present is false" % i)
        shader_hash = _string(item.get("vertex_shader_hash"), "draws[%u].vertex_shader_hash" % i, 34)
        if not HEX_ID.fullmatch(shader_hash):
            raise CaptureError("draws[%u].vertex_shader_hash is not hexadecimal" % i)
        for prefix, role in (("pool", "t33_pool"), ("id", "ia_ids")):
            resource = _nullable_ref(item.get(prefix + "_resource"), "draws[%u].%s_resource" % (i, prefix), len(resources))
            snapshot = _nullable_ref(item.get(prefix + "_snapshot"), "draws[%u].%s_snapshot" % (i, prefix), len(snapshots))
            generation = _uint(item.get(prefix + "_generation"), "draws[%u].%s_generation" % (i, prefix))
            observed = _boolean(item.get(prefix + "_write_observed"), "draws[%u].%s_write_observed" % (i, prefix))
            matched = _boolean(item.get(prefix + "_matched"), "draws[%u].%s_matched" % (i, prefix))
            if matched and not observed:
                raise CaptureError("draws[%u] matches an unobserved %s write" % (i, prefix))
            if (resource is None) != (snapshot is None):
                raise CaptureError("draws[%u] has incomplete %s snapshot reference" % (i, prefix))
            if resource is None:
                if generation:
                    raise CaptureError("draws[%u] has generation for absent %s" % (i, prefix))
                if observed or matched:
                    raise CaptureError("draws[%u] observes absent %s resource" % (i, prefix))
                continue
            if role not in resources[resource]["roles"]:
                raise CaptureError("draws[%u] %s resource lacks role" % (i, prefix))
            if prefix == "id" and offset + instances * 8 > resources[resource]["buffer"]["bytes"]:
                raise CaptureError("draws[%u] ID window exceeds resource descriptor" % i)
            snap = snapshots[snapshot]
            if snap["resource"] != resource or snap["generation"] != generation or snap["mesh_frame"] != frame:
                raise CaptureError("draws[%u] %s snapshot generation/frame mismatch" % (i, prefix))
    ranges.sort()
    for previous, current in zip(ranges, ranges[1:]):
        if current[0] < previous[1]:
            raise CaptureError("draw record ranges overlap")
    return draws


def load_capture(path):
    path = Path(path)
    root = _read_json(path)
    if root.get("schema") != SCHEMA:
        raise CaptureError("unsupported object-classification schema")
    binary_name = _string(root.get("binary"), "binary", 255)
    if not binary_name or Path(binary_name).name != binary_name:
        raise CaptureError("binary must be a leaf filename")
    executable = _dict(root.get("executable"), "executable")
    _u32(executable.get("pe_timestamp"), "executable.pe_timestamp")
    image_size = _u32(executable.get("image_size"), "executable.image_size")
    selection = _dict(root.get("selection"), "selection")
    for field in ("discovery_mesh_frame", "selected_mesh_frame", "scene_frame"):
        _u32(selection.get(field), "selection.%s" % field)
    has_discovery = _boolean(selection.get("has_discovery"), "selection.has_discovery")
    has_selection = _boolean(selection.get("has_selection"), "selection.has_selection")
    sealed = _boolean(selection.get("sealed"), "selection.sealed")
    _boolean(selection.get("missed_window"), "selection.missed_window")
    if sealed and (not has_discovery or not has_selection):
        raise CaptureError("sealed selection lacks discovery/selection")
    limits = _dict(root.get("limits"), "limits")
    for field in ("resources", "writes", "draws", "stacks", "buffer_bytes", "texture_bytes", "single_buffer_bytes"):
        _uint(limits.get(field), "limits.%s" % field)
    if limits["resources"] > 64 or limits["writes"] > 512 or limits["draws"] > 512 or limits["stacks"] > 128:
        raise CaptureError("capture limits exceed reader safety bounds")
    summary = _dict(root.get("summary"), "summary")
    status = _string(summary.get("status"), "summary.status", 64)
    if status not in {"sealed", "selection_window_missed", "stage_missing", "no_selected_frame"}:
        raise CaptureError("unknown summary.status")
    _boolean(summary.get("binary_ok"), "summary.binary_ok")
    count_fields = {
        "resources": "resources", "events_stored": "events", "draws": "draws",
        "snapshots": "snapshots", "blobs": "blobs", "stack_samples": "stacks",
    }
    for field in ("writes_observed", "resource_overflow", "write_overflow", "draw_overflow",
                  "stack_overflow", "snapshot_overflow", "buffer_budget_declines",
                  "texture_budget_declines", "unsafe_associations", "copy_failures",
                  "map_failures", "unmatched_writes", "unobserved_sources", "foreign_writes",
                  "ignored_eye", "ignored_frame",
                  "stage_rejects", "suppressed_recursive", "duplicate_maps",
                  "unmatched_unmaps", "pending_map_overflow", "open_map_snapshots"):
        _uint(summary.get(field), "summary.%s" % field)
    cpu_provenance = _boolean(summary.get("cpu_provenance_available"), "summary.cpu_provenance_available")
    if cpu_provenance != (summary["foreign_writes"] == 0):
        raise CaptureError("summary.cpu_provenance_available disagrees with foreign_writes")
    resources = _validate_resources(root)
    stacks = _validate_stacks(root, image_size)
    events, event_by_version = _validate_events(root, resources, stacks)
    blobs, packed_bytes = _validate_blobs(root, resources)
    snapshots = _validate_snapshots(root, resources, blobs)
    draws = _validate_draws(root, resources, snapshots, selection["selected_mesh_frame"])
    for i, snapshot in enumerate(snapshots):
        if snapshot["foreign_epoch"] > summary["foreign_writes"]:
            raise CaptureError("snapshots[%u].foreign_epoch exceeds observed foreign writes" % i)
    for i, blob in enumerate(blobs):
        if blob["foreign_epoch"] > summary["foreign_writes"]:
            raise CaptureError("blobs[%u].foreign_epoch exceeds observed foreign writes" % i)
    if not cpu_provenance and any(event["source_matched"] for event in events):
        raise CaptureError("source_matched cannot survive foreign-context writes")
    for i, draw in enumerate(draws):
        for prefix in ("pool", "id"):
            observed = draw[prefix + "_write_observed"]
            matched = draw[prefix + "_matched"]
            generation = draw[prefix + "_generation"]
            if observed != (generation != 0):
                raise CaptureError("draws[%u] %s observed flag disagrees with generation" % (i, prefix))
            if matched and (not cpu_provenance or (draw[prefix + "_resource"], generation) not in event_by_version):
                raise CaptureError("draws[%u] %s matched flag lacks stored current-generation provenance" % (i, prefix))
    actual = {"resources": resources, "events": events, "draws": draws,
              "snapshots": snapshots, "blobs": blobs, "stacks": stacks}
    for field, array_name in count_fields.items():
        if _uint(summary.get(field), "summary.%s" % field) != len(actual[array_name]):
            raise CaptureError("summary.%s disagrees with %s array" % (field, array_name))
    if summary["writes_observed"] < len(events):
        raise CaptureError("summary.writes_observed is less than stored events")
    expected_status = "sealed" if sealed else ("selection_window_missed" if selection["missed_window"] else ("stage_missing" if has_selection else "no_selected_frame"))
    if status != expected_status:
        raise CaptureError("summary.status disagrees with selection")
    binary_path = path.with_name(binary_name)
    try:
        if binary_path.exists():
            if binary_path.stat().st_size > MAX_BINARY_BYTES:
                raise CaptureError("binary exceeds reader safety bound")
            binary = binary_path.read_bytes()
        else:
            binary = None
    except OSError as exc:
        raise CaptureError("%s: %s" % (binary_path, exc)) from exc
    if packed_bytes:
        if binary is None:
            raise CaptureError("available blob payload is missing")
        if len(binary) != packed_bytes:
            raise CaptureError("binary length does not match packed available blobs")
    elif binary is not None and len(binary):
        raise CaptureError("binary has bytes but no available blob spans")
    elif binary is None and summary["binary_ok"]:
        raise CaptureError("successful capture is missing its declared binary")
    return {
        "path": path, "binary_path": binary_path, "binary": binary or b"", "root": root,
        "selection": selection, "summary": summary, "resources": resources,
        "stacks": stacks, "events": events, "event_by_version": event_by_version,
        "snapshots": snapshots, "draws": draws, "blobs": blobs,
    }


def _blob_bytes(capture, blob):
    if blob["status"] != "available":
        return None
    begin, size = blob["offset"], blob["bytes"]
    return memoryview(capture["binary"])[begin:begin + size]


def _named_blob(capture, name):
    matches = [blob for blob in capture["blobs"] if blob["name"] == name]
    if len(matches) > 1:
        raise CaptureError("multiple %s blobs" % name)
    return matches[0] if matches else None


def _snapshot_payload(capture, reference, label):
    if reference is None:
        return None, "%s resource was not retained" % label
    snap = capture["snapshots"][reference]
    blob = capture["blobs"][snap["blob"]]
    payload = _blob_bytes(capture, blob)
    if payload is None:
        return None, "%s capture declined: %s" % (label, blob["status"])
    return payload, None


def _decode_depth(blob, payload):
    fmt = blob["format"]
    count = blob["width"] * blob["height"]
    if fmt in (39, 40, 41):
        values = struct.unpack("<%df" % count, payload)
    elif fmt in (19, 20):
        pairs = struct.unpack("<%df" % (count * 2), payload)
        values = pairs[::2]
    elif fmt in (44, 45):
        packed = struct.unpack("<%dI" % count, payload)
        values = tuple((value & 0xffffff) / 16777215.0 for value in packed)
    elif fmt in (53, 55):
        packed = struct.unpack("<%dH" % count, payload)
        values = tuple(value / 65535.0 for value in packed)
    else:
        raise CaptureError("scene_depth has unsupported DXGI format %u" % fmt)
    if any(not math.isfinite(value) for value in values):
        raise CaptureError("scene_depth contains a non-finite value")
    return values


def _caller_ref(capture, ref):
    if ref is None:
        return {"available": False, "reason": "stack_not_stored"}
    stack = capture["stacks"][ref]
    return {"available": bool(stack["game_frames"]), "stack": ref,
            "captured_frames": stack["captured"], "game_frames": stack["game_frames"],
            "game_rvas": [] if not stack["rvas"] else stack["rvas"].split("/"),
            "reason": None if stack["game_frames"] else "no_game_frames"}


def _caller(capture, event):
    return _caller_ref(capture, event["stack"])


def _range_relation(event, wanted):
    if event["kind_id"] == 1:
        return "unknown_map_dirty_extent"
    if wanted is None:
        return "not_projectable_to_source"
    first, end = wanted
    write_first, write_end = event["first"], event["end"]
    if write_first <= first and end <= write_end:
        return "includes"
    if write_end <= first or end <= write_first:
        return "disjoint"
    return "partial"


def _trace_upload_route(capture, resource, generation, wanted):
    if not capture["summary"]["cpu_provenance_available"]:
        return {"status": "unmatched", "reason": "foreign_context_writes_observed",
                "resource": resource, "generation": generation, "chain": [],
                "caller": {"available": False, "reason": "foreign_context_writes_observed"}}
    chain = []
    visited = set()
    current = (resource, generation)
    while True:
        if current in visited:
            raise CaptureError("provenance source chain contains a cycle")
        visited.add(current)
        event = capture["event_by_version"].get(current)
        if event is None:
            res = capture["resources"][current[0]]
            reason = "unobserved_prior_write" if current[1] == 0 and res["unobserved_prior_write"] else "generation_event_not_stored"
            return {"status": "missing", "reason": reason, "resource": current[0],
                    "generation": current[1], "chain": chain, "caller": {"available": False, "reason": reason}}
        step = {"event": event["id"], "resource": event["resource"],
                "generation": event["generation"], "kind": event["kind"],
                "mesh_frame": event["mesh_frame"], "first": event["first"],
                "end": event["end"], "complete": event["complete"],
                "map_type": event["map_type"],
                "requested_range_relation": _range_relation(event, wanted)}
        if event["kind_id"] == 1:
            step["completion_status"] = event["completion_status"]
            step["completion_frame"] = event["completion_frame"]
            step["completion_caller"] = _caller_ref(capture, event["completion_stack"])
        chain.append(step)
        if event["kind_id"] == 6:
            return {"status": "unmatched", "reason": "unknown_write", "resource": resource,
                    "generation": generation, "chain": chain, "caller": _caller(capture, event)}
        if event["kind_id"] in (1, 3):
            status = "matched" if event["complete"] else "incomplete"
            return {"status": status, "reason": None if event["complete"] else "map_not_completed",
                    "resource": resource, "generation": generation, "endpoint_event": event["id"],
                    "endpoint_kind": event["kind"], "chain": chain, "caller": _caller(capture, event)}
        if event["source_resource"] is None:
            return {"status": "missing", "reason": "copy_source_not_retained", "resource": resource,
                    "generation": generation, "chain": chain, "caller": _caller(capture, event)}
        if not event["source_matched"]:
            return {"status": "unmatched", "reason": "copy_source_generation_unmatched",
                    "resource": resource, "generation": generation, "chain": chain,
                    "copy_caller": _caller(capture, event),
                    "source_resource": event["source_resource"], "source_generation": event["source_generation"],
                    "caller": {"available": False, "reason": "source_generation_unmatched"}}
        wanted = wanted if event["kind_id"] == 4 else None
        current = (event["source_resource"], event["source_generation"])


def _pool_mesh_check(record, pool_record, draw_index, record_index):
    comparisons = (
        (record[64:68], pool_record[28:32], "word28"),
        (record[68:72], pool_record[320:324], "word320"),
        (record[72:80], b"\0" * 8, "reserved metadata"),
        (record[80:104], pool_record[4:28], "pose"),
    )
    for actual, expected, label in comparisons:
        if bytes(actual) != bytes(expected):
            raise CaptureError("draw %u record %u Mesh/%s does not match exact t33 snapshot" % (draw_index, record_index, label))


def analyse(capture, include_records=False):
    selection = capture["selection"]
    report = {
        "schema": 1,
        "capture": str(capture["path"]),
        "status": capture["summary"]["status"],
        "selection": selection,
        "limitations": [
            "Ownership is same-frame source association; valid-rigid and history-matched are reported separately and do not gate it.",
            "A raster sample owns a record only when MeshCoverage has a valid integer index and SceneZ equals its stored coverage depth exactly.",
            "Pool slots are interpreted only from the same draw-generation snapshot; they are never identities across frames.",
            "Upload-route provenance is hook-observed and bounded; missing or unmatched generations stay explicit.",
            "A matched Map/Unmap or Update route does not prove which CPU operation initialized an individual record; byte-range relation is reported separately.",
            "The report classifies captured metadata and does not infer that an object is static from absent motion.",
        ],
        "counts": {"draws": len(capture["draws"]), "mesh_records": None,
                   "pixels": None, "coverage_pixels": None, "depth_agreeing_pixels": None,
                   "owned_visible_pixels": None, "owned_visible_records": None,
                   "valid_rigid_owned_pixels": None, "valid_rigid_owned_records": None,
                   "history_matched_owned_pixels": None, "history_matched_owned_records": None,
                   "metadata_groups": None},
        "groups": [], "missing": [],
    }
    if not capture["summary"]["binary_ok"]:
        report["missing"].append({"scope": "capture", "reason": "binary_write_not_confirmed"})
        if include_records:
            report["records"] = []
        return report
    if not selection["sealed"]:
        report["missing"].append({"scope": "capture", "reason": capture["summary"]["status"]})
        if include_records:
            report["records"] = []
        return report

    mesh_blob = _named_blob(capture, "mesh_records")
    coverage_blob = _named_blob(capture, "mesh_coverage")
    scene_blob = _named_blob(capture, "scene_depth")
    for name, blob in (("mesh_records", mesh_blob), ("mesh_coverage", coverage_blob), ("scene_depth", scene_blob)):
        if blob is None:
            report["missing"].append({"scope": name, "reason": "blob_not_listed"})
        elif blob["status"] != "available":
            report["missing"].append({"scope": name, "reason": blob["status"]})
    if any(blob is None or blob["status"] != "available" for blob in (mesh_blob, coverage_blob, scene_blob)):
        if include_records:
            report["records"] = []
        return report

    frame = selection["selected_mesh_frame"]
    if any(blob["mesh_frame"] != frame for blob in (mesh_blob, coverage_blob, scene_blob)):
        raise CaptureError("stage blobs do not belong to selected_mesh_frame")
    mesh_payload = _blob_bytes(capture, mesh_blob)
    count = mesh_blob["subresource_or_record_count"]
    if count > 512 or count * MESH_STRIDE > len(mesh_payload):
        raise CaptureError("mesh_records count exceeds captured buffer")
    mesh_resource = capture["resources"][mesh_blob["source_resource"]]
    mesh_desc = mesh_resource.get("buffer")
    if not mesh_desc or mesh_desc["stride"] != MESH_STRIDE or mesh_desc["bytes"] % MESH_STRIDE:
        raise CaptureError("mesh_records source has incompatible stride/size")
    records = [mesh_payload[i * MESH_STRIDE:(i + 1) * MESH_STRIDE] for i in range(count)]
    report["counts"]["mesh_records"] = count

    ranges = []
    owner_draw = [None] * count
    for draw in capture["draws"]:
        first, end = draw["first_record"], draw["first_record"] + draw["instances"]
        if end > count:
            raise CaptureError("draw %u record range exceeds mesh_records count" % draw["id"])
        ranges.append((first, end, draw["id"]))
    ranges.sort()
    previous_end = 0
    for index, (first, end, draw_id) in enumerate(ranges):
        if index and first < previous_end:
            raise CaptureError("draw record ranges overlap")
        previous_end = end
        for record_index in range(first, end):
            owner_draw[record_index] = draw_id
    if any(draw_id is None for draw_id in owner_draw):
        raise CaptureError("mesh_records contains a record outside all captured draw ranges")

    joins = [None] * count
    for record_index, draw_id in enumerate(owner_draw):
        draw = capture["draws"][draw_id]
        local = record_index - draw["first_record"]
        record = records[record_index]
        words = struct.unpack_from("<32I", record)
        valid, matched = struct.unpack_from("<f", record, 224)[0], struct.unpack_from("<f", record, 236)[0]
        if valid not in (0.0, 1.0) or matched not in (0.0, 1.0):
            raise CaptureError("mesh record %u has invalid validity/match flags" % record_index)
        if words[29] != draw["first_record"] or words[30] != draw["instances"]:
            raise CaptureError("mesh record %u draw metadata disagrees with draw %u" % (record_index, draw_id))
        if not draw["key_present"]:
            raise CaptureError("draw %u lacks key16 evidence" % draw_id)
        if tuple(struct.unpack_from("<16I", record)) != tuple(draw["key16"]):
            raise CaptureError("draw %u key16 does not match mesh record %u" % (draw_id, record_index))
        ids, ids_missing = _snapshot_payload(capture, draw["id_snapshot"], "draw_ids")
        pool, pool_missing = _snapshot_payload(capture, draw["pool_snapshot"], "draw_pool")
        for scope, reason in (("draw %u IA IDs" % draw_id, ids_missing), ("draw %u t33 pool" % draw_id, pool_missing)):
            if reason and {"scope": scope, "reason": reason} not in report["missing"]:
                report["missing"].append({"scope": scope, "reason": reason})
        if ids is None or pool is None:
            continue
        id_at = draw["id_byte_offset"] + local * 8
        if id_at + 8 > len(ids):
            raise CaptureError("draw %u instance %u ID pair lies outside exact snapshot" % (draw_id, local))
        pool_slot, second_word = struct.unpack_from("<2I", ids, id_at)
        pool_at = pool_slot * POOL_STRIDE
        if pool_at + POOL_STRIDE > len(pool):
            raise CaptureError("draw %u instance %u pool slot %u lies outside exact snapshot" % (draw_id, local, pool_slot))
        if words[31] != pool_slot:
            raise CaptureError("mesh record %u pool slot disagrees with exact ID pair" % record_index)
        pool_record = pool[pool_at:pool_at + POOL_STRIDE]
        _pool_mesh_check(record, pool_record, draw_id, record_index)
        joins[record_index] = {
            "draw": draw, "local": local, "id_at": id_at, "pool_at": pool_at,
            "pool_slot": pool_slot, "second_word": second_word, "pool_record": pool_record,
            "valid_rigid": valid == 1.0, "history_matched": matched == 1.0,
        }

    coverage_payload = _blob_bytes(capture, coverage_blob)
    scene_payload = _blob_bytes(capture, scene_blob)
    if coverage_blob["format"] != 16:
        raise CaptureError("mesh_coverage must use DXGI_FORMAT_R32G32_FLOAT (16)")
    if scene_blob["format"] not in DEPTH_FORMATS:
        raise CaptureError("scene_depth uses unsupported depth format")
    if coverage_blob["width"] != scene_blob["width"] or coverage_blob["height"] != scene_blob["height"]:
        raise CaptureError("scene_depth dimensions do not match mesh_coverage")
    pixels = coverage_blob["width"] * coverage_blob["height"]
    coverage_values = struct.unpack("<%df" % (pixels * 2), coverage_payload)
    scene_values = _decode_depth(scene_blob, scene_payload)
    covered = agreeing = 0
    record_pixels = Counter()
    for pixel in range(pixels):
        raw_index, cov_z = coverage_values[pixel * 2:pixel * 2 + 2]
        if not math.isfinite(raw_index) or not math.isfinite(cov_z):
            raise CaptureError("mesh_coverage pixel %u is non-finite" % pixel)
        index = int(raw_index)
        if raw_index != float(index) or index < 0 or index > count:
            raise CaptureError("mesh_coverage pixel %u has invalid record index %r" % (pixel, raw_index))
        if not index:
            continue
        covered += 1
        if scene_values[pixel] != cov_z:
            continue
        agreeing += 1
        record_pixels[index - 1] += 1
    valid_records = {i for i in record_pixels if struct.unpack_from("<f", records[i], 224)[0] == 1.0}
    history_records = {i for i in record_pixels if struct.unpack_from("<f", records[i], 236)[0] == 1.0}
    report["counts"].update({"pixels": pixels, "coverage_pixels": covered,
                             "depth_agreeing_pixels": agreeing,
                             "owned_visible_pixels": sum(record_pixels.values()),
                             "owned_visible_records": len(record_pixels),
                             "valid_rigid_owned_pixels": sum(record_pixels[i] for i in valid_records),
                             "valid_rigid_owned_records": len(valid_records),
                             "history_matched_owned_pixels": sum(record_pixels[i] for i in history_records),
                             "history_matched_owned_records": len(history_records)})

    route_cache = {}
    groups = {}
    owners = []
    for record_index, pixel_count in sorted(record_pixels.items()):
        join = joins[record_index]
        if join is None:
            continue
        draw = join["draw"]
        draw_id, local = draw["id"], join["local"]
        id_at, pool_at = join["id_at"], join["pool_at"]
        pool_slot, second_word = join["pool_slot"], join["second_word"]
        pool_record = join["pool_record"]
        valid_rigid, history_matched = join["valid_rigid"], join["history_matched"]
        word28 = struct.unpack_from("<I", pool_record, 28)[0]
        word320 = struct.unpack_from("<I", pool_record, 320)[0]
        traces = {}
        wanted_ranges = {"pool": (pool_at, pool_at + POOL_STRIDE), "id": (id_at, id_at + 8)}
        for label in ("pool", "id"):
            version = (draw[label + "_resource"], draw[label + "_generation"])
            cache_key = version + wanted_ranges[label]
            if cache_key not in route_cache:
                route_cache[cache_key] = _trace_upload_route(capture, *version, wanted_ranges[label])
            traces[label] = route_cache[cache_key]
            traces[label]["scope"] = "resource_upload_route"
            traces[label]["record_write_attribution"] = "not_proven"
            traces[label]["target_range_relation"] = (traces[label]["chain"][0]["requested_range_relation"]
                                                         if traces[label]["chain"] else "unavailable")
        group_key = (tuple(draw["key16"]), word28, word320, second_word,
                     draw["pool_resource"], draw["pool_generation"],
                     draw["id_resource"], draw["id_generation"],
                     json.dumps(traces, sort_keys=True))
        if group_key not in groups:
            groups[group_key] = {
                "key16": list(draw["key16"]), "t33_word28": word28,
                "t33_word320": word320, "instance_second_word": second_word,
                "pixels": 0, "records": 0,
                "valid_rigid_records": 0, "history_matched_records": 0,
                "source_versions": {
                    "t33_pool": {"resource": draw["pool_resource"], "generation": draw["pool_generation"],
                                 "write_observed": draw["pool_write_observed"], "generation_matched": draw["pool_matched"],
                                 "upload_route": traces["pool"]},
                    "ia_ids": {"resource": draw["id_resource"], "generation": draw["id_generation"],
                               "write_observed": draw["id_write_observed"], "generation_matched": draw["id_matched"],
                               "upload_route": traces["id"]},
                },
            }
        groups[group_key]["pixels"] += pixel_count
        groups[group_key]["records"] += 1
        groups[group_key]["valid_rigid_records"] += int(valid_rigid)
        groups[group_key]["history_matched_records"] += int(history_matched)
        owners.append({
            "record": record_index, "draw": draw_id, "instance": local,
            "pixels": pixel_count, "pool_slot": pool_slot,
            "instance_second_word": second_word, "t33_word28": word28,
            "t33_word320": word320, "key16": list(draw["key16"]),
            "pool_generation": draw["pool_generation"], "id_generation": draw["id_generation"],
            "valid_rigid": valid_rigid, "history_matched": history_matched,
        })
    report["groups"] = list(groups.values())
    report["counts"]["metadata_groups"] = len(report["groups"])
    if include_records:
        report["records"] = owners
    return report


def print_report(report):
    counts = report["counts"]
    print("capture: %s (%s)" % (report["capture"], report["status"]))
    if counts["mesh_records"] is None:
        print("ownership: unavailable")
    else:
        print("ownership: %u exact-depth visible pixels in %u records; %u/%u covered pixels exactly agree with SceneZ" %
              (counts["owned_visible_pixels"], counts["owned_visible_records"],
               counts["depth_agreeing_pixels"], counts["coverage_pixels"]))
        print("record flags: %u valid-rigid, %u history-matched" %
              (counts["valid_rigid_owned_records"], counts["history_matched_owned_records"]))
        print("metadata: %u unique groups" % counts["metadata_groups"])
        for index, group in enumerate(report["groups"]):
            pool = group["source_versions"]["t33_pool"]["upload_route"]
            ids = group["source_versions"]["ia_ids"]["upload_route"]
            print("  group %u: %u px/%u records, t33[28]=%u t33[320]=%u id.second=%u; upload routes pool=%s ids=%s" %
                  (index, group["pixels"], group["records"], group["t33_word28"],
                   group["t33_word320"], group["instance_second_word"], pool["status"], ids["status"]))
    for item in report["missing"]:
        print("missing: %s (%s)" % (item["scope"], item["reason"]))
    print("scope: exact-frame captured evidence only; no static classification is inferred")


def _fixture_value(binary_name="classification_fixture.bin"):
    key = list(range(100, 116))
    pool = bytearray(POOL_STRIDE * 3)
    for slot in (1, 2):
        at = slot * POOL_STRIDE
        struct.pack_into("<6I", pool, at + 4, *(slot * 1000 + i for i in range(6)))
        struct.pack_into("<I", pool, at + 28, 2800 + slot)
        struct.pack_into("<I", pool, at + 320, 3200 + slot)
    ids = struct.pack("<4I", 1, 9001, 2, 9002)
    mesh = bytearray(MESH_STRIDE * 2)
    for i, slot in enumerate((1, 2)):
        at = i * MESH_STRIDE
        struct.pack_into("<16I", mesh, at, *key)
        mesh[at + 64:at + 68] = pool[slot * POOL_STRIDE + 28:slot * POOL_STRIDE + 32]
        mesh[at + 68:at + 72] = pool[slot * POOL_STRIDE + 320:slot * POOL_STRIDE + 324]
        mesh[at + 80:at + 104] = pool[slot * POOL_STRIDE + 4:slot * POOL_STRIDE + 28]
        struct.pack_into("<3I", mesh, at + 116, 0, 2, slot)
        struct.pack_into("<4f", mesh, at + 224, 1.0, 4.0, 2.0, 1.0 if i == 0 else 0.0)
    coverage_pairs = ((1.0, 0.5), (1.0, 0.5), (1.0, 0.5), (2.0, 0.25),
                      (0.0, 0.0), (0.0, 0.0), (0.0, 0.0), (0.0, 0.0))
    coverage = b"".join(struct.pack("<2f", *pair) for pair in coverage_pairs)
    next_float = struct.unpack("<f", struct.pack("<I", 0x3f000001))[0]
    scene = struct.pack("<8f", 0.5, 0.5, next_float, 0.25, 0, 0, 0, 0)
    chunks = [bytes(pool), ids, scene, coverage, bytes(mesh)]
    offsets = []
    cursor = 0
    for chunk in chunks:
        offsets.append(cursor)
        cursor += len(chunk)
    resources = [
        {"id": 0, "identity": "0x1000", "dimension": 1, "roles": ["t33_pool"], "safe_association": True, "generation": 2, "unobserved_prior_write": False, "unmatched": False,
         "buffer": {"bytes": len(pool), "usage": 0, "bind_flags": 8, "cpu_access": 0, "misc_flags": 64, "stride": 336}},
        {"id": 1, "identity": "0x2000", "dimension": 1, "roles": ["ia_ids"], "safe_association": True, "generation": 1, "unobserved_prior_write": False, "unmatched": False,
         "buffer": {"bytes": len(ids), "usage": 0, "bind_flags": 1, "cpu_access": 0, "misc_flags": 0, "stride": 0}},
        {"id": 2, "identity": "0x3000", "dimension": 1, "roles": ["provenance_source"], "safe_association": True, "generation": 1, "unobserved_prior_write": False, "unmatched": False,
         "buffer": {"bytes": len(ids), "usage": 2, "bind_flags": 0, "cpu_access": 65536, "misc_flags": 0, "stride": 0}},
        {"id": 3, "identity": "0x4000", "dimension": 3, "roles": ["provenance_source"], "safe_association": True, "generation": 0, "unobserved_prior_write": True, "unmatched": True,
         "texture2d": {"width": 4, "height": 2, "mips": 1, "array": 1, "format": 40, "sample_count": 1, "bind_flags": 8}},
        {"id": 4, "identity": "0x5000", "dimension": 3, "roles": ["provenance_source"], "safe_association": True, "generation": 0, "unobserved_prior_write": True, "unmatched": True,
         "texture2d": {"width": 4, "height": 2, "mips": 1, "array": 1, "format": 16, "sample_count": 1, "bind_flags": 8}},
        {"id": 5, "identity": "0x6000", "dimension": 1, "roles": ["provenance_source"], "safe_association": True, "generation": 0, "unobserved_prior_write": True, "unmatched": True,
         "buffer": {"bytes": len(mesh), "usage": 0, "bind_flags": 8, "cpu_access": 0, "misc_flags": 64, "stride": 240}},
    ]
    stacks = [
        {"id": 0, "captured": 3, "game_frames": 1, "rvas": "0x100"},
        {"id": 1, "captured": 4, "game_frames": 1, "rvas": "0x200"},
        {"id": 2, "captured": 5, "game_frames": 2, "rvas": "0x300/0x400"},
    ]
    events = [
        {"id": 0, "resource": 0, "kind": "update", "kind_id": 3, "mesh_frame": 10, "generation": 1, "context": "0xaa", "source_resource": None, "source_identity": "0x0", "source_generation": 0, "source_matched": False, "subresource": 0, "first": 0, "end": UINT64_MAX, "mapped": False, "complete": True, "stack": 0},
        {"id": 1, "resource": 0, "kind": "map", "kind_id": 1, "mesh_frame": 11, "generation": 2, "context": "0xaa", "source_resource": None, "source_identity": "0x0", "source_generation": 0, "source_matched": False, "subresource": 0, "first": 0, "end": UINT64_MAX, "mapped": True, "complete": True, "stack": 1},
        {"id": 2, "resource": 2, "kind": "update", "kind_id": 3, "mesh_frame": 11, "generation": 1, "context": "0xaa", "source_resource": None, "source_identity": "0x0", "source_generation": 0, "source_matched": False, "subresource": 0, "first": 0, "end": len(ids), "mapped": False, "complete": True, "stack": 2},
        {"id": 3, "resource": 1, "kind": "copy_resource", "kind_id": 4, "mesh_frame": 11, "generation": 1, "context": "0xaa", "source_resource": 2, "source_identity": "0x3000", "source_generation": 1, "source_matched": True, "subresource": 0, "first": 0, "end": UINT64_MAX, "mapped": False, "complete": True, "stack": 0},
    ]
    for event in events:
        event["map_type"] = 2 if event["kind_id"] == 1 else 0
        event["completion_status"] = "matched" if event["kind_id"] == 1 else "not_applicable"
        event["completion_frame"] = event["mesh_frame"] if event["kind_id"] == 1 else 0
        event["completion_stack"] = event["stack"] if event["kind_id"] == 1 else None
    blobs = []
    names = ("draw_pool", "draw_ids", "scene_depth", "mesh_coverage", "mesh_records")
    source_ids = (0, 1, 3, 4, 5)
    generations = (2, 1, 0, 0, 0)
    formats = (0, 0, 40, 16, 0)
    widths = (0, 0, 4, 4, 0)
    heights = (0, 0, 2, 2, 0)
    rows = (0, 0, 16, 32, 0)
    for i, chunk in enumerate(chunks):
        blobs.append({"id": i, "name": names[i], "status": "available", "source_resource": source_ids[i], "generation": generations[i], "foreign_epoch": 0, "mesh_frame": 11, "subresource_or_record_count": 2 if i == 4 else 0, "texture": i in (2, 3), "format": formats[i], "width": widths[i], "height": heights[i], "row_bytes": rows[i], "offset": offsets[i], "bytes": len(chunk)})
    root = {
        "schema": SCHEMA, "binary": binary_name,
        "executable": {"pe_timestamp": 1, "image_size": 0x10000},
        "selection": {"discovery_mesh_frame": 10, "selected_mesh_frame": 11, "scene_frame": 77, "has_discovery": True, "has_selection": True, "sealed": True, "missed_window": False},
        "limits": {"resources": 64, "writes": 512, "draws": 512, "stacks": 128, "buffer_bytes": 134217728, "texture_bytes": 67108864, "single_buffer_bytes": 16777216},
        "summary": {"status": "sealed", "resources": 6, "events_stored": 4, "writes_observed": 4, "draws": 1, "snapshots": 2, "blobs": 5, "stack_samples": 3,
                    "resource_overflow": 0, "write_overflow": 0, "draw_overflow": 0, "stack_overflow": 0, "snapshot_overflow": 0, "buffer_budget_declines": 0, "texture_budget_declines": 0, "unsafe_associations": 0, "copy_failures": 0, "map_failures": 0, "unmatched_writes": 0, "unobserved_sources": 0, "foreign_writes": 0, "cpu_provenance_available": True, "ignored_eye": 0, "ignored_frame": 0, "stage_rejects": 0, "suppressed_recursive": 0, "duplicate_maps": 0, "unmatched_unmaps": 0, "pending_map_overflow": 0, "open_map_snapshots": 0, "binary_ok": True},
        "resources": resources, "stacks": stacks, "events": events,
        "snapshots": [{"id": 0, "resource": 0, "generation": 2, "foreign_epoch": 0, "mesh_frame": 11, "blob": 0}, {"id": 1, "resource": 1, "generation": 1, "foreign_epoch": 0, "mesh_frame": 11, "blob": 1}],
        "draws": [{"id": 0, "mesh_frame": 11, "eye": 0, "first_record": 0, "instances": 2, "id_byte_offset": 0, "vertex_shader_hash": "0x1234", "key_present": True, "key16": key, "pool_resource": 0, "pool_generation": 2, "pool_write_observed": True, "pool_matched": True, "pool_snapshot": 0, "id_resource": 1, "id_generation": 1, "id_write_observed": True, "id_matched": True, "id_snapshot": 1}],
        "blobs": blobs,
    }
    return root, b"".join(chunks)


def _write_fixture(directory, root, binary):
    directory = Path(directory)
    marker = directory / "classification_fixture.json"
    marker.write_text(json.dumps(root), encoding="utf-8")
    (directory / root["binary"]).write_bytes(binary)
    return marker


def _verify_self_fixture(report):
    counts = report["counts"]
    if report["status"] != "sealed" or counts["mesh_records"] != 2:
        raise CaptureError("self-test fixture did not contain the expected sealed two-record capture")
    if counts["coverage_pixels"] != 4 or counts["depth_agreeing_pixels"] != 3:
        raise CaptureError("self-test coverage/depth interpretation changed")
    if counts["owned_visible_pixels"] != 3 or counts["owned_visible_records"] != 2:
        raise CaptureError("self-test ownership interpretation changed")
    if len(report["groups"]) != 2:
        raise CaptureError("self-test metadata grouping changed")
    group = report["groups"][0]
    if (group["t33_word28"], group["t33_word320"], group["instance_second_word"]) != (2801, 3201, 9001):
        raise CaptureError("self-test t33/instance metadata changed")
    pool_route = group["source_versions"]["t33_pool"]["upload_route"]
    ids_route = group["source_versions"]["ia_ids"]["upload_route"]
    if pool_route.get("endpoint_event") != 1 or pool_route.get("endpoint_kind") != "map":
        raise CaptureError("self-test rewrite generation was not selected")
    if ids_route.get("endpoint_event") != 2 or [x["event"] for x in ids_route["chain"]] != [3, 2]:
        raise CaptureError("self-test copy-source provenance changed")


def verify_fixture(capture, report):
    selection = capture["selection"]
    if (selection["selected_mesh_frame"], selection["scene_frame"], selection["sealed"]) != (23, 900, True):
        raise CaptureError("fixture selected/scene frame changed")
    if len(capture["draws"]) != 3 or report["counts"]["mesh_records"] != 3:
        raise CaptureError("fixture draw/record count changed")
    counts = report["counts"]
    if (counts["coverage_pixels"], counts["depth_agreeing_pixels"],
            counts["owned_visible_pixels"], counts["owned_visible_records"]) != (2, 1, 1, 1):
        raise CaptureError("fixture exact-depth ownership interpretation changed")
    records = report.get("records", [])
    if len(records) != 1:
        raise CaptureError("fixture owner detail is unavailable")
    owner = records[0]
    expected = (1, 1, 1, 0xB0B00028, 0xB0B00140, 0x22222222)
    actual = (owner["record"], owner["draw"], owner["pool_slot"], owner["t33_word28"],
              owner["t33_word320"], owner["instance_second_word"])
    if actual != expected:
        raise CaptureError("fixture exact draw/ID/t33 join changed")
    if len(report["groups"]) != 1:
        raise CaptureError("fixture metadata grouping changed")
    sources = report["groups"][0]["source_versions"]
    pool_route = sources["t33_pool"]["upload_route"]
    ids_route = sources["ia_ids"]["upload_route"]
    if pool_route["status"] != "matched" or pool_route.get("endpoint_kind") != "update":
        raise CaptureError("fixture pool rewrite provenance changed")
    if ids_route["status"] != "matched" or ids_route.get("endpoint_kind") != "map":
        raise CaptureError("fixture ID Map provenance changed")
    if not ids_route["chain"] or ids_route["chain"][-1].get("completion_status") != "matched":
        raise CaptureError("fixture ID Map/Unmap completion changed")
    missing_draw = capture["draws"][2]
    if any(missing_draw[field] for field in ("pool_write_observed", "pool_matched", "id_write_observed", "id_matched")):
        raise CaptureError("fixture missing-provenance control changed")


def verify_pipeline(capture, report):
    selection = capture["selection"]
    if (selection["selected_mesh_frame"], selection["scene_frame"], selection["sealed"]) != (3, 10003, True):
        raise CaptureError("pipeline fixture selected/scene frame changed")
    if len(capture["draws"]) != 1:
        raise CaptureError("pipeline fixture must contain one draw")
    draw = capture["draws"][0]
    if (draw["mesh_frame"], draw["eye"], draw["first_record"], draw["instances"], draw["id_byte_offset"]) != (3, 0, 0, 1, 0):
        raise CaptureError("pipeline fixture draw metadata changed")
    if report["counts"]["owned_visible_pixels"] < 1 or report["counts"]["owned_visible_records"] != 1:
        raise CaptureError("pipeline fixture produced no matched visible ownership")
    records = report.get("records", [])
    if len(records) != 1 or records[0]["record"] != 0 or records[0]["pool_slot"] != 0:
        raise CaptureError("pipeline fixture record/ID/pool join changed")
    if len(report["groups"]) != 1:
        raise CaptureError("pipeline fixture metadata grouping changed")
    sources = report["groups"][0]["source_versions"]
    for label in ("t33_pool", "ia_ids"):
        source = sources[label]
        route = source["upload_route"]
        if route["status"] != "matched" or route.get("endpoint_kind") != "update":
            raise CaptureError("pipeline fixture %s current generation lacks an observed Update route" % label)
        event = capture["events"][route["endpoint_event"]]
        if event["resource"] != source["resource"] or event["generation"] != source["generation"]:
            raise CaptureError("pipeline fixture %s upload-route version mismatch" % label)


def self_test():
    root, binary = _fixture_value()
    with tempfile.TemporaryDirectory() as temp_name:
        temp = Path(temp_name)
        marker = _write_fixture(temp, root, binary)
        capture = load_capture(marker)
        report = analyse(capture, include_records=True)
        _verify_self_fixture(report)
        assert report["records"][0]["pool_slot"] == 1

        def rejected(mutator, *, trim_binary=None, remove_binary=False):
            value = copy.deepcopy(root)
            mutator(value)
            path = _write_fixture(temp, value, binary if trim_binary is None else binary[:trim_binary])
            if remove_binary:
                (temp / value["binary"]).unlink()
            try:
                analyse(load_capture(path))
            except CaptureError:
                return
            raise AssertionError("malformed classification capture accepted")

        rejected(lambda value: None, trim_binary=len(binary) - 1)  # truncation / missing payload
        rejected(lambda value: value["blobs"][3].update(width=0))
        rejected(lambda value: value["blobs"][3].update(row_bytes=24))
        rejected(lambda value: value["summary"].update(draws=2))
        rejected(lambda value: value["snapshots"][0].update(resource=99))
        rejected(lambda value: value["draws"][0].update(pool_generation=1))
        rejected(lambda value: value["draws"][0]["key16"].__setitem__(0, 999))
        rejected(lambda value: value["draws"][0].update(id_byte_offset=16))
        rejected(lambda value: value["resources"][0]["buffer"].update(stride=320))
        rejected(lambda value: value["events"][1].update(completion_status="open"))

        def overlap_draws(value):
            duplicate = copy.deepcopy(value["draws"][0])
            duplicate["id"] = 1
            value["draws"].append(duplicate)
            value["summary"]["draws"] = 2
        rejected(overlap_draws)

        def bad_pool_slot(value):
            value["blobs"][1]["offset"] += 0  # metadata unchanged; payload mutation below is separate
        bad_ids = bytearray(binary)
        ids_at = root["blobs"][1]["offset"]
        struct.pack_into("<I", bad_ids, ids_at, 99)
        path = _write_fixture(temp, root, bytes(bad_ids))
        try:
            analyse(load_capture(path))
        except CaptureError:
            pass
        else:
            raise AssertionError("out-of-range pool slot accepted")

        bad_coverage = bytearray(binary)
        coverage_at = root["blobs"][3]["offset"]
        struct.pack_into("<f", bad_coverage, coverage_at, float("nan"))
        path = _write_fixture(temp, root, bytes(bad_coverage))
        try:
            analyse(load_capture(path))
        except CaptureError:
            pass
        else:
            raise AssertionError("non-finite coverage accepted")

        rejected(lambda value: value["blobs"][3].update(format=40))
        rejected(lambda value: value["blobs"][4].update(subresource_or_record_count=3))
        rejected(lambda value: value["blobs"][1].update(offset=value["blobs"][0]["offset"]))

        # An explicit capture decline is missing evidence, while claiming an
        # available span without the binary is corruption.
        declined = copy.deepcopy(root)
        declined["blobs"][0].update(status="readback_failed", bytes=0)
        shift = len(binary[:root["blobs"][0]["bytes"]])
        for blob in declined["blobs"][1:]:
            blob["offset"] -= shift
        declined_binary = binary[shift:]
        path = _write_fixture(temp, declined, declined_binary)
        missing = analyse(load_capture(path))
        assert any("readback_failed" in item["reason"] for item in missing["missing"])
        rejected(lambda value: None, remove_binary=True)

        unconfirmed = copy.deepcopy(root)
        unconfirmed["summary"]["binary_ok"] = False
        path = _write_fixture(temp, unconfirmed, binary)
        unconfirmed_report = analyse(load_capture(path))
        assert unconfirmed_report["counts"]["mesh_records"] is None
        assert unconfirmed_report["missing"] == [{"scope": "capture", "reason": "binary_write_not_confirmed"}]

        # A generation with no event remains explicit; it is never silently
        # attributed to another rewrite of the same resource.
        missing_prov = copy.deepcopy(root)
        missing_prov["draws"][0]["pool_generation"] = 0
        missing_prov["draws"][0]["pool_write_observed"] = False
        missing_prov["draws"][0]["pool_matched"] = False
        missing_prov["snapshots"][0]["generation"] = 0
        missing_prov["blobs"][0]["generation"] = 0
        path = _write_fixture(temp, missing_prov, binary)
        provenance_report = analyse(load_capture(path))
        assert provenance_report["groups"][0]["source_versions"]["t33_pool"]["upload_route"]["status"] == "missing"

    print("Object classification reader self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", help="classification_<stamp>.json")
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON to stdout")
    parser.add_argument("--records", action="store_true", help="include bounded per-owner record details")
    parser.add_argument("--verify-fixture", action="store_true", help="assert the WARP fixture semantics")
    parser.add_argument("--verify-pipeline", action="store_true", help="assert the production mesh-motion pipeline fixture")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        if args.path or args.verify_fixture or args.verify_pipeline or args.records or args.json:
            parser.error("--self-test must be used alone")
        self_test()
        return 0
    if args.verify_fixture and args.verify_pipeline:
        parser.error("--verify-fixture and --verify-pipeline are mutually exclusive")
    if not args.path:
        parser.error("path is required unless --self-test is used")
    try:
        capture = load_capture(args.path)
        report = analyse(capture, include_records=args.records or args.verify_fixture or args.verify_pipeline)
        if args.verify_fixture:
            verify_fixture(capture, report)
        if args.verify_pipeline:
            verify_pipeline(capture, report)
    except CaptureError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if args.verify_fixture and not args.json:
        print("GPU object-classification fixture verified")
    elif args.verify_pipeline and not args.json:
        print("Object-classification pipeline fixture verified")
    elif args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        print_report(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
