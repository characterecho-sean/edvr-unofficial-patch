#!/usr/bin/env python3
"""Validate an EDVR object-classification capture and report what it holds.

The input is ``classification_<stamp>.json``.  Its named binary sibling is
read without modification.

Only ``edvr_object_classification_v3`` is read: the captures written since
2026-09-23, after EDVR's draw/mesh capture retired.  They carry the executable
identity, ``summary.binary_ok`` and the record-writer section (with its
KinematicRig ownership evidence), and load with every draw/mesh section empty
(status ``draw_capture_retired``).  The v1 and v2 schemas, and the draw
analysis that read them, retired 2026-09-29; an older copy of this tool from
git history reads those.

The record-writer section is version 3 from 2026-09-24: the upload join
(``uploads``, the writer's Map cutoff per source-owner attempt) retired with
the source-owner probe that fed it, so a version 3 section has no ``uploads``
array.  Version 2 (the 2026-09-23 builds) carried one, always empty, and still
loads.
"""

import argparse
from collections import Counter, defaultdict
import contextlib
import copy
import io
import json
from pathlib import Path
import re
import struct
import sys
import tempfile


SCHEMA_V3 = "edvr_object_classification_v3"
POOL_STRIDE = 336
MAX_JSON_BYTES = 128 * 1024 * 1024
MAX_BINARY_BYTES = 384 * 1024 * 1024
MAX_ITEMS = 1_000_000
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
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
HEX_ID = re.compile(r"^0x[0-9a-fA-F]+$")


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


def _address(value, name):
    text = _string(value, name, 34)
    if not HEX_ID.fullmatch(text) or int(text, 16) > UINT64_MAX:
        raise CaptureError("%s must be a hexadecimal pointer" % name)
    return text


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
    if schema != SCHEMA_V3:
        # v1 and v2 (the draw/mesh captures) retired with their readers on
        # 2026-09-29; no build writes them any more.
        raise CaptureError("unsupported object-classification schema (this reader "
                           "reads %s only)" % SCHEMA_V3)
    return _load_capture_v3(path, root)


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
    # Every capture this reader accepts is v3: the draw/mesh capture retired
    # 2026-09-23, so there is no sealed frame to own anything in. Say why and
    # stop; the record-writer section was validated on load.
    reason = ("binary_write_not_confirmed" if not capture["summary"]["binary_ok"]
              else capture["summary"]["status"])
    report["missing"].append({"scope": "capture", "reason": reason})
    if include_records:
        report["records"] = []
    return report


def print_report(report):
    print("capture: %s (%s)" % (report["capture"], report["status"]))
    print("ownership: unavailable")
    for item in report["missing"]:
        print("missing: %s (%s)" % (item["scope"], item["reason"]))
    print("scope: exact-frame captured evidence only; no static classification is inferred")


def _writer_fixture(binary_name="classification_fixture.bin"):
    """A v3 capture with three record-writer events and two KinematicRig
    ownership items: the shape the DLL writes since 2026-09-24 (section version
    3, ownership and no upload join).  The payload bytes are synthetic; the
    validators check sizes, offsets and the cross-references between arrays,
    not content."""
    root = {
        "schema": SCHEMA_V3, "binary": binary_name,
        "executable": {"pe_timestamp": 1788384820, "image_size": 104894464},
        "summary": {"binary_ok": True},
    }
    source = bytes((slot * 31 + i) & 0xFF for slot in (1, 2) for i in range(POOL_STRIDE))
    payloads = (source[:POOL_STRIDE], source[POOL_STRIDE:2 * POOL_STRIDE], source[:POOL_STRIDE])
    contexts = (("0x920078", "0x820000", "0x830000", "0x930000"),
                ("0xa10300", "0xa40250", "0x831000", "0x0"),
                ("0x920078", "0x820000", "0x830000", "0x930000"))
    writers = ("inline_42b4ed6", "direct_43130aa", "inline_42b4ed6")
    chunks = []
    cursor = 0
    records = []
    key_bytes = bytes(range(32))
    entry_bytes = bytes(range(0x38))
    # An inline writer's object snapshot is 176 bytes of the object, padded to
    # the 192 the ownership context reserves from section version 2 on.
    object_size = RECORD_OWNERSHIP_LIMITS["ownership_context_bytes"]
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
            object_bytes = bytes([0x40]) * 176 + bytes(object_size - 176)
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
            "ownership_status": "linked", "ownership_id": 0 if index in {0, 2} else 1,
        })

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

    ownerships = [
        make_ownership(0, "inline_registry_plus_78", "direct_4321940",
                       0x900000, 0x910000, 0x920000, 0x930000, 0,
                       0x940000, 0x950000, 0x960000, 0x970000, UINT32_MAX, None),
        make_ownership(1, "direct_collection_plus_300", "virtual_50",
                       0xA00000, 0xA10000, 0, 0, 0xA40000,
                       0, 0xA50000, 0xA60000, 0, UINT32_MAX, 0),
    ]
    limits = dict(RECORD_WRITER_LIMITS)
    limits.update(RECORD_OWNERSHIP_LIMITS)
    root["record_writers"] = {
        "version": 3, "status": "captured", "hook_status": "finished",
        "limits": limits,
        "summary": {"observed": 3, "stored": 3, "completed": 3,
                    "retained_bytes": cursor, "record_overflow": 0,
                    "byte_budget_declines": 0, "read_faults": 0,
                    "context_failures": 0, "declined_management": 0,
                    "declined_unknown": 0, "unwind_failures": 0,
                    "completion_failures": 0},
        "records": records,
        "ownership_status": "captured",
        "ownership_summary": {
            "attempted": 3, "linked": 3, "stored": 2, "deduplicated": 1,
            "unsupported_writer": 0, "opcode_mismatch": 0, "ancestor_missing": 0,
            "unwind_failed": 0, "tuple_read_fault": 0, "tuple_mismatch": 0,
            "registry_read_fault": 0, "registry_mismatch": 0,
            "record_range_mismatch": 0, "record_read_fault": 0,
            "cache_conflicts": 0, "record_overflow": 0, "byte_budget_declines": 0,
            "read_faults": 0, "ancestor_traces": 0, "ancestor_trace_overflow": 0,
        },
        "ownerships": ownerships,
        "ancestor_traces": [],
    }
    return root, b"".join(chunks)


