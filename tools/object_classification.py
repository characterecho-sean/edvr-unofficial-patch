#!/usr/bin/env python3
"""Validate and analyse an EDVR object-classification capture.

The input is ``classification_<stamp>.json``.  Its named binary sibling is
read without modification.  Visible ownership is reported only for a valid
MeshCoverage record whose stored depth exactly equals SceneZ and whose exact-
frame Mesh bytes agree with the draw-generation ID and t33 snapshots.

Captures written since 2026-09-23 are ``edvr_object_classification_v3``:
EDVR's draw/mesh capture retired, so they carry the executable identity,
``summary.binary_ok`` and the record-writer section only, and load with every
draw/mesh section empty (status ``draw_capture_retired``). v1 and v2 captures
read exactly as before.

The record-writer section is version 3 from 2026-09-24: the upload join
(``uploads``, the writer's Map cutoff per source-owner attempt) retired with
the source-owner probe that fed it, so a version 3 section has no ``uploads``
array. Versions 1 and 2 carry one and read as before (the 2026-09-23 builds
wrote version 2 with it always empty).
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


SCHEMA_V1 = "edvr_object_classification_v1"
SCHEMA_V2 = "edvr_object_classification_v2"
SCHEMA_V3 = "edvr_object_classification_v3"
SCHEMA = SCHEMA_V1
MESH_STRIDE = 240
POOL_STRIDE = 336
MAX_JSON_BYTES = 128 * 1024 * 1024
MAX_BINARY_BYTES = 384 * 1024 * 1024
MAX_ITEMS = 1_000_000
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
SOURCE_OWNER_LIMITS = {
    "attempts": 16, "unwind_frames": 32, "groups": 64, "leaves": 512,
    "descriptors": 4096, "cpu_bytes": 32 * 1024 * 1024,
    "attempt_cpu_bytes": 8 * 1024 * 1024,
    "single_source_bytes": 8 * 1024 * 1024,
}
SOURCE_OWNER_CALLSITE_RVA = 0x4C821B3
RECORD_WRITER_LIMITS = {"records": 65536, "bytes": 64 * 1024 * 1024}
RECORD_OWNERSHIP_LIMITS = {
    "ownership_records": 8192, "ownership_unwind_depth": 32,
    "ownership_traces": 32, "ownership_trace_frames": 32,
    "ownership_outer_bytes": 0x460, "ownership_collection_bytes": 0x2F0,
    "ownership_context_bytes": 0xC0, "ownership_record_tail_bytes": 0x18,
    "ownership_opaque_prefix_bytes": 0x80,
}
RECORD_WRITERS = {
    "direct_369ce91", "primary_42b42ef", "inline_42b4ed6",
    "direct_43130aa", "helper_434d149",
}
RECORD_OWNERSHIP_STATUSES = {
    "linked", "unsupported_writer", "ancestor_missing", "unwind_failed",
    "opcode_mismatch", "tuple_read_fault", "tuple_mismatch", "registry_read_fault",
    "registry_mismatch", "record_range_mismatch", "record_read_fault",
    "cache_conflict", "record_overflow",
}
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


def _address(value, name):
    text = _string(value, name, 34)
    if not HEX_ID.fullmatch(text) or int(text, 16) > UINT64_MAX:
        raise CaptureError("%s must be a hexadecimal pointer" % name)
    return text


def _validate_source_owner(root, executable, resources, events, summary, packed_bytes):
    owner = _dict(root.get("source_owner"), "source_owner")
    owner_statuses = {"not_run", "identity_mismatch", "no_callsite",
                      "no_resource_match", "captured", "partial"}
    status = _string(owner.get("status"), "source_owner.status", 32)
    if status not in owner_statuses:
        raise CaptureError("source_owner.status is unknown")
    _boolean(owner.get("synthetic_fixture"), "source_owner.synthetic_fixture")
    expected_timestamp = _u32(owner.get("expected_pe_timestamp"), "source_owner.expected_pe_timestamp")
    expected_image_size = _u32(owner.get("expected_image_size"), "source_owner.expected_image_size")
    callsite = _u32(owner.get("callsite_rva"), "source_owner.callsite_rva")
    if callsite != SOURCE_OWNER_CALLSITE_RVA or callsite >= expected_image_size:
        raise CaptureError("source_owner callsite is outside the expected executable contract")
    identity_matches = (expected_timestamp == executable["pe_timestamp"] and
                        expected_image_size == executable["image_size"])
    if status == "identity_mismatch" and identity_matches:
        raise CaptureError("source_owner identity status disagrees with executable")
    if status not in {"not_run", "identity_mismatch"} and not identity_matches:
        raise CaptureError("source_owner ran against an unexpected executable")

    limits = _dict(owner.get("limits"), "source_owner.limits")
    for field, maximum in SOURCE_OWNER_LIMITS.items():
        value = _uint(limits.get(field), "source_owner.limits.%s" % field)
        if not value or value > maximum:
            raise CaptureError("source_owner.limits.%s exceeds reader safety bound" % field)

    owner_summary = _dict(owner.get("summary"), "source_owner.summary")
    summary_fields = ("maps_considered", "identity_rejects", "opcode_rejects",
                      "unwind_attempts", "unwind_failures", "callsite_matches",
                      "resource_matches", "attempts_stored", "complete_attempts",
                      "partial_attempts", "read_faults", "group_overflow",
                      "leaf_overflow", "descriptor_overflow", "attempt_overflow",
                      "cpu_byte_declines", "metadata_changes")
    for field in summary_fields:
        _uint(owner_summary.get(field), "source_owner.summary.%s" % field)

    cpu_blobs = _list(root.get("cpu_blobs"), "cpu_blobs",
                      limits["descriptors"] * 2 * limits["attempts"])
    attempts = _list(owner.get("attempts"), "source_owner.attempts", limits["attempts"])
    attempt_statuses = {"captured", "partial", "identity_mismatch", "opcode_mismatch",
                        "unwind_not_found", "unwind_failed", "nested_unreadable",
                        "resource_mismatch", "layout_invalid", "attempt_cap"}
    unwind_statuses = {"not_run", "matched", "callsite_not_found", "metadata_unreadable",
                       "frame_unreadable", "leaf_unreadable", "no_progress", "frame_limit"}
    for ai, attempt in enumerate(attempts):
        attempt = _dict(attempt, "source_owner.attempts[%u]" % ai)
        _sequential_id(attempt, ai, "source_owner.attempts")
        event_ref = _nullable_ref(attempt.get("event"), "source_owner.attempts[%u].event" % ai, len(events))
        resource = _nullable_ref(attempt.get("resource"), "source_owner.attempts[%u].resource" % ai, len(resources))
        if event_ref is None or resource is None:
            raise CaptureError("source_owner.attempts[%u] lacks event/resource" % ai)
        if "t33_pool" not in resources[resource]["roles"] or resources[resource].get("buffer", {}).get("stride") != POOL_STRIDE:
            raise CaptureError("source_owner.attempts[%u] resource is not a t33 pool" % ai)
        generation = _uint(attempt.get("generation"), "source_owner.attempts[%u].generation" % ai)
        mesh_frame = _u32(attempt.get("mesh_frame"), "source_owner.attempts[%u].mesh_frame" % ai)
        foreign_epoch = _uint(attempt.get("foreign_epoch"), "source_owner.attempts[%u].foreign_epoch" % ai)
        if foreign_epoch > summary["foreign_writes"]:
            raise CaptureError("source_owner.attempts[%u].foreign_epoch exceeds foreign writes" % ai)
        event = events[event_ref]
        if (event["kind_id"] != 1 or event["resource"] != resource or
                event["generation"] != generation or event["mesh_frame"] != mesh_frame):
            raise CaptureError("source_owner.attempts[%u] is stale or belongs to another event generation" % ai)
        attempt_status = _string(attempt.get("status"), "source_owner.attempts[%u].status" % ai, 32)
        if attempt_status not in attempt_statuses:
            raise CaptureError("source_owner.attempts[%u].status is unknown" % ai)
        unwind_status = _string(attempt.get("unwind_status"), "source_owner.attempts[%u].unwind_status" % ai, 32)
        if unwind_status not in unwind_statuses:
            raise CaptureError("source_owner.attempts[%u].unwind_status is unknown" % ai)
        unwind_frames = _u32(attempt.get("unwind_frames"), "source_owner.attempts[%u].unwind_frames" % ai)
        if unwind_frames > limits["unwind_frames"]:
            raise CaptureError("source_owner.attempts[%u] exceeds unwind frame cap" % ai)
        return_rva = _u32(attempt.get("return_rva"), "source_owner.attempts[%u].return_rva" % ai)
        if return_rva and return_rva >= executable["image_size"]:
            raise CaptureError("source_owner.attempts[%u].return_rva lies outside executable" % ai)
        for field in ("nested_owner", "nested_owner_field_130", "nested_owner_field_138",
                      "wrapper", "native_resource"):
            _address(attempt.get(field), "source_owner.attempts[%u].%s" % (ai, field))
        if attempt_status in {"captured", "partial"} and attempt["native_resource"] != resources[resource]["identity"]:
            raise CaptureError("source_owner.attempts[%u] wrapped native resource disagrees with event resource" % ai)
        stride = _u32(attempt.get("stride"), "source_owner.attempts[%u].stride" % ai)
        if attempt_status in {"captured", "partial"} and (unwind_status != "matched" or return_rva != callsite or stride != POOL_STRIDE):
            raise CaptureError("source_owner.attempts[%u] capture lacks matched unwind/callsite/stride" % ai)

        for field, maximum in (("group_count", UINT64_MAX), ("groups_scanned", limits["groups"]),
                               ("leaf_count", UINT64_MAX), ("leaves_scanned", limits["leaves"]),
                               ("descriptor_count", UINT64_MAX), ("descriptors_scanned", limits["descriptors"])):
            value = _uint(attempt.get(field), "source_owner.attempts[%u].%s" % (ai, field))
            if value > maximum:
                raise CaptureError("source_owner.attempts[%u].%s exceeds safety bound" % (ai, field))
        retained_bytes = _uint(attempt.get("retained_bytes"), "source_owner.attempts[%u].retained_bytes" % ai)
        if retained_bytes > limits["attempt_cpu_bytes"]:
            raise CaptureError("source_owner.attempts[%u].retained_bytes exceeds attempt budget" % ai)
        groups = _list(attempt.get("groups"), "source_owner.attempts[%u].groups" % ai, limits["groups"])
        leaves = _list(attempt.get("leaves"), "source_owner.attempts[%u].leaves" % ai, limits["leaves"])
        descriptors = _list(attempt.get("descriptors"), "source_owner.attempts[%u].descriptors" % ai, limits["descriptors"])
        if (attempt["groups_scanned"] != len(groups) or attempt["leaves_scanned"] != len(leaves) or
                attempt["descriptors_scanned"] != len(descriptors) or
                attempt["group_count"] < len(groups) or attempt["leaf_count"] < len(leaves) or
                attempt["descriptor_count"] < len(descriptors)):
            raise CaptureError("source_owner.attempts[%u] declared/scanned counts disagree with arrays" % ai)
        for gi, group in enumerate(groups):
            group = _dict(group, "source_owner.attempts[%u].groups[%u]" % (ai, gi))
            _address(group.get("address"), "source_owner.attempts[%u].groups[%u].address" % (ai, gi))
            leaf_count = _uint(group.get("leaf_count"), "source_owner.attempts[%u].groups[%u].leaf_count" % (ai, gi))
            leaves_scanned = _u32(group.get("leaves_scanned"), "source_owner.attempts[%u].groups[%u].leaves_scanned" % (ai, gi))
            if leaves_scanned > leaf_count or leaves_scanned > limits["leaves"]:
                raise CaptureError("source_owner.attempts[%u].groups[%u] leaf counts are invalid" % (ai, gi))
        group_leaf_counts = Counter()
        for li, leaf in enumerate(leaves):
            leaf = _dict(leaf, "source_owner.attempts[%u].leaves[%u]" % (ai, li))
            group = _u32(leaf.get("group"), "source_owner.attempts[%u].leaves[%u].group" % (ai, li))
            if group >= len(groups):
                raise CaptureError("source_owner.attempts[%u].leaves[%u] references missing group" % (ai, li))
            group_leaf_counts[group] += 1
            _address(leaf.get("address"), "source_owner.attempts[%u].leaves[%u].address" % (ai, li))
            _address(leaf.get("owner_link"), "source_owner.attempts[%u].leaves[%u].owner_link" % (ai, li))
            _uint(leaf.get("base_record"), "source_owner.attempts[%u].leaves[%u].base_record" % (ai, li))
            _uint(leaf.get("record_count"), "source_owner.attempts[%u].leaves[%u].record_count" % (ai, li))
        for gi, group in enumerate(groups):
            if group_leaf_counts[gi] != group["leaves_scanned"]:
                raise CaptureError("source_owner.attempts[%u].groups[%u].leaves_scanned disagrees with leaves" % (ai, gi))
        for di, descriptor in enumerate(descriptors):
            label = "source_owner.attempts[%u].descriptors[%u]" % (ai, di)
            descriptor = _dict(descriptor, label)
            leaf_ref = _u32(descriptor.get("leaf"), label + ".leaf")
            if leaf_ref >= len(leaves):
                raise CaptureError("%s references missing leaf" % label)
            list_index = _u32(descriptor.get("list"), label + ".list")
            _u32(descriptor.get("entry"), label + ".entry")
            entry_stride = _u32(descriptor.get("entry_stride"), label + ".entry_stride")
            if list_index > 7 or entry_stride != (72 if list_index == 0 else 80):
                raise CaptureError("%s has invalid list/entry stride" % label)
            _address(descriptor.get("address"), label + ".address")
            source_address = _address(descriptor.get("source_address"), label + ".source_address")
            relative = _uint(descriptor.get("destination_relative_record"), label + ".destination_relative_record")
            record_count = _uint(descriptor.get("record_count"), label + ".record_count")
            first = _uint(descriptor.get("destination_first_record"), label + ".destination_first_record")
            end = _uint(descriptor.get("destination_end_record"), label + ".destination_end_record")
            leaf = leaves[leaf_ref]
            expected_first = min(UINT64_MAX, leaf["base_record"] + relative)
            expected_end = min(UINT64_MAX, expected_first + record_count)
            if first != expected_first or end != expected_end:
                raise CaptureError("%s destination interval disagrees with leaf and descriptor" % label)
            descriptor_status = _string(descriptor.get("status"), label + ".status", 32)
            descriptor_statuses = {"captured", "zero_count", "null_source", "range_overflow",
                                   "source_too_large", "byte_budget", "read_fault", "descriptor_fault",
                                   "descriptor_changed"}
            if descriptor_status not in descriptor_statuses:
                raise CaptureError("%s.status is unknown" % label)
            pool_records = resources[resource]["buffer"]["bytes"] // POOL_STRIDE
            leaf_end = min(UINT64_MAX, leaf["base_record"] + leaf["record_count"])
            range_bad = (first < leaf["base_record"] or end < first or
                         end > leaf_end or end > pool_records)
            unavailable = ("zero_count" if record_count == 0 else
                           "null_source" if int(source_address, 16) == 0 else
                           "range_overflow" if range_bad else None)
            if descriptor_status in {"zero_count", "null_source", "range_overflow"}:
                if descriptor_status != unavailable:
                    raise CaptureError("%s unavailable status disagrees with descriptor fields" % label)
            elif unavailable is not None:
                raise CaptureError("%s ignores an unavailable descriptor condition" % label)
            descriptor["raw_blob"] = _nullable_ref(descriptor.get("raw_blob"), label + ".raw_blob", len(cpu_blobs))
            descriptor["payload_blob"] = _nullable_ref(descriptor.get("payload_blob"), label + ".payload_blob", len(cpu_blobs))

        if attempt_status == "captured" and any(
                descriptor["status"] not in {"captured", "zero_count"} for descriptor in descriptors):
            raise CaptureError("source_owner.attempts[%u] captured status contains partial descriptors" % ai)

    spans = []
    attempt_bytes = Counter()
    blob_statuses = {"available", "read_fault", "byte_budget", "source_too_large",
                     "zero_count", "null_source", "range_overflow",
                     "bin_open_failed", "bin_write_failed"}
    for bi, blob in enumerate(cpu_blobs):
        blob = _dict(blob, "cpu_blobs[%u]" % bi)
        _sequential_id(blob, bi, "cpu_blobs")
        kind = _string(blob.get("kind"), "cpu_blobs[%u].kind" % bi, 32)
        if kind not in {"descriptor", "source_records"}:
            raise CaptureError("cpu_blobs[%u].kind is unknown" % bi)
        attempt_ref = _nullable_ref(blob.get("attempt"), "cpu_blobs[%u].attempt" % bi, len(attempts))
        if attempt_ref is None:
            raise CaptureError("cpu_blobs[%u] lacks attempt" % bi)
        descriptor_ref = _u32(blob.get("descriptor"), "cpu_blobs[%u].descriptor" % bi)
        if descriptor_ref >= len(attempts[attempt_ref]["descriptors"]):
            raise CaptureError("cpu_blobs[%u] references missing descriptor" % bi)
        descriptor = attempts[attempt_ref]["descriptors"][descriptor_ref]
        address = _address(blob.get("address"), "cpu_blobs[%u].address" % bi)
        expected_address = descriptor["address"] if kind == "descriptor" else descriptor["source_address"]
        if address != expected_address:
            raise CaptureError("cpu_blobs[%u] address disagrees with descriptor" % bi)
        offset = _uint(blob.get("offset"), "cpu_blobs[%u].offset" % bi)
        size = _uint(blob.get("bytes"), "cpu_blobs[%u].bytes" % bi)
        blob_status = _string(blob.get("status"), "cpu_blobs[%u].status" % bi, 32)
        if blob_status not in blob_statuses:
            raise CaptureError("cpu_blobs[%u].status is unknown" % bi)
        if offset > MAX_BINARY_BYTES or size > limits["single_source_bytes"] or offset + size > MAX_BINARY_BYTES:
            raise CaptureError("cpu_blobs[%u] binary span exceeds safety bound" % bi)
        if blob_status == "available":
            if not size:
                raise CaptureError("cpu_blobs[%u] available payload is empty" % bi)
            spans.append((offset, offset + size, bi))
            attempt_bytes[attempt_ref] += size
        elif size:
            raise CaptureError("cpu_blobs[%u] unavailable payload declares bytes" % bi)
        if kind == "descriptor" and blob_status == "available" and size != descriptor["entry_stride"]:
            raise CaptureError("cpu_blobs[%u] descriptor byte count disagrees with entry stride" % bi)
        if kind == "source_records" and blob_status == "available" and size != descriptor["record_count"] * POOL_STRIDE:
            raise CaptureError("cpu_blobs[%u] source byte count disagrees with record count" % bi)
    spans.sort()
    cursor = packed_bytes
    for begin, end, index in spans:
        if begin != cursor:
            reason = "overlaps" if begin < cursor else "leaves an unaccounted gap"
            raise CaptureError("cpu_blobs[%u] %s in packed binary" % (index, reason))
        cursor = end
    if cursor - packed_bytes > limits["cpu_bytes"]:
        raise CaptureError("cpu blobs exceed global byte cap")
    if any(size > limits["attempt_cpu_bytes"] for size in attempt_bytes.values()):
        raise CaptureError("cpu blobs exceed per-attempt byte cap")

    for ai, attempt in enumerate(attempts):
        for di, descriptor in enumerate(attempt["descriptors"]):
            label = "source_owner.attempts[%u].descriptors[%u]" % (ai, di)
            raw_ref, payload_ref = descriptor["raw_blob"], descriptor["payload_blob"]
            if raw_ref is not None:
                raw = cpu_blobs[raw_ref]
                if raw["attempt"] != ai or raw["descriptor"] != di or raw["kind"] != "descriptor":
                    raise CaptureError("%s raw_blob points at unrelated bytes" % label)
            if payload_ref is not None:
                payload = cpu_blobs[payload_ref]
                if payload["attempt"] != ai or payload["descriptor"] != di or payload["kind"] != "source_records":
                    raise CaptureError("%s payload_blob points at unrelated bytes" % label)
            if descriptor["status"] == "captured" and summary["binary_ok"]:
                if raw_ref is None or payload_ref is None or cpu_blobs[raw_ref]["status"] != "available" or cpu_blobs[payload_ref]["status"] != "available":
                    raise CaptureError("%s captured status lacks both available blobs" % label)
            elif descriptor["status"] in {"zero_count", "null_source", "range_overflow",
                                           "source_too_large", "read_fault", "descriptor_changed"} and summary["binary_ok"]:
                if raw_ref is None or cpu_blobs[raw_ref]["status"] != "available":
                    raise CaptureError("%s lacks exact raw descriptor bytes" % label)
                if descriptor["status"] != "descriptor_changed" and (
                        payload_ref is None or cpu_blobs[payload_ref]["status"] != descriptor["status"]):
                    raise CaptureError("%s payload status disagrees with descriptor" % label)

    for ei, event in enumerate(events):
        ref = _nullable_ref(event.get("source_owner_attempt"), "events[%u].source_owner_attempt" % ei, len(attempts))
        if ref is not None and attempts[ref]["event"] != ei:
            raise CaptureError("events[%u].source_owner_attempt points at another event" % ei)
        if ref is None and any(attempt["event"] == ei for attempt in attempts):
            raise CaptureError("events[%u] omits its stored source-owner attempt" % ei)
    if owner_summary["attempts_stored"] != len(attempts):
        raise CaptureError("source_owner.summary.attempts_stored disagrees with attempts")
    if owner_summary["complete_attempts"] != sum(a["status"] == "captured" for a in attempts):
        raise CaptureError("source_owner.summary.complete_attempts disagrees with attempts")
    if owner_summary["partial_attempts"] != sum(a["status"] == "partial" for a in attempts):
        raise CaptureError("source_owner.summary.partial_attempts disagrees with attempts")
    if status == "not_run" and attempts:
        raise CaptureError("source_owner not_run capture has attempts")
    if status == "captured" and not any(a["status"] == "captured" for a in attempts):
        raise CaptureError("source_owner captured status lacks a complete attempt")
    return owner, attempts, cpu_blobs, cursor


def _validate_cpu_blob_contents(binary, attempts, cpu_blobs):
    for ai, attempt in enumerate(attempts):
        for di, descriptor in enumerate(attempt["descriptors"]):
            raw_ref = descriptor["raw_blob"]
            if raw_ref is None or cpu_blobs[raw_ref]["status"] != "available":
                continue
            blob = cpu_blobs[raw_ref]
            raw = memoryview(binary)[blob["offset"]:blob["offset"] + blob["bytes"]]
            source_address = struct.unpack_from("<Q", raw, 8)[0]
            relative, count = struct.unpack_from("<2I", raw, 0x38)
            if (source_address != int(descriptor["source_address"], 16) or
                    relative != descriptor["destination_relative_record"] or
                    count != descriptor["record_count"]):
                raise CaptureError("source_owner.attempts[%u].descriptors[%u] metadata disagrees with raw descriptor" % (ai, di))


def _validate_record_writers(root, executable, resources, attempts, packed_bytes, binary_ok):
    diagnostic = root.get("record_writers")
    if diagnostic is None:
        return None, [], [], [], [], packed_bytes
    diagnostic = _dict(diagnostic, "record_writers")
    version = _u32(diagnostic.get("version"), "record_writers.version")
    if version not in {1, 2, 3}:
        raise CaptureError("record_writers.version is unsupported")
    # Version 2 added the KinematicRig ownership; version 3 keeps it and
    # drops the upload join.
    ownership = version >= 2
    status = _string(diagnostic.get("status"), "record_writers.status", 32)
    hook_status = _string(diagnostic.get("hook_status"), "record_writers.hook_status", 32)
    statuses = {"not_run", "captured", "partial", "unavailable"}
    hook_statuses = {"not_run", "installed", "identity_mismatch", "opcode_mismatch",
                     "install_failed", "finished"}
    if status not in statuses or hook_status not in hook_statuses:
        raise CaptureError("record_writers status is unknown")
    if ((status == "not_run") != (hook_status == "not_run") or
            (status == "unavailable") !=
            (hook_status in {"identity_mismatch", "opcode_mismatch", "install_failed"}) or
            status in {"captured", "partial"} and hook_status not in {"installed", "finished"}):
        raise CaptureError("record_writers status and hook_status disagree")

    limits = _dict(diagnostic.get("limits"), "record_writers.limits")
    limit_bounds = dict(RECORD_WRITER_LIMITS)
    if ownership:
        limit_bounds.update(RECORD_OWNERSHIP_LIMITS)
    for field, maximum in limit_bounds.items():
        value = _uint(limits.get(field), "record_writers.limits.%s" % field)
        if not value or value > maximum:
            raise CaptureError("record_writers.limits.%s exceeds reader safety bound" % field)
    summary = _dict(diagnostic.get("summary"), "record_writers.summary")
    summary_fields = ("observed", "stored", "completed", "retained_bytes", "record_overflow",
                      "byte_budget_declines", "read_faults", "context_failures",
                      "declined_management", "declined_unknown", "unwind_failures",
                      "completion_failures")
    for field in summary_fields:
        _uint(summary.get(field), "record_writers.summary.%s" % field)

    ownership_status = None
    ownership_summary = None
    if ownership:
        ownership_status = _string(diagnostic.get("ownership_status"),
                                   "record_writers.ownership_status", 32)
        if ownership_status not in {"not_run", "unavailable", "captured", "partial"}:
            raise CaptureError("record_writers.ownership_status is unknown")
        ownership_summary = _dict(diagnostic.get("ownership_summary"),
                                  "record_writers.ownership_summary")
        ownership_summary_fields = (
            "attempted", "linked", "stored", "deduplicated", "unsupported_writer",
            "opcode_mismatch", "ancestor_missing", "unwind_failed", "tuple_read_fault", "tuple_mismatch",
            "registry_read_fault", "registry_mismatch", "record_range_mismatch",
            "record_read_fault", "cache_conflicts", "record_overflow",
            "byte_budget_declines", "read_faults", "ancestor_traces",
            "ancestor_trace_overflow",
        )
        for field in ownership_summary_fields:
            _uint(ownership_summary.get(field), "record_writers.ownership_summary.%s" % field)

    if version >= 3:
        # Retired with the source-owner probe that fed it (2026-09-24).
        if "uploads" in diagnostic:
            raise CaptureError("record_writers version 3 carries the retired uploads array")
        uploads = []
    else:
        uploads = _list(diagnostic.get("uploads"), "record_writers.uploads", len(attempts))
    upload_attempts = set()
    for ui, upload in enumerate(uploads):
        label = "record_writers.uploads[%u]" % ui
        upload = _dict(upload, label)
        attempt_ref = _u32(upload.get("attempt"), label + ".attempt")
        resource_ref = _u32(upload.get("resource"), label + ".resource")
        generation = _uint(upload.get("generation"), label + ".generation")
        _uint(upload.get("cutoff"), label + ".cutoff")
        if attempt_ref >= len(attempts) or resource_ref >= len(resources):
            raise CaptureError("%s references missing attempt/resource" % label)
        attempt = attempts[attempt_ref]
        if attempt["resource"] != resource_ref or attempt["generation"] != generation:
            raise CaptureError("%s disagrees with its source-owner attempt" % label)
        if attempt_ref in upload_attempts:
            raise CaptureError("record_writers contains duplicate upload attempt")
        upload_attempts.add(attempt_ref)

    records = _list(diagnostic.get("records"), "record_writers.records", limits["records"])
    snapshot_statuses = {"available", "not_applicable", "null_pointer", "read_fault", "byte_budget",
                         "bin_open_failed", "bin_write_failed"}
    writer_rvas = {name: int(name.rsplit("_", 1)[1], 16) for name in RECORD_WRITERS}
    cursor = packed_bytes
    last_sequence = 0
    event_sequences = set()
    for ri, record in enumerate(records):
        label = "record_writers.records[%u]" % ri
        record = _dict(record, label)
        _sequential_id(record, ri, "record_writers.records")
        sequence = _uint(record.get("sequence"), label + ".sequence")
        if not sequence or sequence <= last_sequence:
            raise CaptureError("record_writers begin-event sequences must be nonzero and increase")
        if sequence in event_sequences:
            raise CaptureError("record_writers event sequences must be unique")
        event_sequences.add(sequence)
        last_sequence = sequence
        writer = _string(record.get("writer"), label + ".writer", 32)
        if writer not in RECORD_WRITERS:
            raise CaptureError("%s.writer is unknown" % label)
        return_rva = _address(record.get("return_rva"), label + ".return_rva")
        if int(return_rva, 16) != writer_rvas[writer]:
            raise CaptureError("%s return RVA disagrees with writer" % label)
        _u32(record.get("thread_id"), label + ".thread_id")
        _u32(record.get("observed_frame"), label + ".observed_frame")
        addresses = {}
        for field in ("owner", "key", "builder", "object", "entry"):
            addresses[field] = _address(record.get(field), label + "." + field)
        context_status = _string(record.get("context_status"), label + ".context_status", 32)
        if context_status not in {"complete", "partial", "unavailable"}:
            raise CaptureError("%s.context_status is unknown" % label)
        lookup_status = _string(record.get("lookup_status"), label + ".lookup_status", 32)
        completion_sequence = _uint(record.get("completion_sequence"), label + ".completion_sequence")
        if lookup_status not in {"pending", "complete", "completion_failed"}:
            raise CaptureError("%s.lookup_status is unknown" % label)
        if (lookup_status == "pending") != (completion_sequence == 0):
            raise CaptureError("%s lookup/completion sequence disagree" % label)
        if completion_sequence:
            if completion_sequence <= sequence or completion_sequence in event_sequences:
                raise CaptureError("record_writers completion event must uniquely follow its begin event")
            event_sequences.add(completion_sequence)
        if ownership:
            ownership_link = record.get("ownership_id")
            if ownership_link is not None:
                ownership_link = _u32(ownership_link, label + ".ownership_id")
            record_ownership_status = _string(record.get("ownership_status"),
                                              label + ".ownership_status", 32)
            if record_ownership_status not in RECORD_OWNERSHIP_STATUSES:
                raise CaptureError("%s.ownership_status is unknown" % label)
            if (record_ownership_status == "linked") != (ownership_link is not None):
                raise CaptureError("%s ownership link and status disagree" % label)

        applicable = {"record": True, "key_snapshot": True,
                      "builder_snapshot": writer == "primary_42b42ef",
                      "object_snapshot": writer in {"primary_42b42ef", "inline_42b4ed6", "helper_434d149"},
                      "entry_snapshot": lookup_status != "pending"}
        sizes = {"record": POOL_STRIDE, "key_snapshot": 32,
                 "builder_snapshot": 96,
                 "object_snapshot": (416 if writer == "helper_434d149" else
                                     192 if ownership else 176),
                 "entry_snapshot": 0x38}
        snapshots = {}
        for field in ("record", "key_snapshot", "builder_snapshot", "object_snapshot", "entry_snapshot"):
            snap_label = label + "." + field
            snapshot = _dict(record.get(field), snap_label)
            snap_status = _string(snapshot.get("status"), snap_label + ".status", 32)
            if snap_status not in snapshot_statuses:
                raise CaptureError("%s.status is unknown" % snap_label)
            offset = _uint(snapshot.get("offset"), snap_label + ".offset")
            size = _uint(snapshot.get("bytes"), snap_label + ".bytes")
            if offset != cursor:
                raise CaptureError("%s does not begin at the packed writer cursor" % snap_label)
            if not applicable[field]:
                if snap_status != "not_applicable" or size or (
                        field == "builder_snapshot" and int(addresses["builder"], 16) or
                        field == "object_snapshot" and int(addresses["object"], 16)):
                    raise CaptureError("%s non-applicable snapshot is inconsistent" % snap_label)
            elif snap_status == "available":
                if size != sizes[field]:
                    raise CaptureError("%s available byte count is invalid" % snap_label)
                cursor += size
            elif size:
                raise CaptureError("%s unavailable snapshot declares bytes" % snap_label)
            if field == "key_snapshot":
                pointer = int(addresses["key"], 16)
            elif field == "builder_snapshot":
                pointer = int(addresses["builder"], 16)
            elif field == "object_snapshot":
                pointer = int(addresses["object"], 16)
            elif field == "entry_snapshot":
                pointer = int(addresses["entry"], 16)
            else:
                pointer = None
            if applicable[field] and pointer is not None:
                if ((snap_status == "null_pointer" and pointer != 0) or
                        snap_status in {"available", "read_fault", "byte_budget",
                                        "bin_open_failed", "bin_write_failed"} and pointer == 0):
                    raise CaptureError("%s pointer and status disagree" % snap_label)
            snapshots[field] = snapshot
        required_available = all(snapshots[field]["status"] == "available"
                                 for field in applicable if applicable[field])
        if context_status == "complete" and not required_available and binary_ok:
            raise CaptureError("%s complete context lacks required snapshots" % label)
        if context_status == "partial" and required_available and lookup_status != "pending":
            raise CaptureError("%s partial context has every required snapshot" % label)
        if lookup_status == "pending" and context_status == "complete":
            raise CaptureError("%s pending lookup claims complete context" % label)
        if binary_ok:
            entry_status = snapshots["entry_snapshot"]["status"]
            if (lookup_status == "complete") != (entry_status == "available"):
                raise CaptureError("%s lookup status disagrees with entry snapshot" % label)
            if lookup_status == "pending" and (int(addresses["entry"], 16) or entry_status != "not_applicable"):
                raise CaptureError("%s pending lookup carries completion evidence" % label)

    ownerships = []
    ancestor_traces = []
    if ownership:
        ownerships = _list(diagnostic.get("ownerships"), "record_writers.ownerships",
                           limits["ownership_records"])
        snapshot_statuses = {"available", "not_applicable", "null_pointer", "read_fault",
                             "byte_budget", "bin_open_failed", "bin_write_failed"}
        snapshot_fields = (
            "outer_snapshot", "collection_snapshot", "context_snapshot", "record_tail_snapshot",
            "game_object_prefix", "descriptor_prefix", "provider_prefix", "parent_prefix",
        )
        snapshot_sizes = {
            "outer_snapshot": limits["ownership_outer_bytes"],
            "collection_snapshot": limits["ownership_collection_bytes"],
            "context_snapshot": limits["ownership_context_bytes"],
            "record_tail_snapshot": limits["ownership_record_tail_bytes"],
            "game_object_prefix": limits["ownership_opaque_prefix_bytes"],
            "descriptor_prefix": limits["ownership_opaque_prefix_bytes"],
            "provider_prefix": limits["ownership_opaque_prefix_bytes"],
            "parent_prefix": limits["ownership_opaque_prefix_bytes"],
        }
        branch_rvas = {"direct_4321940": 0x431B212, "virtual_50": 0x431B21F}
        relation_writers = {
            "inline_registry_plus_78": {"inline_42b4ed6", "primary_42b42ef"},
            "direct_collection_plus_300": {"direct_43130aa"},
        }
        for oi, ownership in enumerate(ownerships):
            label = "record_writers.ownerships[%u]" % oi
            ownership = _dict(ownership, label)
            _sequential_id(ownership, oi, "record_writers.ownerships")
            first_sequence = _uint(ownership.get("first_sequence"), label + ".first_sequence")
            if not first_sequence:
                raise CaptureError("%s.first_sequence must be nonzero" % label)
            _u32(ownership.get("observed_frame"), label + ".observed_frame")
            _u32(ownership.get("thread_id"), label + ".thread_id")
            item_status = _string(ownership.get("status"), label + ".status", 32)
            if item_status not in {"captured", "partial"}:
                raise CaptureError("%s.status is unknown" % label)
            branch = _string(ownership.get("branch"), label + ".branch", 32)
            if branch not in branch_rvas:
                raise CaptureError("%s.branch is unknown" % label)
            ancestor_rva = _address(ownership.get("ancestor_return_rva"),
                                    label + ".ancestor_return_rva")
            if int(ancestor_rva, 16) != branch_rvas[branch]:
                raise CaptureError("%s branch and ancestor return RVA disagree" % label)
            unwind_depth = _u32(ownership.get("unwind_depth"), label + ".unwind_depth")
            if not unwind_depth or unwind_depth > limits["ownership_unwind_depth"]:
                raise CaptureError("%s.unwind_depth exceeds capture limit" % label)
            relation = _string(ownership.get("writer_relation"), label + ".writer_relation", 40)
            if relation not in relation_writers:
                raise CaptureError("%s.writer_relation is unknown" % label)
            addresses = {}
            for field in ("writer_owner", "outer", "collection_owner", "registry", "context",
                          "collection_record", "game_object", "descriptor", "provider", "parent"):
                addresses[field] = _address(ownership.get(field), label + "." + field)
            record_index = ownership.get("record_index")
            if record_index is not None:
                record_index = _u32(record_index, label + ".record_index")
            record_status = _string(ownership.get("record_status"), label + ".record_status", 32)
            if record_status not in {"not_applicable", "validated"}:
                raise CaptureError("%s.record_status is unknown" % label)
            direct = relation == "direct_collection_plus_300"
            if direct != (record_status == "validated") or direct != (record_index is not None):
                raise CaptureError("%s direct-record metadata is inconsistent" % label)
            if direct != bool(int(addresses["collection_record"], 16)):
                raise CaptureError("%s collection-record address is inconsistent" % label)
            parent_token = _u32(ownership.get("parent_token"), label + ".parent_token")
            parent_status = _string(ownership.get("parent_status"), label + ".parent_status", 32)
            if parent_status not in {"unresolved", "resolved_absent", "available", "read_fault"}:
                raise CaptureError("%s.parent_status is unknown" % label)

            identity_fields = ("outer_identity", "game_object_identity", "descriptor_identity",
                               "provider_identity", "parent_identity")
            identity_addresses = ("outer", "game_object", "descriptor", "provider", "parent")
            identities = {}
            for field, address_field in zip(identity_fields, identity_addresses):
                identity_label = label + "." + field
                identity = _dict(ownership.get(field), identity_label)
                identity_address = _address(identity.get("address"), identity_label + ".address")
                if int(identity_address, 16) != int(addresses[address_field], 16):
                    raise CaptureError("%s address disagrees with ownership tuple" % identity_label)
                vtable = _address(identity.get("vtable"), identity_label + ".vtable")
                identity_status = _string(identity.get("status"), identity_label + ".status", 32)
                vtable_rva = identity.get("vtable_rva")
                if vtable_rva is not None:
                    vtable_rva = _address(vtable_rva, identity_label + ".vtable_rva")
                pointer = int(identity_address, 16)
                vtable_value = int(vtable, 16)
                if identity_status == "null_pointer":
                    valid_identity = pointer == 0 and vtable_value == 0 and vtable_rva is None
                elif identity_status == "read_fault":
                    valid_identity = pointer != 0 and vtable_value == 0 and vtable_rva is None
                elif identity_status == "module_relative":
                    valid_identity = (pointer != 0 and vtable_value != 0 and vtable_rva is not None and
                                      int(vtable_rva, 16) < executable["image_size"])
                elif identity_status == "outside_image":
                    valid_identity = pointer != 0 and vtable_value != 0 and vtable_rva is None
                else:
                    raise CaptureError("%s.status is unknown" % identity_label)
                if not valid_identity:
                    raise CaptureError("%s fields disagree with status" % identity_label)
                identities[field] = identity

            snapshots = {}
            for field in snapshot_fields:
                snap_label = label + "." + field
                snapshot = _dict(ownership.get(field), snap_label)
                snap_status = _string(snapshot.get("status"), snap_label + ".status", 32)
                if snap_status not in snapshot_statuses:
                    raise CaptureError("%s.status is unknown" % snap_label)
                offset = _uint(snapshot.get("offset"), snap_label + ".offset")
                size = _uint(snapshot.get("bytes"), snap_label + ".bytes")
                if offset != cursor:
                    raise CaptureError("%s does not begin at the packed ownership cursor" % snap_label)
                if field == "record_tail_snapshot":
                    applicable_snapshot = direct
                    pointer = int(addresses["collection_record"], 16)
                else:
                    address_field = {"outer_snapshot": "outer", "collection_snapshot": "collection_owner",
                                     "context_snapshot": "context", "game_object_prefix": "game_object",
                                     "descriptor_prefix": "descriptor", "provider_prefix": "provider",
                                     "parent_prefix": "parent"}[field]
                    pointer = int(addresses[address_field], 16)
                    applicable_snapshot = (field != "parent_prefix" or parent_status == "available")
                if not applicable_snapshot:
                    if snap_status != "not_applicable" or size:
                        raise CaptureError("%s non-applicable snapshot is inconsistent" % snap_label)
                elif snap_status == "available":
                    if not pointer or size != snapshot_sizes[field]:
                        raise CaptureError("%s available span is inconsistent" % snap_label)
                    cursor += size
                elif size:
                    raise CaptureError("%s unavailable snapshot declares bytes" % snap_label)
                elif snap_status == "null_pointer" and pointer:
                    raise CaptureError("%s null snapshot has a nonzero pointer" % snap_label)
                elif snap_status not in {"null_pointer", "not_applicable"} and not pointer:
                    raise CaptureError("%s unavailable snapshot has a null pointer" % snap_label)
                snapshots[field] = snapshot
            parent_pointer = int(addresses["parent"], 16)
            parent_valid = (
                (parent_status == "unresolved" and parent_token != UINT32_MAX and
                 snapshots["parent_prefix"]["status"] == "not_applicable") or
                (parent_status == "resolved_absent" and parent_token == UINT32_MAX and
                 parent_pointer == 0 and snapshots["parent_prefix"]["status"] == "not_applicable") or
                (parent_status == "available" and parent_token == UINT32_MAX and
                 parent_pointer != 0 and snapshots["parent_prefix"]["status"] == "available") or
                (parent_status == "read_fault" and
                 snapshots["parent_prefix"]["status"] == "not_applicable"))
            if not parent_valid:
                raise CaptureError("%s parent resolution fields are inconsistent" % label)
            required_fields = ["outer_snapshot", "collection_snapshot"]
            if int(addresses["context"], 16):
                required_fields.append("context_snapshot")
            elif snapshots["context_snapshot"]["status"] != "null_pointer":
                raise CaptureError("%s null context lacks a null-pointer snapshot" % label)
            if direct:
                required_fields.append("record_tail_snapshot")
            for field, address_field in (("game_object_prefix", "game_object"),
                                         ("descriptor_prefix", "descriptor"),
                                         ("provider_prefix", "provider"),
                                         ("parent_prefix", "parent")):
                if int(addresses[address_field], 16) and (field != "parent_prefix" or
                                                          parent_status == "available"):
                    required_fields.append(field)
            complete = (all(snapshots[field]["status"] == "available" for field in required_fields) and
                        all(identities[field]["status"] != "read_fault" for field in identity_fields) and
                        parent_status != "read_fault")
            if (item_status == "captured") != complete:
                raise CaptureError("%s status disagrees with retained evidence" % label)

        ancestor_traces = _list(diagnostic.get("ancestor_traces"),
                                "record_writers.ancestor_traces", limits["ownership_traces"])
        trace_writer_records = set()
        for ti, trace in enumerate(ancestor_traces):
            label = "record_writers.ancestor_traces[%u]" % ti
            trace = _dict(trace, label)
            trace_status = _string(trace.get("status"), label + ".status", 32)
            if trace_status not in {"ancestor_missing", "unwind_failed"}:
                raise CaptureError("%s.status is unknown" % label)
            writer_record = _u32(trace.get("writer_record"), label + ".writer_record")
            if writer_record >= len(records) or writer_record in trace_writer_records:
                raise CaptureError("%s writer record is missing or duplicated" % label)
            trace_writer_records.add(writer_record)
            return_rva = _address(trace.get("return_rva"), label + ".return_rva")
            if (int(return_rva, 16) != int(records[writer_record]["return_rva"], 16) or
                    records[writer_record]["ownership_status"] != trace_status):
                raise CaptureError("%s disagrees with its writer refusal" % label)
            frames = _list(trace.get("frames"), label + ".frames",
                           limits["ownership_trace_frames"])
            for fi, frame in enumerate(frames):
                frame_label = label + ".frames[%u]" % fi
                frame = _dict(frame, frame_label)
                address = _address(frame.get("address"), frame_label + ".address")
                frame_status = _string(frame.get("status"), frame_label + ".status", 32)
                module_rva = frame.get("module_rva")
                if module_rva is not None:
                    module_rva = _address(module_rva, frame_label + ".module_rva")
                if frame_status == "module_relative":
                    valid_frame = (int(address, 16) != 0 and module_rva is not None and
                                   int(module_rva, 16) < executable["image_size"])
                elif frame_status == "outside_image":
                    # Version 2's unwind writer records the stack-end RIP as a
                    # final zero-address outside-image frame before unwindOne
                    # reports failure.  Keep this narrow compatibility for
                    # that existing format; zero addresses elsewhere remain
                    # malformed evidence.
                    valid_frame = (module_rva is None and
                                   (int(address, 16) != 0 or
                                    (trace_status == "unwind_failed" and
                                     fi == len(frames) - 1)))
                else:
                    raise CaptureError("%s.status is unknown" % frame_label)
                if not valid_frame:
                    raise CaptureError("%s fields disagree with status" % frame_label)

        linked_records = [record for record in records if record["ownership_status"] == "linked"]
        for ri, record in enumerate(records):
            ownership_id = record["ownership_id"]
            if ownership_id is not None and ownership_id >= len(ownerships):
                raise CaptureError("record_writers.records[%u].ownership_id is missing" % ri)
            if record["ownership_status"] == "linked":
                ownership = ownerships[ownership_id]
                if record["writer"] not in relation_writers[ownership["writer_relation"]]:
                    raise CaptureError("record writer and ownership relation disagree")
                if (int(record["owner"], 16) != int(ownership["writer_owner"], 16) or
                        record["thread_id"] != ownership["thread_id"] or
                        record["observed_frame"] != ownership["observed_frame"]):
                    raise CaptureError("record writer and ownership tuple disagree")
        for oi, ownership in enumerate(ownerships):
            sequences = [record["sequence"] for record in linked_records
                         if record["ownership_id"] == oi]
            if not sequences or ownership["first_sequence"] != min(sequences):
                raise CaptureError("ownership first sequence disagrees with linked writers")

        terminal_fields = ("opcode_mismatch", "ancestor_missing", "unwind_failed", "tuple_read_fault", "tuple_mismatch",
                           "registry_read_fault", "registry_mismatch", "record_range_mismatch",
                           "record_read_fault", "cache_conflicts", "record_overflow")
        status_counter = Counter(record["ownership_status"] for record in records)
        if (ownership_summary["stored"] != len(ownerships) or
                ownership_summary["linked"] != len(linked_records) or
                ownership_summary["deduplicated"] != ownership_summary["linked"] - len(ownerships) or
                ownership_summary["unsupported_writer"] != status_counter["unsupported_writer"] or
                ownership_summary["ancestor_traces"] != len(ancestor_traces)):
            raise CaptureError("record ownership summary counts disagree with arrays")
        for field in terminal_fields:
            status_name = "cache_conflict" if field == "cache_conflicts" else field
            if ownership_summary[field] != status_counter[status_name]:
                raise CaptureError("record ownership terminal counter disagrees with records")
        if ownership_summary["attempted"] != ownership_summary["linked"] + sum(
                ownership_summary[field] for field in terminal_fields):
            raise CaptureError("record ownership attempted count is inconsistent")
        complete_ownership = (ownership_summary["attempted"] > 0 and
                              ownership_summary["linked"] == ownership_summary["attempted"] and
                              not ownership_summary["record_overflow"] and
                              not ownership_summary["ancestor_trace_overflow"] and
                              not ownership_summary["byte_budget_declines"] and
                              not ownership_summary["read_faults"] and
                              all(item["status"] == "captured" for item in ownerships))
        expected_ownership_status = (
            "not_run" if hook_status == "not_run" else
            "unavailable" if hook_status in {"identity_mismatch", "opcode_mismatch", "install_failed"} else
            "captured" if complete_ownership else "partial")
        if ownership_status != expected_ownership_status:
            raise CaptureError("record_writers ownership status disagrees with counters")
        if ownership_status in {"not_run", "unavailable"} and (ownerships or ancestor_traces or
                                                               ownership_summary["attempted"]):
            raise CaptureError("inactive ownership diagnostic retains evidence")

    if (summary["stored"] != len(records) or summary["observed"] < len(records) or
            summary["completed"] > summary["stored"]):
        raise CaptureError("record_writers summary counts disagree with records")
    if summary["completed"] != sum(record["lookup_status"] != "pending" for record in records):
        raise CaptureError("record_writers completed count disagrees with record lifecycle")
    retained_bytes = cursor - packed_bytes
    if retained_bytes != summary["retained_bytes"] or retained_bytes > limits["bytes"]:
        raise CaptureError("record_writers retained byte count is invalid")
    complete_capture = (bool(records) and summary["observed"] == summary["stored"] and
                        summary["completed"] == summary["stored"] and
                        not any(summary[field] for field in
                                ("record_overflow", "byte_budget_declines", "read_faults",
                                 "context_failures", "unwind_failures", "completion_failures",
                                 "declined_unknown")))
    expected_status = ("not_run" if hook_status == "not_run" else
                       "unavailable" if hook_status in {"identity_mismatch", "opcode_mismatch", "install_failed"} else
                       "captured" if complete_capture else "partial")
    if status != expected_status:
        raise CaptureError("record_writers top status disagrees with counters")
    if status in {"not_run", "unavailable"} and (records or summary["stored"] or summary["retained_bytes"]):
        raise CaptureError("unavailable record_writers capture retains records")
    return diagnostic, uploads, records, ownerships, ancestor_traces, cursor


def _validate_record_ownership_contents(binary, diagnostic, records, ownerships):
    if not diagnostic or diagnostic["version"] < 2:
        return

    def payload(ownership, field):
        snapshot = ownership[field]
        if snapshot["status"] != "available":
            return None
        begin = snapshot["offset"]
        return memoryview(binary)[begin:begin + snapshot["bytes"]]

    linked_by_ownership = defaultdict(list)
    for record in records:
        if record["ownership_status"] == "linked":
            linked_by_ownership[record["ownership_id"]].append(record)

    for oi, ownership in enumerate(ownerships):
        label = "record_writers.ownerships[%u]" % oi
        writer_owner = int(ownership["writer_owner"], 16)
        collection_owner = int(ownership["collection_owner"], 16)
        registry = int(ownership["registry"], 16)
        context = int(ownership["context"], 16)
        collection_record = int(ownership["collection_record"], 16)
        relation = ownership["writer_relation"]
        linked = linked_by_ownership[oi]
        if relation == "inline_registry_plus_78":
            if writer_owner < 0x78 or registry != writer_owner - 0x78:
                raise CaptureError("%s inline writer/registry relation is invalid" % label)
            if any(int(record["object"], 16) != context for record in linked):
                raise CaptureError("%s inline context disagrees with writer object" % label)
        else:
            if writer_owner < 0x300 or collection_owner != writer_owner - 0x300:
                raise CaptureError("%s direct writer/collection relation is invalid" % label)
            if any(int(record["key"], 16) < 0x250 or
                   int(record["key"], 16) - 0x250 != collection_record for record in linked):
                raise CaptureError("%s direct record disagrees with writer key" % label)

        outer_bytes = payload(ownership, "outer_snapshot")
        if outer_bytes is not None:
            outer_fields = {
                "game_object": struct.unpack_from("<Q", outer_bytes, 0x20)[0],
                "descriptor": struct.unpack_from("<Q", outer_bytes, 0x50)[0],
                "provider": struct.unpack_from("<Q", outer_bytes, 0x180)[0],
                "parent": struct.unpack_from("<Q", outer_bytes, 0x1C0)[0],
                "collection_owner": struct.unpack_from("<Q", outer_bytes, 0x348)[0],
            }
            for field, value in outer_fields.items():
                if value != int(ownership[field], 16):
                    raise CaptureError("%s.%s disagrees with outer snapshot" % (label, field))
            if struct.unpack_from("<I", outer_bytes, 0x1B8)[0] != ownership["parent_token"]:
                raise CaptureError("%s.parent_token disagrees with outer snapshot" % label)

        context_bytes = payload(ownership, "context_snapshot")
        if context_bytes is not None and struct.unpack_from("<Q", context_bytes, 0x30)[0] != registry:
            raise CaptureError("%s.registry disagrees with context snapshot" % label)

        if relation == "direct_collection_plus_300":
            collection_bytes = payload(ownership, "collection_snapshot")
            if collection_bytes is not None:
                base = struct.unpack_from("<Q", collection_bytes, 0x280)[0]
                count = struct.unpack_from("<Q", collection_bytes, 0x298)[0]
                if (collection_record < base or (collection_record - base) % 0x2F0 or
                        (collection_record - base) // 0x2F0 >= count or
                        ownership["record_index"] != (collection_record - base) // 0x2F0):
                    raise CaptureError("%s collection record is outside the retained range" % label)
            record_tail = payload(ownership, "record_tail_snapshot")
            if record_tail is not None and struct.unpack_from("<Q", record_tail, 0x10)[0] != context:
                raise CaptureError("%s context disagrees with collection record" % label)


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


def _capture_binary(path, binary_name, total_packed_bytes, binary_ok):
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
    if total_packed_bytes:
        if binary is None:
            raise CaptureError("available blob payload is missing")
        if len(binary) != total_packed_bytes:
            raise CaptureError("binary length does not match packed available blobs")
    elif binary is not None and len(binary):
        raise CaptureError("binary has bytes but no available blob spans")
    elif binary is None and binary_ok:
        raise CaptureError("successful capture is missing its declared binary")
    return binary_path, binary


V3_RETIRED_SECTIONS = ("selection", "limits", "resources", "stacks", "events", "snapshots",
                       "draws", "blobs", "source_owner", "cpu_blobs")


def _load_capture_v3(path, root):
    """A capture from after the draw/mesh half retired (2026-09-23).

    It holds the executable identity, summary.binary_ok and the record-writer
    section. It comes back in load_capture's shape with every draw/mesh
    section empty, so analyse() treats it like any capture without a sealed
    frame. A v3 file that carries a retired section is refused rather than
    half-read.
    """
    binary_name = _string(root.get("binary"), "binary", 255)
    if not binary_name or Path(binary_name).name != binary_name:
        raise CaptureError("binary must be a leaf filename")
    executable = _dict(root.get("executable"), "executable")
    _u32(executable.get("pe_timestamp"), "executable.pe_timestamp")
    _u32(executable.get("image_size"), "executable.image_size")
    summary = _dict(root.get("summary"), "summary")
    binary_ok = _boolean(summary.get("binary_ok"), "summary.binary_ok")
    for retired in V3_RETIRED_SECTIONS:
        if retired in root:
            raise CaptureError("v3 capture carries the retired %s section" % retired)
    (record_writers, writer_uploads, writer_records, writer_ownerships,
     writer_ancestor_traces, total_packed_bytes) = _validate_record_writers(
        root, executable, [], [], 0, binary_ok)
    binary_path, binary = _capture_binary(path, binary_name, total_packed_bytes, binary_ok)
    _validate_record_ownership_contents(binary or b"", record_writers,
                                        writer_records, writer_ownerships)
    selection = {"discovery_mesh_frame": 0, "selected_mesh_frame": 0, "scene_frame": 0,
                 "has_discovery": False, "has_selection": False, "sealed": False,
                 "missed_window": False}
    return {
        "path": path, "binary_path": binary_path, "binary": binary or b"", "root": root,
        "selection": selection,
        "summary": {"status": "draw_capture_retired", "binary_ok": binary_ok,
                    "cpu_provenance_available": True, "foreign_writes": 0},
        "resources": [], "stacks": [], "events": [], "event_by_version": {},
        "snapshots": [], "draws": [], "blobs": [],
        "source_owner": None, "source_owner_attempts": [], "cpu_blobs": [],
        "record_writers": record_writers, "writer_uploads": writer_uploads,
        "writer_records": writer_records, "writer_ownerships": writer_ownerships,
        "writer_ancestor_traces": writer_ancestor_traces,
    }


def load_capture(path):
    path = Path(path)
    root = _read_json(path)
    schema = root.get("schema")
    if schema == SCHEMA_V3:
        return _load_capture_v3(path, root)
    if schema not in {SCHEMA_V1, SCHEMA_V2}:
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
    source_owner = None
    source_owner_attempts = []
    cpu_blobs = []
    record_writers = None
    writer_uploads = []
    writer_records = []
    writer_ownerships = []
    writer_ancestor_traces = []
    total_packed_bytes = packed_bytes
    if schema == SCHEMA_V2:
        source_owner, source_owner_attempts, cpu_blobs, total_packed_bytes = _validate_source_owner(
            root, executable, resources, events, summary, packed_bytes)
        (record_writers, writer_uploads, writer_records, writer_ownerships,
         writer_ancestor_traces, total_packed_bytes) = _validate_record_writers(
            root, executable, resources, source_owner_attempts, total_packed_bytes,
            summary["binary_ok"])
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
    binary_path, binary = _capture_binary(path, binary_name, total_packed_bytes, summary["binary_ok"])
    if schema == SCHEMA_V2:
        _validate_cpu_blob_contents(binary or b"", source_owner_attempts, cpu_blobs)
        _validate_record_ownership_contents(binary or b"", record_writers,
                                            writer_records, writer_ownerships)
    return {
        "path": path, "binary_path": binary_path, "binary": binary or b"", "root": root,
        "selection": selection, "summary": summary, "resources": resources,
        "stacks": stacks, "events": events, "event_by_version": event_by_version,
        "snapshots": snapshots, "draws": draws, "blobs": blobs,
        "source_owner": source_owner, "source_owner_attempts": source_owner_attempts,
        "cpu_blobs": cpu_blobs,
        "record_writers": record_writers, "writer_uploads": writer_uploads,
        "writer_records": writer_records, "writer_ownerships": writer_ownerships,
        "writer_ancestor_traces": writer_ancestor_traces,
    }


def _blob_bytes(capture, blob):
    if blob["status"] != "available":
        return None
    begin, size = blob["offset"], blob["bytes"]
    return memoryview(capture["binary"])[begin:begin + size]


def _cpu_blob_bytes(capture, reference):
    if reference is None:
        return None
    blob = capture["cpu_blobs"][reference]
    if blob["status"] != "available":
        return None
    return memoryview(capture["binary"])[blob["offset"]:blob["offset"] + blob["bytes"]]


def _writer_snapshot_bytes(capture, snapshot):
    if snapshot["status"] != "available":
        return None
    begin = snapshot["offset"]
    return memoryview(capture["binary"])[begin:begin + snapshot["bytes"]]


def _opaque_writer_context(capture, record):
    values = [record[field] for field in ("owner", "key", "builder", "object", "entry")]
    for field in ("key_snapshot", "builder_snapshot", "object_snapshot", "entry_snapshot"):
        snapshot = record[field]
        payload = _writer_snapshot_bytes(capture, snapshot)
        values.extend((snapshot["status"], None if payload is None else bytes(payload)))
    return tuple(values)


def _record_writer_candidates(capture, cpu_source, cpu_record):
    result = {
        "status": "unavailable", "reason": None,
        "attribution": "candidate_provenance_only", "candidate_count": 0,
        "opaque_context_consensus": "not_applicable", "candidates": [],
    }
    diagnostic = capture["record_writers"]
    if diagnostic is None:
        result["reason"] = "capture_has_no_record_writer_diagnostic"
        return result
    result.update(capture_status=diagnostic["status"], hook_status=diagnostic["hook_status"])
    if diagnostic["status"] in {"not_run", "unavailable"}:
        result["reason"] = "record_writer_%s" % diagnostic["status"]
        return result
    if cpu_source["status"] != "matched":
        result["reason"] = "cpu_source_record_was_not_proven"
        return result
    uploads = [upload for upload in capture["writer_uploads"]
               if upload["attempt"] == cpu_source["attempt"]]
    if len(uploads) != 1:
        result["status"] = "partial" if diagnostic["status"] == "partial" else "unavailable"
        result["reason"] = "source_owner_map_has_no_writer_cutoff"
        return result
    upload = uploads[0]
    cutoff = upload["cutoff"]
    result.update(upload_attempt=upload["attempt"], cutoff=cutoff)
    eligible = [record for record in capture["writer_records"]
                if record["lookup_status"] == "complete" and
                record["sequence"] <= cutoff and
                0 < record["completion_sequence"] <= cutoff]
    unreadable = sum(record["record"]["status"] != "available" for record in eligible)
    candidates = []
    candidate_contexts = []
    for record in eligible:
        payload = _writer_snapshot_bytes(capture, record["record"])
        if payload is None or bytes(payload) != bytes(cpu_record):
            continue
        candidate_contexts.append(_opaque_writer_context(capture, record))
        candidates.append({
            "record": record["id"], "sequence": record["sequence"],
            "completion_sequence": record["completion_sequence"],
            "lookup_status": record["lookup_status"],
            "writer": record["writer"], "return_rva": record["return_rva"],
            "thread_id": record["thread_id"], "observed_frame": record["observed_frame"],
            "owner": record["owner"], "key": record["key"],
            "builder": record["builder"], "object": record["object"],
            "entry": record["entry"],
            "context_status": record["context_status"],
            "key_snapshot_status": record["key_snapshot"]["status"],
            "builder_snapshot_status": record["builder_snapshot"]["status"],
            "object_snapshot_status": record["object_snapshot"]["status"],
            "entry_snapshot_status": record["entry_snapshot"]["status"],
            "ownership_status": (record["ownership_status"] if diagnostic["version"] >= 2
                                 else "unavailable_legacy_capture"),
            "ownership_id": (record["ownership_id"] if diagnostic["version"] >= 2 else None),
        })
    result.update(candidate_count=len(candidates), candidates=candidates,
                  eligible_records=len(eligible), unreadable_eligible_records=unreadable)
    if len(candidates) > 1:
        result["opaque_context_consensus"] = "same" if len(set(candidate_contexts)) == 1 else "different"
    elif len(candidates) == 1:
        result["opaque_context_consensus"] = "single_candidate"
    if diagnostic["status"] == "partial" or unreadable:
        result["status"] = "partial"
        result["reason"] = "record_writer_capture_is_incomplete"
    elif len(candidates) == 0:
        result["status"] = "no_candidate"
        result["reason"] = "no_pre_upload_writer_payload_matches_cpu_source"
    elif len(candidates) == 1:
        result["status"] = "unique_candidate"
        result["reason"] = None
    else:
        result["status"] = "multiple_candidates"
        result["reason"] = "multiple_pre_upload_writer_payloads_match_cpu_source"
    return result


def _record_ownership_candidates(capture, writer_result):
    result = {
        "status": "unavailable", "reason": None,
        "attribution": "candidate_ownership_provenance_only",
        "writer_candidate_count": writer_result["candidate_count"],
        "linked_candidate_count": 0, "ownership_count": 0,
        "ownership_consensus": "not_applicable", "candidates": [],
    }
    diagnostic = capture["record_writers"]
    if diagnostic is None or diagnostic["version"] == 1:
        result["reason"] = "capture_has_no_kinematic_ownership_diagnostic"
        return result
    ownership_status = diagnostic["ownership_status"]
    result["capture_status"] = ownership_status
    if ownership_status in {"not_run", "unavailable"}:
        result["reason"] = "kinematic_ownership_%s" % ownership_status
        return result
    if writer_result["status"] in {"unavailable", "partial"}:
        result["status"] = writer_result["status"]
        result["reason"] = "record_writer_candidates_are_%s" % writer_result["status"]
        return result
    if not writer_result["candidates"]:
        result["status"] = "no_candidate"
        result["reason"] = "no_record_writer_candidate"
        return result

    ownership_ids = []
    refused = Counter()
    for writer in writer_result["candidates"]:
        item = {
            "writer_record": writer["record"], "writer": writer["writer"],
            "writer_sequence": writer["sequence"],
            "ownership_status": writer["ownership_status"],
            "ownership_id": writer["ownership_id"],
        }
        if writer["ownership_status"] == "linked":
            ownership = capture["writer_ownerships"][writer["ownership_id"]]
            item["ownership"] = {
                "id": ownership["id"], "first_sequence": ownership["first_sequence"],
                "status": ownership["status"], "branch": ownership["branch"],
                "ancestor_return_rva": ownership["ancestor_return_rva"],
                "writer_relation": ownership["writer_relation"],
                "outer": ownership["outer"], "collection_owner": ownership["collection_owner"],
                "registry": ownership["registry"], "context": ownership["context"],
                "collection_record": ownership["collection_record"],
                "record_index": ownership["record_index"],
                "game_object": ownership["game_object"], "descriptor": ownership["descriptor"],
                "provider": ownership["provider"], "parent": ownership["parent"],
                "parent_token": ownership["parent_token"],
                "parent_status": ownership["parent_status"],
                "outer_identity": ownership["outer_identity"],
                "game_object_identity": ownership["game_object_identity"],
                "descriptor_identity": ownership["descriptor_identity"],
                "provider_identity": ownership["provider_identity"],
                "parent_identity": ownership["parent_identity"],
            }
            ownership_ids.append(writer["ownership_id"])
        else:
            refused[writer["ownership_status"]] += 1
        result["candidates"].append(item)
    unique_ids = sorted(set(ownership_ids))
    result.update(linked_candidate_count=len(ownership_ids), ownership_count=len(unique_ids),
                  refusal_outcomes=dict(sorted(refused.items())))
    if unique_ids:
        result["ownership_consensus"] = "single" if len(unique_ids) == 1 else "different"
    incomplete = (ownership_status == "partial" or len(ownership_ids) != len(writer_result["candidates"]) or
                  any(capture["writer_ownerships"][item]["status"] == "partial" for item in unique_ids))
    if incomplete:
        result["status"] = "partial"
        result["reason"] = "kinematic_ownership_capture_or_candidate_set_is_incomplete"
    elif len(unique_ids) == 1:
        result["status"] = "unique_candidate"
    elif len(unique_ids) > 1:
        result["status"] = "multiple_candidates"
        result["reason"] = "writer_candidates_link_to_multiple_ownership_records"
    else:
        result["status"] = "no_candidate"
        result["reason"] = "writer_candidates_have_no_linked_ownership_record"
    return result


def _source_result(status, reason, draw, slot, **extra):
    result = {"status": status, "reason": reason, "resource": draw["pool_resource"],
              "generation": draw["pool_generation"], "pool_slot": slot,
              "proof": "same_generation_exact_bytes" if status == "matched" else None}
    result.update(extra)
    return result


def _cpu_source_owner(capture, draw, pool_slot, pool_record, route):
    owner = capture["source_owner"]
    if owner is None:
        return _source_result("unavailable", "legacy_capture_has_no_cpu_source_owner", draw, pool_slot)
    if owner["status"] == "not_run":
        return _source_result("unavailable", "cpu_source_owner_not_run", draw, pool_slot)
    if owner["status"] not in {"captured", "partial"}:
        return _source_result("failure", "source_owner_%s" % owner["status"], draw, pool_slot)
    if route["status"] != "matched" or route.get("endpoint_kind") != "map":
        return _source_result("unmatched", "pool_generation_has_no_complete_map_endpoint", draw, pool_slot)
    event = capture["events"][route["endpoint_event"]]
    if event["resource"] != draw["pool_resource"] or event["generation"] != draw["pool_generation"]:
        return _source_result("unmatched", "copy_route_has_no_validated_slot_translation", draw, pool_slot,
                              event=event["id"])
    attempt_ref = event.get("source_owner_attempt")
    if attempt_ref is None:
        return _source_result("missing", "map_event_has_no_source_owner_attempt", draw, pool_slot,
                              event=event["id"])
    attempt = capture["source_owner_attempts"][attempt_ref]
    common = {"event": event["id"], "attempt": attempt_ref,
              "attempt_status": attempt["status"], "foreign_epoch": attempt["foreign_epoch"]}
    if (not capture["summary"]["cpu_provenance_available"] or attempt["foreign_epoch"] != 0):
        return _source_result("unmatched", "foreign_generation_cannot_be_joined", draw, pool_slot, **common)
    if attempt["status"] not in {"captured", "partial"}:
        return _source_result("failure", "attempt_%s" % attempt["status"], draw, pool_slot, **common)
    candidates = []
    for index, descriptor in enumerate(attempt["descriptors"]):
        if descriptor["destination_first_record"] <= pool_slot < descriptor["destination_end_record"]:
            candidates.append((index, descriptor))
    if not candidates:
        return _source_result("missing", "destination_slot_not_enumerated", draw, pool_slot, **common)
    if len(candidates) != 1:
        return _source_result("overlap", "destination_slot_has_multiple_source_descriptors", draw, pool_slot,
                              descriptors=[index for index, unused in candidates], **common)
    descriptor_index, descriptor = candidates[0]
    leaf = attempt["leaves"][descriptor["leaf"]]
    group = attempt["groups"][leaf["group"]]
    provenance = {
        "descriptor": descriptor_index, "descriptor_address": descriptor["address"],
        "source_address": descriptor["source_address"], "leaf": descriptor["leaf"],
        "leaf_address": leaf["address"], "leaf_owner_link": leaf["owner_link"], "group": leaf["group"],
        "group_address": group["address"],
        "destination_first_record": descriptor["destination_first_record"],
        "destination_end_record": descriptor["destination_end_record"],
    }
    common.update(provenance)
    enumeration_complete = (attempt["group_count"] == attempt["groups_scanned"] and
                            attempt["leaf_count"] == attempt["leaves_scanned"] and
                            attempt["descriptor_count"] == attempt["descriptors_scanned"])
    if attempt["status"] == "partial" and not enumeration_complete:
        return _source_result("partial", "descriptor_enumeration_was_partial", draw, pool_slot, **common)
    if descriptor["status"] != "captured":
        return _source_result("failure", "descriptor_%s" % descriptor["status"], draw, pool_slot, **common)
    payload = _cpu_blob_bytes(capture, descriptor["payload_blob"])
    if payload is None:
        return _source_result("failure", "source_payload_unavailable", draw, pool_slot, **common)
    local_record = pool_slot - descriptor["destination_first_record"]
    at = local_record * POOL_STRIDE
    cpu_record = payload[at:at + POOL_STRIDE]
    source_record_address = int(descriptor["source_address"], 16) + at
    common.update({"source_record": local_record, "source_record_address": "0x%x" % source_record_address,
                   "byte_equal": bytes(cpu_record) == bytes(pool_record)})
    if common["byte_equal"]:
        return _source_result("matched", None, draw, pool_slot, **common)
    return _source_result("mismatch", "cpu_and_gpu_record_bytes_differ", draw, pool_slot, **common)


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
            "CPU source addresses are capture-local provenance only; they are not stable object identities or a static classification.",
            "A CPU source is proven only by unique destination-slot coverage and exact 336-byte equality in the same retained Map generation.",
            "Writer payload equality before a Map cutoff is candidate provenance only; duplicates remain explicit and do not establish object identity or a causal generation link.",
            "Kinematic ownership is retained ancestor/context evidence attached to writer candidates; cache reuse, pointer stability, parent presence, and sentinel values do not establish static or physics state.",
            "The report classifies captured metadata and does not infer that an object is static from absent motion.",
        ],
        "counts": {"draws": len(capture["draws"]), "mesh_records": None,
                   "pixels": None, "coverage_pixels": None, "depth_agreeing_pixels": None,
                   "owned_visible_pixels": None, "owned_visible_records": None,
                   "valid_rigid_owned_pixels": None, "valid_rigid_owned_records": None,
                   "history_matched_owned_pixels": None, "history_matched_owned_records": None,
                   "metadata_groups": None},
        "groups": [], "missing": [],
        "cpu_source_owner": {
            "capture_status": (capture["source_owner"]["status"] if capture["source_owner"] else "unavailable"),
            "visible_record_outcomes": {},
        },
        "record_writers": {
            "capture_status": (capture["record_writers"]["status"]
                               if capture["record_writers"] else "unavailable"),
            "hook_status": (capture["record_writers"]["hook_status"]
                            if capture["record_writers"] else "unavailable"),
            "visible_record_outcomes": {},
        },
        "kinematic_ownership": {
            "capture_status": (capture["record_writers"].get("ownership_status", "unavailable")
                               if capture["record_writers"] else "unavailable"),
            "visible_record_outcomes": {},
        },
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
        cpu_source = _cpu_source_owner(capture, draw, pool_slot, pool_record, traces["pool"])
        writer_candidates = _record_writer_candidates(capture, cpu_source, pool_record)
        ownership_candidates = _record_ownership_candidates(capture, writer_candidates)
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
                "cpu_source_owners": [],
                "record_writer_candidates": [],
                "kinematic_ownership_candidates": [],
                "source_versions": {
                    "t33_pool": {"resource": draw["pool_resource"], "generation": draw["pool_generation"],
                                 "write_observed": draw["pool_write_observed"], "generation_matched": draw["pool_matched"],
                                 "upload_route": traces["pool"], "cpu_source_owner_outcomes": {}},
                    "ia_ids": {"resource": draw["id_resource"], "generation": draw["id_generation"],
                               "write_observed": draw["id_write_observed"], "generation_matched": draw["id_matched"],
                               "upload_route": traces["id"]},
                },
            }
        groups[group_key]["pixels"] += pixel_count
        groups[group_key]["records"] += 1
        groups[group_key]["valid_rigid_records"] += int(valid_rigid)
        groups[group_key]["history_matched_records"] += int(history_matched)
        group_outcomes = groups[group_key]["source_versions"]["t33_pool"]["cpu_source_owner_outcomes"]
        group_outcomes[cpu_source["status"]] = group_outcomes.get(cpu_source["status"], 0) + 1
        groups[group_key]["cpu_source_owners"].append(
            {"record": record_index, "pixels": pixel_count, **cpu_source})
        groups[group_key]["record_writer_candidates"].append(
            {"record": record_index, "pixels": pixel_count, **writer_candidates})
        groups[group_key]["kinematic_ownership_candidates"].append(
            {"record": record_index, "pixels": pixel_count, **ownership_candidates})
        owners.append({
            "record": record_index, "draw": draw_id, "instance": local,
            "pixels": pixel_count, "pool_slot": pool_slot,
            "instance_second_word": second_word, "t33_word28": word28,
            "t33_word320": word320, "key16": list(draw["key16"]),
            "pool_generation": draw["pool_generation"], "id_generation": draw["id_generation"],
            "valid_rigid": valid_rigid, "history_matched": history_matched,
            "cpu_source_owner": cpu_source,
            "record_writer_candidates": writer_candidates,
            "kinematic_ownership_candidates": ownership_candidates,
        })
    report["groups"] = list(groups.values())
    report["counts"]["metadata_groups"] = len(report["groups"])
    report["cpu_source_owner"]["visible_record_outcomes"] = dict(sorted(Counter(
        owner["cpu_source_owner"]["status"] for owner in owners).items()))
    report["record_writers"]["visible_record_outcomes"] = dict(sorted(Counter(
        owner["record_writer_candidates"]["status"] for owner in owners).items()))
    report["kinematic_ownership"]["visible_record_outcomes"] = dict(sorted(Counter(
        owner["kinematic_ownership_candidates"]["status"] for owner in owners).items()))
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
            cpu = group["source_versions"]["t33_pool"]["cpu_source_owner_outcomes"]
            print("  group %u: %u px/%u records, t33[28]=%u t33[320]=%u id.second=%u; upload routes pool=%s ids=%s; CPU sources=%s" %
                  (index, group["pixels"], group["records"], group["t33_word28"],
                   group["t33_word320"], group["instance_second_word"], pool["status"], ids["status"],
                   json.dumps(cpu, sort_keys=True)))
        outcomes = report["cpu_source_owner"]["visible_record_outcomes"]
        print("CPU source owner: capture=%s; visible record outcomes=%s" %
              (report["cpu_source_owner"]["capture_status"], json.dumps(outcomes, sort_keys=True)))
        writer_outcomes = report["record_writers"]["visible_record_outcomes"]
        print("record writers: capture=%s hook=%s; visible record outcomes=%s" %
              (report["record_writers"]["capture_status"], report["record_writers"]["hook_status"],
               json.dumps(writer_outcomes, sort_keys=True)))
        ownership_outcomes = report["kinematic_ownership"]["visible_record_outcomes"]
        print("kinematic ownership candidates: capture=%s; visible record outcomes=%s" %
              (report["kinematic_ownership"]["capture_status"],
               json.dumps(ownership_outcomes, sort_keys=True)))
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


def _source_owner_fixture(root, binary):
    root = copy.deepcopy(root)
    root["schema"] = SCHEMA_V2
    root["executable"].update(pe_timestamp=1788384820, image_size=104894464)
    for event in root["events"]:
        event["source_owner_attempt"] = None
    root["events"][1]["source_owner_attempt"] = 0
    source_address = 0x710000
    descriptor_address = 0x720000
    raw = bytearray(72)
    struct.pack_into("<Q", raw, 8, source_address)
    struct.pack_into("<2I", raw, 0x38, 1, 2)
    pool_blob = root["blobs"][0]
    pool = binary[pool_blob["offset"]:pool_blob["offset"] + pool_blob["bytes"]]
    source = pool[POOL_STRIDE:POOL_STRIDE * 3]
    raw_at = len(binary)
    source_at = raw_at + len(raw)
    root["source_owner"] = {
        "status": "captured", "synthetic_fixture": False,
        "expected_pe_timestamp": 1788384820,
        "expected_image_size": 104894464, "callsite_rva": SOURCE_OWNER_CALLSITE_RVA,
        "limits": copy.deepcopy(SOURCE_OWNER_LIMITS),
        "summary": {"maps_considered": 1, "identity_rejects": 0, "opcode_rejects": 0,
                    "unwind_attempts": 1, "unwind_failures": 0, "callsite_matches": 1,
                    "resource_matches": 1, "attempts_stored": 1, "complete_attempts": 1,
                    "partial_attempts": 0, "read_faults": 0, "group_overflow": 0,
                    "leaf_overflow": 0, "descriptor_overflow": 0, "attempt_overflow": 0,
                    "cpu_byte_declines": 0, "metadata_changes": 0},
        "attempts": [{
            "id": 0, "event": 1, "resource": 0, "generation": 2, "mesh_frame": 11,
            "foreign_epoch": 0, "status": "captured", "unwind_status": "matched",
            "unwind_frames": 2, "return_rva": SOURCE_OWNER_CALLSITE_RVA,
            "nested_owner": "0x700000", "nested_owner_field_130": "0x701300",
            "nested_owner_field_138": "0x701380", "wrapper": "0x700140",
            "native_resource": "0x1000",
            "stride": POOL_STRIDE, "group_count": 1, "groups_scanned": 1,
            "leaf_count": 1, "leaves_scanned": 1, "descriptor_count": 1,
            "descriptors_scanned": 1, "retained_bytes": len(raw) + len(source),
            "groups": [{"address": "0x730000", "leaf_count": 1, "leaves_scanned": 1}],
            "leaves": [{"group": 0, "address": "0x740000", "owner_link": "0x700000",
                        "base_record": 0, "record_count": 3}],
            "descriptors": [{"leaf": 0, "list": 0, "entry": 0, "entry_stride": 72,
                             "address": "0x%x" % descriptor_address,
                             "source_address": "0x%x" % source_address,
                             "destination_relative_record": 1, "record_count": 2,
                             "destination_first_record": 1, "destination_end_record": 3,
                             "raw_blob": 0, "payload_blob": 1, "status": "captured"}],
        }],
    }
    root["cpu_blobs"] = [
        {"id": 0, "kind": "descriptor", "attempt": 0, "descriptor": 0,
         "address": "0x%x" % descriptor_address, "offset": raw_at,
         "bytes": len(raw), "status": "available"},
        {"id": 1, "kind": "source_records", "attempt": 0, "descriptor": 0,
         "address": "0x%x" % source_address, "offset": source_at,
         "bytes": len(source), "status": "available"},
    ]
    return root, binary + bytes(raw) + source


def _record_writer_fixture(root, binary):
    root = copy.deepcopy(root)
    attempt = root["source_owner"]["attempts"][0]
    descriptor = attempt["descriptors"][0]
    source_blob = root["cpu_blobs"][descriptor["payload_blob"]]
    source = binary[source_blob["offset"]:source_blob["offset"] + source_blob["bytes"]]
    payloads = (source[:POOL_STRIDE], source[POOL_STRIDE:2 * POOL_STRIDE], source[:POOL_STRIDE])
    contexts = (("0x920078", "0x820000", "0x830000", "0x930000"),
                ("0xa10300", "0xa40250", "0x831000", "0x0"),
                ("0x920078", "0x820000", "0x830000", "0x930000"))
    writers = ("inline_42b4ed6", "direct_43130aa", "inline_42b4ed6")
    chunks = [binary]
    cursor = len(binary)
    records = []
    key_bytes = bytes(range(32))
    entry_bytes = bytes(range(0x38))
    for index, (payload, context, writer) in enumerate(zip(payloads, contexts, writers)):
        owner, key, entry, object_address = context
        record_span = {"status": "available", "offset": cursor, "bytes": len(payload)}
        chunks.append(payload)
        cursor += len(payload)
        key_span = {"status": "available", "offset": cursor, "bytes": len(key_bytes)}
        chunks.append(key_bytes)
        cursor += len(key_bytes)
        builder_span = {"status": "not_applicable", "offset": cursor, "bytes": 0}
        if writer == "inline_42b4ed6":
            object_bytes = bytes([0x40]) * 176
            object_span = {"status": "available", "offset": cursor, "bytes": len(object_bytes)}
            chunks.append(object_bytes)
            cursor += len(object_bytes)
        else:
            object_span = {"status": "not_applicable", "offset": cursor, "bytes": 0}
        entry_span = {"status": "available", "offset": cursor, "bytes": len(entry_bytes)}
        chunks.append(entry_bytes)
        cursor += len(entry_bytes)
        records.append({
            "id": index, "sequence": index * 2 + 1, "writer": writer,
            "return_rva": "0x" + writer.rsplit("_", 1)[1], "thread_id": 17,
            "observed_frame": 10 if index == 2 else 10 + index, "owner": owner, "key": key,
            "builder": "0x0", "object": object_address, "entry": entry,
            "context_status": "complete", "lookup_status": "complete",
            "completion_sequence": (index + 1) * 2, "record": record_span,
            "key_snapshot": key_span, "builder_snapshot": builder_span,
            "object_snapshot": object_span, "entry_snapshot": entry_span,
        })
    retained = cursor - len(binary)
    root["record_writers"] = {
        "version": 1, "status": "captured", "hook_status": "finished",
        "limits": copy.deepcopy(RECORD_WRITER_LIMITS),
        "summary": {"observed": 3, "stored": 3, "completed": 3,
                    "retained_bytes": retained, "record_overflow": 0,
                    "byte_budget_declines": 0, "read_faults": 0,
                    "context_failures": 0, "declined_management": 0,
                    "declined_unknown": 0, "unwind_failures": 0,
                    "completion_failures": 0},
        "uploads": [{"attempt": 0, "resource": attempt["resource"],
                     "generation": attempt["generation"], "cutoff": 5}],
        "records": records,
    }
    return root, b"".join(chunks)


def _kinematic_ownership_fixture(root, binary):
    root = copy.deepcopy(root)
    diagnostic = root["record_writers"]
    diagnostic["version"] = 2
    diagnostic["limits"].update(RECORD_OWNERSHIP_LIMITS)
    writer_start = diagnostic["records"][0]["record"]["offset"]
    repacked = [binary[:writer_start]]
    writer_cursor = writer_start
    for record in diagnostic["records"]:
        for field in ("record", "key_snapshot", "builder_snapshot", "object_snapshot", "entry_snapshot"):
            snapshot = record[field]
            old_payload = (binary[snapshot["offset"]:snapshot["offset"] + snapshot["bytes"]]
                           if snapshot["status"] == "available" else b"")
            if field == "object_snapshot" and record["writer"] in {
                    "primary_42b42ef", "inline_42b4ed6"}:
                old_payload += bytes(RECORD_OWNERSHIP_LIMITS["ownership_context_bytes"] - len(old_payload))
                snapshot["bytes"] = len(old_payload)
            snapshot["offset"] = writer_cursor
            if old_payload:
                repacked.append(old_payload)
                writer_cursor += len(old_payload)
    binary = b"".join(repacked)
    diagnostic["summary"]["retained_bytes"] = len(binary) - writer_start
    for index, record in enumerate(diagnostic["records"]):
        record["ownership_status"] = "linked"
        record["ownership_id"] = 0 if index in {0, 2} else 1

    chunks = [binary]
    cursor = len(binary)

    def identity(address, status="outside_image"):
        if address == 0:
            return {"address": "0x0", "vtable": "0x0", "status": "null_pointer",
                    "vtable_rva": None}
        if status == "module_relative":
            return {"address": "0x%x" % address, "vtable": "0x710000", "status": status,
                    "vtable_rva": "0x1000"}
        return {"address": "0x%x" % address, "vtable": "0x710000", "status": status,
                "vtable_rva": None}

    def span(data=None, status=None):
        nonlocal cursor
        if data is None:
            return {"status": status, "offset": cursor, "bytes": 0}
        result = {"status": "available", "offset": cursor, "bytes": len(data)}
        chunks.append(bytes(data))
        cursor += len(data)
        return result

    def make_ownership(index, relation, branch, outer, collection, registry, context,
                       collection_record, game_object, descriptor, provider, parent,
                       parent_token, record_index):
        outer_bytes = bytearray(RECORD_OWNERSHIP_LIMITS["ownership_outer_bytes"])
        struct.pack_into("<Q", outer_bytes, 0x20, game_object)
        struct.pack_into("<Q", outer_bytes, 0x50, descriptor)
        struct.pack_into("<Q", outer_bytes, 0x180, provider)
        struct.pack_into("<I", outer_bytes, 0x1B8, parent_token)
        struct.pack_into("<Q", outer_bytes, 0x1C0, parent)
        struct.pack_into("<Q", outer_bytes, 0x348, collection)
        collection_bytes = bytearray(RECORD_OWNERSHIP_LIMITS["ownership_collection_bytes"])
        context_bytes = (bytearray(RECORD_OWNERSHIP_LIMITS["ownership_context_bytes"])
                         if context else None)
        if context_bytes is not None:
            struct.pack_into("<Q", context_bytes, 0x30, registry)
        direct = relation == "direct_collection_plus_300"
        if direct:
            struct.pack_into("<Q", collection_bytes, 0x280, collection_record)
            struct.pack_into("<Q", collection_bytes, 0x298, 1)
            record_tail = bytearray(RECORD_OWNERSHIP_LIMITS["ownership_record_tail_bytes"])
            struct.pack_into("<Q", record_tail, 0x10, context)
        else:
            record_tail = None
        ancestor_rva = 0x431B212 if branch == "direct_4321940" else 0x431B21F
        writer_owner = registry + 0x78 if not direct else collection + 0x300
        result = {
            "id": index, "first_sequence": 1 if index == 0 else 3,
            "observed_frame": 10 if index == 0 else 11, "thread_id": 17,
            "status": "captured", "branch": branch,
            "ancestor_return_rva": "0x%x" % ancestor_rva, "unwind_depth": 3,
            "writer_relation": relation, "writer_owner": "0x%x" % writer_owner,
            "outer": "0x%x" % outer, "collection_owner": "0x%x" % collection,
            "registry": "0x%x" % registry, "context": "0x%x" % context,
            "collection_record": "0x%x" % collection_record,
            "record_index": record_index,
            "record_status": "validated" if direct else "not_applicable",
            "game_object": "0x%x" % game_object, "descriptor": "0x%x" % descriptor,
            "provider": "0x%x" % provider, "parent": "0x%x" % parent,
            "parent_token": parent_token,
            "parent_status": ("available" if parent_token == UINT32_MAX and parent else
                              "resolved_absent" if parent_token == UINT32_MAX else "unresolved"),
            "outer_identity": identity(outer, "module_relative"),
            "game_object_identity": identity(game_object),
            "descriptor_identity": identity(descriptor),
            "provider_identity": identity(provider), "parent_identity": identity(parent),
            "outer_snapshot": span(outer_bytes), "collection_snapshot": span(collection_bytes),
            "context_snapshot": span(context_bytes, "null_pointer"),
            "record_tail_snapshot": span(record_tail, "not_applicable"),
        }
        prefixes = {}
        for field, address in (("game_object_prefix", game_object),
                               ("descriptor_prefix", descriptor),
                               ("provider_prefix", provider), ("parent_prefix", parent)):
            if field == "parent_prefix" and parent_token != UINT32_MAX:
                prefixes[field] = span(None, "not_applicable")
            else:
                prefixes[field] = span(bytearray(RECORD_OWNERSHIP_LIMITS["ownership_opaque_prefix_bytes"])
                                       if address else None,
                                       "not_applicable" if field == "parent_prefix" and not address
                                       else "null_pointer" if not address else None)
        result.update(prefixes)
        return result

    diagnostic["ownerships"] = [
        make_ownership(0, "inline_registry_plus_78", "direct_4321940",
                       0x900000, 0x910000, 0x920000, 0x930000, 0,
                       0x940000, 0x950000, 0x960000, 0x970000, UINT32_MAX, None),
        make_ownership(1, "direct_collection_plus_300", "virtual_50",
                       0xA00000, 0xA10000, 0, 0, 0xA40000,
                       0, 0xA50000, 0xA60000, 0, UINT32_MAX, 0),
    ]
    diagnostic["ownership_status"] = "captured"
    diagnostic["ownership_summary"] = {
        "attempted": 3, "linked": 3, "stored": 2, "deduplicated": 1,
        "unsupported_writer": 0, "opcode_mismatch": 0, "ancestor_missing": 0, "unwind_failed": 0,
        "tuple_read_fault": 0, "tuple_mismatch": 0, "registry_read_fault": 0,
        "registry_mismatch": 0, "record_range_mismatch": 0, "record_read_fault": 0,
        "cache_conflicts": 0, "record_overflow": 0, "byte_budget_declines": 0,
        "read_faults": 0, "ancestor_traces": 0, "ancestor_trace_overflow": 0,
    }
    diagnostic["ancestor_traces"] = []
    diagnostic["summary"]["retained_bytes"] += cursor - len(binary)
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

        # V2 proves each visible pool record only against exact CPU bytes from
        # the same completed Map generation.  A changed byte remains a
        # mismatch rather than being attributed by address or layout alone.
        v2_root, v2_binary = _source_owner_fixture(root, binary)
        path = _write_fixture(temp, v2_root, v2_binary)
        v2_report = analyse(load_capture(path), include_records=True)
        assert v2_report["cpu_source_owner"]["visible_record_outcomes"] == {"matched": 2}
        assert all(record["cpu_source_owner"]["proof"] == "same_generation_exact_bytes"
                   for record in v2_report["records"])
        assert v2_report["record_writers"]["visible_record_outcomes"] == {"unavailable": 2}

        # Writer payload equality is bounded by the nominated Map cutoff and
        # remains candidate-only even when exactly one stored event matches.
        writer_root, writer_binary = _record_writer_fixture(v2_root, v2_binary)
        path = _write_fixture(temp, writer_root, writer_binary)
        writer_report = analyse(load_capture(path), include_records=True)
        assert writer_report["record_writers"]["visible_record_outcomes"] == {"unique_candidate": 2}
        writer_results = [record["record_writer_candidates"] for record in writer_report["records"]]
        assert [item["candidates"][0]["sequence"] for item in writer_results] == [1, 3]
        assert all(item["attribution"] == "candidate_provenance_only" for item in writer_results)
        assert writer_report["kinematic_ownership"]["visible_record_outcomes"] == {"unavailable": 2}

        # V2 writer captures can attach a bounded KinematicRig ancestor tuple.
        # The first and third writer events deliberately deduplicate to one
        # ownership item; the direct recipe exercises a valid null context.
        ownership_root, ownership_binary = _kinematic_ownership_fixture(writer_root, writer_binary)
        path = _write_fixture(temp, ownership_root, ownership_binary)
        ownership_report = analyse(load_capture(path), include_records=True)
        assert ownership_report["kinematic_ownership"]["visible_record_outcomes"] == {
            "unique_candidate": 2}
        ownership_results = [record["kinematic_ownership_candidates"]
                             for record in ownership_report["records"]]
        assert [item["candidates"][0]["ownership"]["branch"]
                for item in ownership_results] == ["direct_4321940", "virtual_50"]
        assert ownership_results[1]["candidates"][0]["ownership"]["context"] == "0x0"
        assert ownership_results[1]["candidates"][0]["ownership"]["record_index"] == 0
        assert ownership_root["record_writers"]["ownership_summary"]["deduplicated"] == 1

        # The v2 producer retains the stack-end RIP as a final zero
        # outside-image frame when unwindOne fails.  Accept only that exact
        # terminal frame shape, while keeping all other frame addresses strict.
        unwind_root = copy.deepcopy(ownership_root)
        unwind_record = unwind_root["record_writers"]["records"][2]
        unwind_record.update(ownership_id=None, ownership_status="unwind_failed")
        unwind_summary = unwind_root["record_writers"]["ownership_summary"]
        unwind_summary.update(linked=2, deduplicated=0, unwind_failed=1,
                              ancestor_traces=1)
        unwind_root["record_writers"]["ownership_status"] = "partial"
        unwind_root["record_writers"]["ancestor_traces"] = [{
            "status": "unwind_failed", "writer_record": 2,
            "return_rva": unwind_record["return_rva"],
            "frames": [
                {"address": "0x431b21f", "status": "module_relative",
                 "module_rva": "0x31b21f"},
                {"address": "0x0", "status": "outside_image",
                 "module_rva": None},
            ],
        }]
        path = _write_fixture(temp, unwind_root, ownership_binary)
        unwind_report = analyse(load_capture(path), include_records=True)
        assert unwind_report["kinematic_ownership"]["visible_record_outcomes"] == {
            "partial": 2}

        def rejected_trace_frame(mutator):
            value = copy.deepcopy(unwind_root)
            mutator(value["record_writers"]["ancestor_traces"][0]["frames"])
            path = _write_fixture(temp, value, ownership_binary)
            try:
                load_capture(path)
            except CaptureError:
                return
            raise AssertionError("malformed unwind trace frame accepted")

        rejected_trace_frame(lambda frames: frames.insert(
            0, {"address": "0x0", "status": "outside_image", "module_rva": None}))
        rejected_trace_frame(lambda frames: frames[0].update(
            address="0x0", status="module_relative", module_rva="0x0"))
        rejected_trace_frame(lambda frames: frames[1].update(module_rva="0x1"))

        ownership_duplicate = copy.deepcopy(ownership_root)
        ownership_duplicate["record_writers"]["uploads"][0]["cutoff"] = 6
        path = _write_fixture(temp, ownership_duplicate, ownership_binary)
        ownership_duplicate_report = analyse(load_capture(path), include_records=True)
        first_ownership = ownership_duplicate_report["records"][0]["kinematic_ownership_candidates"]
        assert first_ownership["writer_candidate_count"] == 2
        assert first_ownership["linked_candidate_count"] == 2
        assert first_ownership["ownership_count"] == 1
        assert first_ownership["status"] == "unique_candidate"

        refused_ownership = copy.deepcopy(ownership_root)
        refused_record = refused_ownership["record_writers"]["records"][0]
        refused_record.update(ownership_id=None, ownership_status="tuple_mismatch")
        refused_ownership["record_writers"]["ownerships"][0]["first_sequence"] = 5
        refused_summary = refused_ownership["record_writers"]["ownership_summary"]
        refused_summary.update(linked=2, deduplicated=0, tuple_mismatch=1)
        refused_ownership["record_writers"]["ownership_status"] = "partial"
        path = _write_fixture(temp, refused_ownership, ownership_binary)
        refused_report = analyse(load_capture(path), include_records=True)
        refused_result = refused_report["records"][0]["kinematic_ownership_candidates"]
        assert refused_result["status"] == "partial"
        assert refused_result["refusal_outcomes"] == {"tuple_mismatch": 1}

        unsupported_ownership = copy.deepcopy(ownership_root)
        unsupported_record = unsupported_ownership["record_writers"]["records"][0]
        unsupported_record.update(ownership_id=None, ownership_status="unsupported_writer")
        unsupported_ownership["record_writers"]["ownerships"][0]["first_sequence"] = 5
        unsupported_summary = unsupported_ownership["record_writers"]["ownership_summary"]
        unsupported_summary.update(attempted=2, linked=2, deduplicated=0, unsupported_writer=1)
        path = _write_fixture(temp, unsupported_ownership, ownership_binary)
        unsupported_report = analyse(load_capture(path), include_records=True)
        assert unsupported_report["records"][0]["kinematic_ownership_candidates"][
            "refusal_outcomes"] == {"unsupported_writer": 1}

        fault_ownership = copy.deepcopy(ownership_root)
        fault_item = fault_ownership["record_writers"]["ownerships"][0]
        fault_span = fault_item["parent_prefix"]
        fault_at, fault_size = fault_span["offset"], fault_span["bytes"]
        fault_span.update(status="not_applicable", bytes=0)
        fault_item.update(status="partial", parent_status="read_fault")
        for item in fault_ownership["record_writers"]["ownerships"][1:]:
            for field in ("outer_snapshot", "collection_snapshot", "context_snapshot",
                          "record_tail_snapshot", "game_object_prefix", "descriptor_prefix",
                          "provider_prefix", "parent_prefix"):
                item[field]["offset"] -= fault_size
        fault_ownership["record_writers"]["summary"]["retained_bytes"] -= fault_size
        fault_ownership["record_writers"]["ownership_summary"]["read_faults"] = 1
        fault_ownership["record_writers"]["ownership_status"] = "partial"
        fault_binary = ownership_binary[:fault_at] + ownership_binary[fault_at + fault_size:]
        path = _write_fixture(temp, fault_ownership, fault_binary)
        fault_ownership_report = analyse(load_capture(path), include_records=True)
        assert fault_ownership_report["kinematic_ownership"]["visible_record_outcomes"] == {"partial": 2}

        duplicate = copy.deepcopy(writer_root)
        duplicate["record_writers"]["uploads"][0]["cutoff"] = 6
        path = _write_fixture(temp, duplicate, writer_binary)
        duplicate_report = analyse(load_capture(path), include_records=True)
        first_duplicate = duplicate_report["records"][0]["record_writer_candidates"]
        assert first_duplicate["status"] == "multiple_candidates"
        assert first_duplicate["candidate_count"] == 2
        assert first_duplicate["opaque_context_consensus"] == "same"
        assert [item["sequence"] for item in first_duplicate["candidates"]] == [1, 5]

        different = copy.deepcopy(duplicate)
        different["record_writers"]["records"][2]["owner"] = "0x899000"
        path = _write_fixture(temp, different, writer_binary)
        different_report = analyse(load_capture(path), include_records=True)
        assert different_report["records"][0]["record_writer_candidates"]["opaque_context_consensus"] == "different"

        no_candidate_bytes = bytearray(writer_binary)
        for record in writer_root["record_writers"]["records"][:2]:
            no_candidate_bytes[record["record"]["offset"] + 28] ^= 1
        path = _write_fixture(temp, writer_root, bytes(no_candidate_bytes))
        no_candidate_report = analyse(load_capture(path), include_records=True)
        assert no_candidate_report["record_writers"]["visible_record_outcomes"] == {"no_candidate": 2}

        writer_partial = copy.deepcopy(writer_root)
        writer_partial["record_writers"]["status"] = "partial"
        writer_partial["record_writers"]["summary"]["record_overflow"] = 1
        path = _write_fixture(temp, writer_partial, writer_binary)
        partial_writer_report = analyse(load_capture(path), include_records=True)
        assert partial_writer_report["record_writers"]["visible_record_outcomes"] == {"partial": 2}
        assert all(record["record_writer_candidates"]["candidate_count"] == 1
                   for record in partial_writer_report["records"])

        writer_fault = copy.deepcopy(writer_root)
        faulty = writer_fault["record_writers"]["records"][0]
        removed_at = faulty["key_snapshot"]["offset"]
        removed_bytes = faulty["key_snapshot"]["bytes"]
        faulty["key_snapshot"].update(status="read_fault", bytes=0)
        faulty["context_status"] = "partial"
        for record in writer_fault["record_writers"]["records"]:
            for field in ("record", "key_snapshot", "builder_snapshot", "object_snapshot", "entry_snapshot"):
                if record[field]["offset"] > removed_at:
                    record[field]["offset"] -= removed_bytes
        writer_fault["record_writers"]["status"] = "partial"
        writer_fault["record_writers"]["summary"]["retained_bytes"] -= removed_bytes
        writer_fault["record_writers"]["summary"]["read_faults"] = 1
        writer_fault["record_writers"]["summary"]["context_failures"] = 1
        fault_binary = writer_binary[:removed_at] + writer_binary[removed_at + removed_bytes:]
        path = _write_fixture(temp, writer_fault, fault_binary)
        fault_report = analyse(load_capture(path), include_records=True)
        assert fault_report["record_writers"]["visible_record_outcomes"] == {"partial": 2}

        for top_status, hook_status in (("unavailable", "opcode_mismatch"), ("not_run", "not_run")):
            inactive = copy.deepcopy(writer_root)
            start = inactive["record_writers"]["records"][0]["record"]["offset"]
            inactive["record_writers"].update(status=top_status, hook_status=hook_status, records=[])
            inactive["record_writers"]["uploads"][0]["cutoff"] = 0
            for field in inactive["record_writers"]["summary"]:
                inactive["record_writers"]["summary"][field] = 0
            path = _write_fixture(temp, inactive, writer_binary[:start])
            inactive_report = analyse(load_capture(path), include_records=True)
            assert inactive_report["record_writers"]["visible_record_outcomes"] == {"unavailable": 2}

            inactive_v2 = copy.deepcopy(inactive)
            inactive_v2["record_writers"]["version"] = 2
            inactive_v2["record_writers"]["limits"].update(RECORD_OWNERSHIP_LIMITS)
            inactive_v2["record_writers"]["ownership_status"] = top_status
            inactive_v2["record_writers"]["ownership_summary"] = {
                "attempted": 0, "linked": 0, "stored": 0, "deduplicated": 0,
                "unsupported_writer": 0, "opcode_mismatch": 0, "ancestor_missing": 0,
                "unwind_failed": 0, "tuple_read_fault": 0, "tuple_mismatch": 0,
                "registry_read_fault": 0, "registry_mismatch": 0,
                "record_range_mismatch": 0, "record_read_fault": 0,
                "cache_conflicts": 0, "record_overflow": 0, "byte_budget_declines": 0,
                "read_faults": 0, "ancestor_traces": 0, "ancestor_trace_overflow": 0,
            }
            inactive_v2["record_writers"]["ownerships"] = []
            inactive_v2["record_writers"]["ancestor_traces"] = []
            path = _write_fixture(temp, inactive_v2, writer_binary[:start])
            inactive_v2_report = analyse(load_capture(path), include_records=True)
            assert inactive_v2_report["kinematic_ownership"]["visible_record_outcomes"] == {
                "unavailable": 2}

            # v3 (2026-09-23): the same writer section and nothing of the
            # retired draw/mesh capture. It loads, reports why no record is
            # owned, and refuses a file that still carries a retired section.
            v3 = {"schema": SCHEMA_V3, "binary": inactive_v2["binary"],
                  "executable": copy.deepcopy(inactive_v2["executable"]),
                  "summary": {"binary_ok": True},
                  "record_writers": copy.deepcopy(inactive_v2["record_writers"])}
            v3["record_writers"]["uploads"] = []
            path = _write_fixture(temp, v3, b"")
            v3_report = analyse(load_capture(path), include_records=True)
            assert v3_report["status"] == "draw_capture_retired"
            assert v3_report["record_writers"]["capture_status"] == top_status
            assert v3_report["missing"] == [{"scope": "capture", "reason": "draw_capture_retired"}]
            assert v3_report["records"] == []
            for retired in ("draws", "source_owner"):
                stale = copy.deepcopy(v3)
                stale[retired] = copy.deepcopy(inactive_v2[retired])
                path = _write_fixture(temp, stale, b"")
                try:
                    load_capture(path)
                except CaptureError:
                    pass
                else:
                    raise AssertionError("v3 capture carrying %s accepted" % retired)
            uploaded = copy.deepcopy(v3)
            uploaded["record_writers"]["uploads"] = copy.deepcopy(
                inactive_v2["record_writers"]["uploads"])
            path = _write_fixture(temp, uploaded, b"")
            try:
                load_capture(path)
            except CaptureError:
                pass
            else:
                raise AssertionError("v3 capture with an upload join accepted")

            # The record-writer section as version 3 (2026-09-24): no upload
            # join at all. It loads the same way, and a version 3 section
            # that still carries an uploads array is refused.
            v3_writers = copy.deepcopy(v3)
            v3_writers["record_writers"]["version"] = 3
            del v3_writers["record_writers"]["uploads"]
            path = _write_fixture(temp, v3_writers, b"")
            v3_writers_report = analyse(load_capture(path), include_records=True)
            assert v3_writers_report["status"] == "draw_capture_retired"
            assert v3_writers_report["record_writers"]["capture_status"] == top_status
            assert v3_writers_report["kinematic_ownership"]["capture_status"] == top_status
            stale_uploads = copy.deepcopy(v3_writers)
            stale_uploads["record_writers"]["uploads"] = []
            path = _write_fixture(temp, stale_uploads, b"")
            try:
                load_capture(path)
            except CaptureError:
                pass
            else:
                raise AssertionError("record_writers version 3 carrying uploads accepted")

        def rejected_writer(mutator):
            value = copy.deepcopy(writer_root)
            mutator(value)
            path = _write_fixture(temp, value, writer_binary)
            try:
                load_capture(path)
            except CaptureError:
                return
            raise AssertionError("malformed record-writer capture accepted")

        rejected_writer(lambda value: value["record_writers"]["limits"].update(
            records=RECORD_WRITER_LIMITS["records"] + 1))
        rejected_writer(lambda value: value["record_writers"]["records"][0].update(sequence=2))
        rejected_writer(lambda value: value["record_writers"]["records"][0].update(return_rva="0x42b42ef"))
        rejected_writer(lambda value: value["record_writers"]["uploads"][0].update(generation=1))

        def rejected_ownership(mutator):
            value = copy.deepcopy(ownership_root)
            mutator(value)
            path = _write_fixture(temp, value, ownership_binary)
            try:
                load_capture(path)
            except CaptureError:
                return
            raise AssertionError("malformed KinematicRig ownership capture accepted")

        rejected_ownership(lambda value: value["record_writers"]["limits"].update(
            ownership_records=RECORD_OWNERSHIP_LIMITS["ownership_records"] + 1))
        rejected_ownership(lambda value: value["record_writers"]["records"][0].update(
            ownership_id=None))
        rejected_ownership(lambda value: value["record_writers"]["ownerships"][0].update(
            ancestor_return_rva="0x431b21f"))
        rejected_ownership(lambda value: value["record_writers"]["ownerships"][1].update(
            record_index=1))
        rejected_ownership(lambda value: value["record_writers"]["ownerships"][0][
            "context_snapshot"].update(bytes=0, status="read_fault"))
        first_source = v2_root["cpu_blobs"][1]["offset"]
        changed = bytearray(v2_binary)
        changed[first_source + 28] ^= 1
        path = _write_fixture(temp, v2_root, bytes(changed))
        changed_report = analyse(load_capture(path), include_records=True)
        assert changed_report["cpu_source_owner"]["visible_record_outcomes"] == {"matched": 1, "mismatch": 1}

        partial = copy.deepcopy(v2_root)
        partial["source_owner"]["status"] = "partial"
        partial_attempt = partial["source_owner"]["attempts"][0]
        partial_attempt.update(status="partial", descriptor_count=2)
        partial["source_owner"]["summary"].update(complete_attempts=0, partial_attempts=1,
                                                    descriptor_overflow=1)
        path = _write_fixture(temp, partial, v2_binary)
        partial_report = analyse(load_capture(path), include_records=True)
        assert partial_report["cpu_source_owner"]["visible_record_outcomes"] == {"partial": 2}

        failed = copy.deepcopy(v2_root)
        failed["source_owner"]["status"] = "partial"
        failed_attempt = failed["source_owner"]["attempts"][0]
        failed_attempt["status"] = "partial"
        failed_attempt["descriptors"][0]["status"] = "read_fault"
        failed["source_owner"]["summary"].update(complete_attempts=0, partial_attempts=1, read_faults=1)
        failed["cpu_blobs"][1].update(bytes=0, status="read_fault")
        path = _write_fixture(temp, failed, v2_binary[:failed["cpu_blobs"][1]["offset"]])
        failed_report = analyse(load_capture(path), include_records=True)
        assert failed_report["cpu_source_owner"]["visible_record_outcomes"] == {"failure": 2}

        wrong_generation = copy.deepcopy(v2_root)
        wrong_generation["source_owner"]["attempts"][0]["generation"] = 1
        path = _write_fixture(temp, wrong_generation, v2_binary)
        try:
            load_capture(path)
        except CaptureError:
            pass
        else:
            raise AssertionError("cross-generation CPU source-owner attempt accepted")

        excessive_cap = copy.deepcopy(v2_root)
        excessive_cap["source_owner"]["limits"]["descriptors"] = SOURCE_OWNER_LIMITS["descriptors"] + 1
        path = _write_fixture(temp, excessive_cap, v2_binary)
        try:
            load_capture(path)
        except CaptureError:
            pass
        else:
            raise AssertionError("oversized CPU source-owner cap accepted")

        unavailable = copy.deepcopy(v2_root)
        unavailable["source_owner"]["status"] = "not_run"
        unavailable["source_owner"]["attempts"] = []
        unavailable["source_owner"]["summary"].update(
            unwind_attempts=0, callsite_matches=0, resource_matches=0,
            attempts_stored=0, complete_attempts=0)
        unavailable["events"][1]["source_owner_attempt"] = None
        unavailable["cpu_blobs"] = []
        path = _write_fixture(temp, unavailable, binary)
        unavailable_report = analyse(load_capture(path), include_records=True)
        assert unavailable_report["cpu_source_owner"]["visible_record_outcomes"] == {"unavailable": 2}

    print("Object classification reader self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", help="classification_<stamp>.json")
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON to stdout")
    parser.add_argument("--records", action="store_true", help="include bounded per-owner record details")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        if args.path or args.records or args.json:
            parser.error("--self-test must be used alone")
        self_test()
        return 0
    if not args.path:
        parser.error("path is required unless --self-test is used")
    try:
        capture = load_capture(args.path)
        report = analyse(capture, include_records=args.records)
    except CaptureError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    if args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        print_report(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