def _write_fixture(directory, root, binary):
    directory = Path(directory)
    marker = directory / "classification_fixture.json"
    marker.write_text(json.dumps(root), encoding="utf-8")
    (directory / root["binary"]).write_bytes(binary)
    return marker


def _spans(section):
    """Every {status, offset, bytes} snapshot dict in a record_writers section."""
    found = []

    def walk(value):
        if isinstance(value, dict):
            if {"status", "offset", "bytes"} <= value.keys():
                found.append(value)
            else:
                for child in value.values():
                    walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)
    walk(section)
    return found


def _fault_span(root, binary, span, status):
    """A snapshot read that faulted: its payload is gone, so every later
    offset moves down by its size and the retained total drops with it."""
    at, size = span["offset"], span["bytes"]
    span.update(status=status, bytes=0)
    for other in _spans(root["record_writers"]):
        if other is not span and other["offset"] > at:
            other["offset"] -= size
    root["record_writers"]["summary"]["retained_bytes"] -= size
    return binary[:at] + binary[at + size:]


def self_test():
    root, binary = _writer_fixture()
    with tempfile.TemporaryDirectory() as temp_name:
        temp = Path(temp_name)

        def load(value, payload=binary):
            return load_capture(_write_fixture(temp, value, payload))

        def rejected(what, mutator, *, base=root, payload=binary):
            value = copy.deepcopy(base)
            mutator(value)
            try:
                load(value, payload)
            except CaptureError:
                return
            raise AssertionError("%s accepted" % what)

        capture = load(root)
        report = analyse(capture, include_records=True)
        assert report["status"] == "draw_capture_retired"
        assert report["missing"] == [{"scope": "capture", "reason": "draw_capture_retired"}]
        assert report["records"] == []
        assert report["record_writers"]["capture_status"] == "captured"
        assert report["record_writers"]["hook_status"] == "finished"
        assert report["kinematic_ownership"]["capture_status"] == "captured"
        assert len(capture["writer_records"]) == 3 and len(capture["writer_ownerships"]) == 2
        assert root["record_writers"]["ownership_summary"]["deduplicated"] == 1
        with contextlib.redirect_stdout(io.StringIO()) as shown:
            print_report(report)
        assert "draw_capture_retired" in shown.getvalue()

        # The command line: exit 0 and machine-readable output on a good
        # capture, exit 2 with a message on a schema this reader retired.
        marker = _write_fixture(temp, root, binary)
        with contextlib.redirect_stdout(io.StringIO()) as shown:
            assert main([str(marker), "--json"]) == 0
        assert json.loads(shown.getvalue())["status"] == "draw_capture_retired"
        old = copy.deepcopy(root)
        old["schema"] = "edvr_object_classification_v2"
        marker = _write_fixture(temp, old, binary)
        with contextlib.redirect_stderr(io.StringIO()) as shown:
            assert main([str(marker)]) == 2
        assert "unsupported object-classification schema" in shown.getvalue()

        # Only v3 is read: the retired schemas, an unknown one and a missing
        # one are refused, and so is a v3 file that still carries a section
        # the draw/mesh capture used to write.
        for schema in ("edvr_object_classification_v1", "edvr_object_classification_v2",
                       "edvr_object_classification_v4", "", None):
            rejected("schema %r" % (schema,), lambda value, schema=schema: value.update(schema=schema))
        rejected("a capture with no schema", lambda value: value.pop("schema"))
        for retired in V3_RETIRED_SECTIONS:
            rejected("a v3 capture carrying %s" % retired,
                     lambda value, retired=retired: value.update({retired: []}))

        # The packed binary must be exactly the writer payload, and a capture
        # whose write was not confirmed says so instead of reading it.
        rejected("a truncated payload", lambda value: None, payload=binary[:-1])
        rejected("trailing bytes", lambda value: None, payload=binary + b"\0")
        _write_fixture(temp, root, binary)
        (temp / root["binary"]).unlink()
        try:
            load_capture(temp / "classification_fixture.json")
        except CaptureError:
            pass
        else:
            raise AssertionError("a missing binary accepted")
        unconfirmed = copy.deepcopy(root)
        unconfirmed["summary"]["binary_ok"] = False
        unconfirmed_report = analyse(load(unconfirmed))
        assert unconfirmed_report["missing"] == [
            {"scope": "capture", "reason": "binary_write_not_confirmed"}]

        # The record-writer section.
        rejected("an oversized record cap", lambda value: value["record_writers"]["limits"].update(
            records=RECORD_WRITER_LIMITS["records"] + 1))
        rejected("a begin sequence that collides with its completion",
                 lambda value: value["record_writers"]["records"][0].update(sequence=2))

        def backwards(value):
            # Record 1 begins later than record 2 does, everything else valid.
            records = value["record_writers"]["records"]
            records[1].update(sequence=9, completion_sequence=10)
            records[2].update(sequence=5, completion_sequence=6)
            value["record_writers"]["ownerships"][1]["first_sequence"] = 9
        rejected("begin sequences that go backwards", backwards)
        rejected("an ownership item whose first sequence is not its earliest writer's",
                 lambda value: value["record_writers"]["ownerships"][0].update(first_sequence=2))
        rejected("a writer whose return RVA is another's",
                 lambda value: value["record_writers"]["records"][0].update(return_rva="0x42b42ef"))
        rejected("a snapshot off the packed cursor",
                 lambda value: value["record_writers"]["records"][1]["record"].update(
                     offset=value["record_writers"]["records"][1]["record"]["offset"] + 1))
        rejected("a retained byte total that is not the packed size",
                 lambda value: value["record_writers"]["summary"].update(
                     retained_bytes=value["record_writers"]["summary"]["retained_bytes"] + 1))
        rejected("a captured status beside an overflow counter",
                 lambda value: value["record_writers"]["summary"].update(record_overflow=1))
        rejected("an unknown section version", lambda value: value["record_writers"].update(version=4))
        rejected("a version 3 section carrying the retired uploads array",
                 lambda value: value["record_writers"].update(uploads=[]))
        rejected("a capture status that disagrees with its hook",
                 lambda value: value["record_writers"].update(hook_status="not_run"))

        # The KinematicRig ownership section.
        rejected("an oversized ownership cap", lambda value: value["record_writers"]["limits"].update(
            ownership_records=RECORD_OWNERSHIP_LIMITS["ownership_records"] + 1))
        rejected("a linked record with no ownership id",
                 lambda value: value["record_writers"]["records"][0].update(ownership_id=None))
        rejected("an ancestor RVA that is not its branch's",
                 lambda value: value["record_writers"]["ownerships"][0].update(
                     ancestor_return_rva="0x431b21f"))
        rejected("a record index that is not the writer's",
                 lambda value: value["record_writers"]["ownerships"][1].update(record_index=1))
        rejected("a context snapshot that lost its bytes",
                 lambda value: value["record_writers"]["ownerships"][0]["context_snapshot"].update(
                     bytes=0, status="read_fault"))

        # The producer retains the stack-end RIP as a final zero outside-image
        # frame when unwindOne fails.  Accept only that exact terminal frame
        # shape, while keeping all other frame addresses strict.
        unwind_root = copy.deepcopy(root)
        unwind_record = unwind_root["record_writers"]["records"][2]
        unwind_record.update(ownership_id=None, ownership_status="unwind_failed")
        unwind_summary = unwind_root["record_writers"]["ownership_summary"]
        unwind_summary.update(linked=2, deduplicated=0, unwind_failed=1, ancestor_traces=1)
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
        unwind_report = analyse(load(unwind_root))
        assert unwind_report["kinematic_ownership"]["capture_status"] == "partial"
        rejected("a trace with a leading zero frame",
                 lambda value: value["record_writers"]["ancestor_traces"][0]["frames"].insert(
                     0, {"address": "0x0", "status": "outside_image", "module_rva": None}),
                 base=unwind_root)
        rejected("a module-relative zero frame",
                 lambda value: value["record_writers"]["ancestor_traces"][0]["frames"][0].update(
                     address="0x0", status="module_relative", module_rva="0x0"),
                 base=unwind_root)
        rejected("a terminal frame with an RVA",
                 lambda value: value["record_writers"]["ancestor_traces"][0]["frames"][1].update(
                     module_rva="0x1"),
                 base=unwind_root)

        # Refusals the hook records stay explicit: a writer the ownership walk
        # does not support, a tuple that disagreed, a read that faulted.
        refused = copy.deepcopy(root)
        refused["record_writers"]["records"][0].update(ownership_id=None,
                                                        ownership_status="tuple_mismatch")
        refused["record_writers"]["ownerships"][0]["first_sequence"] = 5
        refused["record_writers"]["ownership_summary"].update(linked=2, deduplicated=0,
                                                              tuple_mismatch=1)
        refused["record_writers"]["ownership_status"] = "partial"
        assert analyse(load(refused))["kinematic_ownership"]["capture_status"] == "partial"

        unsupported = copy.deepcopy(root)
        unsupported["record_writers"]["records"][0].update(ownership_id=None,
                                                            ownership_status="unsupported_writer")
        unsupported["record_writers"]["ownerships"][0]["first_sequence"] = 5
        unsupported["record_writers"]["ownership_summary"].update(
            attempted=2, linked=2, deduplicated=0, unsupported_writer=1)
        assert load(unsupported)["writer_records"][0]["ownership_status"] == "unsupported_writer"

        faulted = copy.deepcopy(root)
        fault_item = faulted["record_writers"]["ownerships"][0]
        fault_item.update(status="partial", parent_status="read_fault")
        fault_binary = _fault_span(faulted, binary, fault_item["parent_prefix"], "not_applicable")
        faulted["record_writers"]["ownership_summary"]["read_faults"] = 1
        faulted["record_writers"]["ownership_status"] = "partial"
        assert analyse(load(faulted, fault_binary))["kinematic_ownership"][
            "capture_status"] == "partial"

        # A capture the hook did not finish cleanly is partial, never captured.
        overflowed = copy.deepcopy(root)
        overflowed["record_writers"]["status"] = "partial"
        overflowed["record_writers"]["summary"]["record_overflow"] = 1
        assert analyse(load(overflowed))["record_writers"]["capture_status"] == "partial"

        writer_fault = copy.deepcopy(root)
        faulty = writer_fault["record_writers"]["records"][0]
        faulty["context_status"] = "partial"
        fault_binary = _fault_span(writer_fault, binary, faulty["key_snapshot"], "read_fault")
        writer_fault["record_writers"]["status"] = "partial"
        writer_fault["record_writers"]["summary"]["read_faults"] = 1
        writer_fault["record_writers"]["summary"]["context_failures"] = 1
        assert analyse(load(writer_fault, fault_binary))["record_writers"][
            "capture_status"] == "partial"

        # A hook that never ran, or found the game's code changed, retains
        # nothing; both still load, and say why in the report.
        for top_status, hook_status in (("unavailable", "opcode_mismatch"), ("not_run", "not_run")):
            inactive = copy.deepcopy(root)
            section = inactive["record_writers"]
            section.update(status=top_status, hook_status=hook_status, records=[],
                           ownerships=[], ancestor_traces=[], ownership_status=top_status)
            for field in section["summary"]:
                section["summary"][field] = 0
            for field in section["ownership_summary"]:
                section["ownership_summary"][field] = 0
            inactive_report = analyse(load(inactive, b""))
            assert inactive_report["status"] == "draw_capture_retired"
            assert inactive_report["record_writers"]["capture_status"] == top_status
            assert inactive_report["kinematic_ownership"]["capture_status"] == top_status

            # Section version 2 (the 2026-09-23 builds) is the same with an
            # upload join that was always empty; version 3 has none at all.
            v2 = copy.deepcopy(inactive)
            v2["record_writers"].update(version=2, uploads=[])
            assert analyse(load(v2, b""))["record_writers"]["capture_status"] == top_status
            rejected("a version 2 section with an upload join",
                     lambda value: value["record_writers"].update(
                         uploads=[{"attempt": 0, "resource": 0, "generation": 1, "cutoff": 5}]),
                     base=v2, payload=b"")

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
