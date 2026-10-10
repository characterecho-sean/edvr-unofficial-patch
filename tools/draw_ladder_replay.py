#!/usr/bin/env python3
"""Read and validate one EDVR draw-ladder sidecar; never edits flight logs."""

from __future__ import print_function

import argparse
import contextlib
import io
import json
import os
import re
import sys
import tempfile

FORMAT = "edvr.draw-ladder-trace"
SCHEMA_VERSION = 2
PREDICATE_FACT_VERSION = 16
RESOLVE_BIND_PS_HASH = 0x7CECABDE34FFBE9E
MAX_TRACE_BYTES = 256 * 1024 * 1024
MAX_DRAWS = 65536
MAX_SITE_EVENTS = 48
MAX_ACTION_EVENTS = 32
SITE_KIND = {1, 2, 3}
SITE_OUTCOME = {1, 2, 3, 4, 5}
SITE_FLOW = {0, 1}
ACTION_PHASE = {1, 2, 3, 4, 5, 6}
ACTION_OUTCOME = {1, 2, 3}
DRAW_CALL = {1, 2, 3, 4, 5, 6, 7}
DRAW_COMMANDS = {
    ord("D"): "draw", ord("I"): "draw-indexed", ord("N"): "draw-instanced",
    ord("X"): "draw-indexed-instanced", ord("A"): "draw-auto",
    ord("Z"): "draw-indexed-instanced-indirect",
    ord("Y"): "draw-instanced-indirect",
}
SITE_KINDS = {
    1: 2, 2: 3, 3: 3, 4: 1, 5: 2, 6: 2, 7: 1, 8: 1,
    20: 1, 21: 1, 22: 2, 23: 2, 24: 2, 25: 2, 26: 2,
    27: 1, 28: 3, 40: 1, 41: 1, 42: 1, 43: 2, 44: 1,
    45: 1, 46: 1, 47: 1, 48: 2, 49: 2, 50: 2, 51: 2,
    52: 2, 53: 2, 54: 2, 55: 2, 56: 2, 57: 2, 58: 2,
    59: 2, 60: 2, 61: 2, 62: 2, 63: 2, 64: 1, 65: 1,
    66: 2, 67: 3, 68: 3, 69: 3, 70: 1, 71: 1, 72: 3,
    73: 3, 74: 3, 75: 3, 76: 3,
}
NOT_ELIGIBLE_SITES = {
    5, 6, 44, 50, 54, 57, 58, 59, 65,
}
TERMINAL_VERDICTS = {
    1: 2, 5: 10, 6: 2, 22: 18, 23: 2, 24: 2, 25: 16, 26: 15,
    43: 7, 48: 2, 49: 2, 50: 6, 51: 2, 52: 3, 53: 4, 54: 5,
    55: 17, 56: 18, 57: 11, 58: 12, 59: 13, 60: 14, 61: 2,
    62: 9, 63: 8, 66: 1,
    2: 0, 3: 0, 28: 0, 67: 0, 68: 0, 69: 0,
    72: 0, 73: 0, 74: 0, 75: 0, 76: 0,
}

# This table describes only the canonical site order for validating recorded
# visit order and inferring the short-circuited suffix. Arbitration itself is
# intentionally delegated to the production C++ selector/rig.
COMMON = [1, 2, 3, 4, 5, 6, 7, 8]
OFFSCREEN = [70, 22, 20, 21, 23, 24, 25, 26, 27, 28]
EYE = [40, 41, 42, 43, 44, 45, 71, 46, 47, 48, 49, 50, 51, 52, 53,
       54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 67, 68, 66, 69]
FLAT_BYPASS = [72]
AUTO_BYPASS = [73]
DRAW_INDEXED_INSTANCED_INDIRECT_BYPASS = [74]
DRAW_INSTANCED_INDIRECT_BYPASS = [75]
INTERNAL_WORLD_BYPASS = [76]
SEQUENCES = {
    (1, 1): COMMON,
    (2, 1): COMMON,
    (3, 1): COMMON,
    (4, 2): COMMON + OFFSCREEN,
    (6, 4): COMMON + EYE,
    (7, 5): FLAT_BYPASS,
    (8, 6): AUTO_BYPASS,
    (9, 7): DRAW_INDEXED_INSTANCED_INDIRECT_BYPASS,
    (10, 8): DRAW_INSTANCED_INDIRECT_BYPASS,
    (11, 9): INTERNAL_WORLD_BYPASS,
}
TRI_STATES = {"unknown", "no", "yes"}
FORWARD_TRI_FIELDS = {
    "owner": 1 << 0,
    "verdictForwards": 1 << 2,
    "familyAvailable": 1 << 3,
    "initialUiTake": 1 << 4,
    "afterUiTake": 1 << 5,
    "worldReissue": 1 << 6,
    "curveThisDraw": 1 << 7,
    "introCurveThisDraw": 1 << 8,
    "uiDepth": 1 << 9,
    "holoDepth": 1 << 10,
    "composite": 1 << 11,
    "crispPending": 1 << 12,
    "issueBlocked": 1 << 13,
}
FLAG_BITS = {
    "frame": {"pluginDispatch": 1, "runtimeFlat": 2, "drawGateSubscribed": 4},
                   "draw": {"pluginDispatchEnabled": 1, "distanceEnabled": 2,
             "fssHealOn": 4, "quadSkipArmed": 8},
    "action": {"generatedDrawArgsUnavailable": 0x2000,
               "gpuDrawArgsUnavailable": 0x4000,
               "issueCountUnknown": 0x8000},
    "forwardFacts": {
        "owner": 1 << 0, "verdict": 1 << 1, "verdictForwards": 1 << 2,
        "familyAvailable": 1 << 3, "initialUiTake": 1 << 4,
        "afterUiTake": 1 << 5, "worldReissue": 1 << 6,
        "curveThisDraw": 1 << 7, "introCurveThisDraw": 1 << 8,
        "uiDepth": 1 << 9, "holoDepth": 1 << 10, "composite": 1 << 11,
        "crispPending": 1 << 12, "issueBlocked": 1 << 13,
    },
}


class TraceError(ValueError):
    pass


def _candidate_draw_gate(fact):
    """Candidate pure selector from normalized DrawGateWanted input."""
    if fact["known"] == "unknown":
        return None
    if fact["gateWanted"] == "no":
        return {"id": 3, "kind": 3, "outcome": 4, "flow": 1,
                "subsite": 0, "verdict": 0}
    return {"id": 3, "kind": 3, "outcome": 1, "flow": 0,
            "subsite": 0, "verdict": -1}


def _legacy14a_draw_gate(fact):
    """Frozen 14a7ff70 semantics: !drawGateWanted returns DrawVerdict::None."""
    if fact["known"] == "unknown":
        return None
    if fact["gateWanted"] == "no":
        return {"id": 3, "kind": 3, "outcome": 4, "flow": 1,
                "subsite": 0, "verdict": 0}
    return {"id": 3, "kind": 3, "outcome": 1, "flow": 0,
            "subsite": 0, "verdict": -1}


def _candidate_eye_range(fact):
    """Candidate pure selector; return (site event, skipped counter delta)."""
    if fact["known"] == "unknown":
        return None
    for index, (lo, hi) in enumerate(fact["ranges"]):
        if fact["eyeDrawIndex"] >= lo and fact["eyeDrawIndex"] <= hi:
            return ({"id": 49, "kind": 2, "outcome": 4, "flow": 1,
                     "subsite": index, "verdict": 2}, 1)
    return ({"id": 49, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1}, 0)


def _legacy14a_eye_range(fact):
    """Frozen 14a7ff70 inclusive loop, including first matching subsite."""
    if fact["known"] == "unknown":
        return None
    i = 0
    while i < len(fact["ranges"]):
        lo, hi = fact["ranges"][i]
        if fact["eyeDrawIndex"] >= lo and fact["eyeDrawIndex"] <= hi:
            return ({"id": 49, "kind": 2, "outcome": 4, "flow": 1,
                     "subsite": i, "verdict": 2}, 1)
        i += 1
    return ({"id": 49, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1}, 0)


def _legacy14a_night_vision(fact, draw):
    """Frozen 14a boolean claim from mode/failure, shape, and binding hashes."""
    if (draw["kind"] != ord("X") or draw["count"] != 240 or
            draw["instances"] != 1 or draw["vsHash"].upper() != "FCF7BD2896751D96" or
            draw["psHash"].upper() != "F786D34B5E118D5E"):
        return False
    mode = fact.get("callbackMode") if fact.get("callbackModeKnown") == "yes" else fact.get("mode")
    mode_known = (fact.get("callbackModeKnown") == "yes" or
                  fact.get("modeKnown") == "yes")
    if not mode_known:
        return None
    if mode == 0:
        return False
    failed = fact.get("failed")
    if fact.get("failedKnown") != "yes" or failed not in ("yes", "no"):
        return None
    return failed == "no"


def _sunglare_read(obj, name, label, value_type, maximum=None, minimum=0):
    """Decode a typed read envelope without turning unavailable data into a guess."""
    item = obj.get(name)
    where = label + "." + name
    if not isinstance(item, dict) or set(item) != {"reached", "known", "value"}:
        raise TraceError(where + " must be a read envelope")
    reached, known, value = item["reached"], item["known"], item["value"]
    if type(reached) is not bool or type(known) is not bool:
        raise TraceError(where + " has invalid reached/known flags")
    if known and not reached:
        raise TraceError(where + " is known although the read was skipped")
    if not known:
        if value is not None:
            raise TraceError(where + " unavailable value must be null")
        return reached, False, None
    if value_type is bool:
        valid = type(value) is bool
    elif value_type is int:
        valid = type(value) is int and value >= minimum and (maximum is None or value <= maximum)
    else:
        valid = type(value) is value_type
    if not valid:
        raise TraceError(where + " has an invalid value")
    return reached, True, value


def _sunglare_wants(obj, prefix, label, should_evaluate):
    mode = _sunglare_read(obj, prefix + "Mode", label, int, 3)
    stage = prefix[:-len("Wants")]
    damping = _sunglare_read(obj, stage + "ExposureDamping", label, bool)
    probe = _sunglare_read(obj, stage + "Probe", label, bool)
    result = _sunglare_read(obj, prefix + "Result", label, bool)
    if not should_evaluate:
        if any(x[0] for x in (mode, damping, probe, result)):
            raise TraceError(label + " short-circuited wants carries later reads")
        return None, False, None
    if not result[0]:
        raise TraceError(label + "." + prefix + "Result must be observed")
    if not mode[0]:
        raise TraceError(label + "." + prefix + "Mode must be read")
    if not mode[1]:
        if probe[0] and not damping[0]:
            raise TraceError(label + " probe read lacks preceding damping stage")
        if damping[1] and damping[2] and probe[0]:
            raise TraceError(label + " true damping must short-circuit probe")
        return None, False, None
    if mode[2] != 0:
        if damping[0] or probe[0]:
            raise TraceError(label + " non-stock wants short-circuit carries later reads")
        expected = True
    else:
        if not damping[0]:
            raise TraceError(label + " stock wants must read exposure damping")
        if not damping[1]:
            return 0, False, None
        if damping[2]:
            if probe[0]:
                raise TraceError(label + " true damping must short-circuit probe")
            expected = True
        else:
            # Current captures leave the retired probe field unread; older
            # captures may still carry its observed value.
            if probe[0]:
                if not probe[1]:
                    return 0, False, None
                expected = probe[2]
            else:
                expected = False
    if result[1] and result[2] != expected:
        raise TraceError(label + "." + prefix + "Result disagrees with consumed inputs")
    return mode[2], result[1], expected


def _sunglare_selector(fact, draw, label):
    selector = fact.get("selector")
    if not isinstance(selector, dict):
        raise TraceError(label + ".selector must be an object")
    expected_selector_fields = {
        "outerWantsMode", "outerExposureDamping", "outerProbe", "outerTrainShape", "outerWantsResult",
        "helperWantsMode", "helperExposureDamping", "helperProbe", "helperWantsResult", "helperTrainShape",
        "ps0", "ps1", "lastSeenBeforeMs", "nowMs", "lastSeenAfterMs", "actionMode", "action",
    }
    if set(selector) != expected_selector_fields:
        raise TraceError(label + ".selector has missing or unexpected fields")
    outer_shape = _sunglare_read(selector, "outerTrainShape", label + ".selector", bool)
    shape_value = draw["kind"] == ord("N") and draw["count"] == 6 and draw["instances"] >= 2
    if not outer_shape[0] or not outer_shape[1]:
        raise TraceError(label + ".selector.outerTrainShape must be observed")
    mismatch = int(outer_shape[2] != shape_value)
    outer_value, outer_wants_known, outer_wants = _sunglare_wants(
        selector, "outerWants", label + ".selector", shape_value)
    helper_stage = (_sunglare_read(selector, "helperWantsMode",
                                   label + ".selector", int, 3)[0] or
                    _sunglare_read(selector, "helperWantsResult",
                                   label + ".selector", bool)[0])
    helper_called = shape_value and (outer_wants is True or
                                     (outer_wants is None and helper_stage))
    helper_value, helper_wants_known, helper_wants = _sunglare_wants(
        selector, "helperWants", label + ".selector", helper_called)
    helper_shape = _sunglare_read(selector, "helperTrainShape", label + ".selector", bool)
    if not helper_called and helper_shape[0]:
        raise TraceError(label + " skipped helper carries its shape read")
    helper_shape_needed = helper_called and (helper_wants is True or
                                             (helper_wants is None and helper_shape[0]))
    if helper_called and helper_wants is False and helper_shape[0]:
        raise TraceError(label + " short-circuited helper carries its shape read")
    if helper_called and helper_wants is True and not helper_shape[0]:
        raise TraceError(label + " helper wants lacks its shape stage")
    if helper_shape_needed and helper_shape[1] and helper_shape[2] != shape_value:
        mismatch += 1
    descriptor_needed = helper_shape_needed and shape_value

    def texture(slot, needed):
        nonlocal mismatch
        tex = selector.get(slot)
        fields = ("resolveOk", "isTexture2D", "width", "height", "format")
        if not isinstance(tex, dict) or set(tex) != set(fields):
            raise TraceError(label + ".selector." + slot + " is malformed")
        vals = [_sunglare_read(tex, field, label + ".selector." + slot,
                               bool if field in ("resolveOk", "isTexture2D") else int,
                               0xffffffff if field in ("width", "height", "format") else None)
                for field in fields]
        if any(vals[i][0] and not vals[i - 1][0] for i in range(1, len(vals))):
            raise TraceError(label + " resource read lacks its preceding stage")
        for index, accepted in enumerate((True, True, 2048, 1024)):
            if vals[index][1] and vals[index][2] != accepted and any(
                    later[0] for later in vals[index + 1:]):
                raise TraceError(label + " resource miss carries later-stage reads")
        if needed is False:
            if any(v[0] for v in vals):
                raise TraceError(label + " skipped matcher carries resource reads")
            return False, True
        if needed is None and not vals[0][0]:
            if any(v[0] for v in vals[1:]):
                raise TraceError(label + " skipped resolver carries descriptor reads")
            return False, False
        if not vals[0][0]:
            raise TraceError(label + " reached matcher lacks resolver result")
        if not vals[0][1]:
            return False, False
        if not vals[0][2]:
            if any(v[0] for v in vals[1:]):
                raise TraceError(label + " failed resolve carries descriptor reads")
            return False, True
        if not vals[1][0]:
            raise TraceError(label + " successful resolve lacks texture type")
        if not vals[1][1]:
            return False, False
        if not vals[1][2]:
            if any(v[0] for v in vals[2:]):
                raise TraceError(label + " non-texture carries dimensions")
            return False, True
        if not vals[2][0]:
            raise TraceError(label + " texture lacks width")
        if not vals[2][1]:
            return False, False
        if vals[2][2] != 2048:
            if vals[3][0] or vals[4][0]:
                raise TraceError(label + " width miss carries later descriptor reads")
            return False, True
        if not vals[3][0]:
            raise TraceError(label + " width hit lacks height")
        if not vals[3][1]:
            return False, False
        if vals[3][2] != 1024:
            if vals[4][0]:
                raise TraceError(label + " height miss carries format")
            return False, True
        if not vals[4][0]:
            raise TraceError(label + " dimensions hit lack format")
        if not vals[4][1]:
            return False, False
        return vals[4][2] == 98, True

    ps0_match, ps0_known = texture("ps0", descriptor_needed)
    ps1_needed = (False if not descriptor_needed else
                  ps0_match if ps0_known else None)
    ps1_match, ps1_known = texture("ps1", ps1_needed)
    if not shape_value:
        matched_known, matched = True, False
    elif not outer_wants_known:
        matched_known, matched = False, False
    elif outer_wants is False:
        matched_known, matched = True, False
    elif not helper_wants_known:
        matched_known, matched = False, False
    elif helper_wants is False:
        matched_known, matched = True, False
    elif helper_shape_needed and not helper_shape[1]:
        matched_known, matched = False, False
    elif helper_shape_needed and not shape_value:
        matched_known, matched = True, False
    elif not ps0_known or not ps1_known:
        matched_known, matched = False, False
    else:
        matched_known = True
        matched = ps0_match and ps1_match
    before = _sunglare_read(selector, "lastSeenBeforeMs", label + ".selector", int, 0xffffffffffffffff)
    now = _sunglare_read(selector, "nowMs", label + ".selector", int, 0xffffffffffffffff)
    after = _sunglare_read(selector, "lastSeenAfterMs", label + ".selector", int, 0xffffffffffffffff)
    action_mode = _sunglare_read(selector, "actionMode", label + ".selector", int, 3)
    action = _sunglare_read(selector, "action", label + ".selector", int, 3)
    if not selector["ps1"]["format"]["reached"] and any(
            value[0] for value in (before, now, after, action_mode)):
        raise TraceError(label + " action/time stage lacks completed resource reads")
    if not action[0]:
        raise TraceError(label + ".selector.action must always record the returned action")
    mutation_unobserved = matched and not all(x[0] and x[1] for x in (before, now, after))
    if matched_known and not matched:
        if any(x[0] for x in (before, now, after, action_mode)):
            raise TraceError(label + " no-match selector carries action/time stages")
        if action[1] and action[2] != 0:
            mismatch += 1
    if matched and before[1] and after[1] and now[1]:
        expected_after = now[2]
        if after[2] != expected_after:
            mismatch += 1
    elif matched:
        mutation_unobserved = True
    expected_action = 0
    if matched:
        if not action_mode[0] or not action_mode[1] or not action[1]:
            matched_known = False
        else:
            expected_action = 1 if action_mode[2] == 1 else 3
            if action[2] != expected_action:
                mismatch += 1
    elif not action[1]:
        matched_known = False
    return (expected_action if matched_known else None, mismatch,
            helper_value if matched and matched_known else None, mutation_unobserved)


def _replay_sunglare_site(fact, site_id, draw, label, source_action,
                          common2_clamp_after=None):
    site = fact.get("site")
    if not isinstance(site, dict):
        raise TraceError(label + ".site must be an object")
    expected_keys = {"source61ActionNotStock", "worldValue", "probeValue",
                     "clampBefore", "clampAfter", "billboardReached"}
    if set(site) != expected_keys:
        raise TraceError(label + ".site has missing or unexpected fields")
    source = _sunglare_read(site, "source61ActionNotStock", label + ".site", bool)
    clamp_before = _sunglare_read(site, "clampBefore", label + ".site", int, 0xffffffff)
    clamp_after = _sunglare_read(site, "clampAfter", label + ".site", int, 0xffffffff)
    billboard = _sunglare_read(site, "billboardReached", label + ".site", bool)
    mismatch = 0
    unreplayable = False
    expected_claim = False
    world = _sunglare_read(site, "worldValue", label + ".site", int, 0x7fffffff, -0x80000000)
    probe = _sunglare_read(site, "probeValue", label + ".site", bool)
    if site_id == 61:
        if source[0] or world[0] or probe[0] or billboard[0]:
            raise TraceError(label + " site61 carries later-rung observations")
        if not all(x[0] and x[1] for x in (clamp_before, clamp_after)):
            unreplayable = True
        elif clamp_before[2] != common2_clamp_after or clamp_after[2] != clamp_before[2]:
            mismatch += 1
        if source_action is None:
            unreplayable = True
            return None, mismatch, unreplayable
        if source_action == 1:
            return {"id": 61, "kind": 2, "outcome": 4, "flow": 1,
                    "subsite": 0, "verdict": 2}, mismatch, unreplayable
        return {"id": 61, "kind": 2, "outcome": 2, "flow": 0,
                "subsite": 0, "verdict": -1}, mismatch, unreplayable
    if source[0] and source[1] and source_action is not None:
        if source[2] != source_action:
            mismatch += 1
    else:
        unreplayable = True
    if site_id == 62:
        predicate_known = source_action is False
        if source_action is False:
            if world[0] or probe[0]:
                raise TraceError(label + " stock action must short-circuit site62 inputs")
            expected_claim = False
        elif source_action is True:
            if not world[0]:
                raise TraceError(label + " non-stock action must read world value")
            if world[1] and world[2] != 0:
                predicate_known = True
                if probe[0]:
                    raise TraceError(label + " nonzero world value must short-circuit probe")
                expected_claim = True
            elif world[1] and world[2] == 0:
                if not probe[0]:
                    predicate_known = True
                    expected_claim = False
                elif probe[1]:
                    predicate_known = True
                    expected_claim = probe[2]
                else:
                    unreplayable = True
            else:
                unreplayable = True
        else:
            unreplayable = True
        if predicate_known and expected_claim:
            if not all(x[0] and x[1] for x in (clamp_before, clamp_after, billboard)):
                unreplayable = True
        elif predicate_known and (clamp_before[0] or clamp_after[0] or billboard[0]):
            raise TraceError(label + " declining site62 carries claim-only mutation reads")
        if clamp_before[1] and clamp_before[2] != 0:
            mismatch += 1
        if clamp_after[1] and clamp_after[2] != 0:
            mismatch += 1
        if predicate_known and billboard[1] and billboard[2] != expected_claim:
            mismatch += 1
        return ({"id": site_id, "kind": 2, "outcome": 3 if expected_claim else 2,
                 "flow": 1 if expected_claim else 0, "subsite": 0,
                 "verdict": 9 if expected_claim else -1} if not unreplayable else None,
                mismatch, unreplayable)
    # Site63 is visited after a stock/Match decline. The frozen common2 reset
    # and the absence of any production kClamp return make a positive claim
    # unreachable; its observed clamp is evidence, never a selector input.
    if source_action is True:
        if not clamp_before[0]:
            raise TraceError(label + " non-stock site63 lacks the reached clamp check")
        if not clamp_before[1]:
            unreplayable = True
        elif clamp_before[2] != 0:
            mismatch += 1
    elif source_action is False and any(x[0] for x in (clamp_before, clamp_after, billboard)):
        raise TraceError(label + " stock action cannot carry site63 clamp observations")
    elif source_action is None and clamp_before[0]:
        unreplayable = True
    if clamp_after[0] or billboard[0] or world[0] or probe[0]:
        raise TraceError(label + " site63 carries an impossible later-stage observation")
    return {"id": site_id, "kind": 2, "outcome": 2, "flow": 0,
            "subsite": 0, "verdict": -1}, mismatch, unreplayable


def _fss_read(obj, key, label, value_type, maximum=None):
    envelope = obj.get(key)
    if not isinstance(envelope, dict) or set(envelope) != {"reached", "known", "value"}:
        raise TraceError(label + "." + key + " must be a read envelope")
    reached = envelope["reached"]
    known = envelope["known"]
    value = envelope["value"]
    if type(reached) is not bool or type(known) is not bool:
        raise TraceError(label + "." + key + " has invalid availability flags")
    if not reached or not known:
        if value is not None:
            raise TraceError(label + "." + key + " unavailable value must be null")
        if known and not reached:
            raise TraceError(label + "." + key + " skipped read cannot be known")
        return reached, known, None
    if value_type is bool:
        if type(value) is not bool:
            raise TraceError(label + "." + key + " value must be boolean")
    elif value_type is int:
        if type(value) is not int or value < 0 or value > maximum:
            raise TraceError(label + "." + key + " integer is out of range")
    else:
        raise TraceError(label + "." + key + " has unsupported reader type")
    return reached, known, value


def _fss_set_read(obj, key, label, should_reach, value_type, maximum=None,
                  required_when_reached=True):
    value = _fss_read(obj, key, label, value_type, maximum)
    if should_reach is False and value[0]:
        raise TraceError(label + "." + key + " was read past a short circuit")
    if should_reach is True and not value[0]:
        raise TraceError(label + "." + key + " is missing a reached source read")
    if required_when_reached and value[0] and not value[1]:
        raise TraceError(label + "." + key + " is unavailable")
    return value


def _fss_read_pair(obj, names, label, should_reach, value_type, maximum=None):
    return {name: _fss_set_read(obj, name, label, should_reach.get(name),
                                value_type, maximum) for name in names}


def _replay_fss_outer_probe(selector, is_panel, label):
    """Replay only the frozen selector inputs captured before a staged-out FSS handler."""
    no, yes = False, True
    u32 = 0xffffffff

    def read(key, reached, value_type):
        return _fss_set_read(selector, key, label + ".selector", reached,
                             value_type, u32 if value_type is int else None,
                             required_when_reached=False)

    if is_panel:
        outer = read("outerEnabled", yes, bool)
        body = read("bodyFrame", outer[2] if outer[1] else None, int)
        frame_reached = (no if outer[1] and not outer[2] else
                         no if body[0] and body[1] and body[2] == 0 else
                         yes if outer[1] and body[1] and body[2] != 0 else None)
        frame = read("frameNo", frame_reached, int)
        if outer[0] and not outer[1] or (outer[1] and not outer[2]):
            return False if outer[1] else None
        if not body[1]:
            return None
        if body[2] == 0:
            return False
        if not frame[1]:
            return None
        return ((frame[2] - body[2]) & u32) <= 2

    steady = read("outerSteady", yes, bool)
    lockstep = read("outerLockstep", (not steady[2]) if steady[1] else None, bool)
    outer_wants = (True if steady[1] and steady[2] else
                   lockstep[2] if steady[1] and not steady[2] and lockstep[1] else None)
    body = read("bodyFrame", outer_wants, int)
    body_frame_reached = (no if outer_wants is False else
                          no if body[0] and body[1] and body[2] == 0 else
                          yes if outer_wants is True and body[1] and body[2] != 0 else None)
    body_frame = read("bodyFrameNo", body_frame_reached, int)
    body_fresh = (False if body[1] and body[2] == 0 else
                  ((body_frame[2] - body[2]) & u32) <= 2
                  if body[1] and body[2] != 0 and body_frame[1] else None)
    jump_reached = (False if outer_wants is False or body_fresh is True else
                    True if outer_wants is True and body_fresh is False else None)
    jump = read("jumpFrame", jump_reached, int)
    jump_frame_reached = (False if jump_reached is False else
                          False if jump[0] and jump[1] and jump[2] == 0 else
                          True if jump_reached is True and jump[1] and jump[2] != 0 else None)
    jump_frame = read("jumpFrameNo", jump_frame_reached, int)
    jump_fresh = (False if jump[1] and jump[2] == 0 else
                  ((jump_frame[2] - jump[2]) & u32) <= 600
                  if jump[1] and jump[2] != 0 and jump_frame[1] else None)
    latch_reached = (False if jump_fresh is False else
                     True if jump_fresh is True else None)
    latch = read("modeLatch", latch_reached, bool)
    if outer_wants is False:
        return False
    if outer_wants is None:
        return None
    if body_fresh is True:
        return True
    if body_fresh is False and jump_fresh is False:
        return False
    if body_fresh is False and jump_fresh is True and latch[1]:
        return latch[2]
    return None


def _replay_fss_fact(fact, draw, label, predicate_fact_version=7):
    site_id, kind = fact["siteId"], fact["kind"]
    is_panel = site_id == 57
    selector, helper, mutation = fact.get("selector"), fact.get("helper"), fact.get("mutation")
    if not all(isinstance(item, dict) for item in (selector, helper, mutation)):
        raise TraceError(label + " FSS selector/helper/mutation must be objects")
    selector_fields = ({"outerEnabled", "bodyFrame", "frameNo"} if is_panel else
                       {"outerSteady", "outerLockstep", "bodyFrame", "bodyFrameNo",
                        "jumpFrame", "jumpFrameNo", "modeLatch"})
    helper_fields = (({"enabled", "contextNonNull"}) if is_panel else
                     {"steady", "lockstep", "contextNonNull"})
    helper_fields |= {"guardCallReached", "callbackEntered", "vsGetShaderCompleted",
                      "shaderNonNull", "lookupReached", "lookupCompleted", "assignedHash",
                      "releaseReached", "releaseCompleted", "callbackCompleted",
                      "guardReturned", "hashAfterGuard"}
    mutation_fields = ({"matchedHashBefore", "matchedHashAfter"} if is_panel else
                       {"arrivalOpen", "arrivalBefore", "arrivalAfter"})
    provenance_fields = {"handlerInvoked", "rawProbeReached"} if predicate_fact_version >= 8 else set()
    if (set(fact) != {"siteId", "kind", "known", "selector", "helper", "mutation"} | provenance_fields or
            set(selector) != selector_fields or set(helper) != helper_fields or set(mutation) != mutation_fields):
        raise TraceError(label + " FSS payload has missing or unexpected fields")

    def is_skipped(envelope):
        return (isinstance(envelope, dict) and set(envelope) == {"reached", "known", "value"}
                and envelope["reached"] is False and envelope["known"] is False
                and envelope["value"] is None)

    def group_skipped(group):
        return all(is_skipped(envelope) for envelope in group.values())

    if predicate_fact_version >= 8:
        handler_invoked = fact.get("handlerInvoked")
        raw_probe_reached = fact.get("rawProbeReached")
        if type(handler_invoked) is not bool or type(raw_probe_reached) is not bool:
            raise TraceError(label + " FSS provenance fields must be booleans")
        if handler_invoked == raw_probe_reached:
            raise TraceError(label + " FSS handler and raw outer probe provenance contradict")
        if handler_invoked and group_skipped(selector) and group_skipped(helper) and group_skipped(mutation):
            raise TraceError(label + " invoked FSS handler has no actual source reads")
        if raw_probe_reached:
            if not group_skipped(helper) or not group_skipped(mutation):
                raise TraceError(label + " raw FSS probe carries helper or mutation progress")
            outer_pass = _replay_fss_outer_probe(selector, is_panel, label)
            if outer_pass is False:
                event = {"id": site_id, "kind": 2, "outcome": 5,
                         "flow": 0, "subsite": 0, "verdict": -1}
                return event, 0, False, False
            return None, 0, True, False
        if not handler_invoked:
            if not (group_skipped(selector) and group_skipped(helper) and group_skipped(mutation)):
                raise TraceError(label + " FSS reads exist without handler or probe provenance")
            actual = next((event for event in draw["sites"] if event["id"] == site_id), None)
            not_eligible = {"id": site_id, "kind": 2, "outcome": 5,
                            "flow": 0, "subsite": 0, "verdict": -1}
            if actual != not_eligible:
                raise TraceError(label + " empty FSS observations require an uninvoked NotEligible site")
            return None, 0, True, False

    if all(is_skipped(envelope) for group in (selector, helper, mutation)
           for envelope in group.values()):
        actual = next((event for event in draw["sites"] if event["id"] == site_id), None)
        not_eligible = {"id": site_id, "kind": 2, "outcome": 5,
                        "flow": 0, "subsite": 0, "verdict": -1}
        if actual != not_eligible:
            raise TraceError(label + " empty FSS observations require an uninvoked NotEligible site")
        # Dispatch explains the absence of observations. It supplies no raw
        # frozen-selector inputs, so this cannot establish a known decline.
        return None, 0, True, False

    mismatch = 0
    unreplayable = False
    mutation_unobserved = False
    yes = True
    no = False
    u32 = 0xffffffff
    u64 = 0xffffffffffffffff
    tuple_match = (draw["kind"] == (ord("X") if is_panel else ord("N")) and
                   draw["count"] == 6 and draw["instances"] == 1)

    def read(group, key, reached, value_type, maximum=None, required=False):
        nonlocal unreplayable
        obj = selector if group == "selector" else helper if group == "helper" else mutation
        result = _fss_set_read(obj, key, label + "." + group, reached,
                              value_type, maximum, required)
        if reached is None or (result[0] and not result[1]):
            unreplayable = True
        return result

    if is_panel:
        outer = read("selector", "outerEnabled", yes, bool)
        body = read("selector", "bodyFrame", outer[2] if outer[1] else None, int, u32)
        frame = read("selector", "frameNo", False if outer[2] is False else (body[2] != 0) if body[1] else None,
                     int, u32)
        body_fresh = None
        if outer[1]:
            if not outer[2] or (body[1] and body[2] == 0):
                body_fresh = False
            elif body[1] and frame[1]:
                body_fresh = ((frame[2] - body[2]) & u32) <= 2
        outer_pass = (False if outer[1] and not outer[2] else
                      body_fresh if outer[1] else None)
        helper_call = outer_pass
    else:
        steady = read("selector", "outerSteady", yes, bool)
        lockstep = read("selector", "outerLockstep",
                        (not steady[2]) if steady[1] else None, bool)
        outer_wants = (steady[2] or lockstep[2]) if steady[1] and (steady[2] or lockstep[1]) else None
        body = read("selector", "bodyFrame", outer_wants, int, u32)
        body_frame = read("selector", "bodyFrameNo",
                          False if outer_wants is False else (body[2] != 0) if body[1] else None, int, u32)
        body_fresh = None
        if body[1] and body[2] == 0:
            body_fresh = False
        elif body[1] and body_frame[1]:
            body_fresh = ((body_frame[2] - body[2]) & u32) <= 2
        jump = read("selector", "jumpFrame",
                     False if outer_wants is False else (not body_fresh) if body_fresh is not None else None, int, u32)
        jump_frame_reached = (False if outer_wants is False or body_fresh is True else
                              False if jump[0] and jump[1] and jump[2] == 0 else
                              True if jump[0] and jump[1] and jump[2] != 0 else None)
        jump_frame = read("selector", "jumpFrameNo", jump_frame_reached, int, u32)
        jump_fresh = None
        if jump[1] and jump[2] == 0:
            jump_fresh = False
        elif jump[1] and jump_frame[1]:
            jump_fresh = ((jump_frame[2] - jump[2]) & u32) <= 600
        latch = read("selector", "modeLatch",
                     False if outer_wants is False or body_fresh is True else jump_fresh if jump_fresh is not None else None, bool)
        if body_fresh is True:
            outer_pass = True
        elif body_fresh is False and jump_fresh is False:
            outer_pass = False
        elif body_fresh is False and jump_fresh is True and latch[1]:
            outer_pass = latch[2]
        else:
            outer_pass = None
        if outer_wants is False:
            outer_pass = False
        elif outer_wants is None:
            outer_pass = None
        helper_call = outer_pass

    helper_invoked = helper_call
    if helper_call is None:
        if is_panel:
            read("helper", "enabled", None, bool)
        else:
            hsteady = read("helper", "steady", None, bool)
            read("helper", "lockstep",
                 (not hsteady[2]) if hsteady[1] else None, bool)
        read("helper", "contextNonNull", None, bool)
        helper_call = None
        expected_hash = None
        unreplayable = True
    elif helper_call is False:
        for key, typ, maximum in (("enabled" if is_panel else "steady", bool, None),
                                  ("contextNonNull", bool, None)):
            read("helper", key, no, typ, maximum)
        if not is_panel:
            read("helper", "lockstep", no, bool)
        expected_hash = None
    else:
        if is_panel:
            enabled = read("helper", "enabled", yes, bool)
            helper_wants = enabled[2] if enabled[1] else None
        else:
            hsteady = read("helper", "steady", yes, bool)
            hlock = read("helper", "lockstep",
                         (not hsteady[2]) if hsteady[1] else None, bool)
            helper_wants = ((hsteady[2] or hlock[2]) if hsteady[1] and
                            (hsteady[2] or hlock[1]) else None)
        if helper_wants is False:
            read("helper", "contextNonNull", no, bool)
            helper_call = False
        else:
            if not tuple_match:
                read("helper", "contextNonNull", no, bool)
                helper_call = False
            else:
                context = read("helper", "contextNonNull",
                               yes if helper_wants is True else None, bool)
                helper_call = context[2] if context[1] else None
        if helper_call is None:
            unreplayable = True
        if helper_call is False:
            expected_hash = None
        elif helper_call is True:
            if not is_panel:
                # lockstep was already consumed only when steady was false.
                pass
            guard_call = read("helper", "guardCallReached", yes, bool)
            if guard_call[1] and not guard_call[2]:
                raise TraceError(label + " eligible helper skipped its guarded call")
            statuses = ("callbackEntered", "vsGetShaderCompleted", "lookupReached",
                        "lookupCompleted", "releaseReached", "releaseCompleted",
                        "callbackCompleted")
            state = {name: read("helper", name, yes, bool) for name in statuses}
            callback = state["callbackEntered"][2]
            vs_done = state["vsGetShaderCompleted"][2]
            lookup_reached = state["lookupReached"][2]
            lookup_done = state["lookupCompleted"][2]
            release_reached = state["releaseReached"][2]
            release_done = state["releaseCompleted"][2]
            callback_done = state["callbackCompleted"][2]
            assigned = read("helper", "assignedHash", lookup_done, int, u64)
            shader = read("helper", "shaderNonNull", lookup_done, bool)
            if callback is False and any(x is True for x in (vs_done, lookup_reached, lookup_done,
                                     release_reached, release_done, callback_done)):
                raise TraceError(label + " FSS callback progress lacks entry")
            if callback is True and vs_done is False and any(x is True for x in (lookup_reached, lookup_done,
                                                  release_reached, release_done,
                                                  callback_done)):
                raise TraceError(label + " FSS lookup progress lacks VS result")
            if vs_done is True and lookup_reached is False:
                raise TraceError(label + " completed VS read lacks lookup")
            if lookup_reached is False and any(x is True for x in (lookup_done, release_reached, release_done)):
                raise TraceError(label + " FSS release/lookup stages are out of order")
            if lookup_done is True and lookup_reached is False:
                raise TraceError(label + " completed lookup lacks lookup stage")
            if lookup_done is True and shader[1] and release_reached is not None and release_reached != shader[2]:
                raise TraceError(label + " Release stage disagrees with shader pointer")
            if release_done is True and release_reached is False:
                raise TraceError(label + " completed Release lacks a Release call")
            if callback_done is True and (lookup_done is False or (release_reached is True and release_done is False)):
                raise TraceError(label + " callback completion disagrees with stages")
            if release_reached is True and release_done is True and callback_done is False:
                raise TraceError(label + " successful Release lacks callback completion")
            if lookup_done is True and shader[2] is False and callback_done is False:
                raise TraceError(label + " null shader cannot fault in the skipped Release")
            expected_hash = (assigned[2] if lookup_done is True and assigned[1] else
                             0 if state["lookupCompleted"][1] and lookup_done is False else None)
            if expected_hash is None:
                unreplayable = True
            guard_returned = read("helper", "guardReturned", yes, bool)
            hash_after = read("helper", "hashAfterGuard", yes, int, u64)
            # These postguard fields check instrumentation consistency only.
            if guard_returned[1] and callback_done is not None and guard_returned[2] != callback_done:
                mismatch += 1
            if hash_after[1] and expected_hash is not None and hash_after[2] != expected_hash:
                mismatch += 1
        else:
            expected_hash = None

    if helper_call is False:
        read("helper", "guardCallReached", no, bool)
        for key in ("callbackEntered", "vsGetShaderCompleted", "lookupReached",
                    "lookupCompleted", "releaseReached", "releaseCompleted",
                    "callbackCompleted", "guardReturned"):
            read("helper", key, no, bool)
        read("helper", "assignedHash", no, int, u64)
        read("helper", "shaderNonNull", no, bool)
        read("helper", "hashAfterGuard", no, int, u64)

    claim = None
    if helper_call is not False and expected_hash is not None:
        targets = (0xA888D51024D9798E, 0xB018D143700AB803) if is_panel else (0x953C8123AD8DC13B,)
        claim = expected_hash in targets
    elif helper_call is False:
        claim = False
    if claim is None:
        unreplayable = True

    if is_panel:
        before = read("mutation", "matchedHashBefore", helper_invoked, int, u64)
        after = read("mutation", "matchedHashAfter", helper_invoked, int, u64)
        if helper_invoked is not False:
            if not before[1] or not after[1]:
                mutation_unobserved = True
            elif claim is not None:
                expected_after = expected_hash if claim else before[2]
                if after[2] != expected_after:
                    mismatch += 1
    else:
        arrival = read("mutation", "arrivalOpen", claim, bool)
        if claim is None:
            read("mutation", "arrivalBefore", None, int, u32)
            read("mutation", "arrivalAfter", None, int, u32)
        elif claim:
            if not arrival[1]:
                unreplayable = True
            elif arrival[2]:
                before = read("mutation", "arrivalBefore", yes, int, u32)
                after = read("mutation", "arrivalAfter", yes, int, u32)
                if not before[1] or not after[1]:
                    mutation_unobserved = True
                elif after[2] != ((before[2] + 1) & u32):
                    mismatch += 1
            else:
                read("mutation", "arrivalBefore", no, int, u32)
                read("mutation", "arrivalAfter", no, int, u32)
        else:
            read("mutation", "arrivalBefore", no, int, u32)
            read("mutation", "arrivalAfter", no, int, u32)

    # Validate every serialized envelope, including the values at stages whose
    # reads were skipped. The branch checks above own selector reachability.
    for group, obj, fields in (("selector", selector, selector_fields),
                               ("helper", helper, helper_fields),
                               ("mutation", mutation, mutation_fields)):
        for key in fields:
            if group == "selector":
                maximum = u64 if key.endswith("Ms") else u32
                value_type = bool if key in {"outerEnabled", "outerSteady", "outerLockstep", "modeLatch"} else int
            elif group == "helper":
                maximum = u64 if key in ("assignedHash", "hashAfterGuard") else None
                value_type = int if key in ("assignedHash", "hashAfterGuard") else bool
            else:
                maximum = u64 if key.startswith("matchedHash") else u32
                value_type = int if key != "arrivalOpen" else bool
            _fss_read(obj, key, label + "." + group, value_type, maximum)

    if unreplayable:
        event = None
    else:
        event = {"id": site_id, "kind": 2, "outcome": 3 if claim else 2,
                 "flow": 1 if claim else 0, "subsite": 0,
                 "verdict": (11 if is_panel else 12) if claim else -1}
    return event, mismatch, unreplayable, mutation_unobserved


def _replay_remlok_fact(fact, draw, label):
    selector, helper, mutation = fact.get("selector"), fact.get("helper"), fact.get("mutation")
    selector_fields = {"outerMode"}
    helper_fields = {"modeBeforeGate", "dsvNonNull", "resolved", "isTexture2D",
                     "width", "height", "hideMode", "swap"}
    mutation_fields = {"matchesBefore", "matchesAfter", "hiddenBefore", "hiddenAfter",
                       "pendingRightBefore", "pendingRightAfter"}
    if (set(fact) != {"siteId", "kind", "known", "selector", "helper", "mutation"} or
            not all(isinstance(group, dict) for group in (selector, helper, mutation)) or
            set(selector) != selector_fields or set(helper) != helper_fields or
            set(mutation) != mutation_fields):
        raise TraceError(label + " RemLok payload has missing or unexpected fields")
    if fact.get("known") != "yes":
        raise TraceError(label + " RemLok fact availability must be yes; reads carry unknowns")

    no, yes = False, True
    u32, u64 = 0xffffffff, 0xffffffffffffffff
    mismatch = 0
    mutation_unobserved = False

    def is_skipped(envelope):
        return (isinstance(envelope, dict) and set(envelope) == {"reached", "known", "value"}
                and envelope["reached"] is False and envelope["known"] is False
                and envelope["value"] is None)

    def read(group, key, reached, value_type, maximum, required=False):
        obj = selector if group == "selector" else helper if group == "helper" else mutation
        return _fss_set_read(obj, key, label + "." + group, reached,
                             value_type, maximum, required_when_reached=required)

    outer = read("selector", "outerMode", yes, int, u32)
    shape = draw["kind"] == ord("N") and draw["count"] == 3 and draw["instances"] == 1
    helper_call = (False if not shape else
                   (outer[2] != 0) if outer[1] else None)

    mode = read("helper", "modeBeforeGate", helper_call, int, u32)
    active = (False if helper_call is False else
              (mode[2] != 0) if mode[1] else None)
    dsv = read("helper", "dsvNonNull", active, bool, None)
    resolve_call = (False if active is False or (dsv[1] and dsv[2]) else
                    True if active is True and dsv[1] and not dsv[2] else None)
    resolved = read("helper", "resolved", resolve_call, bool, None)
    type_call = (resolved[2] if resolved[1] else None) if resolve_call is not False else False
    texture = read("helper", "isTexture2D", type_call, bool, None)
    desc_call = (texture[2] if texture[1] else None) if type_call is not False else False
    width = read("helper", "width", desc_call, int, u32)
    height_call = False if desc_call is False else (width[2] == 1024 if width[1] else None)
    height = read("helper", "height", height_call, int, u32)
    dimensions_match = (width[2] == 1024 and height[2] == 512
                        if desc_call is True and width[1] and height[1] else
                        False if height_call is False else None)
    mode_call = dimensions_match
    hide_mode = read("helper", "hideMode", mode_call, int, u32)
    swap_call = (False if dimensions_match is False or
                 (hide_mode[1] and hide_mode[2] == 2) else
                 True if dimensions_match is True and hide_mode[1] else None)
    swap = read("helper", "swap", swap_call, bool, None)

    match = dimensions_match is True
    hide = match and hide_mode[1] and hide_mode[2] == 2
    scissor = match and hide_mode[1] and hide_mode[2] != 2
    undecided = dimensions_match is None or (match and not hide_mode[1])

    before_matches = read("mutation", "matchesBefore", helper_call, int, u32)
    after_matches = read("mutation", "matchesAfter", helper_call, int, u32)
    before_hidden = read("mutation", "hiddenBefore", helper_call, int, u64)
    after_hidden = read("mutation", "hiddenAfter", helper_call, int, u64)
    before_pending = read("mutation", "pendingRightBefore", helper_call, bool, None)
    after_pending = read("mutation", "pendingRightAfter", helper_call, bool, None)
    mutation_reads = (before_matches, after_matches, before_hidden, after_hidden,
                      before_pending, after_pending)
    if helper_call is True and any(not value[0] or not value[1] for value in mutation_reads):
        mutation_unobserved = True
    if helper_call is True and all(value[0] and value[1] for value in mutation_reads):
        if dimensions_match is not None and after_matches[2] != ((before_matches[2] + (1 if match else 0)) & u32):
            mismatch += 1
        hidden_trigger_known = dimensions_match is False or (match and hide_mode[1])
        if hidden_trigger_known and after_hidden[2] != ((before_hidden[2] + (1 if hide else 0)) & u64):
            mismatch += 1
        expected_pending = before_pending[2]
        if scissor and swap[1]:
            expected_pending = ((before_matches[2] & 1) != 0) != swap[2]
        pending_trigger_known = dimensions_match is False or hide or (scissor and swap[1])
        if pending_trigger_known and after_pending[2] != expected_pending:
            mismatch += 1
        if dimensions_match is None or not hidden_trigger_known or not pending_trigger_known:
            mutation_unobserved = True
    if helper_call is True and scissor and not swap[1]:
        mutation_unobserved = True

    # Validate all present envelopes, including skipped stages, with strict
    # bool/int types and the original field-specific widths.
    for key in selector_fields:
        read("selector", key, None, int, u32)
    for key in helper_fields:
        value_type = int if key in {"modeBeforeGate", "width", "height", "hideMode"} else bool
        maximum = u32 if key in {"width", "height", "modeBeforeGate", "hideMode"} else None
        read("helper", key, None, value_type, maximum)
    for key in mutation_fields:
        value_type = int if key in {"matchesBefore", "matchesAfter", "hiddenBefore", "hiddenAfter"} else bool
        maximum = u32 if key.startswith("matches") else u64 if key.startswith("hidden") else None
        read("mutation", key, None, value_type, maximum)

    if helper_call is False:
        if not all(is_skipped(value) for group in (helper, mutation) for value in group.values()):
            raise TraceError(label + " skipped RemLok helper carries reads or mutations")
    elif helper_call is None:
        # A source mode that was actually read but unavailable makes the
        # helper reach uncertain; preserve valid instrumentation either way.
        pass

    if helper_call is False or (mode[1] and mode[2] == 0) or (dsv[1] and dsv[2]) or \
            (resolved[1] and not resolved[2]) or (texture[1] and not texture[2]) or \
            dimensions_match is False:
        hide = False
        scissor = False
        undecided = False
    elif mode[0] and not mode[1] or (helper_call is None and not outer[1]):
        undecided = True

    if undecided:
        return None, None, mismatch, mutation_unobserved
    site51 = ({"id": 51, "kind": 2, "outcome": 4, "flow": 1,
               "subsite": 0, "verdict": 2} if hide else
              {"id": 51, "kind": 2, "outcome": 2, "flow": 0,
               "subsite": 0, "verdict": -1})
    site52 = (None if hide else
              {"id": 52, "kind": 2, "outcome": 3 if scissor else 2,
               "flow": 1 if scissor else 0, "subsite": 0,
               "verdict": 3 if scissor else -1})
    return site51, site52, mismatch, mutation_unobserved


def _replay_basic_draw_fact(fact, draw, label):
    """Replay the site-2 foreign-context guard or site-67 distance gate."""
    required = {"siteId", "kind", "known", "context", "distance"}
    if set(fact) != required or fact.get("known") != "yes":
        raise TraceError(label + " has missing or unexpected BasicDraw fields")
    site_id, kind = fact["siteId"], fact["kind"]
    context = fact.get("context")
    distance = fact.get("distance")
    context_names = ("contextIdentity", "ownerContextIdentity",
                     "glareClampBefore", "glareClampAfter")
    if not isinstance(context, dict) or set(context) != set(context_names):
        raise TraceError(label + ".context has missing or unexpected fields")
    if not isinstance(distance, dict) or set(distance) != {"distanceEnabled"}:
        raise TraceError(label + ".distance has missing or unexpected fields")

    applicable_context = site_id == 2 and kind == 15
    applicable_distance = site_id == 67 and kind == 16
    if not (applicable_context or applicable_distance):
        raise TraceError(label + " has a mismatched BasicDraw site/kind")

    reads = {}
    for name in context_names:
        value_type = int
        maximum = (1 << 64) - 1 if name in ("contextIdentity", "ownerContextIdentity") else 0xffffffff
        read = _sunglare_read(context, name, label + ".context", value_type, maximum)
        should_reach = applicable_context
        if read[0] != should_reach:
            raise TraceError(label + ".context." + name + " has inconsistent reachability")
        reads[name] = read
    distance_read = _sunglare_read(distance, "distanceEnabled", label + ".distance", bool)
    if distance_read[0] != applicable_distance:
        raise TraceError(label + ".distance.distanceEnabled has inconsistent reachability")

    event = None
    mismatch = 0
    mutation_unobserved = False
    if applicable_context:
        self_identity = reads["contextIdentity"]
        owner_identity = reads["ownerContextIdentity"]
        before = reads["glareClampBefore"]
        after = reads["glareClampAfter"]
        if after[1] and after[2] != 0:
            mismatch += 1
        if not (before[1] and after[1]):
            mutation_unobserved = True
        # This is a consistency check only; site61 never supplies site2 inputs.
        for source in draw.get("predicateFacts", []):
            if (isinstance(source, dict) and source.get("siteId") == 61 and
                    source.get("kind") == 9 and isinstance(source.get("common2ClampAfter"), dict)):
                source_after = _sunglare_read(source, "common2ClampAfter", label + ".site61",
                                               int, 0xffffffff)
                if after[1] and source_after[1] and after[2] != source_after[2]:
                    mismatch += 1
                source_before = _sunglare_read(source, "common2ClampBefore", label + ".site61",
                                                int, 0xffffffff)
                if before[1] and source_before[1] and before[2] != source_before[2]:
                    mismatch += 1
                break
        if self_identity[1] and owner_identity[1]:
            foreign = self_identity[2] != owner_identity[2]
            event = {"id": 2, "kind": 3, "outcome": 4 if foreign else 1,
                     "flow": 1 if foreign else 0, "subsite": 0,
                     "verdict": 0 if foreign else -1}
    else:
        enabled = distance_read[2] if distance_read[1] else None
        if enabled is not None:
            event = {"id": 67, "kind": 3, "outcome": 2 if enabled else 4,
                     "flow": 0 if enabled else 1, "subsite": 0,
                     "verdict": -1 if enabled else 0}
    return event, mismatch, mutation_unobserved


class _EyeCensusUnavailable(Exception):
    pass


def _replay_eye_census_fact(fact, draw, label):
    """Replay the independent site-48 rule loop from ordered reads."""
    required = {"siteId", "kind", "known", "skipCountGate", "rules",
                "terminalLoopCount", "mutation"}
    if set(fact) != required or fact.get("known") != "yes":
        raise TraceError(label + " has missing or unexpected EyeCensus fields")

    def read(obj, name, path, value_type, maximum=None):
        item = obj.get(name)
        if not isinstance(item, dict) or set(item) != {"reached", "known", "value"}:
            raise TraceError(path + "." + name + " is malformed")
        reached, known, value = item["reached"], item["known"], item["value"]
        if type(reached) is not bool or type(known) is not bool:
            raise TraceError(path + "." + name + " availability must be boolean")
        if known and not reached:
            raise TraceError(path + "." + name + " cannot be known before it is reached")
        if not known:
            if value is not None:
                raise TraceError(path + "." + name + " unknown value must be null")
            return reached, False, None
        if value_type is bool:
            if type(value) is not bool:
                raise TraceError(path + "." + name + " must contain a boolean")
        else:
            _integer(value, path + "." + name + ".value", 0,
                     maximum if maximum is not None else 0xffffffff)
        return reached, True, value

    u8, u32, u64 = 0xff, 0xffffffff, 0xffffffffffffffff
    def req(obj, name, path, value_type, maximum=None):
        result = read(obj, name, path, value_type, maximum)
        if not result[0]:
            raise TraceError(path + "." + name + " was not reached when consumed")
        if not result[1]:
            raise _EyeCensusUnavailable()
        return result[2]

    def must_unread(obj, name, path, value_type, maximum=None):
        result = read(obj, name, path, value_type, maximum)
        if result[0]:
            raise TraceError(path + "." + name + " was reached after a short circuit")

    def validate_unvisited_rule(rule, ri):
        rpath = "%s.rules[%d]" % (label, ri)
        if not isinstance(rule, dict) or set(rule) != {
                "loopCount", "vsHashGate", "heldVsHash", "vsHashCompareExpected",
                "ruleKind", "countHighGate", "countMinimum", "countHighBound",
                "exactCount", "filters"}:
            raise TraceError(rpath + " has missing or unexpected fields")
        for field, maximum in (("loopCount", u32), ("vsHashGate", u64),
                               ("heldVsHash", u64), ("vsHashCompareExpected", u64),
                               ("ruleKind", u8), ("countHighGate", u32),
                               ("countMinimum", u32), ("countHighBound", u32),
                               ("exactCount", u32)):
            must_unread(rule, field, rpath, int, maximum)
        filters = rule.get("filters")
        if not isinstance(filters, list) or len(filters) != 4:
            raise TraceError(rpath + ".filters must contain exactly four filters")
        for fi, filt in enumerate(filters):
            fpath = "%s.filters[%d]" % (rpath, fi)
            if not isinstance(filt, dict) or set(filt) != {x[0] for x in _eye_filter_types(u8, u32)}:
                raise TraceError(fpath + " has missing or unexpected fields")
            for field, typ, maximum in _eye_filter_types(u8, u32):
                must_unread(filt, field, fpath, typ, maximum)

    # Validate the fixed wire envelope even when an early consumed value is
    # unknown. Reach semantics below remain lazy; unknown does not imply false.
    rule_types = (("loopCount", u32), ("vsHashGate", u64), ("heldVsHash", u64),
                  ("vsHashCompareExpected", u64), ("ruleKind", u8),
                  ("countHighGate", u32), ("countMinimum", u32),
                  ("countHighBound", u32), ("exactCount", u32))
    for field in ("skipCountGate", "terminalLoopCount"):
        item = read(fact, field, label, int, u32)
        if item[1] and item[2] > 8:
            raise TraceError(label + "." + field + " exceeds the eight-rule capacity")
    rules = fact.get("rules")
    if not isinstance(rules, list) or len(rules) != 8:
        raise TraceError(label + ".rules must contain exactly eight rules")
    for ri, rule in enumerate(rules):
        rpath = "%s.rules[%d]" % (label, ri)
        if not isinstance(rule, dict) or set(rule) != {x[0] for x in rule_types} | {"filters"}:
            raise TraceError(rpath + " has missing or unexpected fields")
        for field, maximum in rule_types:
            item = read(rule, field, rpath, int, maximum)
            if field == "loopCount" and item[1] and item[2] > 8:
                raise TraceError(rpath + ".loopCount exceeds the eight-rule capacity")
        filters = rule["filters"]
        if not isinstance(filters, list) or len(filters) != 4:
            raise TraceError(rpath + ".filters must contain exactly four filters")
        for fi, filt in enumerate(filters):
            fpath = "%s.filters[%d]" % (rpath, fi)
            if not isinstance(filt, dict) or set(filt) != {x[0] for x in _eye_filter_types(u8, u32)}:
                raise TraceError(fpath + " has missing or unexpected fields")
            for field, typ, maximum in _eye_filter_types(u8, u32):
                read(filt, field, fpath, typ, maximum)
    mutation = fact.get("mutation")
    if not isinstance(mutation, dict) or set(mutation) != {"censusSkippedBefore", "censusSkippedAfter"}:
        raise TraceError(label + ".mutation has missing or unexpected fields")
    for field in mutation:
        read(mutation, field, label + ".mutation", int, u64)

    if req(fact, "skipCountGate", label, int, u32) == 0:
        rules = fact.get("rules")
        if not isinstance(rules, list) or len(rules) != 8:
            raise TraceError(label + ".rules must contain exactly eight rules")
        for ri, rule in enumerate(rules):
            validate_unvisited_rule(rule, ri)
        must_unread(fact, "terminalLoopCount", label, int, u32)
        expected = _eye_site_event(False, 0)
        delta = 0
    else:
        rules = fact.get("rules")
        if not isinstance(rules, list) or len(rules) != 8:
            raise TraceError(label + ".rules must contain exactly eight rules")
        expected = None
        delta = 0
        terminal = None
        loop_exit_index = 8
        for ri, rule in enumerate(rules):
            rpath = "%s.rules[%d]" % (label, ri)
            if not isinstance(rule, dict) or set(rule) != {
                    "loopCount", "vsHashGate", "heldVsHash", "vsHashCompareExpected",
                    "ruleKind", "countHighGate", "countMinimum", "countHighBound",
                    "exactCount", "filters"}:
                raise TraceError(rpath + " has missing or unexpected fields")
            loop_read = read(rule, "loopCount", rpath, int, u32)
            if not loop_read[0]:
                loop_exit_index = ri
                validate_unvisited_rule(rule, ri)
                for field, maximum in (("vsHashGate", u64), ("heldVsHash", u64),
                                       ("vsHashCompareExpected", u64), ("ruleKind", u8),
                                       ("countHighGate", u32), ("countMinimum", u32),
                                       ("countHighBound", u32), ("exactCount", u32)):
                    must_unread(rule, field, rpath, int, maximum)
                for rest in range(ri + 1, 8):
                    validate_unvisited_rule(rules[rest], rest)
                break
            if not loop_read[1]:
                raise _EyeCensusUnavailable()
            if loop_read[2] <= ri or loop_read[2] > 8:
                raise TraceError(rpath + ".loopCount does not enter its observed iteration")
            hash_gate = req(rule, "vsHashGate", rpath, int, u64)
            if hash_gate:
                held_hash = req(rule, "heldVsHash", rpath, int, u64)
                expected_hash = req(rule, "vsHashCompareExpected", rpath, int, u64)
                if held_hash == expected_hash:
                    # Hash rules claim immediately. All later reads are forbidden.
                    for field, maximum in (("ruleKind", u8), ("countHighGate", u32),
                                           ("countMinimum", u32), ("countHighBound", u32),
                                           ("exactCount", u32)):
                        must_unread(rule, field, rpath, int, maximum)
                    _eye_require_filters_unread(rule, rpath, u8, u32, must_unread)
                    expected = _eye_site_event(True, ri)
                    delta = 1
                    terminal = ri
                    for rest in range(ri + 1, 8):
                        validate_unvisited_rule(rules[rest], rest)
                    break
                for field, maximum in (("ruleKind", u8), ("countHighGate", u32),
                                       ("countMinimum", u32), ("countHighBound", u32),
                                       ("exactCount", u32)):
                    must_unread(rule, field, rpath, int, maximum)
                _eye_require_filters_unread(rule, rpath, u8, u32, must_unread)
                continue
            must_unread(rule, "heldVsHash", rpath, int, u64)
            must_unread(rule, "vsHashCompareExpected", rpath, int, u64)
            count_high = req(rule, "countHighGate", rpath, int, u32)
            if count_high:
                minimum = req(rule, "countMinimum", rpath, int, u32)
                if draw["count"] < minimum:
                    must_unread(rule, "countHighBound", rpath, int, u32)
                    must_unread(rule, "exactCount", rpath, int, u32)
                    count_hit = False
                else:
                    high = req(rule, "countHighBound", rpath, int, u32)
                    must_unread(rule, "exactCount", rpath, int, u32)
                    count_hit = draw["count"] <= high
            else:
                must_unread(rule, "countMinimum", rpath, int, u32)
                must_unread(rule, "countHighBound", rpath, int, u32)
                exact = req(rule, "exactCount", rpath, int, u32)
                count_hit = draw["count"] == exact
            kind = req(rule, "ruleKind", rpath, int, u8)
            if not count_hit or kind != draw["kind"]:
                _eye_require_filters_unread(rule, rpath, u8, u32, must_unread)
                continue
            filters = rule.get("filters")
            if not isinstance(filters, list) or len(filters) != 4:
                raise TraceError(rpath + ".filters must contain exactly four filters")
            rule_match = True
            for fi, filt in enumerate(filters):
                fpath = "%s.filters[%d]" % (rpath, fi)
                if not isinstance(filt, dict) or set(filt) != {x[0] for x in _eye_filter_types(u8, u32)}:
                    raise TraceError(fpath + " has missing or unexpected fields")
                if not rule_match:
                    for field, typ, maximum in _eye_filter_types(u8, u32):
                        must_unread(filt, field, fpath, typ, maximum)
                    continue
                mode_off = req(filt, "modeOffGate", fpath, int, u8)
                if mode_off == 0:
                    _eye_filter_unread_after(filt, fpath, "modeAnyGate", u8, u32, must_unread)
                    continue
                mode_any = req(filt, "modeAnyGate", fpath, int, u8)
                if mode_any == 4:
                    _eye_filter_unread_after(filt, fpath, "boundNonNull", u8, u32, must_unread)
                    continue
                bound = req(filt, "boundNonNull", fpath, bool)
                mode_bound = req(filt, "modeAfterBound", fpath, int, u8)
                if mode_bound == 3:
                    if bound:
                        rule_match = False
                    _eye_filter_unread_after(filt, fpath, "resolved", u8, u32, must_unread)
                    continue
                resolved = req(filt, "resolved", fpath, bool)
                if not resolved:
                    rule_match = False
                    for field, typ, maximum in _eye_filter_types(u8, u32):
                        if field in ("modeOffGate", "modeAnyGate", "boundNonNull", "modeAfterBound", "resolved"):
                            continue
                        must_unread(filt, field, fpath, typ, maximum)
                    continue
                texture = req(filt, "isTexture2D", fpath, bool)
                if not texture:
                    rule_match = False
                    for field, typ, maximum in _eye_filter_types(u8, u32):
                        if field in ("modeOffGate", "modeAnyGate", "boundNonNull", "modeAfterBound", "resolved", "isTexture2D"):
                            continue
                        must_unread(filt, field, fpath, typ, maximum)
                    continue
                mode_resolve = req(filt, "modeAfterResolve", fpath, int, u8)
                if mode_resolve == 2:
                    eye_available = req(filt, "eyeSizeAvailable", fpath, bool)
                    if not eye_available:
                        rule_match = False
                        must_unread(filt, "width", fpath, int, u32)
                        must_unread(filt, "height", fpath, int, u32)
                        must_unread(filt, "eyeWidth", fpath, int, u32)
                        must_unread(filt, "eyeHeight", fpath, int, u32)
                    else:
                        width = req(filt, "width", fpath, int, u32)
                        eye_width = req(filt, "eyeWidth", fpath, int, u32)
                        if width != eye_width:
                            rule_match = False
                            must_unread(filt, "height", fpath, int, u32)
                            must_unread(filt, "eyeHeight", fpath, int, u32)
                        else:
                            height = req(filt, "height", fpath, int, u32)
                            eye_height = req(filt, "eyeHeight", fpath, int, u32)
                            if height != eye_height:
                                rule_match = False
                    for field in ("configuredWidth", "configuredHeight"):
                        must_unread(filt, field, fpath, int, u32)
                else:
                    width = req(filt, "width", fpath, int, u32)
                    configured_width = req(filt, "configuredWidth", fpath, int, u32)
                    if width != configured_width:
                        rule_match = False
                        must_unread(filt, "height", fpath, int, u32)
                        must_unread(filt, "configuredHeight", fpath, int, u32)
                    else:
                        height = req(filt, "height", fpath, int, u32)
                        configured_height = req(filt, "configuredHeight", fpath, int, u32)
                        if height != configured_height:
                            rule_match = False
                    must_unread(filt, "eyeSizeAvailable", fpath, bool)
                    must_unread(filt, "eyeWidth", fpath, int, u32)
                    must_unread(filt, "eyeHeight", fpath, int, u32)
            if rule_match:
                expected = _eye_site_event(True, ri)
                delta = 1
                terminal = ri
                for rest in range(ri + 1, 8):
                    validate_unvisited_rule(rules[rest], rest)
                break
        if expected is None:
            terminal_count = req(fact, "terminalLoopCount", label, int, u32)
            if terminal_count > 8:
                raise TraceError(label + ".terminalLoopCount exceeds the eight-rule capacity")
            if terminal_count > loop_exit_index:
                raise TraceError(label + ".terminalLoopCount does not terminate the observed loop")
            expected = _eye_site_event(False, 0)
        else:
            must_unread(fact, "terminalLoopCount", label, int, u32)

    mutation = fact.get("mutation")
    if not isinstance(mutation, dict) or set(mutation) != {"censusSkippedBefore", "censusSkippedAfter"}:
        raise TraceError(label + ".mutation has missing or unexpected fields")
    before = read(mutation, "censusSkippedBefore", label + ".mutation", int, u64)
    after = read(mutation, "censusSkippedAfter", label + ".mutation", int, u64)
    mutation_unobserved = False
    mismatch = 0
    if delta:
        if not (before[0] and before[1] and after[0] and after[1]):
            mutation_unobserved = True
        elif after[2] != ((before[2] + 1) & u64):
            mismatch += 1
    elif before[0] or after[0]:
        # A decline must not fabricate mutation observations.
        raise TraceError(label + " declined EyeCensus fact carries mutation reads")
    return expected, mismatch, mutation_unobserved


def _eye_filter_types(u8, u32):
    return [("modeOffGate", int, u8), ("modeAnyGate", int, u8),
            ("boundNonNull", bool, None), ("modeAfterBound", int, u8),
            ("resolved", bool, None), ("isTexture2D", bool, None),
            ("width", int, u32), ("height", int, u32),
            ("modeAfterResolve", int, u8), ("configuredWidth", int, u32),
            ("configuredHeight", int, u32), ("eyeSizeAvailable", bool, None),
            ("eyeWidth", int, u32), ("eyeHeight", int, u32)]


def _eye_filter_unread_after(filt, path, first, u8, u32, must_unread):
    types = _eye_filter_types(u8, u32)
    start = next(i for i, entry in enumerate(types) if entry[0] == first)
    for field, typ, maximum in types[start:]:
        must_unread(filt, field, path, typ, maximum)


def _eye_require_filters_unread(rule, path, u8, u32, must_unread):
    filters = rule.get("filters")
    if not isinstance(filters, list) or len(filters) != 4:
        raise TraceError(path + ".filters must contain exactly four filters")
    for fi, filt in enumerate(filters):
        fpath = "%s.filters[%d]" % (path, fi)
        if not isinstance(filt, dict) or set(filt) != {x[0] for x in _eye_filter_types(u8, u32)}:
            raise TraceError(fpath + " has missing or unexpected fields")
        for field, typ, maximum in _eye_filter_types(u8, u32):
            must_unread(filt, field, fpath, typ, maximum)


def _eye_site_event(claimed, subsite):
    return ({"id": 48, "kind": 2, "outcome": 4, "flow": 1,
             "subsite": subsite, "verdict": 2} if claimed else
            {"id": 48, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1})


class _ResolveBindUnavailable(Exception):
    pass


class _LoaderPanelUnavailable(Exception):
    pass


class _FssDumpUnavailable(Exception):
    def __init__(self, mutation_unobserved=False):
        self.mutation_unobserved = mutation_unobserved


def _replay_loader_panel_fact(fact, draw, label):
    """Replay site25 from independent outer, helper, and post-collection reads."""
    required = {"siteId", "kind", "known", "outer", "helper"}
    if set(fact) != required or fact.get("known") != "yes":
        raise TraceError(label + " has missing or unexpected LoaderPanel fields")
    if (fact.get("siteId"), fact.get("kind")) != (25, 19):
        raise TraceError(label + " has mismatched LoaderPanel site/kind")

    u32 = 0xffffffff
    i32 = 0x7fffffff
    outer_types = {
        "wants": bool, "eyeDrawsLastFrame": int, "rtvPresent": bool,
        "resolved": bool, "isTexture2D": bool, "targetWidth": int,
        "targetHeight": int, "qsStartIndex": int, "qsBaseVertex": int,
        "textured": bool,
    }
    sequence_types = {name: int for name in (
        "position", "hashBeforeCount", "hashAfterCount", "hashBeforeWidth",
        "hashAfterWidth", "hashBeforeHeight", "hashAfterHeight", "slotCount",
        "slotWidth", "slotHeight", "lenBefore", "lenAfter")}
    panel_types = {
        "frameAnyBefore": bool, "frameAnyAfter": bool,
        "frameFirstPanelDoneBefore": bool, "frameFirstPanelDoneAfter": bool,
        "chainOnBeforeCollection": bool, "chainWidth": int, "chainHeight": int,
        "frameChainPanelBefore": bool, "frameChainPanelAfter": bool,
        "panelOrdinalBefore": int, "panelOrdinalAfter": int, "localOrdinal": int,
    }
    collection_types = {
        "collecting": bool, "capCountGate": int, "guardEntered": bool,
        "guardReturned": bool, "capCountBeforeWrite": int, "capCountAfterWrite": int,
        "ibFillBeforeWrite": int, "ibFillAfterWrite": int,
        "capDroppedBeforeWrite": int, "capDroppedAfterWrite": int,
    }
    withhold_types = {
        "subArmBeforeClear": bool, "subArmAfterClear": bool,
        "subArmBeforeWithhold": bool, "subArmAfterWithhold": bool,
        "chainOrdCountGate": int, "terminalChainOrdCount": int,
        "specDoneGate": bool, "chainOnSpecGate": bool, "retiredGate": bool,
        "frameWithheldBefore": bool, "frameWithheldAfter": bool,
        "dimLiveBefore": bool, "dimLiveAfter": bool,
    }

    def parse_group(obj, expected, path):
        if not isinstance(obj, dict) or set(obj) != set(expected):
            raise TraceError(path + " has missing or unexpected fields")
        values = {}
        for name, value_type in expected.items():
            item = obj.get(name)
            item_path = path + "." + name
            if not isinstance(item, dict) or set(item) != {"reached", "known", "value"}:
                raise TraceError(item_path + " is malformed")
            reached, known, value = item["reached"], item["known"], item["value"]
            if type(reached) is not bool or type(known) is not bool:
                raise TraceError(item_path + " availability must be boolean")
            if known and not reached:
                raise TraceError(item_path + " cannot be known before it is reached")
            if not known:
                if value is not None:
                    raise TraceError(item_path + " unknown value must be null")
                values[name] = (reached, False, None)
                continue
            if value_type is bool:
                if type(value) is not bool:
                    raise TraceError(item_path + " must contain a boolean")
            elif value_type is int:
                low, high = (-i32 - 1, i32) if name == "qsBaseVertex" else (0, u32)
                if path.endswith("sequence") and name in ("position", "lenBefore", "lenAfter"):
                    high = 48
                elif path.endswith("collection"):
                    if name in ("capCountGate", "capCountAfterWrite"):
                        high = 24
                    elif name == "capCountBeforeWrite":
                        high = 23
                elif path.endswith("withhold") and name in ("chainOrdCountGate", "terminalChainOrdCount"):
                    high = 4
                elif ".chainScan[" in path and name == "loopCount":
                    high = 4
                _integer(value, item_path + ".value", low, high)
            values[name] = (reached, True, value)
        return values

    outer_obj = fact.get("outer")
    helper_obj = fact.get("helper")
    if not isinstance(outer_obj, dict) or set(outer_obj) != set(outer_types):
        raise TraceError(label + ".outer has missing or unexpected fields")
    if not isinstance(helper_obj, dict) or set(helper_obj) != {
            "wants", "contextNonNull", "sequence", "panel", "collection", "withhold"}:
        raise TraceError(label + ".helper has missing or unexpected fields")
    outer = parse_group(outer_obj, outer_types, label + ".outer")
    helper_wants = parse_group({"wants": helper_obj["wants"]}, {"wants": bool},
                               label + ".helper")
    helper_context = parse_group({"contextNonNull": helper_obj["contextNonNull"]},
                                 {"contextNonNull": bool}, label + ".helper")
    sequence = parse_group(helper_obj.get("sequence"), sequence_types,
                           label + ".helper.sequence")
    panel = parse_group(helper_obj.get("panel"), panel_types,
                        label + ".helper.panel")
    collection_obj = helper_obj.get("collection")
    if not isinstance(collection_obj, dict) or set(collection_obj) != \
            set(collection_types) | {"collectionMutationUnobserved"}:
        raise TraceError(label + ".helper.collection has missing or unexpected fields")
    collection = parse_group({k: collection_obj[k] for k in collection_types},
                             collection_types, label + ".helper.collection")
    if type(collection_obj["collectionMutationUnobserved"]) is not bool:
        raise TraceError(label + ".helper.collection.collectionMutationUnobserved must be boolean")
    withhold_obj = helper_obj.get("withhold")
    withhold_expected = set(withhold_types) | {"chainScan"}
    if not isinstance(withhold_obj, dict) or set(withhold_obj) != withhold_expected:
        raise TraceError(label + ".helper.withhold has missing or unexpected fields")
    withhold = parse_group({k: withhold_obj[k] for k in withhold_types},
                           withhold_types, label + ".helper.withhold")
    chain_scan = withhold_obj["chainScan"]
    if not isinstance(chain_scan, list) or len(chain_scan) != 4:
        raise TraceError(label + ".helper.withhold.chainScan must contain exactly four slots")
    chain_reads = []
    for index, entry in enumerate(chain_scan):
        parsed = parse_group(entry, {"loopCount": int, "chainOrd": int},
                             label + ".helper.withhold.chainScan[%d]" % index)
        chain_reads.append(parsed)

    def consumed(group, name, path):
        item = group[name]
        if not item[0]:
            raise TraceError(path + " was not reached when consumed")
        if not item[1]:
            raise _LoaderPanelUnavailable()
        return item[2]

    def unread(group, names, path):
        for name in names:
            if group[name][0]:
                raise TraceError(path + "." + name + " was reached after a short circuit")

    def unread_nested(groups):
        for group, names, path in groups:
            unread(group, names, path)

    outer_names = tuple(outer_types)
    helper_root_names = ("wants", "contextNonNull")
    sequence_names = tuple(sequence_types)
    panel_names = tuple(panel_types)
    collection_names = tuple(collection_types)
    withhold_names = tuple(withhold_types)
    helper_path = label + ".helper"
    all_helper_unread = [(helper_wants, ("wants",), helper_path),
                         (helper_context, ("contextNonNull",), helper_path),
                         (sequence, sequence_names, helper_path + ".sequence"),
                         (panel, panel_names, helper_path + ".panel"),
                         (collection, collection_names, helper_path + ".collection"),
                         (withhold, withhold_names, helper_path + ".withhold")]
    # Every fixed chain read must also remain absent when the selector never enters.
    def require_chain_unread():
        for index, entry in enumerate(chain_reads):
            unread(entry, ("loopCount", "chainOrd"),
                   helper_path + ".withhold.chainScan[%d]" % index)

    def require_no_collection_status():
        if collection_obj["collectionMutationUnobserved"]:
            raise TraceError(helper_path + ".collection reports opaque work before an admitted capture")

    def decline_outer_after(fields):
        unread(outer, fields, label + ".outer")
        unread_nested(all_helper_unread)
        require_chain_unread()
        require_no_collection_status()
        return _loader_panel_event(False), 0, False

    try:
        if not consumed(outer, "wants", label + ".outer.wants"):
            return decline_outer_after(outer_names[1:])
        eye_count = consumed(outer, "eyeDrawsLastFrame", label + ".outer.eyeDrawsLastFrame")
        if eye_count >= 100:
            return decline_outer_after(outer_names[2:])
        # Pointer presence is provenance only. The source predicate calls
        # bindingResolve even when the binding lookup returns null.
        consumed(outer, "rtvPresent", label + ".outer.rtvPresent")
        if not consumed(outer, "resolved", label + ".outer.resolved"):
            return decline_outer_after(outer_names[4:])
        if not consumed(outer, "isTexture2D", label + ".outer.isTexture2D"):
            return decline_outer_after(outer_names[5:])
        target_width = consumed(outer, "targetWidth", label + ".outer.targetWidth")
        if target_width < 1024:
            return decline_outer_after(outer_names[6:])
        target_height = consumed(outer, "targetHeight", label + ".outer.targetHeight")
        if target_height < 512:
            return decline_outer_after(outer_names[7:])
        start_index = consumed(outer, "qsStartIndex", label + ".outer.qsStartIndex")
        base_vertex = consumed(outer, "qsBaseVertex", label + ".outer.qsBaseVertex")
        textured = consumed(outer, "textured", label + ".outer.textured")

        helper_wants_value = consumed(helper_wants, "wants", helper_path + ".wants")
        if not helper_wants_value:
            unread(helper_context, ("contextNonNull",), helper_path)
            unread_nested([(sequence, sequence_names, helper_path + ".sequence"),
                           (panel, panel_names, helper_path + ".panel"),
                           (collection, collection_names, helper_path + ".collection"),
                           (withhold, withhold_names, helper_path + ".withhold")])
            require_chain_unread()
            require_no_collection_status()
            return _loader_panel_event(False), 0, False
        context_non_null = consumed(helper_context, "contextNonNull", helper_path + ".contextNonNull")
        if not context_non_null:
            unread_nested([(sequence, sequence_names, helper_path + ".sequence"),
                           (panel, panel_names, helper_path + ".panel"),
                           (collection, collection_names, helper_path + ".collection"),
                           (withhold, withhold_names, helper_path + ".withhold")])
            require_chain_unread()
            require_no_collection_status()
            return _loader_panel_event(False), 0, False
        if draw["kind"] != ord("X") or draw["count"] == 0:
            unread_nested([(sequence, sequence_names, helper_path + ".sequence"),
                           (panel, panel_names, helper_path + ".panel"),
                           (collection, collection_names, helper_path + ".collection"),
                           (withhold, withhold_names, helper_path + ".withhold")])
            require_chain_unread()
            require_no_collection_status()
            return _loader_panel_event(False), 0, False

        # Sequence composition is deterministic from the immutable draw facts and
        # the actual target dimensions passed to the helper.
        pos = consumed(sequence, "position", helper_path + ".sequence.position")
        hashes = ("hashBeforeCount", "hashAfterCount", "hashBeforeWidth",
                  "hashAfterWidth", "hashBeforeHeight", "hashAfterHeight")
        h = {name: consumed(sequence, name, helper_path + ".sequence." + name)
             for name in hashes}
        expected_hash_count = ((h["hashBeforeCount"] ^ draw["count"]) * 16777619) & u32
        expected_hash_width = ((h["hashAfterCount"] ^ target_width) * 16777619) & u32
        expected_hash_height = ((h["hashAfterWidth"] ^ target_height) * 16777619) & u32
        mismatches = 0
        if (h["hashAfterCount"] != expected_hash_count or
                h["hashBeforeWidth"] != h["hashAfterCount"] or
                h["hashAfterWidth"] != expected_hash_width or
                h["hashBeforeHeight"] != h["hashAfterWidth"] or
                h["hashAfterHeight"] != expected_hash_height):
            mismatches += 1
        if pos < 48:
            slot_count = consumed(sequence, "slotCount", helper_path + ".sequence.slotCount")
            slot_width = consumed(sequence, "slotWidth", helper_path + ".sequence.slotWidth")
            slot_height = consumed(sequence, "slotHeight", helper_path + ".sequence.slotHeight")
            before_len = consumed(sequence, "lenBefore", helper_path + ".sequence.lenBefore")
            after_len = consumed(sequence, "lenAfter", helper_path + ".sequence.lenAfter")
            if before_len != pos:
                mismatches += 1
            if (slot_count != draw["count"] or slot_width != target_width or
                    slot_height != target_height or after_len != before_len + 1):
                mismatches += 1
        else:
            unread(sequence, ("slotCount", "slotWidth", "slotHeight", "lenBefore", "lenAfter"),
                   helper_path + ".sequence")

        first_panel = False
        local_ordinal = u32
        if draw["count"] == 30:
            any_before = consumed(panel, "frameAnyBefore", helper_path + ".panel.frameAnyBefore")
            any_after = consumed(panel, "frameAnyAfter", helper_path + ".panel.frameAnyAfter")
            first_done_before = consumed(panel, "frameFirstPanelDoneBefore",
                                         helper_path + ".panel.frameFirstPanelDoneBefore")
            first_panel = not first_done_before
            first_done_after = consumed(panel, "frameFirstPanelDoneAfter",
                                         helper_path + ".panel.frameFirstPanelDoneAfter")
            if not (any_after and first_done_after):
                mismatches += 1
            if any_before and not any_after:
                mismatches += 1
            if first_done_after is not True:
                mismatches += 1
            chain_on = consumed(panel, "chainOnBeforeCollection",
                                helper_path + ".panel.chainOnBeforeCollection")
            if chain_on:
                chain_width = consumed(panel, "chainWidth", helper_path + ".panel.chainWidth")
                if target_width == chain_width:
                    chain_height = consumed(panel, "chainHeight", helper_path + ".panel.chainHeight")
                else:
                    chain_height = None
                    unread(panel, ("chainHeight",), helper_path + ".panel")
                if chain_height is not None and target_height == chain_height:
                    frame_chain_before = consumed(panel, "frameChainPanelBefore",
                                                  helper_path + ".panel.frameChainPanelBefore")
                    frame_chain_after = consumed(panel, "frameChainPanelAfter",
                                                 helper_path + ".panel.frameChainPanelAfter")
                    local_ordinal = consumed(panel, "panelOrdinalBefore",
                                             helper_path + ".panel.panelOrdinalBefore")
                    panel_ord_after = consumed(panel, "panelOrdinalAfter",
                                               helper_path + ".panel.panelOrdinalAfter")
                    if panel_ord_after != ((local_ordinal + 1) & u32):
                        mismatches += 1
                    if not frame_chain_after:
                        mismatches += 1
                else:
                    unread(panel, ("frameChainPanelBefore", "frameChainPanelAfter",
                                   "panelOrdinalBefore", "panelOrdinalAfter"), helper_path + ".panel")
            else:
                unread(panel, ("chainWidth", "chainHeight", "frameChainPanelBefore",
                               "frameChainPanelAfter", "panelOrdinalBefore", "panelOrdinalAfter"),
                       helper_path + ".panel")
        else:
            unread(panel, ("frameAnyBefore", "frameAnyAfter", "frameFirstPanelDoneBefore",
                           "frameFirstPanelDoneAfter", "chainOnBeforeCollection", "chainWidth",
                           "chainHeight", "frameChainPanelBefore", "frameChainPanelAfter",
                           "panelOrdinalBefore", "panelOrdinalAfter"), helper_path + ".panel")
        observed_local = consumed(panel, "localOrdinal", helper_path + ".panel.localOrdinal")
        if observed_local != local_ordinal:
            mismatches += 1

        collecting = consumed(collection, "collecting", helper_path + ".collection.collecting")
        qualifies = (not textured and draw["count"] % 6 == 0)
        drop_names = ("capDroppedBeforeWrite", "capDroppedAfterWrite")
        write_names = ("capCountBeforeWrite", "capCountAfterWrite",
                       "ibFillBeforeWrite", "ibFillAfterWrite")
        if not collecting or not qualifies:
            unread(collection, ("capCountGate", "guardEntered", "guardReturned") +
                   write_names + drop_names, helper_path + ".collection")
        else:
            drop_expected = False
            if pos < 48:
                cap_count = consumed(collection, "capCountGate", helper_path + ".collection.capCountGate")
                if cap_count < 24:
                    guard_entered = consumed(collection, "guardEntered", helper_path + ".collection.guardEntered")
                    guard_returned = consumed(collection, "guardReturned", helper_path + ".collection.guardReturned")
                    if not guard_entered and guard_returned:
                        mismatches += 1
                    writes_reached = [collection[name][0] for name in write_names]
                    if any(writes_reached) and not all(writes_reached):
                        raise TraceError(helper_path + ".collection has a partial write checkpoint")
                    if all(writes_reached):
                        if not guard_entered:
                            raise TraceError(helper_path + ".collection writes outside an entered callback")
                        before_cap = consumed(collection, "capCountBeforeWrite", helper_path + ".collection.capCountBeforeWrite")
                        after_cap = consumed(collection, "capCountAfterWrite", helper_path + ".collection.capCountAfterWrite")
                        before_fill = consumed(collection, "ibFillBeforeWrite", helper_path + ".collection.ibFillBeforeWrite")
                        after_fill = consumed(collection, "ibFillAfterWrite", helper_path + ".collection.ibFillAfterWrite")
                        if after_cap != before_cap + 1:
                            mismatches += 1
                        # Both need and the persisted fill are UINT in the source.
                        expected_fills = {((before_fill + ((draw["count"] * size) & u32)) & u32)
                                          for size in (2, 4)}
                        if after_fill not in expected_fills:
                            mismatches += 1
                        if any(collection[name][0] for name in drop_names):
                            raise TraceError(helper_path + ".collection records a drop alongside a stored draw")
                    else:
                        unread(collection, write_names, helper_path + ".collection")
                        drop_expected = True
                else:
                    unread(collection, ("guardEntered", "guardReturned") + write_names,
                           helper_path + ".collection")
                    drop_expected = True
            else:
                unread(collection, ("capCountGate", "guardEntered", "guardReturned") + write_names,
                       helper_path + ".collection")
                drop_expected = True
            if drop_expected:
                before_drop = consumed(collection, "capDroppedBeforeWrite", helper_path + ".collection.capDroppedBeforeWrite")
                after_drop = consumed(collection, "capDroppedAfterWrite", helper_path + ".collection.capDroppedAfterWrite")
                if after_drop != ((before_drop + 1) & u32):
                    mismatches += 1
            else:
                unread(collection, drop_names, helper_path + ".collection")
        collection_mutation_unobserved = collection_obj["collectionMutationUnobserved"]
        guard_entered_read = collection["guardEntered"]
        if guard_entered_read[0] and guard_entered_read[1] and \
                collection_mutation_unobserved != guard_entered_read[2]:
            raise TraceError(helper_path + ".collection mutation status disagrees with guard entry")
        if collection_mutation_unobserved and not (guard_entered_read[0] and guard_entered_read[1] and guard_entered_read[2]):
            raise TraceError(helper_path + ".collection marks opaque work outside an admitted capture")

        # After the potentially reentrant collection callback, the helper reads
        # the live chain and intro state again. Repeated chain bounds are retained.
        sub_before_clear = consumed(withhold, "subArmBeforeClear", helper_path + ".withhold.subArmBeforeClear")
        sub_after_clear = consumed(withhold, "subArmAfterClear", helper_path + ".withhold.subArmAfterClear")
        if sub_after_clear:
            mismatches += 1
        scan_hit = False
        if local_ordinal != u32:
            first_bound = consumed(withhold, "chainOrdCountGate", helper_path + ".withhold.chainOrdCountGate")
            if first_bound > 4:
                raise TraceError(helper_path + " chain ordinal count exceeds fixed capacity four")
            i = 0
            terminal_seen = False
            if first_bound == 0:
                terminal_count = consumed(withhold, "terminalChainOrdCount",
                                          helper_path + ".withhold.terminalChainOrdCount")
                if terminal_count != first_bound:
                    mismatches += 1
                terminal_seen = True
                require_chain_unread()
            else:
                while i < 4:
                    slot = chain_reads[i]
                    slot_path = helper_path + ".withhold.chainScan[%d]" % i
                    if i == 0:
                        bound = first_bound
                    elif slot["loopCount"][0]:
                        bound = consumed(slot, "loopCount", slot_path + ".loopCount")
                    else:
                        terminal_count = consumed(withhold, "terminalChainOrdCount",
                                                  helper_path + ".withhold.terminalChainOrdCount")
                        if terminal_count > 4:
                            raise TraceError(helper_path + " terminal chain bound exceeds fixed capacity four")
                        if terminal_count > i:
                            raise TraceError(helper_path + " terminal bound continues beyond an unrecorded slot")
                        for j in range(i, 4):
                            unread(chain_reads[j], ("loopCount", "chainOrd"),
                                   helper_path + ".withhold.chainScan[%d]" % j)
                        terminal_seen = True
                        break
                    if bound > 4:
                        raise TraceError(helper_path + " repeated chain bound exceeds fixed capacity four")
                    if bound <= i:
                        raise TraceError(helper_path + " recorded chain slot lies beyond its loop bound")
                    if i == 0:
                        recorded_bound = consumed(slot, "loopCount", slot_path + ".loopCount")
                        if recorded_bound != bound:
                            mismatches += 1
                    ordinal = consumed(slot, "chainOrd", slot_path + ".chainOrd")
                    if ordinal == local_ordinal:
                        scan_hit = True
                        i += 1
                        break
                    i += 1
                if scan_hit:
                    for j in range(i, 4):
                        unread(chain_reads[j], ("loopCount", "chainOrd"),
                               helper_path + ".withhold.chainScan[%d]" % j)
                    unread(withhold, ("terminalChainOrdCount",), helper_path + ".withhold")
                elif i == 4:
                    terminal_count = consumed(withhold, "terminalChainOrdCount",
                                              helper_path + ".withhold.terminalChainOrdCount")
                    if terminal_count > 4:
                        raise TraceError(helper_path + " terminal chain bound exceeds fixed capacity four")
                    terminal_seen = True
                elif not terminal_seen:
                    raise TraceError(helper_path + " chain scan ended without a terminal bound")
        else:
            unread(withhold, ("chainOrdCountGate", "terminalChainOrdCount"), helper_path + ".withhold")
            require_chain_unread()
        if scan_hit:
            sub_before_withhold = consumed(withhold, "subArmBeforeWithhold",
                                           helper_path + ".withhold.subArmBeforeWithhold")
            if sub_before_withhold:
                mismatches += 1
            unread(withhold, ("specDoneGate", "chainOnSpecGate", "retiredGate"), helper_path + ".withhold")
            claim = True
        else:
            spec_done = consumed(withhold, "specDoneGate", helper_path + ".withhold.specDoneGate")
            if spec_done:
                unread(withhold, ("chainOnSpecGate", "retiredGate"), helper_path + ".withhold")
                claim = False
            else:
                chain_on_spec = consumed(withhold, "chainOnSpecGate", helper_path + ".withhold.chainOnSpecGate")
                if chain_on_spec:
                    unread(withhold, ("retiredGate",), helper_path + ".withhold")
                    claim = False
                else:
                    retired = consumed(withhold, "retiredGate", helper_path + ".withhold.retiredGate")
                    claim = not retired and first_panel
        if claim:
            if not scan_hit:
                sub_before_withhold = consumed(withhold, "subArmBeforeWithhold",
                                               helper_path + ".withhold.subArmBeforeWithhold")
                if sub_before_withhold:
                    mismatches += 1
            sub_after_withhold = consumed(withhold, "subArmAfterWithhold",
                                          helper_path + ".withhold.subArmAfterWithhold")
            if not sub_after_withhold:
                mismatches += 1
            frame_withheld_before = consumed(withhold, "frameWithheldBefore",
                                             helper_path + ".withhold.frameWithheldBefore")
            frame_withheld_after = consumed(withhold, "frameWithheldAfter",
                                            helper_path + ".withhold.frameWithheldAfter")
            dim_live_before = consumed(withhold, "dimLiveBefore", helper_path + ".withhold.dimLiveBefore")
            dim_live_after = consumed(withhold, "dimLiveAfter", helper_path + ".withhold.dimLiveAfter")
            if not frame_withheld_after or not dim_live_after:
                mismatches += 1
        else:
            unread(withhold, ("subArmBeforeWithhold", "subArmAfterWithhold",
                              "frameWithheldBefore", "frameWithheldAfter",
                              "dimLiveBefore", "dimLiveAfter"), helper_path + ".withhold")
        return _loader_panel_event(claim), mismatches, collection_mutation_unobserved
    except _LoaderPanelUnavailable:
        return None, 0, collection_obj["collectionMutationUnobserved"]


def _loader_panel_event(claimed):
    return ({"id": 25, "kind": 2, "outcome": 3, "flow": 1,
             "subsite": 0, "verdict": 16} if claimed else
            {"id": 25, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1})


def _replay_resolve_bind_fact(fact, label):
    """Replay site60 from independent outer, shadow, callback, and repair reads."""
    required = {"siteId", "kind", "known", "outer", "helper"}
    if set(fact) != required or fact.get("known") != "yes":
        raise TraceError(label + " has missing or unexpected ResolveBind fields")
    u64 = 0xffffffffffffffff
    read_types = {
        "outer": {"wants": bool, "psPresent": bool, "psHash": int},
        "helper": {
            "wants": bool, "contextNonNull": bool, "psPresent": bool,
            "psHash": int, "lambdaEntered": bool, "psGetReached": bool,
            "psGetCompleted": bool, "shaderNonNull": bool,
            "lookupReached": bool, "lookupCompleted": bool,
            "lookupHash": int, "cacheBeforePresent": bool,
            "cacheBeforeHash": int, "cacheAfterPresent": bool,
            "cacheAfterHash": int, "releaseReached": bool,
            "releaseCompleted": bool, "guardReturned": bool,
        },
    }
    groups = {}
    for group_name, expected in read_types.items():
        group = fact.get(group_name)
        if not isinstance(group, dict) or set(group) != set(expected):
            raise TraceError(label + "." + group_name + " has missing or unexpected fields")
        values = {}
        for name, value_type in expected.items():
            item = group.get(name)
            path = label + "." + group_name + "." + name
            if not isinstance(item, dict) or set(item) != {"reached", "known", "value"}:
                raise TraceError(path + " is malformed")
            reached, known, value = item["reached"], item["known"], item["value"]
            if type(reached) is not bool or type(known) is not bool:
                raise TraceError(path + " availability must be boolean")
            if known and not reached:
                raise TraceError(path + " cannot be known before it is reached")
            if not known:
                if value is not None:
                    raise TraceError(path + " unknown value must be null")
                values[name] = (reached, False, None)
            else:
                if value_type is bool:
                    if type(value) is not bool:
                        raise TraceError(path + " must contain a boolean")
                else:
                    _integer(value, path + ".value", 0, u64)
                values[name] = (reached, True, value)
        groups[group_name] = values

    def consumed(group, name):
        value = groups[group][name]
        if not value[0]:
            raise TraceError(label + "." + group + "." + name + " was not reached when consumed")
        if not value[1]:
            raise _ResolveBindUnavailable()
        return value[2]

    def unread(group, name):
        value = groups[group][name]
        if value[0]:
            raise TraceError(label + "." + group + "." + name + " was reached after a short circuit")

    def unread_group(group, names):
        for name in names:
            unread(group, name)

    outer_fields = ("psPresent", "psHash")
    helper_fields = tuple(read_types["helper"])
    outer_wants = consumed("outer", "wants")
    if not outer_wants:
        unread_group("outer", outer_fields)
        unread_group("helper", helper_fields)
        return _resolve_bind_event(False), 0, False

    outer_present = consumed("outer", "psPresent")
    outer_hash = consumed("outer", "psHash")
    outer_known_shadow = outer_present and outer_hash != 0
    if outer_known_shadow and outer_hash != RESOLVE_BIND_PS_HASH:
        unread_group("helper", helper_fields)
        return _resolve_bind_event(False), 0, False

    # Outer and helper snapshots are separate reads; COM reentry may change them.
    helper_wants = consumed("helper", "wants")
    if not helper_wants:
        unread_group("helper", helper_fields[1:])
        return _resolve_bind_event(False), 0, False
    context_non_null = consumed("helper", "contextNonNull")
    if not context_non_null:
        unread_group("helper", helper_fields[2:])
        return _resolve_bind_event(False), 0, False
    helper_present = consumed("helper", "psPresent")
    helper_hash = consumed("helper", "psHash")
    helper_known_shadow = helper_present and helper_hash != 0
    if helper_known_shadow:
        unread_group("helper", helper_fields[4:])
        return _resolve_bind_event(helper_hash == RESOLVE_BIND_PS_HASH), 0, False

    lambda_entered = consumed("helper", "lambdaEntered")
    guard_returned = consumed("helper", "guardReturned")
    if not lambda_entered:
        if guard_returned:
            raise TraceError(label + " denied fallback lambda cannot report a returned guard")
        unread_group("helper", helper_fields[5:-1])
        return _resolve_bind_event(False), 0, False

    if not consumed("helper", "psGetReached"):
        raise TraceError(label + " entered fallback lambda without reaching PSGetShader")
    ps_get_completed = groups["helper"]["psGetCompleted"]
    if not ps_get_completed[0]:
        if guard_returned:
            raise TraceError(label + " PSGetShader fault cannot return through guardedBudget")
        unread_group("helper", helper_fields[7:-1])
        return _resolve_bind_event(False), 0, False
    if not ps_get_completed[1]:
        raise _ResolveBindUnavailable()
    if ps_get_completed[2] is not True:
        raise TraceError(label + " PSGetShader completion marker is invalid")
    shader_non_null = consumed("helper", "shaderNonNull")
    if not shader_non_null:
        unread_group("helper", helper_fields[8:-1])
        if not guard_returned:
            raise TraceError(label + " null PSGetShader result has no later guarded fault")
        return _resolve_bind_event(False), 0, False

    if not consumed("helper", "lookupReached"):
        raise TraceError(label + " nonnull shader did not reach lookupShaderHash")
    lookup_completed = groups["helper"]["lookupCompleted"]
    if not lookup_completed[0]:
        unread_group("helper", helper_fields[10:-1])
        if guard_returned:
            raise TraceError(label + " lookup fault cannot return through guardedBudget")
        return _resolve_bind_event(False), 0, False
    if not lookup_completed[1]:
        raise _ResolveBindUnavailable()
    if lookup_completed[2] is not True:
        raise TraceError(label + " lookup completion marker is invalid")
    lookup_hash = consumed("helper", "lookupHash")
    mismatch = 0
    mutation_unobserved = False
    repair_completed = False
    if lookup_hash:
        before_present_read = groups["helper"]["cacheBeforePresent"]
        after_present_read = groups["helper"]["cacheAfterPresent"]
        before_hash_read = groups["helper"]["cacheBeforeHash"]
        after_hash_read = groups["helper"]["cacheAfterHash"]

        def cache_snapshot(present_read, hash_read, name):
            # The producer reads both operands unconditionally around repair;
            # an absent pointer may retain a nonzero raw shadow hash.
            if not all(item[0] and item[1] for item in (present_read, hash_read)):
                return None
            return (present_read[2], hash_read[2])

        before_snapshot = cache_snapshot(before_present_read, before_hash_read,
                                         "cacheBefore")
        after_snapshot = cache_snapshot(after_present_read, after_hash_read,
                                        "cacheAfter")
        repair_completed = after_snapshot is not None
        if before_snapshot is None or after_snapshot is None:
            mutation_unobserved = True
        if after_snapshot is not None and after_snapshot != (True, lookup_hash):
            mismatch += 1
    else:
        unread_group("helper", ("cacheBeforePresent", "cacheBeforeHash",
                                "cacheAfterPresent", "cacheAfterHash"))

    release_reached = groups["helper"]["releaseReached"]
    release_completed = groups["helper"]["releaseCompleted"]
    if release_reached[0] and not release_reached[1]:
        raise _ResolveBindUnavailable()
    if release_reached[0]:
        if release_reached[2] is not True:
            raise TraceError(label + " Release reached marker is invalid")
        if release_completed[0] and not release_completed[1]:
            raise _ResolveBindUnavailable()
        if not release_completed[0]:
            if guard_returned:
                raise TraceError(label + " Release fault cannot return through guardedBudget")
        elif not release_completed[1] or release_completed[2] is not True:
            raise TraceError(label + " Release completion marker is invalid")
        elif not guard_returned:
            raise TraceError(label + " completed Release must return through guardedBudget")
    else:
        unread("helper", "releaseCompleted")
        if lookup_hash == 0:
            raise TraceError(label + " successful zero-hash lookup did not reach Release")
        if repair_completed:
            raise TraceError(label + " completed cache repair did not reach Release")
        if guard_returned:
            raise TraceError(label + " missing Release after nonnull PS violates fallback order")

    # A successful lookup with a nonzero hash assigns match after the repair
    # returns and before Release. Release faults therefore preserve that result.
    assigned = bool(lookup_hash and release_reached[0])
    if lookup_hash and not release_reached[0]:
        # Repair did not finish, so the initialized false value survives.
        if not repair_completed:
            mutation_unobserved = True
    return _resolve_bind_event(lookup_hash == RESOLVE_BIND_PS_HASH if assigned else False), \
        mismatch, mutation_unobserved


def _resolve_bind_event(claimed):
    return ({"id": 60, "kind": 2, "outcome": 3, "flow": 1,
             "subsite": 0, "verdict": 14} if claimed else
            {"id": 60, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1})


def _candidate_witchspace_stars(fact, draw):
    """Candidate selector from raw helper inputs, independent of site output."""
    if fact["hiddenKnown"] != "yes":
        return None
    if fact["hidden"] == "no":
        return False
    if fact["contextKnown"] != "yes":
        return None
    if fact["contextValid"] == "no":
        return False
    if fact["shapeReached"] != "yes":
        return None
    if (draw["kind"] not in (ord("X"), ord("N")) or
            draw["instances"] == 0 or draw["count"] < 6):
        return False
    if fact["hashKnown"] != "yes":
        return None
    return fact["vsHash"].upper() == "9AEC596A2B036EA6"


def _legacy14a_witchspace_stars(fact, draw):
    """Frozen 14a helper semantics; zero/absent shadow uses captured fallback."""
    if fact["hiddenKnown"] != "yes":
        return None
    if fact["hidden"] != "yes":
        return False
    if fact["contextKnown"] != "yes":
        return None
    if fact["contextValid"] != "yes":
        return False
    if fact["shapeReached"] != "yes":
        return None
    if (draw["kind"] != ord("X") and draw["kind"] != ord("N")):
        return False
    if draw["instances"] == 0 or draw["count"] < 6:
        return False
    if fact["hashKnown"] != "yes":
        return None
    return fact["vsHash"].upper() == "9AEC596A2B036EA6"


def _replay_witchspace_stars_fact(fact, draw, label):
    required = {"siteId", "kind", "known", "interestMaskKnown", "legacyInterestMask",
                "helperReached", "hiddenKnown", "hidden", "contextKnown", "contextValid",
                "shapeReached", "shapeMatched", "hashKnown", "hashSource", "vsHash",
                "skippedDeltaKnown", "skippedDelta"}
    if set(fact) != required:
        raise TraceError(label + " has missing or unexpected witchspace-star fields")
    tri_fields = ("interestMaskKnown", "helperReached", "hiddenKnown", "hidden",
                  "contextKnown", "contextValid", "shapeReached", "shapeMatched",
                  "hashKnown")
    if any(fact.get(key) not in TRI_STATES for key in tri_fields):
        raise TraceError(label + " has an invalid witchspace-star tri-state")
    if fact["known"] != "yes" or fact["interestMaskKnown"] != "yes":
        raise TraceError(label + " lacks the reached interest-mask input")
    mask_text = fact["legacyInterestMask"]
    if not isinstance(mask_text, str) or not re.match(r"^[0-9A-Fa-f]{16}$", mask_text):
        raise TraceError(label + ".legacyInterestMask must be sixteen hex digits")
    mask = int(mask_text, 16)
    if mask >> 8:
        raise TraceError(label + " legacy interest mask carries unknown IDs")
    interested = bool(mask & (1 << 1))
    if fact["helperReached"] != ("yes" if interested else "no"):
        raise TraceError(label + " helper stage disagrees with the consumed interest mask")
    hash_source = _integer(fact.get("hashSource"), label + ".hashSource", 0, 3)
    hash_text = fact.get("vsHash")
    if not isinstance(hash_text, str) or not re.match(r"^[0-9A-Fa-f]{16}$", hash_text):
        raise TraceError(label + ".vsHash must be sixteen hex digits")
    hash_known = fact["hashKnown"] == "yes"
    if hash_known != (hash_source != 0) or (not hash_known and int(hash_text, 16) != 0):
        raise TraceError(label + " hash availability/source/value disagree")
    if hash_known and (hash_source not in (1, 2, 3)):
        raise TraceError(label + " has an invalid consumed hash source")
    if ((hash_source == 1 and int(hash_text, 16) == 0) or
            (hash_source == 2 and int(hash_text, 16) != 0)):
        raise TraceError(label + " hash source is inconsistent with its consumed value")
    if not interested and hash_known and hash_source != 1:
        raise TraceError(label + " an uninvoked helper cannot claim a fallback hash")
    delta_known = fact.get("skippedDeltaKnown")
    if type(delta_known) is not bool:
        raise TraceError(label + ".skippedDeltaKnown is invalid")
    delta = _integer(fact.get("skippedDelta"), label + ".skippedDelta", 0, 1)
    if not interested and (delta_known or delta != 0):
        raise TraceError(label + " an uninvoked helper cannot report a counter delta")

    hidden_known = fact["hiddenKnown"] == "yes"
    hidden = fact["hidden"]
    context_known = fact["contextKnown"] == "yes"
    context_valid = fact["contextValid"]
    shape_reached = fact["shapeReached"] == "yes"
    shape_matched = fact["shapeMatched"]
    if not hidden_known and hidden != "unknown":
        raise TraceError(label + " unknown hidden state carries a value")
    if hidden_known and hidden not in ("yes", "no"):
        raise TraceError(label + " known hidden state lacks its boolean value")
    if not context_known and context_valid != "unknown":
        raise TraceError(label + " unknown context validity carries a value")
    if context_known and context_valid not in ("yes", "no"):
        raise TraceError(label + " known context validity is missing")
    if not shape_reached and shape_matched != "unknown":
        raise TraceError(label + " unreached shape carries a value")
    if fact["shapeReached"] == "no":
        raise TraceError(label + " shape stage cannot be explicitly marked unreached")
    if shape_reached:
        if hidden != "yes" or context_valid != "yes":
            raise TraceError(label + " shape was recorded before hidden/context gates passed")
        expected_shape = (draw["kind"] in (ord("X"), ord("N")) and
                          draw["instances"] > 0 and draw["count"] >= 6)
        if shape_matched != ("yes" if expected_shape else "no"):
            raise TraceError(label + " captured shape disagrees with draw inputs")
    if interested:
        if not delta_known:
            raise TraceError(label + " invoked helper lacks its counter mutation observation")
        if not hidden_known or hidden not in ("yes", "no"):
            raise TraceError(label + " invoked helper lacks the consumed hidden flag")
        if hidden == "no" and (context_known or shape_reached or hash_known):
            raise TraceError(label + " hidden short-circuit carries later helper inputs")
        if hidden == "yes" and not context_known:
            raise TraceError(label + " hidden helper stage lacks context validity")
        if context_valid == "no" and (shape_reached or hash_known):
            raise TraceError(label + " null-context short-circuit carries later inputs")
        if context_valid == "yes" and not shape_reached:
            raise TraceError(label + " valid context lacks the consumed shape inputs")
        if shape_reached and shape_matched == "no" and hash_known:
            raise TraceError(label + " shape miss carries a VS hash")
        if shape_reached and shape_matched == "yes" and not hash_known:
            raise TraceError(label + " matched shape lacks the consumed VS hash")
    else:
        if not hidden_known or not context_known:
            raise TraceError(label + " staged-out site lacks cheap hidden/context facts")
        if hidden == "yes" and context_valid == "yes" and not shape_reached:
            raise TraceError(label + " staged-out site lacks the counterfactual shape inputs")
        if hash_known and not (hidden == "yes" and context_valid == "yes" and
                               shape_reached and shape_matched == "yes" and
                               hash_source == 1):
            raise TraceError(label + " staged-out hash was not consumed before eligibility")

    normalized = dict(fact)
    normalized["vsHash"] = hash_text
    candidate = _candidate_witchspace_stars(normalized, draw)
    legacy = _legacy14a_witchspace_stars(normalized, draw)
    if candidate != legacy:
        raise TraceError(label + " candidate differs from frozen 14a selector")
    if interested:
        expected_event = None if legacy is None else (
            {"id": 6, "kind": 2, "outcome": 4, "flow": 1,
             "subsite": 0, "verdict": 2} if legacy else
            {"id": 6, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1})
        expected_delta = (1 if legacy else 0) if legacy is not None else None
    else:
        expected_event = {"id": 6, "kind": 2, "outcome": 5,
                          "flow": 0, "subsite": 0, "verdict": -1}
        expected_delta = None
    return expected_event, expected_delta, delta_known, delta, legacy


def _offscreen_rules(fact, label, expected_count, allow_zero_dimensions=False,
                     quad_armed=None):
    count = _integer(fact.get("offscreenRuleCount"),
                     label + ".offscreenRuleCount", 0, 4)
    rules = fact.get("offscreenRules")
    if count != expected_count or not isinstance(rules, list) or len(rules) != count:
        raise TraceError(label + " offscreen rule count/list is inconsistent")
    normalized = []
    for index, rule in enumerate(rules):
        rule_label = "%s.offscreenRules[%d]" % (label, index)
        if not isinstance(rule, dict) or set(rule) != {"kind", "count", "w", "h"}:
            raise TraceError(rule_label + " must contain exactly kind/count/w/h")
        kind = _integer(rule.get("kind"), rule_label + ".kind", 0, 255)
        n = _integer(rule.get("count"), rule_label + ".count")
        width = _integer(rule.get("w"), rule_label + ".w",
                         0 if allow_zero_dimensions else 1)
        height = _integer(rule.get("h"), rule_label + ".h",
                          0 if allow_zero_dimensions else 1)
        allowed_kinds = (0, ord("D"), ord("I"), ord("N"), ord("X"))
        if kind not in allowed_kinds:
            raise TraceError(rule_label + ".kind is outside the config parser domain")
        if kind == 0 and n != 0:
            raise TraceError(rule_label + " wildcard kind requires the parser's zero count")
        if quad_armed is True and (kind == 0 or n == 0 or width == 0 or height == 0):
            raise TraceError(rule_label + " armed quad rule lacks valid dimensions/kind/count")
        if quad_armed is False and (kind != 0 or n != 0 or width != 0 or height != 0):
            raise TraceError(rule_label + " unarmed quad must preserve the zero-initialized raw rule")
        normalized.append({
            "kind": kind,
            "count": n, "w": width, "h": height,
        })
    return normalized


def _offscreen_probe(fact, label, should_reach):
    tri_fields = ("offscreenProbeReached", "offscreenProbeResolved",
                  "offscreenProbeTexture2D")
    if any(fact.get(name) not in TRI_STATES for name in tri_fields):
        raise TraceError(label + " has an invalid offscreen probe tri-state")
    reached = fact["offscreenProbeReached"]
    resolved = fact["offscreenProbeResolved"]
    texture = fact["offscreenProbeTexture2D"]
    width = _integer(fact.get("offscreenTargetW"), label + ".offscreenTargetW")
    height = _integer(fact.get("offscreenTargetH"), label + ".offscreenTargetH")
    if reached != ("yes" if should_reach else "no"):
        raise TraceError(label + " probe reachability disagrees with earlier gates")
    if not should_reach:
        if resolved != "unknown" or texture != "unknown" or width != 0 or height != 0:
            raise TraceError(label + " skipped probe carries resolution outputs")
        return False, 0, 0
    if resolved == "unknown":
        raise TraceError(label + " reached probe lacks its resolution result")
    if resolved == "no":
        if texture != "unknown" or width != 0 or height != 0:
            raise TraceError(label + " failed resolution carries target outputs")
        return False, 0, 0
    if texture not in ("yes", "no"):
        raise TraceError(label + " resolved probe lacks its texture type")
    if texture == "yes" and (width == 0 or height == 0):
        raise TraceError(label + " resolved texture has invalid dimensions")
    if texture == "no" and (width != 0 or height != 0):
        raise TraceError(label + " non-texture target carries dimensions")
    return texture == "yes", width, height


def _replay_offscreen_census_fact(fact, draw, label):
    required = {"siteId", "kind", "known", "offscreenRuleCount", "offscreenRules",
                "quadArmed", "offscreenEyeDrawsLastFrame",
                "offscreenProbeReached", "offscreenProbeResolved",
                "offscreenProbeTexture2D", "offscreenTargetW", "offscreenTargetH",
                "censusSkippedDeltaKnown", "censusSkippedDelta"}
    if set(fact) != required:
        raise TraceError(label + " has missing or unexpected offscreen-census fields")
    if fact["known"] != "yes":
        raise TraceError(label + " lacks offscreen census inputs")
    if (fact["quadArmed"] != "unknown" or
            _integer(fact["offscreenEyeDrawsLastFrame"],
                     label + ".offscreenEyeDrawsLastFrame") != 0):
        raise TraceError(label + " census fact carries quad-only inputs")
    rules = _offscreen_rules(fact, label, fact["offscreenRuleCount"])
    if len(rules) > 4:
        raise TraceError(label + " exceeds the bounded offscreen rule table")
    texture2d, width, height = _offscreen_probe(fact, label, bool(rules))
    matched = False
    if texture2d:
        for rule in rules:
            if width != rule["w"] or height != rule["h"]:
                continue
            if rule["kind"] and (rule["kind"] != draw["kind"] or
                                  rule["count"] != draw["count"]):
                continue
            matched = True
            break
    expected_event = ({"id": 24, "kind": 2, "outcome": 4, "flow": 1,
                       "subsite": 0, "verdict": 2}
                      if matched else
                      {"id": 24, "kind": 2, "outcome": 2, "flow": 0,
                       "subsite": 0, "verdict": -1})
    delta_known = fact.get("censusSkippedDeltaKnown")
    if type(delta_known) is not bool:
        raise TraceError(label + ".censusSkippedDeltaKnown is invalid")
    delta = _integer(fact.get("censusSkippedDelta"), label + ".censusSkippedDelta", 0, 1)
    if not delta_known and delta != 0:
        raise TraceError(label + " unknown census mutation carries a value")
    return expected_event, (1 if matched else 0), delta_known, delta, None


def _replay_offscreen_quad_fact(fact, draw, label):
    required = {"siteId", "kind", "known", "offscreenRuleCount", "offscreenRules",
                "quadArmed", "offscreenEyeDrawsLastFrame",
                "offscreenProbeReached", "offscreenProbeResolved",
                "offscreenProbeTexture2D", "offscreenTargetW", "offscreenTargetH",
                "censusSkippedDeltaKnown", "censusSkippedDelta"}
    if set(fact) != required:
        raise TraceError(label + " has missing or unexpected offscreen-quad fields")
    if fact["known"] != "yes" or fact.get("quadArmed") not in ("yes", "no"):
        raise TraceError(label + " lacks its quad configuration inputs")
    if (fact["censusSkippedDeltaKnown"] is not False or
            _integer(fact["censusSkippedDelta"], label + ".censusSkippedDelta") != 0):
        raise TraceError(label + " quad fact carries census-only mutation")
    armed = fact["quadArmed"] == "yes"
    rules = _offscreen_rules(fact, label, 1,
                             allow_zero_dimensions=not armed,
                             quad_armed=armed)
    rule = rules[0]
    eye_draws = _integer(fact.get("offscreenEyeDrawsLastFrame"),
                         label + ".offscreenEyeDrawsLastFrame")
    prefix = (fact["quadArmed"] == "yes" and eye_draws < 100 and
              draw["kind"] == rule["kind"] and draw["count"] == rule["count"])
    texture2d, width, height = _offscreen_probe(fact, label, prefix)
    claimed = prefix and texture2d and width == rule["w"] and height == rule["h"]
    expected_event = ({"id": 26, "kind": 2, "outcome": 3, "flow": 1,
                       "subsite": 0, "verdict": 15}
                      if claimed else
                      {"id": 26, "kind": 2, "outcome": 2, "flow": 0,
                       "subsite": 0, "verdict": -1})
    return expected_event, None, True, 0, None


def _tri(fact, key, label):
    value = fact.get(key)
    if value not in TRI_STATES:
        raise TraceError(label + "." + key + " is an invalid tri-state")
    return value


def _resource_observation(fact, label, reached, expected_match):
    """Validate one lazy resource stage and independently evaluate its raw descriptor."""
    required = {"source", "resolveReached", "resolved", "rawAvailable",
                "texture2D", "a", "b", "fmt"}
    if not isinstance(fact, dict) or set(fact) != required:
        raise TraceError(label + " has missing or unexpected resource fields")
    source = _integer(fact.get("source"), label + ".source", 0, 4)
    resolve_reached = _tri(fact, "resolveReached", label)
    resolved = _tri(fact, "resolved", label)
    raw_available = _tri(fact, "rawAvailable", label)
    texture = _tri(fact, "texture2D", label)
    a = _integer(fact.get("a"), label + ".a")
    b = _integer(fact.get("b"), label + ".b")
    fmt = _integer(fact.get("fmt"), label + ".fmt")
    if not reached:
        if (source != 0 or resolve_reached != "unknown" or resolved != "unknown" or
                raw_available != "unknown" or texture != "unknown" or a or b or fmt):
            raise TraceError(label + " skipped resource stage carries inputs or outputs")
        return "not-reached"
    if source == 0:
        raise TraceError(label + " reached resource stage is marked not reached")
    if source in (1, 2):
        if resolve_reached != "yes":
            raise TraceError(label + " fresh resolver stage lacks its attempted call")
        if source == 1:
            if resolved != "yes" or raw_available != "yes" or texture not in ("yes", "no"):
                raise TraceError(label + " successful resolve lacks raw descriptor availability")
        elif (resolved != "no" or raw_available != "no" or texture != "no" or
              a or b or fmt):
            raise TraceError(label + " failed resolve carries descriptor data")
    elif source == 3:
        if (resolve_reached != "no" or resolved != "yes" or raw_available != "yes" or
                texture not in ("yes", "no")):
            raise TraceError(label + " raw-shadow hit has inconsistent availability")
    else:
        if (resolve_reached != "no" or resolved != "unknown" or
                raw_available != "unknown" or texture != "unknown" or a or b or fmt):
            raise TraceError(label + " warm cache without raw shadow exposes cached output")
        return None
    if texture == "yes":
        return expected_match(a, b, fmt)
    return False


def _holo_eye_size(fact, depth_w, depth_h, label):
    required = {"reached", "statePresent", "result", "readMask", "depthW", "depthH",
                "eyeW", "eyeH", "renderW", "renderH"}
    if not isinstance(fact, dict) or set(fact) != required:
        raise TraceError(label + " has missing or unexpected eye-size fields")
    reached = _tri(fact, "reached", label)
    state = _tri(fact, "statePresent", label)
    result = _tri(fact, "result", label)
    mask = _integer(fact.get("readMask"), label + ".readMask", 0, 63)
    values = {name: _integer(fact.get(name), label + "." + name)
              for name in ("depthW", "depthH", "eyeW", "eyeH", "renderW", "renderH")}
    if reached == "unknown":
        if state != "unknown" or result != "unknown" or mask or any(values.values()):
            raise TraceError(label + " skipped eye-size test carries inputs or outputs")
        return None, False
    if reached != "yes":
        raise TraceError(label + " reached must be yes or unknown")
    if state == "unknown" or result == "unknown":
        raise TraceError(label + " reached eye-size test lacks observed availability")
    if state == "no":
        expected_mask = 0
        expected_result = "no"
    else:
        expected_mask = 1
        expected_result = "no"
        w = depth_w
        h = depth_h
        if w:
            expected_mask |= 2
            if h:
                expected_mask |= 4
                eye_w = values["eyeW"]
                if eye_w and abs(w - eye_w) <= 2:
                    expected_mask |= 8
                    if abs(h - values["eyeH"]) <= 2:
                        expected_result = "yes"
                if expected_result != "yes":
                    expected_mask |= 16
                    render_w = values["renderW"]
                    if render_w and abs(w - render_w) <= 2:
                        expected_mask |= 32
                        if abs(h - values["renderH"]) <= 2:
                            expected_result = "yes"
    if ((mask & 1 and values["depthW"] != depth_w) or
            (mask & 2 and values["depthH"] != depth_h)):
        raise TraceError(label + " depth dimensions differ from the reached resource")
    consumed_bits = (1, 2, 4, 8, 16, 32)
    consumed_names = ("depthW", "depthH", "eyeW", "eyeH", "renderW", "renderH")
    for bit, name in zip(consumed_bits, consumed_names):
        if not (expected_mask & bit) and values[name] != 0:
            raise TraceError(label + " has an unconsumed dimension input")
    if mask != expected_mask:
        raise TraceError(label + " dimension read mask disagrees with short-circuit order")
    return expected_result == "yes", result != expected_result


def _replay_holo_fact(fact, draw, label):
    required = {"siteId", "kind", "known", "gates", "pattern", "depth", "eyeSize",
                "predicateResult", "missedBefore", "missedAfter", "missNotedBefore",
                "missNotedAfter", "missedDeltaKnown", "missedDelta"}
    if set(fact) != required or fact.get("known") != "yes":
        raise TraceError(label + " has missing Holo fields or unavailable input facts")
    gates = fact.get("gates")
    gate_fields = {"enabled", "shapeReached", "shapeMatched", "helperReached",
                   "helperEnabled", "helperShapeReached", "helperShapeMatched"}
    if not isinstance(gates, dict) or set(gates) != gate_fields:
        raise TraceError(label + ".gates has missing or unexpected fields")
    g = {key: _tri(gates, key, label + ".gates") for key in gate_fields}
    if g["enabled"] == "unknown":
        raise TraceError(label + " outer Holo gate is unavailable")
    outer_shape = draw["kind"] == ord("X") and draw["count"] == 6 and draw["instances"] == 1
    if g["shapeReached"] != ("yes" if g["enabled"] == "yes" else "unknown"):
        raise TraceError(label + " outer shape reachability disagrees with enabled gate")
    if g["shapeReached"] == "yes" and g["shapeMatched"] != ("yes" if outer_shape else "no"):
        raise TraceError(label + " outer shape result disagrees with draw inputs")
    if g["shapeReached"] == "unknown" and g["shapeMatched"] != "unknown":
        raise TraceError(label + " skipped outer shape carries a result")
    helper_reached = g["enabled"] == "yes" and outer_shape
    if g["helperReached"] != ("yes" if helper_reached else "unknown"):
        raise TraceError(label + " helper reachability disagrees with outer short circuit")
    if not helper_reached:
        if any(g[k] != "unknown" for k in ("helperEnabled", "helperShapeReached", "helperShapeMatched")):
            raise TraceError(label + " skipped helper carries later gate values")
    else:
        if g["helperEnabled"] == "unknown":
            raise TraceError(label + " reached helper lacks its internal enabled state")
        helper_shape_reached = g["helperEnabled"] == "yes"
        if g["helperShapeReached"] != ("yes" if helper_shape_reached else "unknown"):
            raise TraceError(label + " helper shape reachability disagrees with its gate")
        if helper_shape_reached and g["helperShapeMatched"] != ("yes" if outer_shape else "no"):
            raise TraceError(label + " helper shape result disagrees with draw inputs")
        if not helper_shape_reached and g["helperShapeMatched"] != "unknown":
            raise TraceError(label + " skipped helper shape carries a result")
    should_pattern = helper_reached and g["helperEnabled"] == "yes" and outer_shape
    if should_pattern and fact.get("pattern", {}).get("source") not in (1, 2):
        raise TraceError(label + ".pattern must use a fresh resolver observation")
    pattern_result = _resource_observation(
        fact.get("pattern"), label + ".pattern", should_pattern,
        lambda a, b, fmt: a == 256 and b == 256 and fmt == 70)
    pattern_match = pattern_result is True
    should_depth = should_pattern and pattern_match
    if should_depth and fact.get("depth", {}).get("source") not in (1, 2):
        raise TraceError(label + ".depth must use a fresh resolver observation")
    depth_result = _resource_observation(
        fact.get("depth"), label + ".depth", should_depth, lambda a, b, fmt: True)
    depth_texture = depth_result is True
    eye_reached = should_depth and depth_texture
    if eye_reached:
        eye_result, eye_mismatch = _holo_eye_size(
            fact.get("eyeSize"), fact["depth"].get("a"), fact["depth"].get("b"),
            label + ".eyeSize")
        if eye_result is None:
            raise TraceError(label + " reached eye-size test lacks its input observation")
    else:
        eye_result, eye_mismatch = _holo_eye_size(
            fact.get("eyeSize"), 0, 0, label + ".eyeSize")
        if eye_result is not None:
            raise TraceError(label + " eye-size test was recorded before depth Texture2D passed")
    replayable = (pattern_result != None and
                  (not should_depth or depth_result != None))
    if not replayable:
        expected_claim = None
    else:
        expected_claim = bool(should_depth and depth_texture and eye_result)
    pred = _tri(fact, "predicateResult", label)
    if pred == "unknown":
        raise TraceError(label + " lacks observed Holo result")
    observation_mismatches = int(eye_mismatch)
    if expected_claim is not None and pred != ("yes" if expected_claim else "no"):
        observation_mismatches += 1

    before = _integer(fact.get("missedBefore"), label + ".missedBefore", 0, (1 << 64) - 1)
    after = _integer(fact.get("missedAfter"), label + ".missedAfter", 0, (1 << 64) - 1)
    noted_before = _tri(fact, "missNotedBefore", label)
    noted_after = _tri(fact, "missNotedAfter", label)
    delta_known = fact.get("missedDeltaKnown")
    if type(delta_known) is not bool:
        raise TraceError(label + ".missedDeltaKnown must be boolean")
    delta = _integer(fact.get("missedDelta"), label + ".missedDelta", 0, 1)
    should_increment = (expected_claim is False and should_depth and depth_texture and
                        eye_result is False and noted_before == "no")
    if noted_before not in ("yes", "no") or not delta_known:
        raise TraceError(label + " lacks complete Holo mutation snapshots")
    expected_delta = 1 if should_increment else 0
    expected_after = (before + expected_delta) & ((1 << 64) - 1)
    expected_noted_after = ("yes" if noted_before == "yes" or
                            (should_increment and expected_after >= 60) else "no")
    if (delta != expected_delta or after != expected_after or
            noted_after != expected_noted_after):
        observation_mismatches += 1
    expected_event = None if expected_claim is None else (
        {"id": 53, "kind": 2, "outcome": 3, "flow": 1,
         "subsite": 0, "verdict": 4} if expected_claim else
        {"id": 53, "kind": 2, "outcome": 2, "flow": 0,
         "subsite": 0, "verdict": -1})
    return expected_event, None, True, 0, observation_mismatches, expected_claim


def _replay_scrim_fact(fact, draw, label):
    required = {"siteId", "kind", "known", "gates", "wash", "ui", "predicateResult"}
    if set(fact) != required or fact.get("known") != "yes":
        raise TraceError(label + " has missing Scrim fields or unavailable input facts")
    gates = fact.get("gates")
    gate_fields = {"enabled", "shapeReached", "shapeMatched", "helperReached",
                   "helperEnabled", "helperShapeReached", "helperShapeMatched"}
    if not isinstance(gates, dict) or set(gates) != gate_fields:
        raise TraceError(label + ".gates has missing or unexpected fields")
    g = {key: _tri(gates, key, label + ".gates") for key in gate_fields}
    if g["enabled"] == "unknown":
        raise TraceError(label + " outer Scrim gate is unavailable")
    outer_shape = draw["kind"] == ord("X") and draw["instances"] == 1 and draw["count"] >= 100
    if g["shapeReached"] != ("yes" if g["enabled"] == "yes" else "unknown"):
        raise TraceError(label + " outer shape reachability disagrees with enabled gate")
    if g["shapeReached"] == "yes" and g["shapeMatched"] != ("yes" if outer_shape else "no"):
        raise TraceError(label + " outer shape result disagrees with draw inputs")
    if g["shapeReached"] == "unknown" and g["shapeMatched"] != "unknown":
        raise TraceError(label + " skipped outer shape carries a result")
    helper_reached = g["enabled"] == "yes" and outer_shape
    if g["helperReached"] != ("yes" if helper_reached else "unknown"):
        raise TraceError(label + " helper reachability disagrees with outer short circuit")
    if not helper_reached:
        if any(g[k] != "unknown" for k in ("helperEnabled", "helperShapeReached", "helperShapeMatched")):
            raise TraceError(label + " skipped helper carries later gate values")
    else:
        if g["helperEnabled"] == "unknown":
            raise TraceError(label + " reached helper lacks its internal enabled state")
        helper_shape_reached = g["helperEnabled"] == "yes"
        if g["helperShapeReached"] != ("yes" if helper_shape_reached else "unknown"):
            raise TraceError(label + " helper shape reachability disagrees with its gate")
        if helper_shape_reached and g["helperShapeMatched"] != ("yes" if outer_shape else "no"):
            raise TraceError(label + " helper shape result disagrees with draw inputs")
        if not helper_shape_reached and g["helperShapeMatched"] != "unknown":
            raise TraceError(label + " skipped helper shape carries a result")
    should_wash = helper_reached and g["helperEnabled"] == "yes" and outer_shape
    wash = _resource_observation(fact.get("wash"), label + ".wash", should_wash,
                                 lambda a, b, fmt: a == 16 and b == 16 and
                                 fmt in (70, 71, 72))
    should_ui = should_wash and wash is True
    if wash is None:
        # A warm cache can have matched without publishing raw wash inputs.
        # Validate the observed UI stage, but never infer a wash predicate
        # from its reachability or promote this path to replayable.
        should_ui = fact.get("ui", {}).get("source") != 0
    ui = _resource_observation(fact.get("ui"), label + ".ui", should_ui,
                               lambda a, b, fmt: a >= 1024)
    replayable = wash != None and (not should_ui or ui != None)
    expected_claim = None if not replayable else bool(should_ui and ui is True)
    pred = _tri(fact, "predicateResult", label)
    if pred == "unknown":
        raise TraceError(label + " lacks observed Scrim result")
    observation_mismatches = int(expected_claim is not None and
                                 pred != ("yes" if expected_claim else "no"))
    expected_event = None if expected_claim is None else (
        {"id": 55, "kind": 2, "outcome": 3, "flow": 1,
         "subsite": 0, "verdict": 17} if expected_claim else
        {"id": 55, "kind": 2, "outcome": 2, "flow": 0,
         "subsite": 0, "verdict": -1})
    return expected_event, None, True, 0, observation_mismatches, expected_claim


def _fss_dump_event(claimed):
    return ({"id": 59, "kind": 2, "outcome": 3, "flow": 1,
             "subsite": 0, "verdict": 13} if claimed else
            {"id": 59, "kind": 2, "outcome": 2, "flow": 0,
             "subsite": 0, "verdict": -1})


def _replay_fss_dump_fact(fact, draw, label, fss_dump_interested):
    """Replay site59 from lazy raw reads and the selector's post-guard hash."""
    if set(fact) != {"siteId", "kind", "known", "handlerInvoked", "outer", "helper"}:
        raise TraceError(label + " has missing or unexpected FSS-dump fields")
    if (fact.get("siteId"), fact.get("kind"), fact.get("known")) != (59, 20, "yes"):
        raise TraceError(label + " has mismatched FSS-dump site/kind/availability")
    if type(fact.get("handlerInvoked")) is not bool:
        raise TraceError(label + ".handlerInvoked must be a boolean")
    u32 = 0xffffffff
    outer_types = {
        "wants": {"frame": (int, u32), "done": (bool, None),
                  "seriesWant": (int, u32), "seriesDone": (bool, None)},
        "bodyFrame": (int, u32), "frameNo": (int, u32),
    }
    helper_types = {
        "wants": outer_types["wants"],
        "contextNonNull": (bool, None), "guardCallReached": (bool, None),
        "callbackEntered": (bool, None), "vsGetShaderReached": (bool, None),
        "vsGetShaderCompleted": (bool, None), "shaderNonNull": (bool, None),
        "lookupReached": (bool, None), "lookupCompleted": (bool, None),
        "lookupHash": (int, 0xffffffffffffffff),
        "releaseReached": (bool, None), "releaseCompleted": (bool, None),
        "callbackCompleted": (bool, None), "guardReturned": (bool, None),
        "hashAfterGuard": (int, 0xffffffffffffffff), "dumping": (bool, None),
        "counters": {
            "ringBefore": (int, 255), "ringAfter": (int, 255),
            "compositeBefore": (int, 255), "compositeAfter": (int, 255),
            "tonemapBefore": (int, 255), "tonemapAfter": (int, 255),
        },
        "pendingKindBefore": (int, u32), "pendingKindAfter": (int, u32),
        "pendingEyeBefore": (int, u32), "pendingEyeAfter": (int, u32),
    }

    def parse_read(item, path, spec):
        if not isinstance(item, dict) or set(item) != {"reached", "known", "value"}:
            raise TraceError(path + " is malformed")
        reached, known, value = item["reached"], item["known"], item["value"]
        if type(reached) is not bool or type(known) is not bool:
            raise TraceError(path + " availability must be boolean")
        if known and not reached:
            raise TraceError(path + " cannot be known before it is reached")
        if not known:
            if value is not None:
                raise TraceError(path + " unknown value must be null")
            return (reached, False, None)
        typ, high = spec
        if typ is bool:
            if type(value) is not bool:
                raise TraceError(path + ".value must be a boolean")
        else:
            _integer(value, path + ".value", 0, high)
        return (reached, True, value)

    def parse_group(obj, specs, path):
        if not isinstance(obj, dict) or set(obj) != set(specs):
            raise TraceError(path + " has missing or unexpected fields")
        parsed = {}
        for name, spec in specs.items():
            field_path = path + "." + name
            if isinstance(spec, dict):
                parsed[name] = parse_group(obj[name], spec, field_path)
            else:
                parsed[name] = parse_read(obj[name], field_path, spec)
        return parsed

    outer = parse_group(fact.get("outer"), outer_types, label + ".outer")
    helper = parse_group(fact.get("helper"), helper_types, label + ".helper")
    if fss_dump_interested is None:
        raise _FssDumpUnavailable()
    if fact["handlerInvoked"] != fss_dump_interested:
        raise TraceError(label + " handler marker disagrees with site6 raw interest mask")

    def value(group, name, mutation_required=False):
        read = group[name]
        if not read[0]:
            raise TraceError(label + "." + name + " was not reached when consumed")
        if not read[1]:
            raise _FssDumpUnavailable(mutation_required)
        return read[2]

    def unread(group, names, path):
        for name in names:
            read = group[name]
            if isinstance(read, dict):
                for nested_name, nested_read in read.items():
                    if nested_read[0] or nested_read[1] or nested_read[2] is not None:
                        raise TraceError(path + "." + name + "." + nested_name +
                                         " was not consumed by the short-circuit path")
                continue
            if read[0] or read[1] or read[2] is not None:
                raise TraceError(path + "." + name + " was not consumed by the short-circuit path")

    def wants(group, path):
        frame = value(group, "frame")
        if frame != 0:
            done = value(group, "done")
        else:
            unread(group, ("done",), path)
            done = None
        left = frame != 0 and not done
        if left:
            unread(group, ("seriesWant", "seriesDone"), path)
            return True
        series_want = value(group, "seriesWant")
        if series_want != 0:
            series_done = value(group, "seriesDone")
        else:
            unread(group, ("seriesDone",), path)
            series_done = None
        return series_want != 0 and not series_done

    def check_unread_all(group, specs, path):
        for name, spec in specs.items():
            if isinstance(spec, dict):
                check_unread_all(group[name], spec, path + "." + name)
            elif group[name][0] or group[name][1] or group[name][2] is not None:
                raise TraceError(path + "." + name + " is unexpectedly reached")

    # The interest marker is consistency-only. The source of admission is the
    # earlier raw site6 interest mask, and unentered site59 bodies stay empty.
    if not fss_dump_interested:
        if fact["handlerInvoked"]:
            raise TraceError(label + " unselected FSS dump handler was marked invoked")
        check_unread_all(outer, outer_types, label + ".outer")
        check_unread_all(helper, helper_types, label + ".helper")
        return {"id": 59, "kind": 2, "outcome": 5, "flow": 0,
                "subsite": 0, "verdict": -1}, 0, False
    if not fact["handlerInvoked"]:
        raise TraceError(label + " selected FSS dump handler was not marked invoked")

    outer_wants = wants(outer["wants"], label + ".outer.wants")
    if not outer_wants:
        unread(outer, ("bodyFrame", "frameNo"), label + ".outer")
        check_unread_all(helper, helper_types, label + ".helper")
        return _fss_dump_event(False), 0, False
    body_frame = value(outer, "bodyFrame")
    if body_frame == 0:
        unread(outer, ("frameNo",), label + ".outer")
        check_unread_all(helper, helper_types, label + ".helper")
        return _fss_dump_event(False), 0, False
    frame_no = value(outer, "frameNo")
    if ((frame_no - body_frame) & u32) > 2:
        check_unread_all(helper, helper_types, label + ".helper")
        return _fss_dump_event(False), 0, False

    helper_wants = wants(helper["wants"], label + ".helper.wants")
    if not helper_wants:
        unread(helper, ("contextNonNull", "guardCallReached", "callbackEntered",
                        "vsGetShaderReached", "vsGetShaderCompleted", "shaderNonNull",
                        "lookupReached", "lookupCompleted", "lookupHash", "releaseReached",
                        "releaseCompleted", "callbackCompleted", "guardReturned",
                        "hashAfterGuard", "dumping", "counters", "pendingKindBefore",
                        "pendingKindAfter", "pendingEyeBefore", "pendingEyeAfter"), label + ".helper")
        return _fss_dump_event(False), 0, False
    context = value(helper, "contextNonNull")
    if not context:
        unread(helper, ("guardCallReached", "callbackEntered", "vsGetShaderReached",
                        "vsGetShaderCompleted", "shaderNonNull", "lookupReached",
                        "lookupCompleted", "lookupHash", "releaseReached", "releaseCompleted",
                        "callbackCompleted", "guardReturned", "hashAfterGuard", "dumping",
                        "counters", "pendingKindBefore", "pendingKindAfter", "pendingEyeBefore",
                        "pendingEyeAfter"), label + ".helper")
        return _fss_dump_event(False), 0, False
    shape_ok = (draw["kind"] == ord("N") and draw["instances"] == 1 and
                draw["count"] in (3, 4, 6))
    if not shape_ok:
        unread(helper, ("guardCallReached", "callbackEntered", "vsGetShaderReached",
                        "vsGetShaderCompleted", "shaderNonNull", "lookupReached",
                        "lookupCompleted", "lookupHash", "releaseReached", "releaseCompleted",
                        "callbackCompleted", "guardReturned", "hashAfterGuard", "dumping",
                        "counters", "pendingKindBefore", "pendingKindAfter", "pendingEyeBefore",
                        "pendingEyeAfter"), label + ".helper")
        return _fss_dump_event(False), 0, False

    if value(helper, "guardCallReached") is not True:
        raise TraceError(label + " guard call marker disagrees with consumed helper gates")
    entered = value(helper, "callbackEntered")
    lookup_hash = None
    lookup_completed = False
    expected_callback_completed = False
    if not entered:
        unread(helper, ("vsGetShaderReached", "vsGetShaderCompleted", "shaderNonNull",
                        "lookupReached", "lookupCompleted", "lookupHash", "releaseReached",
                        "releaseCompleted", "callbackCompleted"), label + ".helper")
    else:
        if value(helper, "vsGetShaderReached") is not True:
            raise TraceError(label + " entered callback without reaching VSGetShader")
        vs_completed = value(helper, "vsGetShaderCompleted")
        if not vs_completed:
            unread(helper, ("shaderNonNull", "lookupReached", "lookupCompleted", "lookupHash",
                            "releaseReached", "releaseCompleted"), label + ".helper")
        else:
            shader_nonnull = value(helper, "shaderNonNull")
            if value(helper, "lookupReached") is not True:
                raise TraceError(label + " completed getter did not reach hash lookup")
            lookup_completed = value(helper, "lookupCompleted")
            if lookup_completed:
                lookup_hash = value(helper, "lookupHash")
                if not shader_nonnull:
                    if lookup_hash != 0:
                        raise TraceError(label + " null shader lookup must return zero")
                    expected_callback_completed = True
                    unread(helper, ("releaseReached", "releaseCompleted"), label + ".helper")
                else:
                    if value(helper, "releaseReached") is not True:
                        raise TraceError(label + " completed lookup did not reach Release")
                    release_completed = value(helper, "releaseCompleted")
                    expected_callback_completed = release_completed
            else:
                unread(helper, ("lookupHash", "releaseReached", "releaseCompleted"),
                       label + ".helper")
        callback_completed = value(helper, "callbackCompleted")
        if callback_completed != expected_callback_completed:
            raise TraceError(label + " callback completion disagrees with nested query completion")
    guard_returned = value(helper, "guardReturned")
    if guard_returned != bool(entered and callback_completed):
        raise TraceError(label + " guard result disagrees with callback completion")
    hash_after = value(helper, "hashAfterGuard")
    expected_hash = lookup_hash if lookup_completed else 0
    if hash_after != expected_hash:
        raise TraceError(label + " consumed post-guard hash disagrees with local assignment provenance")

    family = None
    if hash_after == 0x7E38A6AA1269C901 and draw["count"] == 4:
        family = ("ring", 0, "ring")
    elif hash_after == 0x953C8123AD8DC13B and draw["count"] == 6:
        family = ("composite", 1, "composite")
    elif hash_after == 0x2D78DC3FD2C0C543 and draw["count"] == 3:
        family = ("tonemap", 2, "tonemap")
    if family is None:
        unread(helper, ("dumping", "counters", "pendingKindBefore", "pendingKindAfter",
                        "pendingEyeBefore", "pendingEyeAfter"), label + ".helper")
        return _fss_dump_event(False), 0, False

    counter_key = family[0]
    counters = helper["counters"]
    selected = counter_key
    before_name, after_name = selected + "Before", selected + "After"
    before = value(counters, before_name, mutation_required=True)
    after = value(counters, after_name, mutation_required=True)
    expected_after = (before + 1) & 0xff
    if after != expected_after:
        raise TraceError(label + " selected category counter is not one uint8 increment")
    other_names = [name for name in counters if name not in (before_name, after_name)]
    unread(counters, other_names, label + ".helper.counters")
    occurrence = after
    if occurrence > 2:
        unread(helper, ("dumping", "pendingKindBefore", "pendingKindAfter",
                        "pendingEyeBefore", "pendingEyeAfter"), label + ".helper")
        return _fss_dump_event(False), 0, False
    dumping = value(helper, "dumping")
    if not dumping:
        unread(helper, ("pendingKindBefore", "pendingKindAfter", "pendingEyeBefore",
                        "pendingEyeAfter"), label + ".helper")
        return _fss_dump_event(False), 0, False

    kind_before = value(helper, "pendingKindBefore", mutation_required=True)
    kind_after = value(helper, "pendingKindAfter", mutation_required=True)
    eye_before = value(helper, "pendingEyeBefore", mutation_required=True)
    eye_after = value(helper, "pendingEyeAfter", mutation_required=True)
    expected_eye = (occurrence - 1) & u32
    if kind_after != family[1] or eye_after != expected_eye:
        raise TraceError(label + " pending family/eye writes disagree with the derived category")
    if kind_before == kind_after and eye_before == eye_after:
        # Idempotent writes still count as a claim; values are source write
        # checks and never select the result.
        pass
    return _fss_dump_event(True), 0, False


def _target_sharp_fact(fact, draw, label, admitted):
    required = {"siteId", "kind", "known", "handlerInvoked", "inputs", "eyeSize"}
    if set(fact) != required or (fact.get("siteId"), fact.get("kind"), fact.get("known")) != (54, 21, "yes"):
        raise TraceError(label + " has missing or unexpected TargetSharp fields")
    if type(fact.get("handlerInvoked")) is not bool:
        raise TraceError(label + ".handlerInvoked disagrees with reached TargetSharp handler")
    inputs = fact.get("inputs")
    names = ("outerSharp", "outerFailed", "helperSharp", "helperFailed",
             "srv0Present", "srv0Resolved", "srv0Texture2D", "srv0Width", "srv0Height",
             "aux1Present", "aux2Present", "aux3Present", "vsPresent",
             "queriedShaderHash", "configuredShaderHash")
    if not isinstance(inputs, dict) or set(inputs) != set(names):
        raise TraceError(label + ".inputs has missing or unexpected raw reads")
    domains = {name: (bool, None) for name in names}
    for name in ("srv0Width", "srv0Height"):
        domains[name] = (int, 0xffffffff)
    for name in ("queriedShaderHash", "configuredShaderHash"):
        domains[name] = (int, 0xffffffffffffffff)
    reads = {}
    for name, (typ, high) in domains.items():
        reads[name] = _sunglare_read(inputs, name, label + ".inputs", typ, high)
    eye = fact.get("eyeSize")
    if not isinstance(eye, dict):
        raise TraceError(label + ".eyeSize must be an object")

    def validate_unvisited_eye(width=0, height=0):
        sized, mismatch = _holo_eye_size(eye, width, height, label + ".eyeSize")
        if sized is not None:
            raise TraceError(label + " reached eye-size helper before its raw dimensions")
        return mismatch

    if admitted is False:
        if fact["handlerInvoked"]:
            raise TraceError(label + " was recorded although site6 interest bit0 did not admit TargetSharp")
        if any(reached or known for reached, known, _ in reads.values()):
            raise TraceError(label + " not-eligible observation contains consumed raw inputs")
        validate_unvisited_eye()
        return None, False
    if admitted is not True or not fact["handlerInvoked"]:
        raise TraceError(label + ".handlerInvoked disagrees with reached TargetSharp handler")
    if not reads["outerSharp"][0]:
        raise TraceError(label + " invoked handler lacks its first outer sharp read")

    def value(name):
        read = reads[name]
        return read[2] if read[1] else None

    def unread(*fields):
        for name in fields:
            reached, known, _ = reads[name]
            if reached or known:
                raise TraceError(label + ".inputs." + name + " violates lazy read order")

    tail = names[4:]
    after_srv0 = names[9:]
    if value("outerSharp") is None:
        unread("outerFailed", "helperSharp", "helperFailed", *tail)
        validate_unvisited_eye()
        return None, False
    if value("outerSharp") is False:
        unread("outerFailed", "helperSharp", "helperFailed", *tail)
        validate_unvisited_eye()
        return False, False
    if not reads["outerFailed"][0]:
        raise TraceError(label + " sharp outer gate lacks its lazy failed read")
    if value("outerFailed") is None:
        unread("helperSharp", "helperFailed", *tail)
        validate_unvisited_eye()
        return None, False
    if value("outerFailed"):
        unread("helperSharp", "helperFailed", *tail)
        validate_unvisited_eye()
        return False, False
    if not reads["helperSharp"][0]:
        raise TraceError(label + " admitted outer gate lacks helper sharp read")
    if value("helperSharp") is None:
        unread("helperFailed", *tail)
        validate_unvisited_eye()
        return None, False
    if not value("helperSharp"):
        unread("helperFailed", *tail)
        validate_unvisited_eye()
        return False, False
    if not reads["helperFailed"][0]:
        raise TraceError(label + " helper sharp gate lacks its lazy failed read")
    if value("helperFailed") is None:
        unread(*tail)
        validate_unvisited_eye()
        return None, False
    if value("helperFailed"):
        unread(*tail)
        validate_unvisited_eye()
        return False, False
    if (draw["kind"], draw["count"], draw["instances"]) != (ord("X"), 6, 1):
        unread(*tail)
        validate_unvisited_eye()
        return False, False

    for name in ("srv0Present", "srv0Resolved"):
        if not reads[name][0]:
            raise TraceError(label + " eligible helper lacks " + name)
        if value(name) is None:
            unread("srv0Texture2D", "srv0Width", "srv0Height", *after_srv0)
            validate_unvisited_eye()
            return None, False
    if value("srv0Present") is False and value("srv0Resolved") is True:
        raise TraceError(label + " resolves a null srv0")
    if value("srv0Resolved") is False:
        unread("srv0Texture2D", "srv0Width", "srv0Height", *after_srv0)
        validate_unvisited_eye()
        return False, False
    if not reads["srv0Texture2D"][0]:
        raise TraceError(label + " resolved srv0 lacks texture type")
    if value("srv0Texture2D") is None:
        unread("srv0Width", "srv0Height", *after_srv0)
        validate_unvisited_eye()
        return None, False
    if value("srv0Texture2D") is False:
        unread("srv0Width", "srv0Height", *after_srv0)
        validate_unvisited_eye()
        return False, False
    for name in ("srv0Width", "srv0Height"):
        if not reads[name][0]:
            raise TraceError(label + " Texture2D srv0 lacks dimensions")
        if value(name) is None:
            unread("aux1Present", "aux2Present", "aux3Present", "vsPresent",
                   "queriedShaderHash", "configuredShaderHash")
            validate_unvisited_eye()
            return None, False
    if value("srv0Width") is None or value("srv0Height") is None:
        _holo_eye_size(eye, value("srv0Width") or 0,
                       value("srv0Height") or 0, label + ".eyeSize")
        return None, False
    eye_sized, eye_mismatch = _holo_eye_size(eye, value("srv0Width"),
                                             value("srv0Height"), label + ".eyeSize")
    if eye_sized is None:
        unread("aux1Present", "aux2Present", "aux3Present", "vsPresent",
               "queriedShaderHash", "configuredShaderHash")
        return None, eye_mismatch
    if eye_sized:
        unread("aux1Present", "aux2Present", "aux3Present", "vsPresent",
               "queriedShaderHash", "configuredShaderHash")
        return False, eye_mismatch
    later_aux = (("aux2Present", "aux3Present"),
                 ("aux3Present",), ())
    for index, name in enumerate(("aux1Present", "aux2Present", "aux3Present")):
        if not reads[name][0]:
            raise TraceError(label + " non-eye srv0 lacks " + name)
        if value(name) is None:
            unread(*later_aux[index], "vsPresent", "queriedShaderHash",
                   "configuredShaderHash")
            return None, eye_mismatch
        if value(name):
            unread(*later_aux[index], "vsPresent", "queriedShaderHash",
                   "configuredShaderHash")
            return False, eye_mismatch
    if not reads["vsPresent"][0]:
        raise TraceError(label + " empty auxiliary slots lack VS presence read")
    if value("vsPresent") is None:
        unread("queriedShaderHash", "configuredShaderHash")
        return None, eye_mismatch
    if not value("vsPresent"):
        unread("queriedShaderHash", "configuredShaderHash")
        return False, eye_mismatch
    for name in ("queriedShaderHash", "configuredShaderHash"):
        if not reads[name][0]:
            raise TraceError(label + " present VS lacks consumed shader hash")
        if value(name) is None:
            return None, eye_mismatch
    return value("queriedShaderHash") == value("configuredShaderHash"), eye_mismatch


def _sunglare_nomination_fact(fact, draw, label):
    names = ("worldMode", "boundCbIdentity", "nominatedBeforeIdentity",
             "resourceResolved", "isBuffer", "byteWidth", "callbackInvoked",
             "nominatedAfterIdentity", "callbackTargetAfterIdentity")
    if (set(fact) != {"siteId", "kind", "known", "inputs"} or
            (fact.get("siteId"), fact.get("kind"), fact.get("known")) != (45, 22, "yes")):
        raise TraceError(label + " has missing or unexpected Sunglare nomination fields")
    inputs = fact["inputs"]
    if not isinstance(inputs, dict) or set(inputs) != set(names):
        raise TraceError(label + ".inputs has missing or unexpected raw reads")
    domains = {name: (bool, None) for name in names}
    domains["worldMode"] = (int, 0x7fffffff)
    domains["byteWidth"] = (int, 0xffffffff)
    for name in ("boundCbIdentity", "nominatedBeforeIdentity",
                 "nominatedAfterIdentity", "callbackTargetAfterIdentity"):
        domains[name] = (int, 12 * MAX_DRAWS)
    reads = {name: _sunglare_read(inputs, name, label + ".inputs", typ, high,
                                  -0x80000000 if name == "worldMode" else 0)
             for name, (typ, high) in domains.items()}

    def value(name):
        reached, known, val = reads[name]
        return val if reached and known else None

    def unread(*fields):
        for name in fields:
            reached, known, _ = reads[name]
            if reached or known:
                raise TraceError(label + ".inputs." + name + " violates lazy read order")

    if not reads["worldMode"][0] or not reads["callbackInvoked"][0]:
        raise TraceError(label + " lacks unconditional mode or callback-marker read")
    mode = value("worldMode")
    if draw["count"] <= 10000 or mode == 0:
        unread("boundCbIdentity", "nominatedBeforeIdentity", "resourceResolved",
               "isBuffer", "byteWidth")
    if ((reads["nominatedBeforeIdentity"][0] and not reads["boundCbIdentity"][0]) or
            (reads["resourceResolved"][0] and not reads["nominatedBeforeIdentity"][0]) or
            (reads["isBuffer"][0] and not reads["resourceResolved"][0]) or
            (reads["byteWidth"][0] and not reads["isBuffer"][0])):
        raise TraceError(label + " has a source read without its immediate lazy predecessor")
    bound_value = value("boundCbIdentity")
    before_value = value("nominatedBeforeIdentity")
    resolved_value = value("resourceResolved")
    buffer_value = value("isBuffer")
    if ((bound_value == 0 and reads["nominatedBeforeIdentity"][0]) or
            (bound_value is not None and before_value is not None and bound_value == before_value and
             any(reads[name][0] for name in ("resourceResolved", "isBuffer", "byteWidth"))) or
            (resolved_value is False and any(reads[name][0] for name in ("isBuffer", "byteWidth"))) or
            (buffer_value is False and reads["byteWidth"][0])):
        raise TraceError(label + " has a source read after a known short circuit")
    eligible = mode is not None and mode != 0 and draw["count"] > 10000
    suffix = names[1:6]
    if mode is False or (mode is not None and mode == 0) or draw["count"] <= 10000:
        unread(*suffix)
    elif mode is not None and mode != 0 and not reads["boundCbIdentity"][0]:
        raise TraceError(label + " nomination candidate lacks bound-CB identity")
    elif reads["boundCbIdentity"][0]:
        bound = value("boundCbIdentity")
        if bound == 0:
            unread("nominatedBeforeIdentity", "resourceResolved", "isBuffer", "byteWidth")
        elif bound is not None and not reads["nominatedBeforeIdentity"][0]:
            raise TraceError(label + " non-null binding lacks prior nomination identity")
        elif value("nominatedBeforeIdentity") == bound and bound is not None:
            unread("resourceResolved", "isBuffer", "byteWidth")
        elif value("nominatedBeforeIdentity") is not None and not reads["resourceResolved"][0]:
            raise TraceError(label + " changed binding lacks resource-resolution result")
        if bound is None and any(reads[name][0] for name in
                                 ("resourceResolved", "isBuffer", "byteWidth")) and not reads["nominatedBeforeIdentity"][0]:
            raise TraceError(label + " unknown binding continued without prior nomination read")
        if value("resourceResolved") is False:
            unread("isBuffer", "byteWidth")
        elif value("resourceResolved") is True and not reads["isBuffer"][0]:
            raise TraceError(label + " resolved resource lacks buffer classification")
        if value("isBuffer") is False:
            unread("byteWidth")
        elif value("isBuffer") is True and not reads["byteWidth"][0]:
            raise TraceError(label + " buffer resource lacks byte width")

    unavailable = (not reads["callbackInvoked"][1] or any(
        reads[n][0] and not reads[n][1] for n in
        ("worldMode", "boundCbIdentity", "nominatedBeforeIdentity",
         "resourceResolved", "isBuffer", "byteWidth")))

    # An unknown reached value can decide whether the selector proceeds. Keep
    # the observation unavailable instead of treating its zero placeholder as false.
    callback = False
    if mode is not None and mode != 0 and draw["count"] > 10000:
        bound = value("boundCbIdentity")
        before = value("nominatedBeforeIdentity")
        resolved = value("resourceResolved")
        is_buffer = value("isBuffer")
        width = value("byteWidth")
        callback = bool(bound and before is not None and bound != before and
                        resolved is True and is_buffer is True and width == 208)
    marker = value("callbackInvoked")
    mismatches = 0
    if marker is not None and not unavailable and marker != callback:
        mismatches += 1
    after = value("nominatedAfterIdentity")
    target = value("callbackTargetAfterIdentity")
    after_observed = (marker is True or
        (marker is None and (reads["nominatedAfterIdentity"][0] or
                             reads["callbackTargetAfterIdentity"][0])))
    if callback or after_observed:
        if not reads["nominatedAfterIdentity"][0] or not reads["callbackTargetAfterIdentity"][0]:
            raise TraceError(label + " invoked callback lacks post-mutation identities")
        if after is None or target is None:
            unavailable = True
        else:
            bound = value("boundCbIdentity")
            if callback and bound is not None and (after != bound or target != bound):
                mismatches += 1
    else:
        unread("nominatedAfterIdentity", "callbackTargetAfterIdentity")
    expected_event = {"id": 45, "kind": 1, "outcome": 1, "flow": 0,
                      "subsite": 0, "verdict": -1}
    return expected_event, mismatches, unavailable


_FSS_CHROME_FIELDS = {
        "siteId", "kind", "known", "outerHeal", "outerCensus", "outerTemporal",
    "kindReached", "drawKind", "kindMatched", "countReached", "drawCount",
    "countMatched", "budgetEntered", "budgetResult", "hashReached", "vsHash",
    "hashMatched", "srvReached", "srvNonNull", "resourceReached",
    "resourceNonNull", "queryReached", "texture2D", "dimensionsReached",
    "width", "height", "chromeMatched", "healForSkip", "latchReached",
    "latchOn", "frameReached", "frameNo", "priorFrameNo", "ordinalCountBefore",
    "frameChanged", "ordinal", "ordinalCountAfter", "helperReached",
    "startInstance", "baseVertex", "maskReached", "skipMask", "ordinalInRange",
    "maskBit", "terminalSkip",
}
_FSS_CHROME_TRI_FIELDS = _FSS_CHROME_FIELDS - {
    "siteId", "kind", "known", "drawKind", "drawCount", "vsHash", "width",
    "height", "frameNo", "priorFrameNo", "ordinalCountBefore", "ordinal",
    "ordinalCountAfter", "startInstance", "baseVertex", "skipMask",
}


def _legacy14a_fss_chrome_skip(fact, draw, label):
    """Frozen site-1 selector; it consumes only the serialized site-1 inputs."""
    if set(fact) != _FSS_CHROME_FIELDS:
        raise TraceError(label + " has missing or unexpected FSS chrome fields")
    if fact["known"] != "yes" or any(fact.get(key) not in TRI_STATES
                                      for key in _FSS_CHROME_TRI_FIELDS):
        raise TraceError(label + " has invalid FSS chrome availability")
    integer_ranges = {
        "siteId": (1, 1), "kind": (23, 23), "drawKind": (0, 255),
        "drawCount": (0, 0xffffffff), "width": (0, 0xffffffff),
        "height": (0, 0xffffffff), "frameNo": (0, 0xffffffff),
        "priorFrameNo": (0, 0xffffffff), "ordinalCountBefore": (0, 0xffffffff),
        "ordinal": (0, 0xffffffff), "ordinalCountAfter": (0, 0xffffffff),
        "startInstance": (0, 0xffffffff), "baseVertex": (-0x80000000, 0x7fffffff),
        "skipMask": (0, 0xffffffff),
    }
    for key, bounds in integer_ranges.items():
        _integer(fact.get(key), label + "." + key, *bounds)
    if not isinstance(fact.get("vsHash"), str) or not re.match(
            r"^[0-9A-Fa-f]{16}$", fact["vsHash"]):
        raise TraceError(label + ".vsHash must be sixteen hexadecimal digits")

    def require(condition, message):
        if not condition:
            raise TraceError(label + " " + message)

    def unknown(*keys):
        for key in keys:
            require(fact[key] == "unknown", key + " must be unknown before its branch")

    def zero(*keys):
        for key in keys:
            expected = "0000000000000000" if key == "vsHash" else 0
            require(fact[key] == expected, key + " must be zero when unavailable")

    # Preserve the source's short-circuit order: heal, census, temporal.
    require(fact["outerHeal"] != "unknown", "lacks the first outer gate")
    if fact["outerHeal"] == "yes":
        unknown("outerCensus", "outerTemporal")
        outer = True
    else:
        require(fact["outerCensus"] != "unknown", "lacks the reached census gate")
        if fact["outerCensus"] == "yes":
            unknown("outerTemporal")
            outer = True
        else:
            require(fact["outerTemporal"] != "unknown", "lacks the reached temporal gate")
            outer = fact["outerTemporal"] == "yes"
    require(fact["kindReached"] == ("yes" if outer else "no"),
            "has inconsistent kind-gate reachability")
    shape = False
    if outer:
        require(fact["drawKind"] == draw["kind"], "kind input differs from draw facts")
        kind_match = fact["drawKind"] == ord("X")
        require(fact["kindMatched"] == ("yes" if kind_match else "no"),
                "kind comparison differs from frozen X gate")
        if kind_match:
            require(fact["countReached"] == "yes", "lacks reached count gate")
            require(fact["drawCount"] == draw["count"], "count input differs from draw facts")
            shape = fact["drawCount"] == 6
            require(fact["countMatched"] == ("yes" if shape else "no"),
                    "count comparison differs from frozen six-count gate")
        else:
            require(fact["countReached"] == "no", "count gate must be unreached after kind miss")
            unknown("countMatched")
            zero("drawCount")
    else:
        unknown("kindMatched", "countReached", "countMatched")
        zero("drawKind", "drawCount")
    if not shape:
        require(fact["budgetEntered"] == "no" and fact["budgetResult"] == "unknown",
                "budget must not run after an outer-shape miss")
        unknown("hashMatched")
        zero("vsHash")
        require(fact["hashReached"] == "no", "hash must be unreached after shape miss")
        require(fact["srvReached"] == fact["resourceReached"] ==
                fact["queryReached"] == fact["dimensionsReached"] == "no",
                "D3D stages must be unreached after shape miss")
        unknown("srvNonNull", "resourceNonNull", "texture2D")
        require(fact["chromeMatched"] == "no", "shape miss cannot match chrome")
    else:
        require(fact["budgetEntered"] in ("yes", "no"), "budget entry result is unavailable")
        require(fact["budgetResult"] in ("yes", "no"), "budget result is unavailable")
        if fact["budgetEntered"] == "no":
            require(fact["budgetResult"] == "no" and fact["hashReached"] == "no",
                    "declined budget must not reach the hash read")
            unknown("hashMatched")
            zero("vsHash")
            require(fact["srvReached"] == fact["resourceReached"] ==
                    fact["queryReached"] == fact["dimensionsReached"] == "no",
                    "D3D stages must be unreached when budget declines")
            unknown("srvNonNull", "resourceNonNull", "texture2D")
            require(fact["chromeMatched"] == "no", "unentered probe must not match chrome")
        else:
            require(fact["hashReached"] == "yes", "entered budget lacks hash read")
            hash_value = int(fact["vsHash"], 16)
            hash_match = hash_value in (0xA888D51024D9798E, 0xB018D143700AB803)
            require(fact["hashMatched"] == ("yes" if hash_match else "no"),
                    "VS hash comparison differs from frozen hashes")
            if not hash_match:
                require(fact["srvReached"] == fact["resourceReached"] ==
                        fact["queryReached"] == fact["dimensionsReached"] == "no",
                        "hash miss must short-circuit all D3D reads")
                unknown("srvNonNull", "resourceNonNull", "texture2D")
                require(fact["chromeMatched"] == "no", "hash miss cannot match chrome")
            else:
                require(fact["srvReached"] == "yes", "hash hit lacks SRV read")
                if fact["srvNonNull"] == "no":
                    require(fact["resourceReached"] == fact["queryReached"] ==
                            fact["dimensionsReached"] == "no", "null SRV must short-circuit")
                    unknown("resourceNonNull", "texture2D")
                    require(fact["chromeMatched"] == "no", "null SRV cannot match chrome")
                else:
                    require(fact["srvNonNull"] == "yes" and fact["resourceReached"] == "yes",
                            "SRV result lacks resource stage")
                    if fact["resourceNonNull"] == "no":
                        require(fact["queryReached"] == fact["dimensionsReached"] == "no",
                                "missing resource must short-circuit query")
                        unknown("texture2D")
                        require(fact["chromeMatched"] == "no", "missing resource cannot match chrome")
                    else:
                        require(fact["resourceNonNull"] == "yes" and
                                fact["queryReached"] == "yes", "resource result lacks QI stage")
                        if fact["texture2D"] == "no":
                            require(fact["dimensionsReached"] == "no" and
                                    fact["chromeMatched"] == "no", "non-2D resource must short-circuit desc")
                            zero("width", "height")
                        else:
                            require(fact["texture2D"] == "yes" and
                                    fact["dimensionsReached"] == "yes", "2D query lacks GetDesc")
                            texture_match = (fact["width"] >= 2000 and
                                             fact["height"] >= 1000 and
                                             fact["height"] < fact["width"])
                            require(fact["chromeMatched"] == ("yes" if texture_match else "no"),
                                    "texture dimensions differ from frozen thresholds")

    chrome = fact["chromeMatched"] == "yes"
    if not chrome:
        unknown("healForSkip", "latchOn", "frameChanged", "maskBit")
        require(fact["latchReached"] == fact["frameReached"] ==
                fact["helperReached"] == fact["maskReached"] == "no" and
                fact["terminalSkip"] == "no", "chrome miss must not reach skip stages")
        zero("frameNo", "priorFrameNo", "ordinalCountBefore", "ordinal",
             "ordinalCountAfter", "startInstance", "baseVertex", "skipMask")
    else:
        require(fact["healForSkip"] in ("yes", "no"), "chrome match lacks heal state")
        if fact["healForSkip"] == "no":
            require(fact["latchReached"] == fact["frameReached"] ==
                    fact["helperReached"] == fact["maskReached"] == "no" and
                    fact["terminalSkip"] == "no", "heal-off must short-circuit skip stages")
            unknown("latchOn", "frameChanged", "maskBit")
            zero("frameNo", "priorFrameNo", "ordinalCountBefore", "ordinal",
                 "ordinalCountAfter", "startInstance", "baseVertex", "skipMask")
        else:
            require(fact["latchReached"] == "yes" and fact["latchOn"] in ("yes", "no"),
                    "heal-on chrome match lacks mode latch result")
            if fact["latchOn"] == "no":
                require(fact["frameReached"] == fact["helperReached"] ==
                        fact["maskReached"] == "no" and fact["terminalSkip"] == "no",
                        "latch-off must short-circuit frame/helper stages")
                unknown("frameChanged", "maskBit")
                zero("frameNo", "priorFrameNo", "ordinalCountBefore", "ordinal",
                     "ordinalCountAfter", "startInstance", "baseVertex", "skipMask")
            else:
                require(fact["frameReached"] == fact["helperReached"] ==
                        fact["maskReached"] == "yes", "mode latch hit lacks helper and mask")
                changed = fact["priorFrameNo"] != fact["frameNo"]
                require(fact["frameChanged"] == ("yes" if changed else "no"),
                        "frame transition differs from captured frame values")
                ordinal = 0 if changed else fact["ordinalCountBefore"]
                after = (ordinal + 1) & 0xffffffff
                require(fact["ordinal"] == ordinal and fact["ordinalCountAfter"] == after,
                        "ordinal transition differs from post-increment counter")
                in_range = ordinal < 32
                require(fact["ordinalInRange"] == ("yes" if in_range else "no"),
                        "ordinal range differs from frozen bound")
                if in_range:
                    bit = bool((fact["skipMask"] >> ordinal) & 1)
                    require(fact["maskBit"] == ("yes" if bit else "no"),
                            "mask-bit result differs from frozen bit test")
                else:
                    bit = False
                    unknown("maskBit")
                require(fact["terminalSkip"] == ("yes" if bit else "no"),
                        "terminal result differs from frozen selector")
    terminal = fact["terminalSkip"] == "yes"
    event = {"id": 1, "kind": 2, "outcome": 4 if terminal else 2,
             "flow": 1 if terminal else 0, "subsite": 0,
             "verdict": 2 if terminal else -1}
    return event, 0


def _fss_chrome_skip_fact_fixture(draw, **inputs):
    """Build explicit consumed site-1 facts for offline oracle fixtures."""
    values = {
        "outerHeal": True, "outerCensus": False, "outerTemporal": False,
        "budgetEntered": True, "budgetResult": True,
        "vsHash": 0xA888D51024D9798E, "srvNonNull": True,
        "resourceNonNull": True, "texture2D": True, "width": 2000,
        "height": 1000, "healForSkip": True, "latchOn": True,
        "frameNo": 12, "priorFrameNo": 11, "ordinal": 0, "mask": 1,
        "startInstance": 4, "baseVertex": -3,
        "drawKind": draw["kind"], "drawCount": draw["count"],
    }
    values.update(inputs)
    f = {key: "unknown" for key in _FSS_CHROME_TRI_FIELDS}
    f.update({"siteId": 1, "kind": 23, "known": "yes", "drawKind": 0,
              "drawCount": 0, "vsHash": "%016X" % 0, "width": 0, "height": 0,
              "frameNo": 0, "priorFrameNo": 0, "ordinalCountBefore": 0,
              "ordinal": 0, "ordinalCountAfter": 0, "startInstance": 0,
              "baseVertex": 0, "skipMask": 0})
    outer = bool(values["outerHeal"] or values["outerCensus"] or values["outerTemporal"])
    f["outerHeal"] = "yes" if values["outerHeal"] else "no"
    if not values["outerHeal"]:
        f["outerCensus"] = "yes" if values["outerCensus"] else "no"
        if not values["outerCensus"]:
            f["outerTemporal"] = "yes" if values["outerTemporal"] else "no"
    f["kindReached"] = "yes" if outer else "no"
    shape = False
    if outer:
        f["drawKind"] = values["drawKind"]
        kind_match = values["drawKind"] == ord("X")
        f["kindMatched"] = "yes" if kind_match else "no"
        if kind_match:
            f["countReached"] = "yes"
            f["drawCount"] = values["drawCount"]
            shape = values["drawCount"] == 6
            f["countMatched"] = "yes" if shape else "no"
        else:
            f["countReached"] = "no"
    if not shape:
        f.update(budgetEntered="no", budgetResult="unknown", hashReached="no",
                 srvReached="no", resourceReached="no", queryReached="no",
                 dimensionsReached="no", chromeMatched="no", latchReached="no",
                 frameReached="no", helperReached="no", maskReached="no",
                 terminalSkip="no")
        return f
    entered = bool(values["budgetEntered"])
    f["budgetEntered"] = "yes" if entered else "no"
    f["budgetResult"] = "yes" if values["budgetResult"] else "no"
    if not entered:
        f.update(hashReached="no", srvReached="no", resourceReached="no",
                 queryReached="no", dimensionsReached="no", chromeMatched="no",
                 latchReached="no", frameReached="no", helperReached="no",
                 maskReached="no", terminalSkip="no")
        return f
    f["hashReached"] = "yes"
    hash_value = values["vsHash"]
    f["vsHash"] = "%016X" % hash_value
    hash_match = hash_value in (0xA888D51024D9798E, 0xB018D143700AB803)
    f["hashMatched"] = "yes" if hash_match else "no"
    if not hash_match:
        f.update(srvReached="no", resourceReached="no", queryReached="no",
                 dimensionsReached="no", chromeMatched="no", latchReached="no",
                 frameReached="no", helperReached="no", maskReached="no",
                 terminalSkip="no")
        return f
    f["srvReached"] = "yes"
    f["srvNonNull"] = "yes" if values["srvNonNull"] else "no"
    if not values["srvNonNull"]:
        f.update(resourceReached="no", queryReached="no", dimensionsReached="no",
                 chromeMatched="no", latchReached="no", frameReached="no",
                 helperReached="no", maskReached="no", terminalSkip="no")
        return f
    f["resourceReached"] = "yes"
    f["resourceNonNull"] = "yes" if values["resourceNonNull"] else "no"
    if not values["resourceNonNull"]:
        f.update(queryReached="no", dimensionsReached="no", chromeMatched="no",
                 latchReached="no", frameReached="no", helperReached="no",
                 maskReached="no", terminalSkip="no")
        return f
    f["queryReached"] = "yes"
    f["texture2D"] = "yes" if values["texture2D"] else "no"
    if not values["texture2D"]:
        f.update(dimensionsReached="no", chromeMatched="no", latchReached="no",
                 frameReached="no", helperReached="no", maskReached="no",
                 terminalSkip="no")
        return f
    f["dimensionsReached"] = "yes"
    f["width"], f["height"] = values["width"], values["height"]
    chrome = values["width"] >= 2000 and values["height"] >= 1000 and values["height"] < values["width"]
    f["chromeMatched"] = "yes" if chrome else "no"
    if not chrome:
        f.update(latchReached="no", frameReached="no", helperReached="no",
                 maskReached="no", terminalSkip="no")
        return f
    f["healForSkip"] = "yes" if values["healForSkip"] else "no"
    if not values["healForSkip"]:
        f.update(latchReached="no", frameReached="no", helperReached="no",
                 maskReached="no", terminalSkip="no")
        return f
    f["latchReached"] = "yes"
    f["latchOn"] = "yes" if values["latchOn"] else "no"
    if not values["latchOn"]:
        f.update(frameReached="no", helperReached="no", maskReached="no",
                 terminalSkip="no")
        return f
    f.update(frameReached="yes", helperReached="yes", maskReached="yes",
             frameNo=values["frameNo"], priorFrameNo=values["priorFrameNo"],
             startInstance=values["startInstance"], baseVertex=values["baseVertex"],
             frameChanged="yes" if values["frameNo"] != values["priorFrameNo"] else "no",
             ordinal=values["ordinal"], ordinalCountBefore=values["ordinal"],
             ordinalCountAfter=(values["ordinal"] + 1) & 0xffffffff,
             skipMask=values["mask"], ordinalInRange="yes" if values["ordinal"] < 32 else "no")
    if values["ordinal"] < 32:
        bit = bool((values["mask"] >> values["ordinal"]) & 1)
        f["maskBit"] = "yes" if bit else "no"
    else:
        bit = False
    f["terminalSkip"] = "yes" if bit else "no"
    return f


def _replay_predicate_facts(draw, label, predicate_fact_version=1):
    facts = draw.get("predicateFacts")
    maximum = 21 if predicate_fact_version >= 16 else 20 if predicate_fact_version >= 15 else 19 if predicate_fact_version >= 14 else 18 if predicate_fact_version >= 13 else 17 if predicate_fact_version >= 12 else 16 if predicate_fact_version >= 11 else 15 if predicate_fact_version >= 10 else 14 if predicate_fact_version >= 9 else 12 if predicate_fact_version >= 8 else 11 if predicate_fact_version >= 7 else 9 if predicate_fact_version >= 6 else 6 if predicate_fact_version >= 5 else 4 if predicate_fact_version >= 3 else 3 if predicate_fact_version >= 2 else 2
    if not isinstance(facts, list) or len(facts) > maximum:
        raise TraceError(label + ".predicateFacts must be a bounded array (type %s, count %s)" %
                         (type(facts).__name__, len(facts) if isinstance(facts, list) else "n/a"))
    supported_ids = ((1, 2, 3, 6, 24, 25, 26, 45, 48, 49, 50, 51, 52, 53, 54, 55, 57, 58, 59, 60, 61, 62, 63, 67) if predicate_fact_version >= 16 else
                     (2, 3, 6, 24, 25, 26, 45, 48, 49, 50, 51, 52, 53, 54, 55, 57, 58, 59, 60, 61, 62, 63, 67) if predicate_fact_version >= 15 else
                     (2, 3, 6, 24, 25, 26, 48, 49, 50, 51, 52, 53, 54, 55, 57, 58, 59, 60, 61, 62, 63, 67) if predicate_fact_version >= 14 else
                     (2, 3, 6, 24, 25, 26, 48, 49, 50, 51, 52, 53, 55, 57, 58, 59, 60, 61, 62, 63, 67) if predicate_fact_version >= 13 else
                     (2, 3, 6, 24, 25, 26, 48, 49, 50, 51, 52, 53, 55, 57, 58, 60, 61, 62, 63, 67) if predicate_fact_version >= 12 else
                     (2, 3, 6, 24, 26, 48, 49, 50, 51, 52, 53, 55, 57, 58, 60, 61, 62, 63, 67) if predicate_fact_version >= 11 else
                     (2, 3, 6, 24, 26, 48, 49, 50, 51, 52, 53, 55, 57, 58, 61, 62, 63, 67) if predicate_fact_version >= 10 else
                     (2, 3, 6, 24, 26, 49, 50, 51, 52, 53, 55, 57, 58, 61, 62, 63, 67) if predicate_fact_version >= 9 else
                     (3, 6, 24, 26, 49, 50, 51, 52, 53, 55, 57, 58, 61, 62, 63) if predicate_fact_version >= 8 else
                     (3, 6, 24, 26, 49, 50, 53, 55, 57, 58, 61, 62, 63) if predicate_fact_version >= 7 else
                     (3, 6, 24, 26, 49, 50, 53, 55, 61, 62, 63) if predicate_fact_version >= 6 else
                     (3, 6, 24, 26, 49, 50, 53, 55) if predicate_fact_version >= 5 else
                     (3, 6, 24, 26, 49, 50) if predicate_fact_version >= 4 else
                     (3, 6, 49, 50) if predicate_fact_version >= 3 else
                     (3, 49, 50) if predicate_fact_version >= 2 else (3, 49))
    expected = {event["id"] for event in draw["sites"] if event["id"] in supported_ids}
    by_site = {}
    sunglare_action = None
    sunglare62_expected = None
    sunglare_mutation_missing = set()
    fss_dump_interested = None
    target_sharp_interested = None
    target_sharp_facts = 0
    sunglare_nomination_facts = 0
    fss_chrome_skip_mismatches = 0
    fss_chrome_skip_terminal = 0
    for index, fact in enumerate(facts):
        fact_label = "%s.predicateFacts[%d]" % (label, index)
        if not isinstance(fact, dict):
            raise TraceError(fact_label + " must be an object")
        site_id = _integer(fact.get("siteId"), fact_label + ".siteId", 1, 76)
        kind = _integer(fact.get("kind"), fact_label + ".kind", 1,
                        23 if predicate_fact_version >= 16 else
                        22 if predicate_fact_version >= 15 else
                        21 if predicate_fact_version >= 14 else
                        20 if predicate_fact_version >= 13 else
                        19 if predicate_fact_version >= 12 else
                        18 if predicate_fact_version >= 11 else
                        17 if predicate_fact_version >= 10 else
                        16 if predicate_fact_version >= 9 else
                        14 if predicate_fact_version >= 8 else
                        13 if predicate_fact_version >= 7 else
                        11 if predicate_fact_version >= 6 else
                        8 if predicate_fact_version >= 5 else
                        6 if predicate_fact_version >= 4 else
                        4 if predicate_fact_version >= 3 else
                        3 if predicate_fact_version >= 2 else 2)
        if site_id in by_site:
            raise TraceError(fact_label + " duplicates a supported site fact")
        supported_pairs = ((1, 23), (2, 15), (3, 1), (6, 4), (24, 5), (25, 19), (26, 6), (45, 22), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (54, 21), (55, 8),
                           (57, 12), (58, 13), (59, 20), (60, 18), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 16 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (25, 19), (26, 6), (45, 22), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (54, 21), (55, 8),
                           (57, 12), (58, 13), (59, 20), (60, 18), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 15 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (25, 19), (26, 6), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (54, 21), (55, 8),
                           (57, 12), (58, 13), (59, 20), (60, 18), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 14 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (25, 19), (26, 6), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (55, 8),
                           (57, 12), (58, 13), (59, 20), (60, 18), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 13 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (25, 19), (26, 6), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (55, 8),
                           (57, 12), (58, 13), (60, 18), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 12 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (26, 6), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (55, 8),
                           (57, 12), (58, 13), (60, 18), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 11 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (26, 6), (48, 17), (49, 2), (50, 3), (51, 14), (53, 7), (55, 8),
                           (57, 12), (58, 13), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 10 else (
            (2, 15), (3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3), (51, 14), (53, 7), (55, 8),
                           (57, 12), (58, 13), (61, 9), (62, 10), (63, 11), (67, 16)) if predicate_fact_version >= 9 else (
            (3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3), (51, 14), (53, 7), (55, 8),
                            (57, 12), (58, 13), (61, 9), (62, 10), (63, 11)) if predicate_fact_version >= 8 else (
            (3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3),
                           (53, 7), (55, 8), (57, 12), (58, 13), (61, 9), (62, 10), (63, 11)) if predicate_fact_version >= 7 else (
            (3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3),
                           (53, 7), (55, 8), (61, 9), (62, 10), (63, 11)) if predicate_fact_version >= 6 else (
            (3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3),
                           (53, 7), (55, 8)) if predicate_fact_version >= 5 else (
            (3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3)) if predicate_fact_version >= 4 else (
            (3, 1), (6, 4), (49, 2), (50, 3)) if predicate_fact_version >= 3 else (
            (3, 1), (49, 2), (50, 3)) if predicate_fact_version >= 2 else ((3, 1), (49, 2))
        if (site_id, kind) not in supported_pairs:
            raise TraceError(fact_label + " has an unsupported site/kind pair")
        if site_id == 52:
            raise TraceError(fact_label + " derived RemLok site 52 cannot have a separate fact")
        known = fact.get("known")
        if known not in TRI_STATES:
            raise TraceError(fact_label + ".known is invalid")
        if known == "no":
            raise TraceError(fact_label + ".known must be yes or unknown")
        if kind == 23:
            if predicate_fact_version < 16:
                raise TraceError(fact_label + " has unsupported FSS chrome fact")
            expected_event, fact_mismatches = _legacy14a_fss_chrome_skip(
                fact, draw, fact_label)
            fss_chrome_skip_mismatches += fact_mismatches
            fss_chrome_skip_terminal += int(expected_event["verdict"] == 2)
            by_site[site_id] = (expected_event, None, True, 0,
                                fact_mismatches, None)
        elif kind in (9, 10, 11):
            required = {"siteId", "kind", "known", "site"}
            if kind == 9:
                required |= {"selector", "common2ClampBefore", "common2ClampAfter"}
            if set(fact) != required:
                raise TraceError(fact_label + " has missing or unexpected Sunglare fields")
            cache_mismatches = 0
            expected_delta = None
            if kind == 9:
                if site_id != 61:
                    raise TraceError(fact_label + " has mismatched Sunglare source site")
                sunglare_action, cache_mismatches, _, mutation_unobserved = _sunglare_selector(fact, draw, fact_label)
                common2_clamp_after = None
                for field in ("common2ClampBefore", "common2ClampAfter"):
                    value = _sunglare_read(fact, field, fact_label, int, 0xffffffff)
                    if not value[0] or not value[1]:
                        raise TraceError(fact_label + " lacks the common2 clamp reset observation")
                    if field == "common2ClampAfter" and value[2] != 0:
                        cache_mismatches += 1
                    if field == "common2ClampAfter":
                        common2_clamp_after = value[2]
                if mutation_unobserved:
                    expected_delta = 0
                    sunglare_mutation_missing.add(site_id)
                event, site_mismatch, site_unreplayable = _replay_sunglare_site(
                    fact, site_id, draw, fact_label, sunglare_action,
                    common2_clamp_after)
                cache_mismatches += site_mismatch
                if site_unreplayable or mutation_unobserved:
                    event = None
            else:
                if site_id not in (62, 63) or 61 not in by_site:
                    raise TraceError(fact_label + " lacks preceding site61 source provenance")
                if kind == 10 and site_id != 62 or kind == 11 and site_id != 63:
                    raise TraceError(fact_label + " has mismatched Sunglare fact kind")
                if sunglare_action == 1:
                    raise TraceError(fact_label + " skip action makes the later Sunglare sites unreachable")
                if site_id == 63 and 62 not in by_site:
                    raise TraceError(fact_label + " lacks preceding site62 chain provenance")
                source_action = (None if sunglare_action is None else sunglare_action != 0)
                event, site_mismatch, site_unreplayable = _replay_sunglare_site(
                    fact, site_id, draw, fact_label, source_action)
                cache_mismatches += site_mismatch
                if site_id == 62:
                    sunglare62_expected = event
                elif sunglare62_expected is not None and sunglare62_expected.get("outcome") == 3:
                    raise TraceError(fact_label + " site63 is unreachable after site62 claims")
                elif site_id == 63 and sunglare62_expected is None:
                    site_unreplayable = True
                if site_unreplayable:
                    event = None
            by_site[site_id] = (event, expected_delta,
                                expected_delta is None, 0, cache_mismatches, None)
        elif kind in (15, 16):
            if predicate_fact_version < 9:
                raise TraceError(fact_label + " has unsupported BasicDraw fact")
            event, fact_mismatches, fact_mutation_unobserved = \
                _replay_basic_draw_fact(fact, draw, fact_label)
            by_site[site_id] = (event, None, not fact_mutation_unobserved,
                                0, fact_mismatches, None)
        elif kind == 17:
            if site_id != 48 or predicate_fact_version < 10:
                raise TraceError(fact_label + " has unsupported EyeCensus fact")
            try:
                event, fact_mismatches, fact_mutation_unobserved = \
                    _replay_eye_census_fact(fact, draw, fact_label)
            except _EyeCensusUnavailable:
                event, fact_mismatches, fact_mutation_unobserved = None, 0, False
            by_site[site_id] = (event, None, True, 0, fact_mismatches,
                                fact_mutation_unobserved)
        elif kind == 18:
            if site_id != 60 or predicate_fact_version < 11:
                raise TraceError(fact_label + " has unsupported ResolveBind fact")
            try:
                event, fact_mismatches, fact_mutation_unobserved = \
                    _replay_resolve_bind_fact(fact, fact_label)
            except _ResolveBindUnavailable:
                event, fact_mismatches, fact_mutation_unobserved = None, 0, False
            by_site[site_id] = (event, None, True, 0, fact_mismatches,
                                fact_mutation_unobserved)
        elif kind == 19:
            if site_id != 25 or predicate_fact_version < 12:
                raise TraceError(fact_label + " has unsupported LoaderPanel fact")
            try:
                event, fact_mismatches, fact_mutation_unobserved = \
                    _replay_loader_panel_fact(fact, draw, fact_label)
            except _LoaderPanelUnavailable:
                event, fact_mismatches, fact_mutation_unobserved = None, 0, False
            by_site[site_id] = (event, None, True, 0, fact_mismatches,
                                fact_mutation_unobserved)
        elif kind == 20:
            if site_id != 59 or predicate_fact_version < 13:
                raise TraceError(fact_label + " has unsupported FSS-dump fact")
            try:
                event, fact_mismatches, fact_mutation_unobserved = \
                    _replay_fss_dump_fact(fact, draw, fact_label, fss_dump_interested)
            except _FssDumpUnavailable as exc:
                event, fact_mismatches, fact_mutation_unobserved = None, 0, exc.mutation_unobserved
            by_site[site_id] = (event, None, True, 0, fact_mismatches,
                                fact_mutation_unobserved)
        elif kind == 22:
            if site_id != 45 or predicate_fact_version < 15:
                raise TraceError(fact_label + " has unsupported Sunglare nomination fact")
            event, fact_mismatches, fact_unavailable = \
                _sunglare_nomination_fact(fact, draw, fact_label)
            expected_event = event
            by_site[site_id] = (event, None, not fact_unavailable, 0,
                                fact_mismatches, None)
            sunglare_nomination_facts += 1
        elif kind == 21:
            if site_id != 54 or predicate_fact_version < 14:
                raise TraceError(fact_label + " has unsupported TargetSharp fact")
            if target_sharp_interested is None:
                raise TraceError(fact_label + " lacks preceding site6 admission source")
            expected_claim, fact_mismatches = _target_sharp_fact(
                fact, draw, fact_label, target_sharp_interested)
            expected_event = (None if expected_claim is None and fact["handlerInvoked"] else
                {"id": 54, "kind": 2, "outcome": 3, "flow": 1,
                 "subsite": 0, "verdict": 5} if expected_claim else
                {"id": 54, "kind": 2, "outcome": 5, "flow": 0,
                 "subsite": 0, "verdict": -1} if not fact["handlerInvoked"] else
                {"id": 54, "kind": 2, "outcome": 2, "flow": 0,
                 "subsite": 0, "verdict": -1})
            by_site[site_id] = (expected_event, None, True, 0,
                                fact_mismatches, expected_claim)
            target_sharp_facts += 1
        elif kind == 14:
            if site_id != 51 or predicate_fact_version < 8:
                raise TraceError(fact_label + " has unsupported RemLok fact")
            event, derived, fact_mismatches, fact_mutation_unobserved = \
                _replay_remlok_fact(fact, draw, fact_label)
            by_site[site_id] = (event, None, True, 0, fact_mismatches,
                                fact_mutation_unobserved)
            if event is not None and event["outcome"] == 4:
                if 52 in expected:
                    raise TraceError(fact_label + " RemLok site 52 is reached after a terminal hide")
            elif 52 in expected:
                by_site[52] = (derived, None, True, 0, fact_mismatches, None)
            elif derived is not None:
                if 52 not in expected:
                    raise TraceError(fact_label + " RemLok claim lacks its derived site 52 event")
        elif kind in (12, 13):
            required = {"siteId", "kind", "known", "selector", "helper", "mutation"}
            if predicate_fact_version >= 8:
                required |= {"handlerInvoked", "rawProbeReached"}
            if set(fact) != required:
                raise TraceError(fact_label + " has missing or unexpected FSS fields")
            if known != "yes":
                raise TraceError(fact_label + " FSS fact availability must be yes; individual reads carry unknowns")
            if ((site_id, kind) != (57, 12) and (site_id, kind) != (58, 13)):
                raise TraceError(fact_label + " has mismatched FSS site/kind")
            event, fact_mismatches, fact_unreplayable, fact_mutation_unobserved = \
                _replay_fss_fact(fact, draw, fact_label, predicate_fact_version)
            by_site[site_id] = (event, None, True, 0, fact_mismatches,
                                fact_mutation_unobserved)
        elif kind == 3:
            cache_mismatches = 0
            required = {"siteId", "kind", "known", "dispatchEnabled",
                        "activeMaskKnown", "activePluginMask", "candidateKnown",
                        "candidatePresent", "modeKnown", "mode", "shapeReached",
                        "shapeMatched", "callbackReached", "callbackModeKnown",
                        "callbackMode", "failedKnown", "failed"}
            if set(fact) != required:
                raise TraceError(fact_label + " has missing or unexpected night-vision fields")
            tri_fields = ("dispatchEnabled", "activeMaskKnown", "candidateKnown",
                          "candidatePresent", "modeKnown", "shapeReached",
                          "shapeMatched", "callbackReached", "callbackModeKnown",
                          "failedKnown", "failed")
            if any(fact.get(key) not in TRI_STATES for key in tri_fields):
                raise TraceError(fact_label + " has an invalid night-vision tri-state")
            if fact["known"] != "yes" or fact["dispatchEnabled"] == "unknown":
                raise TraceError(fact_label + " lacks the reached dispatch input")
            active_mask = fact.get("activePluginMask")
            if not isinstance(active_mask, str) or not re.match(r"^[0-9A-Fa-f]{16}$", active_mask):
                raise TraceError(fact_label + ".activePluginMask must be sixteen hex digits")
            active_mask = int(active_mask, 16)
            mode = _integer(fact.get("mode"), fact_label + ".mode", 0, 3)
            callback_mode = _integer(fact.get("callbackMode"), fact_label + ".callbackMode", 0, 3)
            if fact["modeKnown"] == "unknown" and mode != 0:
                raise TraceError(fact_label + " unknown mode must not carry a value")
            if fact["callbackModeKnown"] == "unknown" and callback_mode != 0:
                raise TraceError(fact_label + " unknown callback mode must not carry a value")
            if fact["failedKnown"] == "unknown" and fact["failed"] != "unknown":
                raise TraceError(fact_label + " unknown failure state must not carry a value")
            if fact["dispatchEnabled"] == "no":
                if (fact["activeMaskKnown"] != "unknown" or active_mask != 0 or
                        fact["candidateKnown"] != "unknown" or
                        fact["candidatePresent"] != "unknown" or
                        fact["shapeReached"] != "no" or
                        fact["shapeMatched"] != "unknown" or
                        fact["callbackReached"] != "no" or
                        fact["callbackModeKnown"] != "unknown" or
                        fact["failedKnown"] != "unknown"):
                    raise TraceError(fact_label + " dispatch short-circuit carries later-stage inputs")
            else:
                if (fact["activeMaskKnown"] != "yes" or
                        fact["candidateKnown"] != "yes" or
                        fact["candidatePresent"] == "unknown"):
                    raise TraceError(fact_label + " lacks a required candidate-cache stage")
                if fact["candidatePresent"] == "no":
                    if (fact["shapeReached"] != "no" or
                            fact["shapeMatched"] != "unknown" or
                            fact["callbackReached"] != "no" or
                            fact["callbackModeKnown"] != "unknown" or
                            fact["failedKnown"] != "unknown"):
                        raise TraceError(fact_label + " candidate miss carries later-stage inputs")
                elif (fact["shapeReached"] != "yes" or
                      fact["shapeMatched"] not in ("yes", "no")):
                    raise TraceError(fact_label + " lacks the reached shape stage")
                if fact["shapeMatched"] == "no" and fact["callbackReached"] != "no":
                    raise TraceError(fact_label + " shape miss has an inconsistent callback stage")
                if fact["shapeMatched"] == "yes":
                    if fact["callbackReached"] == "no":
                        raise TraceError(fact_label + " shape hit is missing its callback stage")
                    if fact["callbackReached"] == "yes" and fact["callbackModeKnown"] != "yes":
                        raise TraceError(fact_label + " callback lacks its consumed mode")
                    if fact["callbackReached"] == "unknown" and (
                            fact["callbackModeKnown"] != "unknown" or
                            fact["failedKnown"] != "unknown"):
                        raise TraceError(fact_label + " unknown callback carries outputs")
                    if fact["callbackReached"] == "yes":
                        if callback_mode == 0 and fact["failedKnown"] != "unknown":
                            raise TraceError(fact_label + " mode zero must not read failure state")
                        if callback_mode != 0 and fact["failedKnown"] not in ("yes", "unknown"):
                            raise TraceError(fact_label + " nonzero mode lacks failure availability")
            normalized = dict(fact)
            expected_event = None
            expected_delta = None
            legacy_claim = _legacy14a_night_vision(normalized, draw)
            active_known = (fact["dispatchEnabled"] == "no" or
                            (fact["activeMaskKnown"] == "yes" and
                             fact["modeKnown"] == "yes"))
            if fact["dispatchEnabled"] == "no":
                expected_event = {"id": 50, "kind": 2, "outcome": 5,
                                  "flow": 0, "subsite": 0, "verdict": -1}
            elif active_known:
                expected_active = mode != 0
                actual_active = bool(active_mask & (1 << 1))
                pair_match = (draw["vsHash"].upper() == "FCF7BD2896751D96" and
                              draw["psHash"].upper() == "F786D34B5E118D5E")
                expected_candidate = expected_active and pair_match
                cached_candidate = int(draw["candidateMask"], 16) & (1 << 1) != 0
                if actual_active != expected_active or cached_candidate != expected_candidate:
                    cache_mismatches += 1
                if fact["candidatePresent"] != ("yes" if cached_candidate else "no"):
                    cache_mismatches += 1
                if not expected_candidate:
                    if fact["shapeReached"] != "no":
                        cache_mismatches += 1
                    expected_event = {"id": 50, "kind": 2, "outcome": 5,
                                      "flow": 0, "subsite": 0, "verdict": -1}
                else:
                    shape_match = (draw["kind"] == ord("X") and
                                   draw["count"] == 240 and draw["instances"] == 1)
                    if (fact["shapeReached"] != "yes" or
                            fact["shapeMatched"] != ("yes" if shape_match else "no")):
                        cache_mismatches += 1
                    if not shape_match:
                        expected_event = {"id": 50, "kind": 2, "outcome": 2,
                                          "flow": 0, "subsite": 0, "verdict": -1}
                    elif fact["callbackReached"] != "yes" or fact["callbackModeKnown"] != "yes":
                        expected_event = None
                    elif callback_mode != mode:
                        cache_mismatches += 1
                        expected_event = None
                    elif legacy_claim is None:
                        expected_event = None
                    elif legacy_claim:
                        expected_event = {"id": 50, "kind": 2, "outcome": 3,
                                          "flow": 1, "subsite": 0, "verdict": 6}
                    else:
                        expected_event = {"id": 50, "kind": 2, "outcome": 2,
                                          "flow": 0, "subsite": 0, "verdict": -1}
            expected_delta = None
            by_site[site_id] = (expected_event, expected_delta, True, 0,
                                cache_mismatches,
                                legacy_claim)
        elif kind == 4:
            expected_event, expected_delta, delta_known, observed_delta, legacy_claim = \
                _replay_witchspace_stars_fact(fact, draw, fact_label)
            if site_id == 6:
                interest_mask = int(fact["legacyInterestMask"], 16)
                fss_dump_interested = bool(interest_mask & (1 << 4))
                target_sharp_interested = bool(interest_mask & 1)
            by_site[site_id] = (expected_event, expected_delta, delta_known,
                                observed_delta, 0, legacy_claim)
        elif kind == 5:
            expected_event, expected_delta, delta_known, observed_delta, _ = \
                _replay_offscreen_census_fact(fact, draw, fact_label)
            by_site[site_id] = (expected_event, expected_delta, delta_known,
                                observed_delta, 0, None)
        elif kind == 6:
            expected_event, expected_delta, delta_known, observed_delta, _ = \
                _replay_offscreen_quad_fact(fact, draw, fact_label)
            by_site[site_id] = (expected_event, expected_delta, delta_known,
                                observed_delta, 0, None)
        elif kind == 7:
            expected_event, expected_delta, delta_known, observed_delta, fact_mismatches, legacy_claim = \
                _replay_holo_fact(fact, draw, fact_label)
            by_site[site_id] = (expected_event, expected_delta, delta_known,
                                observed_delta, fact_mismatches, legacy_claim)
        elif kind == 8:
            expected_event, expected_delta, delta_known, observed_delta, fact_mismatches, legacy_claim = \
                _replay_scrim_fact(fact, draw, fact_label)
            by_site[site_id] = (expected_event, expected_delta, delta_known,
                                observed_delta, fact_mismatches, legacy_claim)
        elif kind == 1:
            if set(fact) != {"siteId", "kind", "known", "gateWanted"}:
                raise TraceError(fact_label + " has unexpected draw-gate fields")
            gate_wanted = fact.get("gateWanted")
            if gate_wanted not in TRI_STATES:
                raise TraceError(fact_label + ".gateWanted is invalid")
            if (known == "yes") != (gate_wanted != "unknown"):
                raise TraceError(fact_label + " gate input availability is inconsistent")
            normalized = {"known": known, "gateWanted": gate_wanted}
            candidate = _candidate_draw_gate(normalized)
            legacy = _legacy14a_draw_gate(normalized)
            if candidate != legacy:
                raise TraceError(fact_label + " candidate differs from frozen 14a selector")
            expected_event = candidate
            expected_delta = None
        else:
            required = {"siteId", "kind", "known", "eyeDrawIndex", "ranges",
                        "censusSkippedDeltaKnown", "censusSkippedDelta"}
            if set(fact) != required:
                raise TraceError(fact_label + " fields are missing or unexpected")
            eye_index = _integer(fact.get("eyeDrawIndex"), fact_label + ".eyeDrawIndex")
            ranges = fact.get("ranges")
            if not isinstance(ranges, list) or len(ranges) > 4:
                raise TraceError(fact_label + ".ranges must contain at most four ranges")
            normalized_ranges = []
            for ri, pair in enumerate(ranges):
                if not isinstance(pair, list) or len(pair) != 2:
                    raise TraceError("%s.ranges[%d] must be a pair" % (fact_label, ri))
                lo = _integer(pair[0], "%s.ranges[%d].lo" % (fact_label, ri), 1)
                hi = _integer(pair[1], "%s.ranges[%d].hi" % (fact_label, ri), 1)
                if hi < lo:
                    raise TraceError("%s.ranges[%d] is inverted" % (fact_label, ri))
                normalized_ranges.append((lo, hi))
            if known == "unknown" and ranges:
                raise TraceError(fact_label + " unknown range table must not carry inputs")
            normalized = {"known": known, "eyeDrawIndex": eye_index,
                          "ranges": normalized_ranges}
            candidate = _candidate_eye_range(normalized)
            legacy = _legacy14a_eye_range(normalized)
            if candidate != legacy:
                raise TraceError(fact_label + " candidate differs from frozen 14a selector")
            delta_known = fact.get("censusSkippedDeltaKnown")
            if type(delta_known) is not bool:
                raise TraceError(fact_label + ".censusSkippedDeltaKnown is invalid")
            delta = _integer(fact.get("censusSkippedDelta"),
                             fact_label + ".censusSkippedDelta")
            if (not delta_known and delta != 0) or delta > 1:
                raise TraceError(fact_label + " has invalid counter delta availability")
            expected_delta = candidate[1] if candidate is not None else None
            if expected_delta is not None and not delta_known:
                # A valid input fact can be replayed even if its side effect
                # observation is unavailable; report that separately below.
                pass
            expected_event = candidate[0] if candidate is not None else None
        if kind not in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23):
            by_site[site_id] = (expected_event, expected_delta,
                                fact.get("censusSkippedDeltaKnown", True),
                                fact.get("censusSkippedDelta", 0), 0, None)
    if set(by_site) != expected:
        raise TraceError(label + ".predicateFacts do not exactly cover visited supported sites")
    target_event = next((event for event in draw["sites"] if event["id"] == 54), None)
    if target_event is not None:
        if target_sharp_interested is None:
            raise TraceError(label + " site54 lacks site6 admission provenance")
        if not target_sharp_interested and target_event["outcome"] != 5:
            raise TraceError(label + " TargetSharp site was invoked without interest bit0")
        if target_sharp_interested and target_event["outcome"] == 5:
            raise TraceError(label + " admitted TargetSharp site is marked not eligible")
    mismatches = 0
    unreplayable = 0
    mutation_unobserved = 0
    replayed = 0
    nv_replayed = 0
    nv_unreplayable = 0
    nv_mismatches = 0
    stars_replayed = 0
    stars_unreplayable = 0
    stars_mismatches = 0
    census_replayed = 0
    census_unreplayable = 0
    census_mismatches = 0
    quad_replayed = 0
    quad_unreplayable = 0
    quad_mismatches = 0
    holo_replayed = 0
    holo_unreplayable = 0
    holo_mismatches = 0
    scrim_replayed = 0
    scrim_unreplayable = 0
    scrim_mismatches = 0
    sunglare_replayed = 0
    sunglare_unreplayable = 0
    sunglare_mismatches = 0
    fss_replayed = 0
    fss_unreplayable = 0
    fss_mismatches = 0
    remlok_replayed = 0
    remlok_unreplayable = 0
    remlok_mismatches = 0
    basic_draw_replayed = 0
    basic_draw_unreplayable = 0
    basic_draw_mismatches = 0
    eye_census_replayed = 0
    eye_census_unreplayable = 0
    eye_census_mismatches = 0
    resolve_bind_replayed = 0
    resolve_bind_unreplayable = 0
    resolve_bind_mismatches = 0
    loader_panel_replayed = 0
    loader_panel_unreplayable = 0
    loader_panel_mismatches = 0
    loader_panel_mutation_unobserved = 0
    fss_dump_replayed = 0
    fss_dump_unreplayable = 0
    fss_dump_mismatches = 0
    target_sharp_replayed = 0
    target_sharp_unreplayable = 0
    target_sharp_mismatches = 0
    target_sharp_not_eligible = int(target_event is not None and target_event["outcome"] == 5)
    sunglare_nomination_replayed = 0
    sunglare_nomination_unreplayable = 0
    sunglare_nomination_mismatches = 0
    for site_id, (expected_event, expected_delta, delta_known,
                  observed_delta, cache_mismatches, legacy_claim) in by_site.items():
        mismatches += cache_mismatches
        site_unreplayable = expected_event is None or (
            site_id in (6, 50) and legacy_claim is None) or (
            site_id == 45 and not delta_known)
        if site_unreplayable:
            unreplayable += 1
        actual = next(event for event in draw["sites"] if event["id"] == site_id)
        if expected_event is not None:
            if any(actual[key] != expected_event[key]
                   for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                mismatches += 1
            else:
                replayed += 1
        if site_id in (2, 67):
            if site_unreplayable:
                basic_draw_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                basic_draw_replayed += 1
            else:
                basic_draw_mismatches += 1
            if cache_mismatches:
                basic_draw_mismatches += cache_mismatches
            if site_id == 2 and not delta_known:
                mutation_unobserved += 1
        if site_id == 48:
            if site_unreplayable:
                eye_census_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                eye_census_replayed += 1
            else:
                eye_census_mismatches += 1
            if cache_mismatches:
                eye_census_mismatches += cache_mismatches
            if legacy_claim:
                mutation_unobserved += 1
        if site_id == 60:
            if site_unreplayable:
                resolve_bind_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                resolve_bind_replayed += 1
            else:
                resolve_bind_mismatches += 1
            if cache_mismatches:
                resolve_bind_mismatches += cache_mismatches
            if legacy_claim:
                mutation_unobserved += 1
        if site_id == 25:
            if site_unreplayable:
                loader_panel_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                loader_panel_replayed += 1
            else:
                loader_panel_mismatches += 1
            if cache_mismatches:
                loader_panel_mismatches += cache_mismatches
            if legacy_claim:
                mutation_unobserved += 1
                loader_panel_mutation_unobserved += 1
        if site_id == 59:
            if site_unreplayable:
                fss_dump_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                fss_dump_replayed += 1
            else:
                fss_dump_mismatches += 1
            if cache_mismatches:
                fss_dump_mismatches += cache_mismatches
            if legacy_claim:
                mutation_unobserved += 1
        if site_id == 54:
            if site_unreplayable:
                target_sharp_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                target_sharp_replayed += 1
            else:
                target_sharp_mismatches += 1
            target_sharp_mismatches += cache_mismatches
        if site_id == 45:
            if site_unreplayable:
                sunglare_nomination_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                sunglare_nomination_replayed += 1
            else:
                sunglare_nomination_mismatches += 1
            sunglare_nomination_mismatches += cache_mismatches
        if site_id in (61, 62, 63):
            if site_unreplayable:
                sunglare_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                sunglare_replayed += 1
            else:
                sunglare_mismatches += 1
            if cache_mismatches:
                sunglare_mismatches += cache_mismatches
            if expected_delta is not None and not delta_known:
                mutation_unobserved += 1
        if site_id in (57, 58):
            if site_unreplayable:
                fss_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                fss_replayed += 1
            else:
                fss_mismatches += 1
            if cache_mismatches:
                fss_mismatches += cache_mismatches
            if legacy_claim:
                mutation_unobserved += 1
        if site_id in (51, 52):
            if site_unreplayable:
                remlok_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                remlok_replayed += 1
            else:
                remlok_mismatches += 1
            if cache_mismatches:
                remlok_mismatches += cache_mismatches
            if site_id == 51 and legacy_claim:
                mutation_unobserved += 1
        if site_id == 50:
            if site_unreplayable:
                nv_unreplayable += 1
                if cache_mismatches:
                    nv_mismatches += 1
            else:
                nv_bad = cache_mismatches != 0
                if expected_event is not None and any(
                        actual[key] != expected_event[key]
                        for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                    nv_bad = True
                if (actual["outcome"] == 3) != legacy_claim:
                    mismatches += 1
                    nv_bad = True
                if nv_bad:
                    nv_mismatches += 1
                else:
                    nv_replayed += 1
        if site_id == 6:
            if site_unreplayable:
                stars_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                stars_replayed += 1
            else:
                stars_mismatches += 1
            if expected_delta is not None:
                if not delta_known:
                    mutation_unobserved += 1
                elif expected_delta != observed_delta:
                    mismatches += 1
                    stars_mismatches += 1
        if site_id == 49 and expected_delta is not None:
            if not delta_known:
                mutation_unobserved += 1
            elif expected_delta != observed_delta:
                mismatches += 1
        if site_id == 24:
            if site_unreplayable:
                census_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                census_replayed += 1
            else:
                census_mismatches += 1
            if expected_delta is not None:
                if not delta_known:
                    mutation_unobserved += 1
                elif expected_delta != observed_delta:
                    mismatches += 1
                    census_mismatches += 1
        if site_id == 26:
            if site_unreplayable:
                quad_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                quad_replayed += 1
            else:
                quad_mismatches += 1
        if site_id in (53, 55):
            replay_counter = "holo" if site_id == 53 else "scrim"
            if site_unreplayable:
                if replay_counter == "holo":
                    holo_unreplayable += 1
                else:
                    scrim_unreplayable += 1
            elif expected_event is not None and not any(
                    actual[key] != expected_event[key]
                    for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                if replay_counter == "holo":
                    holo_replayed += 1
                else:
                    scrim_replayed += 1
            else:
                if replay_counter == "holo":
                    holo_mismatches += 1
                else:
                    scrim_mismatches += 1
            if cache_mismatches:
                if replay_counter == "holo":
                    holo_mismatches += cache_mismatches
                else:
                    scrim_mismatches += cache_mismatches
    return {"factCount": len(by_site), "replayed": replayed,
            "unreplayable": unreplayable, "mismatches": mismatches,
            "mutationUnobserved": mutation_unobserved,
            "nightVisionFacts": sum(1 for site_id in by_site if site_id == 50),
            "nightVisionReplayed": nv_replayed,
            "nightVisionUnreplayable": nv_unreplayable,
            "nightVisionMismatches": nv_mismatches,
            "witchspaceStarsFacts": sum(1 for site_id in by_site if site_id == 6),
            "witchspaceStarsReplayed": stars_replayed,
            "witchspaceStarsUnreplayable": stars_unreplayable,
            "witchspaceStarsMismatches": stars_mismatches,
            "offscreenCensusFacts": sum(1 for site_id in by_site if site_id == 24),
            "offscreenCensusReplayed": census_replayed,
            "offscreenCensusUnreplayable": census_unreplayable,
            "offscreenCensusMismatches": census_mismatches,
            "offscreenQuadFacts": sum(1 for site_id in by_site if site_id == 26),
            "offscreenQuadReplayed": quad_replayed,
            "offscreenQuadUnreplayable": quad_unreplayable,
            "offscreenQuadMismatches": quad_mismatches,
            "holoFacts": sum(1 for site_id in by_site if site_id == 53),
            "holoReplayed": holo_replayed,
            "holoUnreplayable": holo_unreplayable,
            "holoMismatches": holo_mismatches,
            "scrimFacts": sum(1 for site_id in by_site if site_id == 55),
            "scrimReplayed": scrim_replayed,
            "scrimUnreplayable": scrim_unreplayable,
            "scrimMismatches": scrim_mismatches,
            "sunglareFacts": sum(1 for site_id in by_site if site_id in (61, 62, 63)),
            "sunglareReplayed": sunglare_replayed,
            "sunglareUnreplayable": sunglare_unreplayable,
            "sunglareMismatches": sunglare_mismatches,
            "fssFacts": sum(1 for site_id in by_site if site_id in (57, 58)),
            "fssReplayed": fss_replayed,
            "fssUnreplayable": fss_unreplayable,
            "fssMismatches": fss_mismatches,
            "remlokFacts": sum(1 for site_id in by_site if site_id in (51, 52)),
            "remlokReplayed": remlok_replayed,
            "remlokUnreplayable": remlok_unreplayable,
            "remlokMismatches": remlok_mismatches,
            "basicDrawFacts": sum(1 for site_id in by_site if site_id in (2, 67)),
            "basicDrawReplayed": basic_draw_replayed,
            "basicDrawUnreplayable": basic_draw_unreplayable,
            "basicDrawMismatches": basic_draw_mismatches,
            "eyeCensusFacts": sum(1 for site_id in by_site if site_id == 48),
            "eyeCensusReplayed": eye_census_replayed,
            "eyeCensusUnreplayable": eye_census_unreplayable,
            "eyeCensusMismatches": eye_census_mismatches,
            "resolveBindFacts": sum(1 for site_id in by_site if site_id == 60),
            "resolveBindReplayed": resolve_bind_replayed,
            "resolveBindUnreplayable": resolve_bind_unreplayable,
            "resolveBindMismatches": resolve_bind_mismatches,
            "loaderPanelFacts": sum(1 for site_id in by_site if site_id == 25),
            "loaderPanelReplayed": loader_panel_replayed,
            "loaderPanelUnreplayable": loader_panel_unreplayable,
            "loaderPanelMismatches": loader_panel_mismatches,
            "loaderPanelMutationUnobserved": loader_panel_mutation_unobserved,
            "fssDumpFacts": sum(1 for site_id in by_site if site_id == 59),
            "fssDumpReplayed": fss_dump_replayed,
            "fssDumpUnreplayable": fss_dump_unreplayable,
            "fssDumpMismatches": fss_dump_mismatches,
            "targetSharpFacts": target_sharp_facts,
            "targetSharpReplayed": target_sharp_replayed,
            "targetSharpUnreplayable": target_sharp_unreplayable,
            "targetSharpMismatches": target_sharp_mismatches,
            "targetSharpNotEligible": target_sharp_not_eligible,
            "sunglareNominationFacts": sunglare_nomination_facts,
            "sunglareNominationReplayed": sunglare_nomination_replayed,
            "sunglareNominationUnreplayable": sunglare_nomination_unreplayable,
            "sunglareNominationMismatches": sunglare_nomination_mismatches,
            "fssChromeSkipFacts": int(1 in by_site),
            "fssChromeSkipReplayed": int(1 in by_site and not fss_chrome_skip_mismatches),
            "fssChromeSkipMismatches": fss_chrome_skip_mismatches,
            "fssChromeSkipTerminal": fss_chrome_skip_terminal}


def _integer(value, label, low=0, high=0xffffffff):
    if type(value) is not int or value < low or value > high:
        raise TraceError("%s must be an integer in %d..%d" % (label, low, high))
    return value


_FORWARD_INPUT_READS = {
    "verdictOrdinal": (-0x8000, 0x7fff),
    "issueBlockedEntry": bool, "objectProbeLedgerOn": bool,
    "uiDepthThisDraw": bool, "holoDepthThisDraw": bool,
    "compositeThisDraw": bool, "curveThisDrawBeforeSkipClear": bool,
    "seedDiagnostics": bool, "uiLayerLiveEyeGate": bool,
    "uiLayerLiveFallbackGate": bool, "uiLayerWatchingGate": bool,
    "curveThisDrawCurveGate": bool,
    "engineVelocityCacheFamily": (-0x80000000, 0x7fffffff),
    "introCurveThisDrawStripGate": bool,
    "issueBlockedBeforeOriginal": bool, "originalCallReturned": bool,
    "crispPendingAfterOriginal": bool, "issueBlockedAfterOriginal": bool,
    "planetPending": bool, "planetSolarPending": bool,
}


def _forward_read(fact, name, label):
    read = fact.get(name)
    if not isinstance(read, dict) or set(read) != {"reached", "known", "value"}:
        raise TraceError(label + "." + name + " must be a raw read envelope")
    reached, known, value = read["reached"], read["known"], read["value"]
    if type(reached) is not bool or type(known) is not bool or (known and not reached):
        raise TraceError(label + "." + name + " has invalid reached/known markers")
    if not known:
        if value is not None:
            raise TraceError(label + "." + name + " unknown value must be null")
        return reached, False, None
    domain = _FORWARD_INPUT_READS[name]
    if domain is bool:
        if type(value) is not bool:
            raise TraceError(label + "." + name + " must contain a boolean")
    else:
        if type(value) is not int or value < domain[0] or value > domain[1]:
            raise TraceError(label + "." + name + " is outside its source domain")
    return reached, True, value


def _replay_forward_inputs(observation, draw, label):
    """Derive the supported None/Skip forwarder-local action from raw inputs.

    This deliberately does not model nested D3D work performed by the real
    draw callback, nor the complete hook/selector ladder.
    """
    if not isinstance(observation, dict):
        return {"status": "unavailable", "reason": "missing observation", "mismatches": 0}
    expected_keys = {"kind", "version"} | set(_FORWARD_INPUT_READS)
    if set(observation) != expected_keys:
        raise TraceError(label + " has missing or unexpected ForwardingObservation fields")
    if (type(observation.get("kind")) is not int or observation["kind"] != 1 or
            type(observation.get("version")) is not int or observation["version"] != 1):
        raise TraceError(label + " has an unsupported ForwardingObservation kind/version")
    reads = {name: _forward_read(observation, name, label)
             for name in _FORWARD_INPUT_READS}

    # Resolve owner solely from site 2's BasicDraw raw context identities.
    owner = None
    for fact in draw.get("predicateFacts", []):
        if isinstance(fact, dict) and fact.get("siteId") == 2 and fact.get("kind") == 15:
            context = fact.get("context")
            if isinstance(context, dict):
                self_read = _sunglare_read(context, "contextIdentity", label + ".site2", int,
                                            (1 << 64) - 1)
                owner_read = _sunglare_read(context, "ownerContextIdentity", label + ".site2", int,
                                             (1 << 64) - 1)
                if self_read[0] and self_read[1] and owner_read[0] and owner_read[1]:
                    owner = self_read[2] == owner_read[2]
            break
    if owner is None:
        return {"status": "unavailable", "reason": "site-2 owner identities unavailable",
                "mismatches": 0}

    def expect_reached(name, expected):
        if reads[name][0] != expected:
            raise TraceError(label + "." + name + " reachability disagrees with forwarding source order")

    expect_reached("verdictOrdinal", True)
    if not reads["verdictOrdinal"][1]:
        return {"status": "unavailable", "reason": "verdict ordinal unknown", "mismatches": 0}
    ordinal = reads["verdictOrdinal"][2]
    if ordinal not in (0, 2):
        return {"status": "unavailable", "reason": "unsupported verdict ordinal",
                "mismatches": 0, "verdictOrdinal": ordinal}
    if draw.get("route") not in (4, 6) or draw.get("kind") not in (
            ord("D"), ord("I"), ord("N"), ord("X")):
        return {"status": "unavailable", "reason": "unsupported route or draw command",
                "mismatches": 0, "verdictOrdinal": ordinal}

    expect_reached("issueBlockedEntry", owner)
    blocked_entry = reads["issueBlockedEntry"]
    if owner and blocked_entry[1] and blocked_entry[2]:
        # This early swallow is outside the supported all-side-work-off slice.
        return {"status": "unavailable", "reason": "issue-blocked entry path",
                "mismatches": 0, "verdictOrdinal": ordinal}
    if owner and not blocked_entry[1]:
        return {"status": "unavailable", "reason": "issue-blocked entry unknown",
                "mismatches": 0, "verdictOrdinal": ordinal}
    expect_reached("objectProbeLedgerOn", owner)
    object_gate = reads["objectProbeLedgerOn"]
    if owner and (not object_gate[1] or object_gate[2]):
        return {"status": "unavailable", "reason": "object-probe ledger active or unknown",
                "mismatches": 0, "verdictOrdinal": ordinal}

    for name in ("uiDepthThisDraw", "holoDepthThisDraw", "compositeThisDraw"):
        expect_reached(name, True)
        if not reads[name][1] or reads[name][2]:
            return {"status": "unavailable", "reason": "depth/composite side-work active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}

    if not owner:
        return {"status": "unavailable", "reason": "foreign-context forwarding is outside supported scope",
                "mismatches": 0, "verdictOrdinal": ordinal}

    expected_actions = None
    if ordinal == 2:
        expect_reached("curveThisDrawBeforeSkipClear", True)
        if not reads["curveThisDrawBeforeSkipClear"][1]:
            return {"status": "unavailable", "reason": "skip curve flag unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        if reads["curveThisDrawBeforeSkipClear"][2]:
            return {"status": "unavailable", "reason": "skip curve-clear mutation outside supported scope",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        for name in ("seedDiagnostics", "uiLayerLiveEyeGate", "uiLayerLiveFallbackGate",
                     "uiLayerWatchingGate", "curveThisDrawCurveGate", "engineVelocityCacheFamily",
                     "introCurveThisDrawStripGate", "issueBlockedBeforeOriginal",
                     "originalCallReturned", "crispPendingAfterOriginal",
                     "issueBlockedAfterOriginal", "planetPending", "planetSolarPending"):
            expect_reached(name, False)
        expected_actions = _forward_draw_envelope(draw) + [
            _forward_action(3, 2, 2, draw, 0)] + _forward_draw_end(draw)
        return {"status": "eligible", "expectedActions": expected_actions,
                "mismatches": 0, "verdictOrdinal": ordinal, "owner": owner,
                "skipCurveClear": reads["curveThisDrawBeforeSkipClear"][2],
                "terminalProof": "unavailable", "alteredDrawClass": "not-classified"}
    else:
        expect_reached("curveThisDrawBeforeSkipClear", False)
        expect_reached("seedDiagnostics", owner)
        if owner and (not reads["seedDiagnostics"][1] or reads["seedDiagnostics"][2]):
            return {"status": "unavailable", "reason": "seed diagnostics active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("uiLayerLiveEyeGate", owner)
        eye_live = reads["uiLayerLiveEyeGate"]
        if owner and not eye_live[1]:
            return {"status": "unavailable", "reason": "live eye gate unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        if owner and eye_live[2]:
            return {"status": "unavailable", "reason": "live eye gate active",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        ui_layer = False
        expect_reached("uiLayerLiveFallbackGate", owner and not ui_layer)
        fallback = reads["uiLayerLiveFallbackGate"]
        if owner and not ui_layer and (not fallback[1] or fallback[2]):
            return {"status": "unavailable", "reason": "live fallback gate active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("uiLayerWatchingGate", owner and not ui_layer)
        watching = reads["uiLayerWatchingGate"]
        if owner and not ui_layer and (not watching[1] or watching[2]):
            return {"status": "unavailable", "reason": "UI watching gate active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("curveThisDrawCurveGate", True)
        curve = reads["curveThisDrawCurveGate"]
        if not curve[1] or curve[2]:
            return {"status": "unavailable", "reason": "curve substitution active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("engineVelocityCacheFamily", True)
        family = reads["engineVelocityCacheFamily"]
        if not family[1]:
            return {"status": "unavailable", "reason": "engine velocity family unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("introCurveThisDrawStripGate", True)
        strip = reads["introCurveThisDrawStripGate"]
        if not strip[1] or strip[2]:
            return {"status": "unavailable", "reason": "intro strip active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("issueBlockedBeforeOriginal", owner)
        before = reads["issueBlockedBeforeOriginal"]
        if owner and (not before[1] or before[2]):
            return {"status": "unavailable", "reason": "pre-original issue block active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("originalCallReturned", True)
        callback = reads["originalCallReturned"]
        if not callback[1]:
            return {"status": "unavailable", "reason": "external callback result unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("crispPendingAfterOriginal", callback[2])
        crisp = reads["crispPendingAfterOriginal"]
        if callback[2] and (not crisp[1] or crisp[2]):
            return {"status": "unavailable", "reason": "crisp reissue active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("issueBlockedAfterOriginal", owner)
        after = reads["issueBlockedAfterOriginal"]
        if owner and not after[1]:
            return {"status": "unavailable", "reason": "post-original issue block active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        if owner and after[2]:
            expect_reached("planetPending", False)
            expect_reached("planetSolarPending", False)
            return {"status": "unavailable", "reason": "post-original issue block active",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("planetPending", owner)
        planet = reads["planetPending"]
        if owner and not planet[1]:
            return {"status": "unavailable", "reason": "planet side-work active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expect_reached("planetSolarPending", owner and not planet[2])
        if owner and planet[2]:
            return {"status": "unavailable", "reason": "planet side-work active",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        solar = reads["planetSolarPending"]
        if owner and (not solar[1] or solar[2]):
            return {"status": "unavailable", "reason": "solar planet side-work active or unknown",
                    "mismatches": 0, "verdictOrdinal": ordinal}
        expected_actions = _forward_draw_envelope(draw) + [
            _forward_original_action(draw, callback[2])] + _forward_draw_end(draw)
        return {"status": "eligible", "expectedActions": expected_actions,
                "mismatches": 0, "verdictOrdinal": ordinal,
                "callbackReturned": callback[2], "engineVelocityCacheFamily": family[2],
                "owner": owner, "terminalProof": "unavailable",
                "alteredDrawClass": "pool-family" if family[2] >= 0 else "none"}
    return {"status": "eligible", "expectedActions": expected_actions,
            "mismatches": 0, "verdictOrdinal": ordinal,
            "terminalProof": "unavailable"}


def _forward_action(action_id, phase, outcome, draw, issue_count):
    call = {ord("D"): 1, ord("I"): 2, ord("N"): 3, ord("X"): 4}.get(draw["kind"])
    if call is None:
        raise TraceError("forwarding-local replay requires a direct draw command")
    start = (draw["args"]["base"] & 0xffffffff) if draw["kind"] in (ord("D"), ord("N")) else draw["args"]["start"]
    base_vertex = 0 if draw["kind"] in (ord("D"), ord("N")) else draw["args"]["base"]
    return {"id": action_id, "phase": phase, "outcome": outcome, "call": call,
            "flags": 0, "issueCount": issue_count, "issueCountKnown": True,
            "count": draw["count"], "instances": draw["instances"],
            "start": start, "startInstance": draw["args"]["startInstance"],
            "baseVertex": base_vertex}


def _forward_draw_envelope(draw):
    return [_forward_action(1, 1, 2, draw, 0)]


def _forward_original_action(draw, returned):
    return _forward_action(2, 2, 2 if returned else 3, draw, 1 if returned else 0)


def _forward_draw_end(draw):
    return [_forward_action(17, 3, 2, draw, 0)]


def _fss_chrome_skip_action_plan(draw, issue_blocked_entry=False):
    """Exact swallowed-original plan when the independently replayed site 1 skips."""
    if issue_blocked_entry is not False:
        raise TraceError("site-1 terminal fixture must have issueBlockedEntry false")
    return (_forward_draw_envelope(draw) +
            [_forward_action(3, 2, 2, draw, 0)] + _forward_draw_end(draw))


def _flat_bypass_action_plan(draw):
    """Replay the flat profile's direct original call from recorded draw facts."""
    kind = draw["kind"]
    call = {ord("D"): 1, ord("I"): 2, ord("N"): 3, ord("X"): 4,
            ord("A"): 5, ord("Z"): 6, ord("Y"): 7}[kind]
    action_id = {ord("D"): 2, ord("I"): 2, ord("N"): 2, ord("X"): 2,
                 ord("A"): 18, ord("Z"): 19, ord("Y"): 20}[kind]
    count, instances = draw["count"], draw["instances"]
    args = draw["args"]
    if kind in (ord("D"), ord("N")):
        start = args["base"] & 0xffffffff
        start_instance, base_vertex = args["startInstance"], 0
    elif kind in (ord("I"), ord("X")):
        start, start_instance, base_vertex = args["start"], args["startInstance"], args["base"]
    elif kind in (ord("Y"), ord("Z")):
        start, start_instance, base_vertex = draw["argumentByteOffset"], 0, 0
    else:
        start, start_instance, base_vertex = 0, 0, 0

    def action(action_id, phase, outcome, flags, issue_count):
        return {"id": action_id, "phase": phase, "outcome": outcome, "call": call,
                "flags": flags, "issueCount": issue_count, "issueCountKnown": True,
                "count": count, "instances": instances, "start": start,
                "startInstance": start_instance, "baseVertex": base_vertex}

    gpu_args_unavailable = (FLAG_BITS["action"]["gpuDrawArgsUnavailable"]
                            if kind in (ord("A"), ord("Y"), ord("Z")) else 0)
    return [action(1, 1, 2, 0, 0),
            action(action_id, 2, 2, gpu_args_unavailable, 1),
            action(17, 3, 2, 0, 0)]


def validate_trace(data, expected_log=None, expected_build_stamp=None):
    if not isinstance(data, dict):
        raise TraceError("sidecar root must be an object")
    if data.get("format") != FORMAT:
        raise TraceError("unknown trace format")
    if type(data.get("schemaVersion")) is not int or data["schemaVersion"] not in (1, SCHEMA_VERSION):
        raise TraceError("unsupported schemaVersion")
    schema_version = data["schemaVersion"]
    if schema_version == SCHEMA_VERSION:
        if type(data.get("predicateFactVersion")) is not int or data["predicateFactVersion"] not in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, PREDICATE_FACT_VERSION):
            raise TraceError("unsupported predicateFactVersion")
        predicate_fact_version = data["predicateFactVersion"]
        forward_input_version = data.get("forwardInputVersion", 0)
        if type(forward_input_version) is not int or forward_input_version not in (0, 1):
            raise TraceError("unsupported forwardInputVersion")
    elif "predicateFactVersion" in data or "forwardInputVersion" in data:
        raise TraceError("schemaVersion 1 cannot declare predicate or forwarding inputs")
    else:
        predicate_fact_version = 0
        forward_input_version = 0
    if forward_input_version and schema_version != SCHEMA_VERSION:
        raise TraceError("schemaVersion 1 cannot declare forwarding inputs")
    version = data.get("buildVersion")
    if not isinstance(version, str) or not version or len(version) > 128:
        raise TraceError("buildVersion is missing or invalid")
    stamp = data.get("buildStamp")
    if not isinstance(stamp, str) or not re.match(r"^[0-9A-Fa-f]{8}$", stamp):
        raise TraceError("buildStamp must be eight hexadecimal digits")
    log_name = data.get("logFile")
    if (not isinstance(log_name, str) or not log_name or
            os.path.basename(log_name) != log_name or
            "/" in log_name or "\\" in log_name):
        raise TraceError("logFile must be a basename")
    if expected_log is not None and log_name.lower() != os.path.basename(expected_log).lower():
        raise TraceError("sidecar belongs to a different graphics log")
    if expected_build_stamp is not None and stamp.upper() != expected_build_stamp.upper():
        raise TraceError("sidecar build stamp does not match the graphics log")

    semantics = data.get("semantics")
    if (not isinstance(semantics, dict) or
            semantics.get("equivalence") != "observed-selector-and-action-order" or
            semantics.get("predicateEquivalence") is not False):
        raise TraceError("sidecar must state action/selector scope and predicate limitation")
    flag_bits = semantics.get("flagBits")
    if not isinstance(flag_bits, dict) or set(flag_bits) != set(FLAG_BITS):
        raise TraceError("sidecar flagBits metadata is missing or incomplete")
    for group, expected in FLAG_BITS.items():
        actual = flag_bits.get(group)
        if (not isinstance(actual, dict) or set(actual) != set(expected) or
                any(type(actual.get(name)) is not int or actual[name] != bit
                    for name, bit in expected.items())):
            raise TraceError("sidecar flagBits.%s does not match schema" % group)

    frame = data.get("frame")
    if not isinstance(frame, dict):
        raise TraceError("frame metadata is missing")
    frame_no = _integer(frame.get("frameNo"), "frame.frameNo")
    completed = _integer(frame.get("completedFrameNo"), "frame.completedFrameNo")
    if frame_no != completed:
        raise TraceError("capture does not cover one complete owner frame")
    for key in ("configEpoch", "eyeDrawsThisFrame", "eyeDrawsLastFrame",
                "sceneDrawsThisFrame", "stateFlags"):
        _integer(frame.get(key), "frame." + key)
    counters = frame.get("sceneCounters")
    if not isinstance(counters, list) or len(counters) != 4:
        raise TraceError("frame.sceneCounters must contain four counters")
    for i, value in enumerate(counters):
        _integer(value, "frame.sceneCounters[%d]" % i)

    draws = data.get("draws")
    if not isinstance(draws, list) or len(draws) > MAX_DRAWS:
        raise TraceError("draws must be a bounded array")
    route_counts = {}
    total_sites = 0
    total_actions = 0
    inferred_unvisited = 0
    predicate_replay = {"factCount": 0, "replayed": 0,
                        "unreplayable": 0, "mismatches": 0,
                        "mutationUnobserved": 0, "nightVisionFacts": 0,
                        "nightVisionReplayed": 0,
                        "nightVisionUnreplayable": 0,
                        "nightVisionMismatches": 0,
                        "witchspaceStarsFacts": 0,
                        "witchspaceStarsReplayed": 0,
                        "witchspaceStarsUnreplayable": 0,
                        "witchspaceStarsMismatches": 0,
                        "offscreenCensusFacts": 0,
                        "offscreenCensusReplayed": 0,
                        "offscreenCensusUnreplayable": 0,
                        "offscreenCensusMismatches": 0,
                        "offscreenQuadFacts": 0,
                        "offscreenQuadReplayed": 0,
                        "offscreenQuadUnreplayable": 0,
                        "offscreenQuadMismatches": 0,
                        "holoFacts": 0, "holoReplayed": 0,
                        "holoUnreplayable": 0, "holoMismatches": 0,
                        "scrimFacts": 0, "scrimReplayed": 0,
                        "scrimUnreplayable": 0, "scrimMismatches": 0,
                        "sunglareFacts": 0, "sunglareReplayed": 0,
                        "sunglareUnreplayable": 0, "sunglareMismatches": 0,
                        "fssFacts": 0, "fssReplayed": 0,
                        "fssUnreplayable": 0, "fssMismatches": 0,
                        "remlokFacts": 0, "remlokReplayed": 0,
                        "remlokUnreplayable": 0, "remlokMismatches": 0,
                        "basicDrawFacts": 0, "basicDrawReplayed": 0,
                        "basicDrawUnreplayable": 0, "basicDrawMismatches": 0,
                        "eyeCensusFacts": 0, "eyeCensusReplayed": 0,
                        "eyeCensusUnreplayable": 0, "eyeCensusMismatches": 0,
                        "resolveBindFacts": 0, "resolveBindReplayed": 0,
                        "resolveBindUnreplayable": 0, "resolveBindMismatches": 0,
                        "loaderPanelFacts": 0, "loaderPanelReplayed": 0,
                        "loaderPanelUnreplayable": 0, "loaderPanelMismatches": 0,
                        "loaderPanelMutationUnobserved": 0,
                        "fssDumpFacts": 0, "fssDumpReplayed": 0,
                        "fssDumpUnreplayable": 0, "fssDumpMismatches": 0,
                        "targetSharpFacts": 0, "targetSharpReplayed": 0,
                        "targetSharpUnreplayable": 0, "targetSharpMismatches": 0,
                        "targetSharpNotEligible": 0,
                        "sunglareNominationFacts": 0,
                        "sunglareNominationReplayed": 0,
                        "sunglareNominationUnreplayable": 0,
                        "sunglareNominationMismatches": 0,
                        "fssChromeSkipFacts": 0,
                        "fssChromeSkipReplayed": 0,
                        "fssChromeSkipMismatches": 0,
                        "fssChromeSkipTerminal": 0}
    forward_replay = {"factCount": 0, "replayed": 0, "unavailable": 0,
                      "mismatches": 0, "eligible": 0, "expectedActions": 0,
                      "observedActionMismatches": 0, "forwardFactsMismatches": 0,
                      "terminalProofAvailable": 0, "terminalProofUnavailable": 0,
                      "externalCallbackInvocations": 0,
                      "externalCallbackWorkUnobserved": 0, "originalActionIssues": 0,
                      "poolFamilyDraws": 0,
                      "alteredDrawClassCounts": {"none": 0, "pool-family": 0,
                                                 "not-classified": 0}}
    for index, draw in enumerate(draws):
        label = "draws[%d]" % index
        if not isinstance(draw, dict):
            raise TraceError(label + " must be an object")
        if _integer(draw.get("index"), label + ".index") != index:
            raise TraceError(label + " index is not contiguous")
        for key in ("eyeDrawIndex", "count", "instances", "rtv0Generation",
                    "dsv0Generation", "rtv0Width", "rtv0Height", "flags"):
            _integer(draw.get(key), label + "." + key)
        kind = _integer(draw.get("kind"), label + ".kind", 0, 255)
        if kind not in (ord("D"), ord("I"), ord("N"), ord("X"),
                        ord("A"), ord("Z"), ord("Y")):
            raise TraceError(label + " has an unknown draw kind")
        if draw.get("command") != DRAW_COMMANDS[kind]:
            raise TraceError(label + " command name does not match its kind")
        args = draw.get("args")
        if not isinstance(args, dict):
            raise TraceError(label + ".args is missing")
        _integer(args.get("start"), label + ".args.start")
        _integer(args.get("base"), label + ".args.base", -0x80000000, 0x7fffffff)
        _integer(args.get("startInstance"), label + ".args.startInstance")
        for key in ("vsHash", "psHash", "candidateMask"):
            value = draw.get(key)
            if not isinstance(value, str) or not re.match(r"^[0-9A-Fa-f]{16}$", value):
                raise TraceError(label + "." + key + " must be sixteen hex digits")
        resources = draw.get("resources")
        if not isinstance(resources, dict):
            raise TraceError(label + ".resources is missing")
        for key in ("vs", "ps", "rtv0", "dsv0", "argumentBuffer"):
            _integer(resources.get(key), label + ".resources." + key, 0, 6 * MAX_DRAWS)
        argument_buffer_known = draw.get("argumentBufferKnown")
        draw_parameters_known = draw.get("drawParametersKnown")
        if type(argument_buffer_known) is not bool or type(draw_parameters_known) is not bool:
            raise TraceError(label + " must state argument and draw-parameter availability")
        argument_offset = _integer(draw.get("argumentByteOffset"),
                                   label + ".argumentByteOffset")
        if kind in (ord("Y"), ord("Z")):
            if not argument_buffer_known or draw_parameters_known:
                raise TraceError(label + " indirect command availability is inconsistent")
        elif argument_buffer_known or resources["argumentBuffer"] != 0 or argument_offset != 0:
            raise TraceError(label + " has an argument buffer on a non-indirect command")
        if kind == ord("A"):
            if draw_parameters_known:
                raise TraceError(label + " DrawAuto parameters must be marked unavailable")
        elif kind in (ord("D"), ord("I"), ord("N"), ord("X")) and not draw_parameters_known:
            raise TraceError(label + " direct command parameters must be recorded")
        route = _integer(draw.get("route"), label + ".route", 1, 11)
        sequence = _integer(draw.get("sequence"), label + ".sequence", 1, 9)
        ordered = SEQUENCES.get((route, sequence))
        if ordered is None:
            raise TraceError(label + " has an unsupported route/sequence pair")
        expected_route = {
            ord("A"): (8, 6), ord("Z"): (9, 7), ord("Y"): (10, 8),
        }.get(kind)
        if expected_route and route != 7 and (route, sequence) != expected_route:
            raise TraceError(label + " bypass command does not match its route")
        if (route == 7 and kind in (ord("A"), ord("Y"), ord("Z")) and
                (draw["count"] != 0 or draw["instances"] != 0)):
            raise TraceError(label + " flat Auto/indirect source counts must be zero")
        if kind in (ord("D"), ord("I"), ord("N"), ord("X")) and route in (8, 9, 10):
            raise TraceError(label + " classifier command uses a bypass route")
        if (kind in (ord("A"), ord("Y"), ord("Z"))) and draw_parameters_known:
            raise TraceError(label + " GPU-derived draw parameters must be unavailable")
        route_counts[route] = route_counts.get(route, 0) + 1

        sites = draw.get("sites")
        if not isinstance(sites, list) or len(sites) > MAX_SITE_EVENTS:
            raise TraceError(label + ".sites exceeds the fixed event capacity")
        total_sites += len(sites)
        seen = set()
        last_position = -1
        stopped = False
        terminal_id = -1
        terminal_verdict = -1
        for event_index, event in enumerate(sites):
            event_label = "%s.sites[%d]" % (label, event_index)
            if not isinstance(event, dict):
                raise TraceError(event_label + " must be an object")
            site_id = _integer(event.get("id"), event_label + ".id", 1, 76)
            if site_id not in ordered or site_id in seen:
                raise TraceError(event_label + " is not a unique member of its route sequence")
            pos = ordered.index(site_id)
            if pos != event_index:
                raise TraceError(label + " reached sites are not a contiguous canonical prefix")
            if stopped:
                raise TraceError(label + " contains a visit after a terminal site")
            seen.add(site_id)
            last_position = pos
            site_kind = _integer(event.get("kind"), event_label + ".kind", 1, 3)
            if SITE_KINDS.get(site_id) != site_kind:
                raise TraceError(event_label + " kind disagrees with stable site metadata")
            outcome = _integer(event.get("outcome"), event_label + ".outcome", 1, 5)
            flow = _integer(event.get("flow"), event_label + ".flow", 0, 1)
            _integer(event.get("subsite"), event_label + ".subsite", 0, 65535)
            verdict = _integer(event.get("verdict"), event_label + ".verdict", -1, 18)
            if outcome == 3 and site_kind != 2:
                raise TraceError(event_label + " Claimed outcome requires a Claim site")
            if outcome == 4 and site_id not in TERMINAL_VERDICTS:
                raise TraceError(event_label + " Exited outcome requires a terminal site")
            if outcome == 5 and site_id not in NOT_ELIGIBLE_SITES:
                raise TraceError(event_label + " NotEligible outcome is not allowed for this site")
            if outcome in (3, 4):
                if flow != 1:
                    raise TraceError(event_label + " terminal outcome must stop the ladder")
                if site_id not in TERMINAL_VERDICTS:
                    raise TraceError(event_label + " has no stable terminal verdict mapping")
                if verdict != TERMINAL_VERDICTS[site_id]:
                    raise TraceError(event_label + " verdict does not match its terminal site")
                stopped = True
                terminal_id = site_id
                terminal_verdict = verdict
            else:
                if flow != 0:
                    raise TraceError(event_label + " nonterminal outcome cannot stop the ladder")
                if verdict != -1:
                    raise TraceError(event_label + " nonterminal site must use verdict -1")
        if not sites or not stopped:
            raise TraceError(label + " is missing its terminal claim/exit event")
        inferred_unvisited += len(ordered) - last_position - 1

        winner = _integer(draw.get("winnerSiteId"), label + ".winnerSiteId", -1, 32767)
        verdict = _integer(draw.get("verdict"), label + ".verdict", -1, 18)
        terminal_outcome = sites[-1]["outcome"]
        # Despite the legacy field name, winnerSiteId is the terminal stop
        # site for both claims and exits. Keeping exit identity distinguishes
        # the several None/Skip reasons in recorded traces.
        if terminal_id != winner:
            raise TraceError(label + " terminal site does not match winnerSiteId")
        if terminal_id not in TERMINAL_VERDICTS:
            raise TraceError(label + " terminal site has no stable verdict mapping")
        if terminal_verdict != TERMINAL_VERDICTS[terminal_id]:
            raise TraceError(label + " terminal site verdict does not match its stable mapping")
        if terminal_verdict != verdict:
            raise TraceError(label + " final verdict differs from terminal site result")

        if schema_version == SCHEMA_VERSION:
            replay = _replay_predicate_facts(draw, label, predicate_fact_version)
            for key in predicate_replay:
                predicate_replay[key] += replay[key]
        elif "predicateFacts" in draw:
            raise TraceError(label + " schemaVersion 1 cannot contain predicateFacts")

        forwarding_observation = None
        if forward_input_version == 1:
            if "forwardInputsReached" not in draw or type(draw["forwardInputsReached"]) is not bool:
                raise TraceError(label + ".forwardInputsReached must be present in forwarding-input version 1")
            if "forwardInputs" not in draw:
                raise TraceError(label + " is missing the required forwardInputs key")
            forwarding_observation = draw["forwardInputs"]
            if draw["forwardInputsReached"] != (forwarding_observation is not None):
                raise TraceError(label + " forwarding-input reached marker disagrees with fact cardinality")
            if forwarding_observation is not None and not isinstance(forwarding_observation, dict):
                raise TraceError(label + ".forwardInputs must be an object or null")
        if "forwardFacts" not in draw:
            raise TraceError(label + " must explicitly mark forwardFacts present or null")
        facts = draw["forwardFacts"]
        if facts is not None:
            if not isinstance(facts, dict):
                raise TraceError(label + ".forwardFacts must be an object or null")
            present = _integer(facts.get("presentMask"), label + ".forwardFacts.presentMask", 0, (1 << 14) - 1)
            facts_verdict = _integer(facts.get("verdictOrdinal"), label + ".forwardFacts.verdictOrdinal", -1, 18)
            _integer(facts.get("family"), label + ".forwardFacts.family", 0, 65535)
            verdict_bit = FLAG_BITS["forwardFacts"]["verdict"]
            if bool(present & verdict_bit) != (facts_verdict != -1):
                raise TraceError(label + ".forwardFacts verdict availability disagrees with mask")
            if present & verdict_bit and facts_verdict != verdict and forward_input_version != 1:
                raise TraceError(label + ".forwardFacts verdict differs from final draw verdict")
            for field, bit in FORWARD_TRI_FIELDS.items():
                value = facts.get(field)
                if value not in TRI_STATES:
                    raise TraceError(label + ".forwardFacts." + field + " is invalid")
                if bool(present & bit) != (value != "unknown"):
                    raise TraceError(label + ".forwardFacts." + field + " availability disagrees with mask")

        actions = draw.get("actions")
        if not isinstance(actions, list) or len(actions) > MAX_ACTION_EVENTS:
            raise TraceError(label + ".actions exceeds the fixed event capacity")
        if schema_version == SCHEMA_VERSION and replay["fssChromeSkipTerminal"] and actions != _fss_chrome_skip_action_plan(draw):
            raise TraceError(label + " site-1 Skip action plan differs from swallowed original")
        total_actions += len(actions)
        for action_index, action in enumerate(actions):
            action_label = "%s.actions[%d]" % (label, action_index)
            if not isinstance(action, dict):
                raise TraceError(action_label + " must be an object")
            action_id = _integer(action.get("id"), action_label + ".id", 1, 20)
            phase = _integer(action.get("phase"), action_label + ".phase", 1, 6)
            _integer(action.get("outcome"), action_label + ".outcome", 1, 3)
            call = _integer(action.get("call"), action_label + ".call", 1, 7)
            _integer(action.get("flags"), action_label + ".flags", 0, 65535)
            _integer(action.get("issueCount"), action_label + ".issueCount", 0, 65535)
            issue_count_known = action.get("issueCountKnown")
            if (type(issue_count_known) is not bool or
                    issue_count_known != (action["flags"] & FLAG_BITS["action"]["issueCountUnknown"] == 0)):
                raise TraceError(action_label + ".issueCountKnown disagrees with action flags")
            if not issue_count_known and action["issueCount"] != 0:
                raise TraceError(action_label + " unknown issueCount must be zero")
            gpu_args_unavailable = bool(
                action["flags"] & FLAG_BITS["action"]["gpuDrawArgsUnavailable"])
            if gpu_args_unavailable != (action["phase"] == 2 and call in (5, 6, 7)):
                raise TraceError(action_label + " GPU-argument availability disagrees with call kind")
            for key in ("count", "instances", "start", "startInstance"):
                _integer(action.get(key), action_label + "." + key)
            _integer(action.get("baseVertex"), action_label + ".baseVertex",
                     -0x80000000, 0x7fffffff)
            generated_args_unavailable = bool(
                action["flags"] & FLAG_BITS["action"]["generatedDrawArgsUnavailable"])
            generated_action_requires_flag = (
                (action_id == 6 and phase == 2) or
                (action_id == 4 and phase == 2 and not issue_count_known))
            if generated_action_requires_flag and not generated_args_unavailable:
                raise TraceError(action_label +
                                 " helper-generated draw arguments must be marked unavailable")
            if generated_args_unavailable:
                if (phase != 2 or issue_count_known or action_id not in (4, 6, 7, 12) or
                        call != 4 or any(action[key] != 0 for key in
                                         ("count", "instances", "start", "startInstance", "baseVertex"))):
                    raise TraceError(action_label +
                                     " generated draw arguments must be explicitly unavailable and zeroed")

        if len(actions) < 2:
            raise TraceError(label + " is missing draw begin/end action records")
        begin, end = actions[0], actions[-1]
        if (begin["id"], begin["phase"], begin["outcome"]) != (1, 1, 2):
            raise TraceError(label + " draw begin action is missing or malformed")
        if (end["id"], end["phase"], end["outcome"]) != (17, 3, 2):
            raise TraceError(label + " draw end action is missing or malformed")
        if any(action["id"] in (1, 17) for action in actions[1:-1]):
            raise TraceError(label + " draw begin/end action is out of order")
        bypass_original = None
        if route == 7:
            bypass_original = {
                ord("D"): (2, 1), ord("I"): (2, 2), ord("N"): (2, 3),
                ord("X"): (2, 4), ord("A"): (18, 5), ord("Z"): (19, 6),
                ord("Y"): (20, 7),
            }[kind]
        elif route == 8:
            bypass_original = (18, 5)
        elif route == 9:
            bypass_original = (19, 6)
        elif route == 10:
            bypass_original = (20, 7)
        elif route == 11:
            bypass_original = (2, None)
        if bypass_original:
            if len(actions) != 3:
                raise TraceError(label + " bypass route must contain only begin, original, end")
            original = actions[1]
            if (original["id"], original["phase"]) != (bypass_original[0], 2):
                raise TraceError(label + " bypass original action is missing or malformed")
            if bypass_original[1] is not None and original["call"] != bypass_original[1]:
                raise TraceError(label + " bypass original action call kind is wrong")
            if original["outcome"] == 2:
                if not original["issueCountKnown"] or original["issueCount"] != 1:
                    raise TraceError(label + " applied bypass original must record one known issue")
            elif original["outcome"] == 3:
                terminal_subsite = sites[-1]["subsite"]
                if (route not in (8, 9, 10) or terminal_subsite != 1 or
                        not original["issueCountKnown"] or original["issueCount"] != 0):
                    raise TraceError(label + " declined bypass original lacks a blocked-route marker")
            else:
                raise TraceError(label + " bypass original outcome must be applied or declined")
            expected_command_action = {ord("A"): 18, ord("Z"): 19,
                                       ord("Y"): 20}.get(kind)
            if expected_command_action and original["id"] != expected_command_action:
                raise TraceError(label + " bypass action ID does not match command")
        if route == 7 and actions != _flat_bypass_action_plan(draw):
            raise TraceError(label + " flat bypass actions disagree with recorded draw arguments")
        if facts is not None and facts.get("issueBlocked") == "yes" and forward_input_version != 1:
            blocked = [action for action in actions
                       if action["id"] == 3 and action["phase"] == 2]
            if (len(blocked) != 1 or blocked[0]["outcome"] != 3 or
                    blocked[0]["issueCount"] != 0 or not blocked[0]["issueCountKnown"]):
                raise TraceError(label + " issueBlocked fact lacks its declined swallow action")

        if forward_input_version == 1:
            forward_replay["factCount"] += int(forwarding_observation is not None)
            local = _replay_forward_inputs(forwarding_observation, draw,
                                           label + ".forwardInputs")
            if local["status"] == "unavailable":
                forward_replay["unavailable"] += 1
            else:
                forward_replay["eligible"] += 1
                forward_replay["replayed"] += 1
                forward_replay["expectedActions"] += len(local["expectedActions"])
                expected_actions = local["expectedActions"]
                forward_replay["alteredDrawClassCounts"][local["alteredDrawClass"]] += 1
                if actions != expected_actions:
                    forward_replay["mismatches"] += 1
                    forward_replay["observedActionMismatches"] += 1
                if local.get("terminalProof") == "available":
                    forward_replay["terminalProofAvailable"] += 1
                    if local["verdictOrdinal"] != local["terminalVerdict"]:
                        forward_replay["mismatches"] += 1
                else:
                    forward_replay["terminalProofUnavailable"] += 1
                if "callbackReturned" in local:
                    forward_replay["externalCallbackInvocations"] += 1
                    forward_replay["externalCallbackWorkUnobserved"] += 1
                    forward_replay["originalActionIssues"] += int(local["callbackReturned"])
                if local.get("engineVelocityCacheFamily", -1) >= 0:
                    forward_replay["poolFamilyDraws"] += 1
                if (facts is not None and local.get("verdictOrdinal") is not None):
                    fwd_mismatch = False
                    if present & FLAG_BITS["forwardFacts"]["verdict"]:
                        fwd_mismatch |= facts_verdict != local["verdictOrdinal"]
                    if present & FLAG_BITS["forwardFacts"]["owner"]:
                        fwd_mismatch |= facts["owner"] != ("yes" if local.get("owner") else "no")
                    raw_fact_fields = {
                        "uiDepth": "uiDepthThisDraw", "holoDepth": "holoDepthThisDraw",
                        "composite": "compositeThisDraw",
                    }
                    for field, raw_name in raw_fact_fields.items():
                        if (present & FLAG_BITS["forwardFacts"][field] and
                                local.get("status") == "eligible"):
                            raw_value = _forward_read(forwarding_observation, raw_name,
                                                      label + ".forwardInputs")
                            if raw_value[1]:
                                fwd_mismatch |= facts[field] != ("yes" if raw_value[2] else "no")
                    if facts.get("issueBlocked") == "yes":
                        fwd_mismatch = True
                    if fwd_mismatch:
                        forward_replay["mismatches"] += 1
                        forward_replay["forwardFactsMismatches"] += 1

    footer = data.get("footer")
    if not isinstance(footer, dict):
        raise TraceError("capture footer is missing; sidecar may be truncated")
    if footer.get("complete") is not True or footer.get("truncated") is not False or footer.get("overflow") is not False:
        raise TraceError("capture is incomplete, truncated, or overflowed")
    if _integer(footer.get("drawCount"), "footer.drawCount") != len(draws):
        raise TraceError("footer drawCount does not match recorded draws")

    if route_counts and all(route == 7 for route in route_counts):
        scope = "flat bypass/actions; no VR classifier parity"
    elif route_counts and all(route == 8 for route in route_counts):
        scope = "DrawAuto bypass/actions; GPU-derived counts unavailable; no VR classifier parity"
    elif route_counts and all(route in (9, 10) for route in route_counts):
        scope = "indirect-draw bypass/actions; GPU argument contents unavailable; no VR classifier parity"
    elif route_counts and all(route == 11 for route in route_counts):
        scope = "internal/world bypass actions; nested originals remain in parent draw; no classifier parity"
    elif (6 in route_counts or 4 in route_counts) and any(route >= 7 for route in route_counts):
        scope = "observed VR selector/action order plus non-classifier bypass samples; predicate equivalence not established"
    elif 6 in route_counts or 4 in route_counts:
        scope = "observed VR selector/action order; predicate equivalence not established"
    else:
        scope = "observed early-route/action order; predicate equivalence not established"
    return {
        "frameNo": frame_no,
        "drawCount": len(draws),
        "siteCount": total_sites,
        "actionCount": total_actions,
        "inferredUnvisitedSiteCount": inferred_unvisited,
        "routeCounts": route_counts,
        "scope": scope,
        "predicateReplay": (dict(
            status=("mismatch" if predicate_replay["mismatches"] else
                    "unreplayable" if predicate_replay["unreplayable"] else
                    "mutation-unobserved" if predicate_replay["mutationUnobserved"] else
                    "no-supported-facts" if not predicate_replay["factCount"] else
                    "supported-facts-replayed"),
            predicateFactVersion=predicate_fact_version,
            nightVisionStatus=("unavailable-v1" if predicate_fact_version < 2 else
                               "not-visited" if not predicate_replay["nightVisionFacts"] else
                               "mismatch" if predicate_replay["nightVisionMismatches"] else
                               "unreplayable" if predicate_replay["nightVisionUnreplayable"] else
                               "replayed"),
            witchspaceStarsStatus=("unavailable-v1-or-v2" if predicate_fact_version < 3 else
                                   "not-visited" if not predicate_replay["witchspaceStarsFacts"] else
                                   "mismatch" if predicate_replay["witchspaceStarsMismatches"] else
                                   "unreplayable" if predicate_replay["witchspaceStarsUnreplayable"] else
                                   "replayed"),
            offscreenCensusStatus=("unavailable-v1-v3" if predicate_fact_version < 4 else
                                   "not-visited" if not predicate_replay["offscreenCensusFacts"] else
                                   "mismatch" if predicate_replay["offscreenCensusMismatches"] else
                                   "unreplayable" if predicate_replay["offscreenCensusUnreplayable"] else
                                   "replayed"),
            offscreenQuadStatus=("unavailable-v1-v3" if predicate_fact_version < 4 else
                                 "not-visited" if not predicate_replay["offscreenQuadFacts"] else
                                 "mismatch" if predicate_replay["offscreenQuadMismatches"] else
                                 "unreplayable" if predicate_replay["offscreenQuadUnreplayable"] else
                                 "replayed"),
            holoStatus=("unavailable-v1-v4" if predicate_fact_version < 5 else
                        "not-visited" if not predicate_replay["holoFacts"] else
                        "mismatch" if predicate_replay["holoMismatches"] else
                        "unreplayable" if predicate_replay["holoUnreplayable"] else
                        "replayed"),
            scrimStatus=("unavailable-v1-v4" if predicate_fact_version < 5 else
                         "not-visited" if not predicate_replay["scrimFacts"] else
                         "mismatch" if predicate_replay["scrimMismatches"] else
                         "unreplayable" if predicate_replay["scrimUnreplayable"] else
                         "replayed"),
            sunglareStatus=("unavailable-v1-v5" if predicate_fact_version < 6 else
                            "not-visited" if not predicate_replay["sunglareFacts"] else
                            "mismatch" if predicate_replay["sunglareMismatches"] else
                            "unreplayable" if predicate_replay["sunglareUnreplayable"] else
                            "replayed"),
            fssStatus=("unavailable-v1-v6" if predicate_fact_version < 7 else
                       "not-visited" if not predicate_replay["fssFacts"] else
                       "mismatch" if predicate_replay["fssMismatches"] else
                       "unreplayable" if predicate_replay["fssUnreplayable"] else
                       "replayed"),
             remlokStatus=("unavailable-before-v8" if predicate_fact_version < 8 else
                           "not-visited" if not predicate_replay["remlokFacts"] else
                           "mismatch" if predicate_replay["remlokMismatches"] else
                           "unreplayable" if predicate_replay["remlokUnreplayable"] else
                           "replayed"),
             basicDrawStatus=("unavailable-before-v9" if predicate_fact_version < 9 else
                              "not-visited" if not predicate_replay["basicDrawFacts"] else
                              "mismatch" if predicate_replay["basicDrawMismatches"] else
                              "unreplayable" if predicate_replay["basicDrawUnreplayable"] else
                              "replayed"),
             eyeCensusStatus=("unavailable-before-v10" if predicate_fact_version < 10 else
                              "not-visited" if not predicate_replay["eyeCensusFacts"] else
                              "mismatch" if predicate_replay["eyeCensusMismatches"] else
                              "unreplayable" if predicate_replay["eyeCensusUnreplayable"] else
                              "replayed"),
             resolveBindStatus=("unavailable-before-v11" if predicate_fact_version < 11 else
                                "not-visited" if not predicate_replay["resolveBindFacts"] else
                                "mismatch" if predicate_replay["resolveBindMismatches"] else
                                "unreplayable" if predicate_replay["resolveBindUnreplayable"] else
                                "replayed"),
             loaderPanelStatus=("unavailable-before-v12" if predicate_fact_version < 12 else
                                "not-visited" if not predicate_replay["loaderPanelFacts"] else
                                "mismatch" if predicate_replay["loaderPanelMismatches"] else
                                "unreplayable" if predicate_replay["loaderPanelUnreplayable"] else
                                "mutation-unobserved" if predicate_replay["loaderPanelMutationUnobserved"] else
                                "replayed"),
             fssDumpStatus=("unavailable-before-v13" if predicate_fact_version < 13 else
                            "not-visited" if not predicate_replay["fssDumpFacts"] else
                            "mismatch" if predicate_replay["fssDumpMismatches"] else
                            "unreplayable" if predicate_replay["fssDumpUnreplayable"] else
                            "replayed"),
             targetSharpStatus=("unavailable-before-v14" if predicate_fact_version < 14 else
                                "not-visited" if not predicate_replay["targetSharpFacts"] else
                                "mismatch" if predicate_replay["targetSharpMismatches"] else
                                "unreplayable" if predicate_replay["targetSharpUnreplayable"] else
                                "not-eligible" if predicate_replay["targetSharpNotEligible"] and
                                                   not predicate_replay["targetSharpReplayed"] else
                                "replayed"),
             sunglareNominationStatus=("unavailable-before-v15" if predicate_fact_version < 15 else
                                       "not-visited" if not predicate_replay["sunglareNominationFacts"] else
                                       "mismatch" if predicate_replay["sunglareNominationMismatches"] else
                                       "unreplayable" if predicate_replay["sunglareNominationUnreplayable"] else
                                       "replayed"),
             fssChromeSkipStatus=("unavailable-before-v16" if predicate_fact_version < 16 else
                                  "not-visited" if not predicate_replay["fssChromeSkipFacts"] else
                                  "mismatch" if predicate_replay["fssChromeSkipMismatches"] else
                                  "replayed"),
             **predicate_replay)
            if schema_version == SCHEMA_VERSION else
            {"status": "unavailable", "factCount": 0, "replayed": 0,
             "unreplayable": 0,
             "mismatches": 0, "mutationUnobserved": 0,
             "predicateFactVersion": 0, "nightVisionStatus": "unavailable-v1",
             "nightVisionFacts": 0, "nightVisionReplayed": 0,
             "nightVisionUnreplayable": 0, "nightVisionMismatches": 0,
             "witchspaceStarsStatus": "unavailable-v1-or-v2",
             "witchspaceStarsFacts": 0, "witchspaceStarsReplayed": 0,
             "witchspaceStarsUnreplayable": 0, "witchspaceStarsMismatches": 0,
             "offscreenCensusStatus": "unavailable-v1-v3",
             "offscreenCensusFacts": 0, "offscreenCensusReplayed": 0,
             "offscreenCensusUnreplayable": 0, "offscreenCensusMismatches": 0,
             "offscreenQuadStatus": "unavailable-v1-v3",
             "offscreenQuadFacts": 0, "offscreenQuadReplayed": 0,
             "offscreenQuadUnreplayable": 0, "offscreenQuadMismatches": 0,
             "holoStatus": "unavailable-v1-v4", "holoFacts": 0,
             "holoReplayed": 0, "holoUnreplayable": 0, "holoMismatches": 0,
             "scrimStatus": "unavailable-v1-v4", "scrimFacts": 0,
             "scrimReplayed": 0, "scrimUnreplayable": 0, "scrimMismatches": 0,
             "sunglareStatus": "unavailable-v1", "sunglareFacts": 0,
             "sunglareReplayed": 0, "sunglareUnreplayable": 0,
             "sunglareMismatches": 0,
             "fssStatus": "unavailable-v1", "fssFacts": 0,
             "fssReplayed": 0, "fssUnreplayable": 0,
              "fssMismatches": 0, "remlokStatus": "unavailable-before-v8",
              "remlokFacts": 0, "remlokReplayed": 0,
              "remlokUnreplayable": 0, "remlokMismatches": 0,
              "basicDrawStatus": "unavailable-before-v9",
              "basicDrawFacts": 0, "basicDrawReplayed": 0,
              "basicDrawUnreplayable": 0, "basicDrawMismatches": 0,
              "eyeCensusStatus": "unavailable-before-v10",
              "eyeCensusFacts": 0, "eyeCensusReplayed": 0,
              "eyeCensusUnreplayable": 0, "eyeCensusMismatches": 0,
              "resolveBindStatus": "unavailable-before-v11",
              "resolveBindFacts": 0, "resolveBindReplayed": 0,
              "resolveBindUnreplayable": 0, "resolveBindMismatches": 0,
             "loaderPanelStatus": "unavailable-before-v12",
             "loaderPanelFacts": 0, "loaderPanelReplayed": 0,
             "loaderPanelUnreplayable": 0, "loaderPanelMismatches": 0,
              "loaderPanelMutationUnobserved": 0,
              "fssDumpStatus": "unavailable-before-v13",
              "fssDumpFacts": 0, "fssDumpReplayed": 0,
             "fssDumpUnreplayable": 0, "fssDumpMismatches": 0,
             "targetSharpStatus": "unavailable-before-v14",
             "targetSharpFacts": 0, "targetSharpReplayed": 0,
             "targetSharpUnreplayable": 0, "targetSharpMismatches": 0,
             "targetSharpNotEligible": 0,
             "sunglareNominationStatus": "unavailable-before-v15",
             "sunglareNominationFacts": 0, "sunglareNominationReplayed": 0,
             "sunglareNominationUnreplayable": 0,
             "sunglareNominationMismatches": 0}),
        "forwardReplay": dict(
            status=("unavailable" if forward_input_version == 0 else
                    "mismatch" if forward_replay["mismatches"] else
                    "unavailable" if forward_replay["unavailable"] else
                    "replayed" if forward_replay["replayed"] else "not-visited"),
            forwardInputVersion=forward_input_version,
            localScope="forwarder-local None/Skip action envelope only",
            wholeForwardingEquivalence=False,
            **forward_replay),
        "buildVersion": version,
        "buildStamp": stamp.upper(),
        "logFile": log_name,
    }


def read_trace(path, expected_log=None, expected_build_stamp=None):
    size = os.path.getsize(path)
    if size > MAX_TRACE_BYTES:
        raise TraceError("sidecar exceeds the %d-byte reader limit" % MAX_TRACE_BYTES)
    with open(path, "r", encoding="utf-8") as stream:
        data = json.load(stream)
    summary = validate_trace(data, expected_log, expected_build_stamp)
    return data, summary


def discover_sidecars(log_path):
    """Find only strict basename-derived siblings; never follow JSON paths."""
    directory = os.path.dirname(os.path.abspath(log_path))
    stem = os.path.splitext(os.path.basename(log_path))[0]
    rx = re.compile(r"^%s\.draw-ladder-(\d+)\.json$" % re.escape(stem), re.IGNORECASE)
    found = []
    try:
        names = os.listdir(directory)
    except OSError:
        return []
    for name in names:
        match = rx.match(name)
        if match:
            found.append((int(match.group(1)), os.path.join(directory, name)))
    return [path for _, path in sorted(found)]


def format_summary(summary, sidecar_path=None):
    routes = ", ".join("%s:%d" % (route, count)
                       for route, count in sorted(summary["routeCounts"].items())) or "none"
    lines = ["DRAW-LADDER TRACE: frame %d, %d draw(s), %d site event(s), %d action event(s)" %
             (summary["frameNo"], summary["drawCount"], summary["siteCount"],
              summary["actionCount"]),
             "  build %s (%s), routes %s" %
             (summary["buildVersion"], summary["buildStamp"], routes),
             "  coverage: %s" % summary["scope"],
             "  inferred nonvisited suffix sites: %d (from route sequence after terminal; NotEligible is a reached event)" %
             summary["inferredUnvisitedSiteCount"],
             "  recorded selector/action order is observed; whole-ladder predicate equivalence is not established."]
    replay = summary["predicateReplay"]
    lines.append("  supported predicate facts: %s (%d selector matches, %d unreplayable, %d mismatch, %d mutation unobserved); whole-ladder equivalence remains false" %
                 (replay["status"], replay["replayed"], replay["unreplayable"],
                  replay["mismatches"], replay["mutationUnobserved"]))
    forward = summary.get("forwardReplay", {"status": "unavailable", "forwardInputVersion": 0,
                                             "replayed": 0, "unavailable": 0,
                                             "mismatches": 0, "externalCallbackInvocations": 0,
                                             "externalCallbackWorkUnobserved": 0,
                                             "originalActionIssues": 0,
                                             "terminalProofUnavailable": 0})
    lines.append("  forwarding inputs v%d: %s (%d local plans, %d unavailable, %d mismatch); terminal proof unavailable for %d plan(s); full forwarding equivalence remains false; original callback invoked %d time(s), %d original action issue(s), nested callback work unobserved on %d invocation(s)" %
                 (forward["forwardInputVersion"], forward["status"], forward["replayed"],
                  forward["unavailable"], forward["mismatches"], forward.get("terminalProofUnavailable", 0),
                  forward.get("externalCallbackInvocations", 0), forward.get("originalActionIssues", 0),
                  forward.get("externalCallbackWorkUnobserved", 0)))
    classes = forward.get("alteredDrawClassCounts", {})
    if classes:
        lines.append("  forwarding-local altered draw classes (not action claims): none %d, pool-family %d, not-classified %d" %
                     (classes.get("none", 0), classes.get("pool-family", 0),
                      classes.get("not-classified", 0)))
    if replay["nightVisionStatus"] == "unavailable-v1":
        lines.append("  NightVisionClaim site 50: unavailable in predicate fact version 1")
    else:
        lines.append("  NightVisionClaim site 50: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay["nightVisionStatus"], replay["nightVisionFacts"],
                      replay["nightVisionReplayed"], replay["nightVisionUnreplayable"],
                      replay["nightVisionMismatches"]))
    if replay.get("witchspaceStarsStatus", "unavailable-v1-or-v2") == "unavailable-v1-or-v2":
        lines.append("  WitchspaceStarsSkip site 6: unavailable before predicate fact version 3")
    else:
        lines.append("  WitchspaceStarsSkip site 6: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay["witchspaceStarsStatus"], replay.get("witchspaceStarsFacts", 0),
                      replay.get("witchspaceStarsReplayed", 0),
                      replay.get("witchspaceStarsUnreplayable", 0),
                      replay.get("witchspaceStarsMismatches", 0)))
    for status_key, facts_key, replayed_key, unknown_key, mismatch_key, label in (
            ("offscreenCensusStatus", "offscreenCensusFacts", "offscreenCensusReplayed",
             "offscreenCensusUnreplayable", "offscreenCensusMismatches",
             "OffscreenCensusSkip site 24"),
            ("offscreenQuadStatus", "offscreenQuadFacts", "offscreenQuadReplayed",
             "offscreenQuadUnreplayable", "offscreenQuadMismatches",
             "OffscreenQuadSkip site 26")):
        if replay.get(status_key, "unavailable-v1-v3") == "unavailable-v1-v3":
            lines.append("  %s: unavailable before predicate fact version 4" % label)
        else:
            lines.append("  %s: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                         (label, replay[status_key], replay.get(facts_key, 0),
                          replay.get(replayed_key, 0), replay.get(unknown_key, 0),
                          replay.get(mismatch_key, 0)))
    for status_key, facts_key, replayed_key, unknown_key, mismatch_key, label in (
            ("holoStatus", "holoFacts", "holoReplayed", "holoUnreplayable",
             "holoMismatches", "Holo claim site 53"),
            ("scrimStatus", "scrimFacts", "scrimReplayed", "scrimUnreplayable",
             "scrimMismatches", "Scrim claim site 55")):
        if replay.get(status_key, "unavailable-v1-v4") == "unavailable-v1-v4":
            lines.append("  %s: unavailable before predicate fact version 5" % label)
        else:
            lines.append("  %s: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                         (label, replay[status_key], replay.get(facts_key, 0),
                          replay.get(replayed_key, 0), replay.get(unknown_key, 0),
                          replay.get(mismatch_key, 0)))
    if replay.get("predicateFactVersion", 0) < 6:
        lines.append("  Sunglare sites 61-63: unavailable before predicate fact version 6")
    else:
        lines.append("  Sunglare sites 61-63: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay["sunglareStatus"], replay.get("sunglareFacts", 0),
                      replay.get("sunglareReplayed", 0), replay.get("sunglareUnreplayable", 0),
                      replay.get("sunglareMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 7:
        lines.append("  FSS sites 57-58: unavailable before predicate fact version 7")
    else:
        lines.append("  FSS sites 57-58: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay["fssStatus"], replay.get("fssFacts", 0),
                      replay.get("fssReplayed", 0), replay.get("fssUnreplayable", 0),
                      replay.get("fssMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 8:
        lines.append("  RemLok sites 51-52: unavailable before predicate fact version 8")
    else:
        lines.append("  RemLok sites 51-52: %s (%d event(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay["remlokStatus"], replay.get("remlokFacts", 0),
                      replay.get("remlokReplayed", 0), replay.get("remlokUnreplayable", 0),
                      replay.get("remlokMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 9:
        lines.append("  BasicDraw sites 2 and 67: unavailable before predicate fact version 9")
    else:
        lines.append("  BasicDraw sites 2 and 67: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay.get("basicDrawStatus", "not-visited"),
                      replay.get("basicDrawFacts", 0), replay.get("basicDrawReplayed", 0),
                      replay.get("basicDrawUnreplayable", 0), replay.get("basicDrawMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 10:
        lines.append("  EyeCensusSkip site 48: unavailable before predicate fact version 10")
    else:
        lines.append("  EyeCensusSkip site 48: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay.get("eyeCensusStatus", "not-visited"),
                      replay.get("eyeCensusFacts", 0), replay.get("eyeCensusReplayed", 0),
                      replay.get("eyeCensusUnreplayable", 0), replay.get("eyeCensusMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 11:
        lines.append("  ResolveBindClaim site 60: unavailable before predicate fact version 11")
    else:
        lines.append("  ResolveBindClaim site 60: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay.get("resolveBindStatus", "not-visited"),
                      replay.get("resolveBindFacts", 0), replay.get("resolveBindReplayed", 0),
                      replay.get("resolveBindUnreplayable", 0), replay.get("resolveBindMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 12:
        lines.append("  OffscreenLoaderPanel site 25: unavailable before predicate fact version 12")
    else:
        lines.append("  OffscreenLoaderPanel site 25: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch, %d mutation-unobserved)" %
                     (replay.get("loaderPanelStatus", "not-visited"),
                      replay.get("loaderPanelFacts", 0), replay.get("loaderPanelReplayed", 0),
                      replay.get("loaderPanelUnreplayable", 0), replay.get("loaderPanelMismatches", 0),
                      replay.get("loaderPanelMutationUnobserved", 0)))
    if replay.get("predicateFactVersion", 0) < 13:
        lines.append("  FssDumpClaim site 59: unavailable before predicate fact version 13")
    else:
        lines.append("  FssDumpClaim site 59: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay.get("fssDumpStatus", "not-visited"),
                      replay.get("fssDumpFacts", 0), replay.get("fssDumpReplayed", 0),
                      replay.get("fssDumpUnreplayable", 0), replay.get("fssDumpMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 15:
        lines.append("  Sunglare nomination site 45: unavailable before predicate fact version 15")
    else:
        lines.append("  Sunglare nomination site 45: %s (%d fact(s), %d replayed, %d unreplayable, %d mismatch)" %
                     (replay.get("sunglareNominationStatus", "not-visited"),
                      replay.get("sunglareNominationFacts", 0),
                      replay.get("sunglareNominationReplayed", 0),
                      replay.get("sunglareNominationUnreplayable", 0),
                      replay.get("sunglareNominationMismatches", 0)))
    if replay.get("predicateFactVersion", 0) < 16:
        lines.append("  FssChromeSkip site 1: unavailable before predicate fact version 16")
    else:
        lines.append("  FssChromeSkip site 1: %s (%d fact(s), %d replayed, %d mismatch, %d terminal Skip)" %
                     (replay.get("fssChromeSkipStatus", "not-visited"),
                      replay.get("fssChromeSkipFacts", 0),
                      replay.get("fssChromeSkipReplayed", 0),
                      replay.get("fssChromeSkipMismatches", 0),
                      replay.get("fssChromeSkipTerminal", 0)))
    if sidecar_path:
        lines.insert(0, "[edvr] draw-ladder sidecar: %s" % sidecar_path)
    return "\n".join(lines)


def predicate_replay_gate_failure(summary):
    """Return why a supported-fact replay cannot pass its correctness gate."""
    replay = summary.get("predicateReplay", {})
    status = replay.get("status", "unavailable")
    if status in ("mismatch", "unreplayable", "mutation-unobserved"):
        return ("supported predicate replay %s (mismatches=%d, unreplayable=%d, "
                "mutation unobserved=%d)" %
                (status, replay.get("mismatches", 0), replay.get("unreplayable", 0),
                 replay.get("mutationUnobserved", 0)))
    # Schema 1 is a valid historical selector/order capture, but carries no
    # predicate facts and cannot establish predicate parity.
    return None


def _fixture():
    return {
        "format": FORMAT, "schemaVersion": 2,
        # Existing vectors exercise the v1-v5 reader contract. Dedicated v6
        # Sunglare fixtures below opt in explicitly.
        "predicateFactVersion": 5,
        "buildVersion": "fixture",
        "buildStamp": "1234ABCD", "logFile": "edvr_gfx_20261001_010203.log",
            "semantics": {"equivalence": "observed-selector-and-action-order",
                      "predicateEquivalence": False,
                      "predicateNote": "only declared fact families are independently replayed",
                      "identityNote": "opaque ordinals", "flagBits": FLAG_BITS},
        "frame": {"frameNo": 12, "completedFrameNo": 12, "configEpoch": 1,
                  "eyeDrawsThisFrame": 1, "eyeDrawsLastFrame": 0,
                  "sceneDrawsThisFrame": 1, "stateFlags": 0,
                  "sceneCounters": [0, 0, 0, 0]},
        "draws": [{"index": 0, "eyeDrawIndex": 1, "kind": ord("X"),
                   "command": "draw-indexed-instanced",
                   "count": 240, "instances": 1,
                   "args": {"start": 0, "base": 0, "startInstance": 0},
                   "vsHash": "FCF7BD2896751D96", "psHash": "F786D34B5E118D5E",
                   "resources": {"vs": 1, "ps": 2, "rtv0": 3, "dsv0": 4,
                                 "argumentBuffer": 0},
                   "argumentBufferKnown": False, "argumentByteOffset": 0,
                   "drawParametersKnown": True,
                   "rtv0Generation": 1, "dsv0Generation": 1,
                   "rtv0Width": 2400, "rtv0Height": 2400,
                   "route": 7, "sequence": 5, "candidateMask": "0000000000000000",
                   "flags": 0,
                   "sites": [{"id": 72, "kind": 3, "outcome": 4, "flow": 1,
                              "subsite": 0, "verdict": 0}],
                   "predicateFacts": [],
                   "winnerSiteId": 72, "verdict": 0,
                   "forwardFacts": None,
                   "actions": [
                       {"id": 1, "phase": 1, "outcome": 2, "call": 4,
                        "flags": 0, "issueCount": 0, "issueCountKnown": True,
                        "count": 240, "instances": 1, "start": 0,
                        "startInstance": 0, "baseVertex": 0},
                       {"id": 2, "phase": 2, "outcome": 2, "call": 4,
                        "flags": 0, "issueCount": 1, "issueCountKnown": True,
                        "count": 240, "instances": 1, "start": 0,
                        "startInstance": 0, "baseVertex": 0},
                       {"id": 17, "phase": 3, "outcome": 2, "call": 4,
                        "flags": 0, "issueCount": 0, "issueCountKnown": True,
                        "count": 240, "instances": 1, "start": 0,
                        "startInstance": 0, "baseVertex": 0}] }],
        "footer": {"complete": True, "truncated": False, "overflow": False,
                   "drawCount": 1},
    }


def self_test():
    global read_trace
    base = _fixture()
    try:
        summary = validate_trace(base, "edvr_gfx_20261001_010203.log", "1234abcd")
        if summary["scope"] != "flat bypass/actions; no VR classifier parity":
            raise TraceError("flat capture was mislabelled")
    except Exception as exc:
        print("draw-ladder trace fixture rejected: %s" % exc)
        return 1

    legacy_v1 = json.loads(json.dumps(base))
    # Native schema12 captures may have no LoaderPanel visit at all, including
    # an initial empty frame or a bypass-only frame. All aggregate keys still
    # exist before any per-draw accumulation or version-specific status read.
    loader_metric_names = ("loaderPanelFacts", "loaderPanelReplayed",
                          "loaderPanelUnreplayable", "loaderPanelMismatches",
                          "loaderPanelMutationUnobserved")
    for version in (1, 11, 12):
        no_loader = json.loads(json.dumps(base))
        no_loader["predicateFactVersion"] = version
        for empty in (False, True):
            candidate = json.loads(json.dumps(no_loader))
            if empty:
                candidate["draws"] = []
                candidate["footer"]["drawCount"] = 0
            no_loader_replay = validate_trace(candidate)["predicateReplay"]
            if any(no_loader_replay[name] != 0 for name in loader_metric_names):
                print("no-loader capture lacks zeroed LoaderPanel aggregate metrics")
                return 1
            expected_status = "unavailable-before-v12" if version < 12 else "not-visited"
            if no_loader_replay["loaderPanelStatus"] != expected_status:
                print("no-loader capture has an incorrect version-specific LoaderPanel status")
                return 1
    v7_bypass = json.loads(json.dumps(base))
    v7_bypass["predicateFactVersion"] = 7
    v7_replay = validate_trace(v7_bypass)["predicateReplay"]
    if v7_replay["fssStatus"] != "not-visited" or v7_replay["fssFacts"] != 0:
        print("v7 bypass did not initialize empty FSS summary counters")
        return 1
    legacy_v1["schemaVersion"] = 1
    legacy_v1.pop("predicateFactVersion")
    for legacy_draw in legacy_v1["draws"]:
        legacy_draw.pop("predicateFacts")
    if validate_trace(legacy_v1)["predicateReplay"]["status"] != "unavailable":
        print("schemaVersion 1 did not report predicate parity unavailable")
        return 1
    if predicate_replay_gate_failure(validate_trace(legacy_v1)) is not None:
        print("schemaVersion 1 historical capture failed the supported-fact gate")
        return 1

    legacy_v2_predicates = json.loads(json.dumps(base))
    legacy_v2_predicates["predicateFactVersion"] = 1
    if (validate_trace(legacy_v2_predicates)["predicateReplay"]["nightVisionStatus"] !=
            "unavailable-v1"):
        print("schemaVersion 2/factVersion 1 did not preserve historical NV-unavailable status")
        return 1

    no_op = json.loads(json.dumps(base))
    no_op["draws"][0]["instances"] = 0
    for action in no_op["draws"][0]["actions"]:
        action["instances"] = 0
    try:
        validate_trace(no_op)
    except Exception as exc:
        print("draw-ladder reader rejected a legal zero-instance direct draw: %s" % exc)
        return 1

    vr = json.loads(json.dumps(base))
    d = vr["draws"][0]
    d["route"] = 6
    d["sequence"] = 4
    d["sites"] = []
    for site_id in [1, 2, 3, 4, 5, 6, 7, 8, 40, 41, 42, 43]:
        kind = SITE_KINDS[site_id]
        outcome = 3 if site_id == 43 else (1 if kind == 1 else 2)
        d["sites"].append({
            "id": site_id, "kind": kind, "outcome": outcome,
            "flow": 1 if site_id == 43 else 0,
            "subsite": 6 if site_id == 8 else 0,
            "verdict": 7 if site_id == 43 else -1,
        })
    d["winnerSiteId"] = 43
    d["verdict"] = 7
    d["predicateFacts"] = [{"siteId": 3, "kind": 1, "known": "yes",
                            "gateWanted": "yes"}, {
        "siteId": 6, "kind": 4, "known": "yes", "interestMaskKnown": "yes",
        "legacyInterestMask": "%016X" % (1 << 1), "helperReached": "yes",
        "hiddenKnown": "yes", "hidden": "no", "contextKnown": "unknown",
        "contextValid": "unknown", "shapeReached": "unknown",
        "shapeMatched": "unknown", "hashKnown": "unknown", "hashSource": 0,
        "vsHash": "0000000000000000", "skippedDeltaKnown": True,
        "skippedDelta": 0,
    }]
    mask = (1 << 14) - 1
    d["forwardFacts"] = {
        "presentMask": mask, "verdictOrdinal": 7, "family": 3,
        "familyAvailable": "yes", "owner": "yes", "verdictForwards": "yes",
        "initialUiTake": "no", "afterUiTake": "yes", "worldReissue": "no",
        "curveThisDraw": "yes", "introCurveThisDraw": "no", "uiDepth": "yes",
        "holoDepth": "no", "composite": "no", "crispPending": "no",
        "issueBlocked": "no",
    }
    try:
        summary = validate_trace(vr)
        if summary["scope"].startswith("flat") or summary["inferredUnvisitedSiteCount"] == 0:
            raise TraceError("VR trace scope or short-circuited suffix was not reported")
    except Exception as exc:
        print("draw-ladder VR trace fixture rejected: %s" % exc)
        return 1

    # Independent input-driven selector fixtures: boundaries are inclusive,
    # overlaps retain the first configured range index, and known-empty is
    # distinct from an unknown/unreplayable range snapshot.
    selector_cases = [
        (1, [[1, 4]], ({"id": 49, "kind": 2, "outcome": 4, "flow": 1,
                        "subsite": 0, "verdict": 2}, 1)),
        (4, [[1, 4]], ({"id": 49, "kind": 2, "outcome": 4, "flow": 1,
                        "subsite": 0, "verdict": 2}, 1)),
        (6, [[1, 7], [5, 9]], ({"id": 49, "kind": 2, "outcome": 4, "flow": 1,
                                "subsite": 0, "verdict": 2}, 1)),
        (10, [], ({"id": 49, "kind": 2, "outcome": 2, "flow": 0,
                   "subsite": 0, "verdict": -1}, 0)),
    ]
    for gate_wanted, expected in (
            ("yes", {"id": 3, "kind": 3, "outcome": 1, "flow": 0,
                     "subsite": 0, "verdict": -1}),
            ("no", {"id": 3, "kind": 3, "outcome": 4, "flow": 1,
                    "subsite": 0, "verdict": 0})):
        gate_fact = {"known": "yes", "gateWanted": gate_wanted}
        if (_candidate_draw_gate(gate_fact) != expected or
                _legacy14a_draw_gate(gate_fact) != expected):
            print("draw-ladder draw-gate selector fixture failed")
            return 1
    if (_candidate_draw_gate({"known": "unknown", "gateWanted": "unknown"}) is not None or
            _legacy14a_draw_gate({"known": "unknown", "gateWanted": "unknown"}) is not None):
        print("draw-ladder unknown draw-gate fixture was treated as off")
        return 1
    for eye_index, ranges, expected in selector_cases:
        fact = {"known": "yes", "eyeDrawIndex": eye_index, "ranges": ranges}
        if (_candidate_eye_range(fact) != expected or
                _legacy14a_eye_range(fact) != expected):
            print("draw-ladder predicate selector boundary fixture failed")
            return 1
    if (_candidate_eye_range({"known": "unknown", "eyeDrawIndex": 4, "ranges": []}) is not None or
            _legacy14a_eye_range({"known": "unknown", "eyeDrawIndex": 4, "ranges": []}) is not None):
        print("draw-ladder unknown predicate fixture was treated as off")
        return 1

    # The site-replay test constructs a real route prefix ending at either
    # EyeRangeSkip or the following claim, so SiteEvent values are expectations.
    def range_trace(eye_index, ranges, expected, known="yes"):
        trace = json.loads(json.dumps(vr))
        draw = trace["draws"][0]
        ids = [1, 2, 3, 4, 5, 6, 7, 8, 40, 41, 42, 43, 44, 45, 71,
               46, 47, 48, 49]
        site49_event, expected_delta = expected
        stop = 49 if site49_event["outcome"] == 4 else 50
        if stop == 50:
            ids.append(50)
        draw["sites"] = []
        for site_id in ids:
            kind = SITE_KINDS[site_id]
            if site_id == 3:
                outcome, flow, subsite, verdict = 1, 0, 0, -1
            elif site_id == 44:
                outcome, flow, subsite, verdict = 5, 0, 0, -1
            elif site_id == 49:
                outcome, flow, subsite, verdict = (site49_event["outcome"],
                    site49_event["flow"], site49_event["subsite"],
                    site49_event["verdict"])
            elif site_id == 50:
                outcome, flow, subsite, verdict = 3, 1, 0, 6
            else:
                outcome, flow, subsite, verdict = (1 if kind == 1 else 2), 0, 0, -1
            draw["sites"].append({"id": site_id, "kind": kind, "outcome": outcome,
                                  "flow": flow, "subsite": subsite, "verdict": verdict})
        draw["winnerSiteId"] = stop
        draw["verdict"] = TERMINAL_VERDICTS[stop]
        draw["forwardFacts"] = None
        if stop == 50:
            draw["vsHash"] = "FCF7BD2896751D96"
            draw["psHash"] = "F786D34B5E118D5E"
            draw["candidateMask"] = "%016X" % (1 << 1)
        draw["predicateFacts"] = [
            {"siteId": 3, "kind": 1, "known": "yes", "gateWanted": "yes"},
            {"siteId": 6, "kind": 4, "known": "yes", "interestMaskKnown": "yes",
             "legacyInterestMask": "%016X" % (1 << 1), "helperReached": "yes",
             "hiddenKnown": "yes", "hidden": "no", "contextKnown": "unknown",
             "contextValid": "unknown", "shapeReached": "unknown",
             "shapeMatched": "unknown", "hashKnown": "unknown", "hashSource": 0,
             "vsHash": "0000000000000000", "skippedDeltaKnown": True,
             "skippedDelta": 0},
            {"siteId": 49, "kind": 2, "known": known,
             "eyeDrawIndex": eye_index, "ranges": ranges if known != "unknown" else [],
             "censusSkippedDeltaKnown": known != "unknown",
             "censusSkippedDelta": expected_delta if known != "unknown" else 0}]
        if stop == 50:
            draw["predicateFacts"].append({
                "siteId": 50, "kind": 3, "known": "yes",
                "dispatchEnabled": "yes", "activeMaskKnown": "yes",
                "activePluginMask": "%016X" % (1 << 1),
                "candidateKnown": "yes", "candidatePresent": "yes",
                "modeKnown": "yes", "mode": 2, "shapeReached": "yes",
                "shapeMatched": "yes", "callbackReached": "yes",
                "callbackModeKnown": "yes", "callbackMode": 2,
                "failedKnown": "yes", "failed": "no"})
        return trace

    def resource_fact(source=0, texture="unknown", a=0, b=0, fmt=0):
        if source == 0:
            return {"source": 0, "resolveReached": "unknown", "resolved": "unknown",
                    "rawAvailable": "unknown", "texture2D": "unknown",
                    "a": 0, "b": 0, "fmt": 0}
        if source == 1:
            return {"source": 1, "resolveReached": "yes", "resolved": "yes",
                    "rawAvailable": "yes", "texture2D": texture,
                    "a": a, "b": b, "fmt": fmt}
        if source == 2:
            return {"source": 2, "resolveReached": "yes", "resolved": "no",
                    "rawAvailable": "no", "texture2D": "no",
                    "a": 0, "b": 0, "fmt": 0}
        if source == 4:
            return {"source": 4, "resolveReached": "no", "resolved": "unknown",
                    "rawAvailable": "unknown", "texture2D": "unknown",
                    "a": 0, "b": 0, "fmt": 0}
        return {"source": 3, "resolveReached": "no", "resolved": "yes",
                "rawAvailable": "yes", "texture2D": texture,
                "a": a, "b": b, "fmt": fmt}

    def holo_eye_fact(depth_w, depth_h, mode="match"):
        if mode == "state-absent":
            return {"reached": "yes", "statePresent": "no", "result": "no",
                    "readMask": 0, "depthW": 0, "depthH": 0,
                    "eyeW": 0, "eyeH": 0, "renderW": 0, "renderH": 0}
        if mode == "match":
            return {"reached": "yes", "statePresent": "yes", "result": "yes",
                    "readMask": 15, "depthW": depth_w, "depthH": depth_h,
                    "eyeW": depth_w, "eyeH": depth_h, "renderW": 0, "renderH": 0}
        if mode == "plus-two":
            return {"reached": "yes", "statePresent": "yes", "result": "yes",
                    "readMask": 15, "depthW": depth_w, "depthH": depth_h,
                    "eyeW": depth_w + 2, "eyeH": depth_h + 2,
                    "renderW": 0, "renderH": 0}
        if mode == "minus-two":
            return {"reached": "yes", "statePresent": "yes", "result": "yes",
                    "readMask": 15, "depthW": depth_w, "depthH": depth_h,
                    "eyeW": depth_w - 2, "eyeH": depth_h - 2,
                    "renderW": 0, "renderH": 0}
        if mode == "zero-eye-height":
            return {"reached": "yes", "statePresent": "yes", "result": "no",
                    "readMask": 31, "depthW": depth_w, "depthH": depth_h,
                    "eyeW": depth_w, "eyeH": 0, "renderW": 0, "renderH": 0}
        if mode == "plus-three":
            return {"reached": "yes", "statePresent": "yes", "result": "no",
                    "readMask": 23, "depthW": depth_w, "depthH": depth_h,
                    "eyeW": depth_w + 3, "eyeH": 0, "renderW": 0, "renderH": 0}
        return {"reached": "unknown", "statePresent": "unknown", "result": "unknown",
                "readMask": 0, "depthW": 0, "depthH": 0,
                "eyeW": 0, "eyeH": 0, "renderW": 0, "renderH": 0}

    def holo_fact(draw, enabled=True, pattern_source=1, pattern_fmt=70,
                  depth_source=1, eye_mode="match", before=0, noted=False):
        shape = draw["kind"] == ord("X") and draw["count"] == 6 and draw["instances"] == 1
        helper = enabled and shape
        helper_enabled = enabled if helper else None
        pattern_reached = helper and helper_enabled
        pattern = resource_fact(pattern_source, "yes", 256, 256, pattern_fmt) if pattern_reached else resource_fact()
        pattern_match = (pattern_source == 1 and pattern_fmt == 70)
        depth_reached = pattern_reached and pattern_match
        depth = resource_fact(depth_source, "yes", 2000, 2000, 28) if depth_reached and depth_source == 1 else (
            resource_fact(2) if depth_reached else resource_fact())
        depth_match = depth_reached and depth_source == 1
        eye = holo_eye_fact(2000, 2000, eye_mode) if depth_match else holo_eye_fact(0, 0, "skip")
        eye_yes = eye_mode in ("match", "plus-two", "minus-two")
        predicate = bool(depth_match and eye_yes)
        noted_before = "yes" if noted else "no"
        increment = bool(depth_match and not eye_yes and not noted)
        after = (before + int(increment)) & ((1 << 64) - 1)
        noted_after = "yes" if noted or (increment and after >= 60) else "no"
        return {"siteId": 53, "kind": 7, "known": "yes",
                "gates": {
                    "enabled": "yes" if enabled else "no",
                    "shapeReached": "yes" if enabled else "unknown",
                    "shapeMatched": ("yes" if shape else "no") if enabled else "unknown",
                    "helperReached": "yes" if helper else "unknown",
                    "helperEnabled": "yes" if helper_enabled else "unknown",
                    "helperShapeReached": "yes" if helper_enabled else "unknown",
                    "helperShapeMatched": ("yes" if shape else "no") if helper_enabled else "unknown"},
                "pattern": pattern, "depth": depth, "eyeSize": eye,
                "predicateResult": "yes" if predicate else "no",
                "missedBefore": before, "missedAfter": after,
                "missNotedBefore": noted_before, "missNotedAfter": noted_after,
                "missedDeltaKnown": True, "missedDelta": int(increment)}

    def scrim_fact(draw, enabled=True, wash_source=1, wash_width=16,
                   wash_height=16, wash_fmt=71, ui_source=1, ui_width=2048):
        shape = draw["kind"] == ord("X") and draw["instances"] == 1 and draw["count"] >= 100
        helper = enabled and shape
        wash_reached = helper
        wash = resource_fact(wash_source, "yes", wash_width, wash_height, wash_fmt) if wash_reached and wash_source in (1, 3) else (
            resource_fact(wash_source) if wash_reached else resource_fact())
        wash_match = wash_reached and wash_source in (1, 3) and wash_width == 16 and wash_height == 16 and wash_fmt in (70, 71, 72)
        ui_reached = wash_match
        ui = resource_fact(ui_source, "yes", ui_width, 1080, 28) if ui_reached and ui_source in (1, 3) else (
            resource_fact(ui_source) if ui_reached else resource_fact())
        predicate = bool(ui_reached and ui_source in (1, 3) and ui_width >= 1024)
        helper_enabled = enabled if helper else None
        return {"siteId": 55, "kind": 8, "known": "yes",
                "gates": {
                    "enabled": "yes" if enabled else "no",
                    "shapeReached": "yes" if enabled else "unknown",
                    "shapeMatched": ("yes" if shape else "no") if enabled else "unknown",
                    "helperReached": "yes" if helper else "unknown",
                    "helperEnabled": "yes" if helper_enabled else "unknown",
                    "helperShapeReached": "yes" if helper_enabled else "unknown",
                    "helperShapeMatched": ("yes" if shape else "no") if helper_enabled else "unknown"},
                "wash": wash, "ui": ui,
                "predicateResult": "yes" if predicate else "no"}

    def holo_scrim_trace(target, draw_kind="X", draw_count=6, holo=None, scrim=None,
                         holo_options=None, scrim_options=None):
        decline49 = {"id": 49, "kind": 2, "outcome": 2, "flow": 0,
                     "subsite": 0, "verdict": -1}
        trace = range_trace(1, [], (decline49, 0))
        draw = trace["draws"][0]
        draw["route"], draw["sequence"] = 6, 4
        draw["kind"], draw["command"] = ord(draw_kind), DRAW_COMMANDS[ord(draw_kind)]
        draw["count"], draw["instances"] = draw_count, 1
        draw["vsHash"], draw["psHash"], draw["candidateMask"] = (
            "0000000000000000", "0000000000000000", "0000000000000000")
        nv = next(f for f in draw["predicateFacts"] if f["siteId"] == 50)
        nv.update(activePluginMask="0000000000000000", mode=0,
                  candidatePresent="no", shapeReached="no", shapeMatched="unknown",
                  callbackReached="no", callbackModeKnown="unknown", callbackMode=0,
                  failedKnown="unknown", failed="unknown")
        sites = []
        for site_id in COMMON + EYE[:EYE.index(target) + 1]:
            site_kind = SITE_KINDS[site_id]
            if site_id == target:
                outcome, flow, subsite, verdict = 3, 1, 0, TERMINAL_VERDICTS[site_id]
            elif site_id == 50:
                outcome, flow, subsite, verdict = 5, 0, 0, -1
            elif site_id in NOT_ELIGIBLE_SITES:
                outcome, flow, subsite, verdict = 5, 0, 0, -1
            elif site_kind == 1:
                outcome, flow, subsite, verdict = 1, 0, 0, -1
            else:
                outcome, flow, subsite, verdict = 2, 0, 0, -1
            sites.append({"id": site_id, "kind": site_kind, "outcome": outcome,
                          "flow": flow, "subsite": subsite, "verdict": verdict})
        facts = draw["predicateFacts"]
        if 53 in [site["id"] for site in sites]:
            fact = holo if holo is not None else holo_fact(draw, **(holo_options or {}))
            facts.append(fact)
            site = next(s for s in sites if s["id"] == 53)
            if fact["predicateResult"] == "yes":
                site.update(outcome=3, flow=1, verdict=4)
                target = 53
                sites = sites[:sites.index(site) + 1]
        if 55 in [site["id"] for site in sites]:
            fact = scrim if scrim is not None else scrim_fact(draw, **(scrim_options or {}))
            facts.append(fact)
            site = next(s for s in sites if s["id"] == 55)
            if fact["predicateResult"] == "yes":
                site.update(outcome=3, flow=1, verdict=17)
                target = 55
        if target not in [s["id"] for s in sites]:
            sites.extend({"id": site_id, "kind": SITE_KINDS[site_id],
                          "outcome": (1 if SITE_KINDS[site_id] == 1 else 2),
                          "flow": 0, "subsite": 0, "verdict": -1}
                         for site_id in EYE[EYE.index(sites[-1]["id"]) + 1:EYE.index(target) + 1])
        sites = sites[:next(i for i, site in enumerate(sites) if site["id"] == target) + 1]
        draw["sites"] = sites
        draw["winnerSiteId"] = target
        draw["verdict"] = TERMINAL_VERDICTS[target]
        draw["predicateFacts"] = facts
        draw["forwardFacts"] = None
        return trace

    holo_positive = holo_scrim_trace(53)
    holo_positive_summary = validate_trace(holo_positive)
    if (holo_positive_summary["predicateReplay"]["holoStatus"] != "replayed" or
            holo_positive_summary["predicateReplay"]["holoReplayed"] != 1 or
            holo_positive_summary["predicateReplay"]["factCount"] != 5):
        print("Holo exact resource/eye-size claim did not replay")
        return 1

    holo_boundary = validate_trace(holo_scrim_trace(53, holo_options={"eye_mode": "plus-two"}))
    holo_negative_boundary = validate_trace(holo_scrim_trace(
        53, holo_options={"eye_mode": "minus-two"}))
    if (holo_boundary["predicateReplay"]["holoStatus"] != "replayed" or
            holo_negative_boundary["predicateReplay"]["holoStatus"] != "replayed"):
        print("Holo inclusive positive/negative two eye-size boundary did not replay")
        return 1

    holo_miss = holo_scrim_trace(56, holo_options={"eye_mode": "zero-eye-height", "before": 59})
    holo_miss_summary = validate_trace(holo_miss)
    miss_fact = next(f for f in holo_miss["draws"][0]["predicateFacts"] if f["siteId"] == 53)
    if (holo_miss_summary["predicateReplay"]["holoStatus"] != "replayed" or
            miss_fact["missedBefore"] != 59 or miss_fact["missedAfter"] != 60 or
            miss_fact["missNotedAfter"] != "yes"):
        print("Holo zero-height short circuit or 59-to-60 counter mutation did not replay")
        return 1

    holo_wrap = holo_scrim_trace(56, holo_options={"eye_mode": "plus-three",
                                                   "before": (1 << 64) - 1})
    wrap_summary = validate_trace(holo_wrap)
    wrap_fact = next(f for f in holo_wrap["draws"][0]["predicateFacts"] if f["siteId"] == 53)
    if (wrap_summary["predicateReplay"]["holoStatus"] != "replayed" or
            wrap_fact["missedAfter"] != 0 or wrap_fact["missNotedAfter"] != "no"):
        print("Holo uint64 miss counter wrap did not replay")
        return 1

    holo_state_absent = validate_trace(holo_scrim_trace(
        56, holo_options={"eye_mode": "state-absent"}))
    if holo_state_absent["predicateReplay"]["holoStatus"] != "replayed":
        print("Holo absent-state early return did not replay")
        return 1

    holo_pattern_miss = validate_trace(holo_scrim_trace(
        56, holo_options={"pattern_fmt": 71}))
    holo_depth_failure = validate_trace(holo_scrim_trace(
        56, holo_options={"depth_source": 2}))
    if (holo_pattern_miss["predicateReplay"]["holoStatus"] != "replayed" or
            holo_depth_failure["predicateReplay"]["holoStatus"] != "replayed"):
        print("Holo pattern/depth failures did not preserve lazy short circuits")
        return 1

    scrim_positive = holo_scrim_trace(55, draw_count=120)
    scrim_summary = validate_trace(scrim_positive)
    if (scrim_summary["predicateReplay"]["scrimStatus"] != "replayed" or
            scrim_summary["predicateReplay"]["scrimReplayed"] != 1 or
            scrim_summary["predicateReplay"]["factCount"] != 6):
        print("Scrim ordered wash/UI selector did not replay the six-fact path")
        return 1

    scrim_failed_resolve = validate_trace(holo_scrim_trace(
        56, draw_count=120, scrim_options={"wash_source": 2}))
    scrim_raw_shadow = validate_trace(holo_scrim_trace(
        55, draw_count=120, scrim_options={"wash_source": 3}))
    scrim_ui_failure = validate_trace(holo_scrim_trace(
        56, draw_count=120, scrim_options={"ui_source": 2}))
    if (scrim_failed_resolve["predicateReplay"]["scrimStatus"] != "replayed" or
            scrim_raw_shadow["predicateReplay"]["scrimStatus"] != "replayed" or
            scrim_ui_failure["predicateReplay"]["scrimStatus"] != "replayed"):
        print("Scrim fresh resolve failure was not treated as a known selector miss")
        return 1

    scrim_warm_cache = validate_trace(holo_scrim_trace(
        56, draw_count=120, scrim_options={"wash_source": 4}))
    if scrim_warm_cache["predicateReplay"]["scrimStatus"] != "unreplayable":
        print("Scrim warm cache without raw descriptor was treated as replayable")
        return 1

    scrim_warm_claim = json.loads(json.dumps(scrim_positive))
    next(f for f in scrim_warm_claim["draws"][0]["predicateFacts"]
         if f["siteId"] == 55)["wash"] = resource_fact(4)
    if validate_trace(scrim_warm_claim)["predicateReplay"]["scrimStatus"] != "unreplayable":
        print("Scrim warm wash cache with reached UI was treated as replayable")
        return 1

    holo_observation_mutation = json.loads(json.dumps(holo_positive))
    holo_obs = next(f for f in holo_observation_mutation["draws"][0]["predicateFacts"]
                    if f["siteId"] == 53)
    holo_obs["eyeSize"]["result"] = "no"
    if validate_trace(holo_observation_mutation)["predicateReplay"]["holoMismatches"] == 0:
        print("Holo observed eye-size result mutation was not detected")
        return 1

    holo_mutation = json.loads(json.dumps(holo_miss))
    holo_obs = next(f for f in holo_mutation["draws"][0]["predicateFacts"]
                    if f["siteId"] == 53)
    holo_obs["missedAfter"] = 61
    if validate_trace(holo_mutation)["predicateReplay"]["holoMismatches"] == 0:
        print("Holo counter after-value mutation was not detected")
        return 1

    for label, edit in (
            ("missing Holo fact", lambda d: d["predicateFacts"].pop()),
            ("duplicate Holo fact", lambda d: d["predicateFacts"].append(
                json.loads(json.dumps(next(f for f in d["predicateFacts"] if f["siteId"] == 53))))),
            ("unfinished Holo stages", lambda d: d["predicateFacts"][-1].pop("eyeSize")),
            ("missing reached eye-size inputs", lambda d: next(
                f for f in d["predicateFacts"] if f["siteId"] == 53).__setitem__(
                    "eyeSize", {"reached": "unknown", "statePresent": "unknown",
                                "result": "unknown", "readMask": 0,
                                "depthW": 0, "depthH": 0, "eyeW": 0, "eyeH": 0,
                                "renderW": 0, "renderH": 0}))):
        malformed = json.loads(json.dumps(holo_positive))
        edit(malformed["draws"][0])
        try:
            validate_trace(malformed)
        except TraceError:
            continue
        print("Holo/Scrim strict facts accepted %s" % label)
        return 1

    historical_v4 = json.loads(json.dumps(base))
    historical_v4["predicateFactVersion"] = 4
    if validate_trace(historical_v4)["predicateReplay"]["holoStatus"] != "unavailable-v1-v4":
        print("predicate fact version 4 did not remain compatible after v5")
        return 1

    def stars_trace(interested=True, kind="X", count=6, instances=1,
                    hidden="yes", context_valid="yes", hash_known=True,
                    hash_source=3, vs_hash="0000000000000000", claimed=False):
        trace = json.loads(json.dumps(vr))
        draw = trace["draws"][0]
        draw["kind"] = ord(kind)
        draw["command"] = DRAW_COMMANDS[ord(kind)]
        draw["count"] = count
        draw["instances"] = instances
        shape_match = (kind in ("X", "N") and instances > 0 and count >= 6)
        fact = {
            "siteId": 6, "kind": 4, "known": "yes", "interestMaskKnown": "yes",
            "legacyInterestMask": "%016X" % ((1 << 1) if interested else 0),
            "helperReached": "yes" if interested else "no",
            "hiddenKnown": "yes", "hidden": hidden,
            "contextKnown": "yes" if hidden == "yes" else
                ("yes" if not interested else "unknown"),
            "contextValid": context_valid if hidden == "yes" else
                ("yes" if not interested else "unknown"),
            "shapeReached": "yes" if hidden == "yes" and context_valid == "yes" else "unknown",
            "shapeMatched": ("yes" if shape_match else "no")
                if hidden == "yes" and context_valid == "yes" else "unknown",
            "hashKnown": "yes" if hash_known and hidden == "yes" and
                context_valid == "yes" and shape_match else "unknown",
            "hashSource": hash_source if hash_known and hidden == "yes" and
                context_valid == "yes" and shape_match else 0,
            "vsHash": vs_hash if hash_known and hidden == "yes" and
                context_valid == "yes" and shape_match else "0000000000000000",
            "skippedDeltaKnown": interested,
            "skippedDelta": int(claimed) if interested else 0,
        }
        draw["predicateFacts"] = [f for f in draw["predicateFacts"] if f["siteId"] != 6]
        draw["predicateFacts"].append(fact)
        event = next(site for site in draw["sites"] if site["id"] == 6)
        if not interested:
            event.update(outcome=5, flow=0, subsite=0, verdict=-1)
        elif claimed:
            event.update(outcome=4, flow=1, subsite=0, verdict=2)
            draw["sites"] = draw["sites"][:draw["sites"].index(event) + 1]
            draw["winnerSiteId"] = 6
            draw["verdict"] = 2
            draw["forwardFacts"] = None
        else:
            event.update(outcome=2, flow=0, subsite=0, verdict=-1)
        return trace

    def offscreen_census_fact(rules, resolved="yes", texture="yes",
                              width=640, height=480, delta=0,
                              delta_known=True):
        reached = bool(rules)
        return {"siteId": 24, "kind": 5, "known": "yes",
                "offscreenRuleCount": len(rules), "offscreenRules": rules,
                "quadArmed": "unknown", "offscreenEyeDrawsLastFrame": 0,
                "offscreenProbeReached": "yes" if reached else "no",
                "offscreenProbeResolved": resolved if reached else "unknown",
                "offscreenProbeTexture2D": texture if reached and resolved == "yes" else "unknown",
                "offscreenTargetW": width if reached and resolved == "yes" and texture == "yes" else 0,
                "offscreenTargetH": height if reached and resolved == "yes" and texture == "yes" else 0,
                "censusSkippedDeltaKnown": delta_known,
                "censusSkippedDelta": delta if delta_known else 0}

    def offscreen_quad_fact(rule, draw_kind, draw_count, armed="yes", eye_draws=0,
                            resolved="yes", texture="yes", width=640,
                            height=480):
        reached = (armed == "yes" and eye_draws < 100 and
                   draw_kind == rule["kind"] and draw_count == rule["count"])
        return {"siteId": 26, "kind": 6, "known": "yes",
                "offscreenRuleCount": 1, "offscreenRules": [rule],
                "quadArmed": armed,
                "offscreenEyeDrawsLastFrame": eye_draws,
                "offscreenProbeReached": "yes" if reached else "no",
                "offscreenProbeResolved": resolved if reached else "unknown",
                "offscreenProbeTexture2D": texture if reached and resolved == "yes" else "unknown",
                "offscreenTargetW": width if reached and resolved == "yes" and texture == "yes" else 0,
                "offscreenTargetH": height if reached and resolved == "yes" and texture == "yes" else 0,
                "censusSkippedDeltaKnown": False, "censusSkippedDelta": 0}

    def offscreen_trace(terminal, kind="X", count=6, rules=None,
                        probe_resolved="yes", texture="yes", width=640,
                        height=480, delta=None, quad_rule=None, armed="yes",
                        eye_draws=0):
        trace = json.loads(json.dumps(vr))
        draw = trace["draws"][0]
        draw["route"] = 4
        draw["sequence"] = 2
        draw["kind"] = ord(kind)
        draw["command"] = DRAW_COMMANDS[ord(kind)]
        draw["count"] = count
        common_events = [event for event in draw["sites"] if event["id"] in COMMON]
        # The shared VR source fixture has a declined gate event, but this
        # offscreen case supplies gateWanted=yes and must observe that site.
        next(event for event in common_events if event["id"] == 3)["outcome"] = 1
        offscreen_ids = [70, 22, 20, 21, 23]
        if terminal == 24:
            offscreen_ids.append(24)
        elif terminal == 26:
            offscreen_ids.extend((24, 25, 26))
        else:
            offscreen_ids.extend((24, 25, 26, 27, 28))
        events = list(common_events)
        for site_id in offscreen_ids:
            final = site_id == terminal
            outcome = (4 if site_id in (24, 28) else 3) if final else (
                1 if SITE_KINDS[site_id] == 1 else 2)
            events.append({"id": site_id, "kind": SITE_KINDS[site_id],
                           "outcome": outcome, "flow": 1 if final else 0,
                           "subsite": 0, "verdict":
                               (TERMINAL_VERDICTS[site_id] if final else -1)})
        draw["sites"] = events
        draw["winnerSiteId"] = terminal
        draw["verdict"] = TERMINAL_VERDICTS[terminal]
        draw["forwardFacts"] = None
        if rules is None:
            rules = [{"kind": ord("X"), "count": count, "w": 640, "h": 480}]
        facts = [fact for fact in draw["predicateFacts"] if fact["siteId"] in (3, 6)]
        if delta is None:
            delta = int(terminal == 24 and bool(rules) and
                        probe_resolved == "yes" and texture == "yes" and
                        any(width == rule["w"] and height == rule["h"] and
                            (rule["kind"] == 0 or
                             (rule["kind"] == draw["kind"] and rule["count"] == count))
                            for rule in rules))
        facts.append(offscreen_census_fact(
            rules, probe_resolved, texture, width, height,
            delta if terminal == 24 else 0))
        if terminal in (26, 28):
            if quad_rule is None:
                quad_rule = {"kind": draw["kind"], "count": count,
                             "w": width, "h": height}
            facts.append(offscreen_quad_fact(quad_rule, draw["kind"], count,
                                             armed, eye_draws,
                                             probe_resolved, texture,
                                             width, height))
        draw["predicateFacts"] = facts
        return trace

    try:
        for eye_index, ranges, expected in selector_cases:
            result = validate_trace(range_trace(eye_index, ranges, expected))
            if result["predicateReplay"]["mismatches"] or result["predicateReplay"]["unreplayable"]:
                raise TraceError("known selector fixture did not replay completely")
        failed_mode = range_trace(1, [], selector_cases[3][2])
        failed_draw = failed_mode["draws"][0]
        failed_nv = next(fact for fact in failed_draw["predicateFacts"]
                         if fact["siteId"] == 50)
        failed_nv["failed"] = "yes"
        failed_draw["sites"][-1].update(outcome=2, flow=0, subsite=0, verdict=-1)
        failed_draw["sites"].append({"id": 51, "kind": 2, "outcome": 3,
                                     "flow": 1, "subsite": 0, "verdict": 2})
        failed_draw.update(winnerSiteId=51, verdict=2)
        failed_summary = validate_trace(failed_mode)
        if (failed_summary["predicateReplay"]["nightVisionStatus"] != "replayed" or
                predicate_replay_gate_failure(failed_summary) is not None):
            raise TraceError("failed NV mode did not replay as a negative selector")
        unknown_result = validate_trace(range_trace(4, [], selector_cases[3][2], "unknown"))
        if unknown_result["predicateReplay"]["unreplayable"] != 1:
            raise TraceError("unknown range fact was coerced to configured-off")
        known_empty = validate_trace(range_trace(4, [], selector_cases[3][2]))
        if known_empty["predicateReplay"]["unreplayable"]:
            raise TraceError("known empty range table became unreplayable")
        old_v2_site50 = range_trace(4, [], selector_cases[3][2])
        old_v2_site50["predicateFactVersion"] = 1
        old_v2_site50["draws"][0]["predicateFacts"] = [
            fact for fact in old_v2_site50["draws"][0]["predicateFacts"]
            if fact["siteId"] not in (6, 50)]
        old_v2_summary = validate_trace(old_v2_site50)
        if old_v2_summary["predicateReplay"]["nightVisionStatus"] != "unavailable-v1":
            raise TraceError("v2/factVersion1 site50 capture did not remain readable and NV-unavailable")
        old_fact_v2 = range_trace(4, [], selector_cases[3][2])
        old_fact_v2["predicateFactVersion"] = 2
        old_fact_v2["draws"][0]["predicateFacts"] = [
            fact for fact in old_fact_v2["draws"][0]["predicateFacts"]
            if fact["siteId"] != 6]
        old_fact_v2_summary = validate_trace(old_fact_v2)
        if (old_fact_v2_summary["predicateReplay"]["nightVisionStatus"] != "replayed" or
                old_fact_v2_summary["predicateReplay"]["witchspaceStarsStatus"] !=
                "unavailable-v1-or-v2"):
            raise TraceError("predicate fact version 2 capture did not retain NV and report site 6 unavailable")
        old_fact_v3 = offscreen_trace(24)
        old_fact_v3["predicateFactVersion"] = 3
        old_fact_v3["draws"][0]["predicateFacts"] = [
            fact for fact in old_fact_v3["draws"][0]["predicateFacts"]
            if fact["siteId"] in (3, 6)]
        old_fact_v3_summary = validate_trace(old_fact_v3)
        if (old_fact_v3_summary["predicateReplay"]["witchspaceStarsStatus"] != "replayed" or
                old_fact_v3_summary["predicateReplay"]["offscreenCensusStatus"] !=
                "unavailable-v1-v3"):
            raise TraceError("predicate fact version 3 compatibility lost Stars or claimed offscreen facts")

        census_rules = [
            {"kind": ord("N"), "count": 6, "w": 640, "h": 480},
            {"kind": 0, "count": 0, "w": 640, "h": 480},
        ]
        census_match = offscreen_trace(24, rules=census_rules, delta=1)
        census_summary = validate_trace(census_match)
        census_replay = census_summary["predicateReplay"]
        if (census_replay["offscreenCensusStatus"] != "replayed" or
                census_replay["offscreenCensusReplayed"] != 1 or
                census_replay["mutationUnobserved"] != 0 or
                predicate_replay_gate_failure(census_summary) is not None):
            raise TraceError("offscreen census wildcard/ordered-rule claim did not replay: %r sites=%r" %
                             (census_replay, census_match["draws"][0]["sites"]))

        census_zero = offscreen_trace(26, rules=[])
        if (validate_trace(census_zero)["predicateReplay"]["offscreenCensusStatus"] != "replayed" or
                next(fact for fact in census_zero["draws"][0]["predicateFacts"]
                     if fact["siteId"] == 24)["offscreenProbeReached"] != "no"):
            raise TraceError("zero-rule offscreen census did not record a skipped probe distinctly")
        census_unresolved = offscreen_trace(28, rules=[
            {"kind": ord("X"), "count": 6, "w": 640, "h": 480}],
            probe_resolved="no", delta=0, quad_rule={"kind": ord("D"), "count": 8,
                                                         "w": 2, "h": 2})
        if validate_trace(census_unresolved)["predicateReplay"]["offscreenCensusStatus"] != "replayed":
            raise TraceError("failed offscreen resource resolution was confused with a skipped probe")
        census_nontexture = offscreen_trace(28, rules=[
            {"kind": ord("X"), "count": 6, "w": 640, "h": 480}],
            texture="no", delta=0, quad_rule={"kind": ord("D"), "count": 8,
                                                "w": 2, "h": 2})
        if validate_trace(census_nontexture)["predicateReplay"]["offscreenCensusStatus"] != "replayed":
            raise TraceError("non-texture offscreen probe was not a known selector miss")
        census_kind_zero = offscreen_trace(28, rules=[
            {"kind": ord("X"), "count": 0, "w": 640, "h": 480}],
            quad_rule={"kind": 0, "count": 0, "w": 0, "h": 0}, armed="no")
        if validate_trace(census_kind_zero)["predicateReplay"]["offscreenCensusStatus"] != "replayed":
            raise TraceError("parser-valid KIND:0 census rule was rejected or matched a six-index draw")

        quad_rule = {"kind": ord("X"), "count": 6, "w": 640, "h": 480}
        quad_match = offscreen_trace(26, rules=[], quad_rule=quad_rule)
        quad_summary = validate_trace(quad_match)
        if (quad_summary["predicateReplay"]["offscreenQuadStatus"] != "replayed" or
                quad_summary["predicateReplay"]["offscreenQuadReplayed"] != 1 or
                quad_summary["predicateReplay"]["factCount"] != 4):
            raise TraceError("offscreen quad did not replay the maximum four-fact canonical path")
        quad_before_cutoff = offscreen_trace(26, rules=[], quad_rule=quad_rule,
                                             eye_draws=99)
        if validate_trace(quad_before_cutoff)["predicateReplay"]["offscreenQuadStatus"] != "replayed":
            raise TraceError("quad did not claim at the last pre-scene draw count")

        quad_unarmed = offscreen_trace(28, rules=[], quad_rule={
            "kind": 0, "count": 0, "w": 0, "h": 0}, armed="no")
        if validate_trace(quad_unarmed)["predicateReplay"]["offscreenQuadStatus"] != "replayed":
            raise TraceError("unarmed quad raw-zero configuration did not replay")
        quad_cutoff = offscreen_trace(28, rules=[], quad_rule=quad_rule, eye_draws=100)
        if (validate_trace(quad_cutoff)["predicateReplay"]["offscreenQuadStatus"] != "replayed" or
                quad_cutoff["draws"][0]["predicateFacts"][-1]["offscreenProbeReached"] != "no"):
            raise TraceError("quad's exact eye-draw cutoff failed to short-circuit")
        quad_shape_miss = offscreen_trace(28, kind="D", count=8, rules=[],
                                          quad_rule=quad_rule)
        if validate_trace(quad_shape_miss)["predicateReplay"]["offscreenQuadStatus"] != "replayed":
            raise TraceError("quad kind/count miss did not skip its resource probe")
        quad_probe_failed = offscreen_trace(28, rules=[], quad_rule=quad_rule,
                                            probe_resolved="no")
        if validate_trace(quad_probe_failed)["predicateReplay"]["offscreenQuadStatus"] != "replayed":
            raise TraceError("quad failed resource resolution did not replay as a known miss")
        quad_nontexture = offscreen_trace(28, rules=[], quad_rule=quad_rule,
                                          texture="no")
        if validate_trace(quad_nontexture)["predicateReplay"]["offscreenQuadStatus"] != "replayed":
            raise TraceError("quad non-texture result did not replay as a known miss")
        quad_dimension_miss = offscreen_trace(28, rules=[], quad_rule=quad_rule,
                                              width=641)
        if validate_trace(quad_dimension_miss)["predicateReplay"]["offscreenQuadStatus"] != "replayed":
            raise TraceError("quad target-dimension mismatch did not replay")

        census_mutation = json.loads(json.dumps(census_match))
        census_mutation_fact = next(fact for fact in census_mutation["draws"][0]["predicateFacts"]
                                    if fact["siteId"] == 24)
        census_mutation_fact["censusSkippedDelta"] = 0
        census_mutation_summary = validate_trace(census_mutation)
        if (census_mutation_summary["predicateReplay"]["offscreenCensusMismatches"] == 0 or
                predicate_replay_gate_failure(census_mutation_summary) is None):
            raise TraceError("offscreen census mutation mismatch passed the gate")
        quad_mutation = json.loads(json.dumps(quad_match))
        quad_mutation["draws"][0]["predicateFacts"][-1]["offscreenTargetW"] = 639
        quad_mutation_summary = validate_trace(quad_mutation)
        if (quad_mutation_summary["predicateReplay"]["offscreenQuadMismatches"] == 0 or
                predicate_replay_gate_failure(quad_mutation_summary) is None):
            raise TraceError("offscreen quad fact/output disagreement passed the gate")

        for malformed_fact in (
                {"offscreenRuleCount": 5},
                {"offscreenRules": [{"kind": ord("Q"), "count": 1, "w": 1, "h": 1}]},
                {"offscreenRules": [{"kind": 0, "count": 1, "w": 1, "h": 1}]},
                {"offscreenRules": [{"kind": ord("X"), "count": 1, "w": 0, "h": 1}]},
                {"offscreenProbeResolved": "unknown"},
                {"offscreenProbeTexture2D": "unknown"},
                {"offscreenTargetW": 0},
                {"offscreenProbeTexture2D": "no", "offscreenTargetW": 640},
                {"offscreenProbeReached": "no"},):
            malformed = json.loads(json.dumps(census_match))
            malformed_fact_target = next(fact for fact in malformed["draws"][0]["predicateFacts"]
                                         if fact["siteId"] == 24)
            malformed_fact_target.update(malformed_fact)
            try:
                validate_trace(malformed)
            except TraceError:
                continue
            raise TraceError("malformed offscreen census stage/config was accepted: %r" % malformed_fact)

        missing_offscreen = json.loads(json.dumps(quad_match))
        missing_offscreen["draws"][0]["predicateFacts"] = [
            fact for fact in missing_offscreen["draws"][0]["predicateFacts"]
            if fact["siteId"] != 24]
        try:
            validate_trace(missing_offscreen)
        except TraceError:
            pass
        else:
            raise TraceError("visited site 24 without its v4 predicate fact was accepted")
        duplicate_offscreen = json.loads(json.dumps(quad_match))
        duplicate_offscreen["draws"][0]["predicateFacts"].append(
            next(fact for fact in duplicate_offscreen["draws"][0]["predicateFacts"]
                 if fact["siteId"] == 26))
        try:
            validate_trace(duplicate_offscreen)
        except TraceError:
            pass
        else:
            raise TraceError("duplicate offscreen predicate fact was accepted")
        stars_cases = [
            (stars_trace(hidden="no"), "replayed"),
            (stars_trace(hidden="yes", context_valid="no"), "replayed"),
            (stars_trace(kind="D", hidden="yes"), "replayed"),
            (stars_trace(count=5, hidden="yes"), "replayed"),
            (stars_trace(kind="N", instances=0, hidden="yes"), "replayed"),
            (stars_trace(hidden="yes", hash_source=3,
                         vs_hash="0000000000000000"), "replayed"),
            (stars_trace(hidden="yes", hash_source=2,
                         vs_hash="0000000000000000"), "replayed"),
            # The cached binding hash is zero, so the helper falls back; only
            # this captured lookup result can independently prove the hit.
            (stars_trace(hidden="yes", hash_source=3,
                         vs_hash="9AEC596A2B036EA6", claimed=True), "replayed"),
            (stars_trace(kind="N", hidden="yes", hash_source=3,
                         vs_hash="9AEC596A2B036EA6", claimed=True), "replayed"),
            (stars_trace(hidden="yes", hash_source=1,
                         vs_hash="9AEC596A2B036EA6", claimed=True), "replayed"),
            # Interest is staging only. A known cached miss replays the false
            # selector; an uncaptured fallback cannot be coerced into a miss.
            (stars_trace(interested=False, hidden="no"), "replayed"),
            (stars_trace(interested=False, hidden="yes", hash_known=True,
                         hash_source=1, vs_hash="0000000000000001"), "replayed"),
            (stars_trace(interested=False, hidden="yes", hash_known=False), "unreplayable"),
        ]
        for star_trace, expected_status in stars_cases:
            star_summary = validate_trace(star_trace)["predicateReplay"]
            if star_summary["witchspaceStarsStatus"] != expected_status:
                raise TraceError("witchspace-star selector fixture did not report %s: %r" %
                                 (expected_status, star_summary))
        stars_claim_mismatch = stars_trace(hidden="yes", hash_source=1,
                                           vs_hash="9AEC596A2B036EA6")
        stars_claim_mismatch_summary = validate_trace(stars_claim_mismatch)
        if (stars_claim_mismatch_summary["predicateReplay"]["witchspaceStarsStatus"] != "mismatch" or
                predicate_replay_gate_failure(stars_claim_mismatch_summary) is None):
            raise TraceError("witchspace-star selector mismatch passed replay gate")
        mutation_mismatch = stars_trace(hidden="yes", hash_source=1,
                                       vs_hash="9AEC596A2B036EA6", claimed=True)
        mutation_mismatch["draws"][0]["predicateFacts"][-1]["skippedDelta"] = 0
        mutation_summary = validate_trace(mutation_mismatch)
        if (mutation_summary["predicateReplay"]["witchspaceStarsStatus"] != "mismatch" or
                predicate_replay_gate_failure(mutation_summary) is None):
            raise TraceError("witchspace-star skipped-counter mismatch passed replay gate")
        for bad_key, bad_value in (("hidden", "unknown"),
                                   ("hashSource", 1),
                                   ("hashSource", 2)):
            malformed = stars_trace(hidden="yes", hash_source=1,
                                    vs_hash="0000000000000001")
            malformed["draws"][0]["predicateFacts"][-1][bad_key] = bad_value
            if bad_key == "hashSource" and bad_value == 1:
                malformed["draws"][0]["predicateFacts"][-1]["vsHash"] = "0000000000000000"
            elif bad_key == "hashSource" and bad_value == 2:
                malformed["draws"][0]["predicateFacts"][-1]["vsHash"] = "0000000000000001"
            try:
                validate_trace(malformed)
            except TraceError:
                continue
            raise TraceError("malformed witchspace-star input provenance was accepted")
        missing_stars = json.loads(json.dumps(vr))
        missing_stars["draws"][0]["predicateFacts"] = [
            fact for fact in missing_stars["draws"][0]["predicateFacts"]
            if fact["siteId"] != 6]
        try:
            validate_trace(missing_stars)
        except TraceError:
            pass
        else:
            raise TraceError("visited WitchspaceStars site without its v3 fact was accepted")
        duplicate_stars = json.loads(json.dumps(vr))
        duplicate_stars["draws"][0]["predicateFacts"].append(
            next(fact for fact in duplicate_stars["draws"][0]["predicateFacts"]
                 if fact["siteId"] == 6))
        try:
            validate_trace(duplicate_stars)
        except TraceError:
            pass
        else:
            raise TraceError("duplicate WitchspaceStars site facts were accepted")
        gate_off = json.loads(json.dumps(vr))
        gate_off_draw = gate_off["draws"][0]
        gate_off_draw["sites"] = gate_off_draw["sites"][:3]
        gate_off_draw["sites"][-1].update(outcome=4, flow=1, subsite=0, verdict=0)
        gate_off_draw.update(winnerSiteId=3, verdict=0, forwardFacts=None)
        gate_off_draw["predicateFacts"] = [
            {"siteId": 3, "kind": 1, "known": "yes", "gateWanted": "no"}]
        if validate_trace(gate_off)["predicateReplay"]["mismatches"]:
            raise TraceError("known disabled draw gate did not replay")
        gate_unknown = json.loads(json.dumps(gate_off))
        gate_unknown["draws"][0]["predicateFacts"] = [
            {"siteId": 3, "kind": 1, "known": "unknown",
             "gateWanted": "unknown"}]
        if validate_trace(gate_unknown)["predicateReplay"]["unreplayable"] != 1:
            raise TraceError("unknown draw gate was coerced to configured-off")
        if predicate_replay_gate_failure(validate_trace(gate_unknown)) is None:
            raise TraceError("unknown draw gate passed the replay gate")
        mismatch = json.loads(json.dumps(gate_off))
        mismatch["draws"][0]["predicateFacts"][0]["gateWanted"] = "yes"
        if predicate_replay_gate_failure(validate_trace(mismatch)) is None:
            raise TraceError("supported predicate mismatch passed the replay gate")
        missing_mutation = range_trace(4, [[4, 4]], selector_cases[0][2])
        missing_mutation["draws"][0]["predicateFacts"][-1]["censusSkippedDeltaKnown"] = False
        missing_mutation["draws"][0]["predicateFacts"][-1]["censusSkippedDelta"] = 0
        if predicate_replay_gate_failure(validate_trace(missing_mutation)) is None:
            raise TraceError("unobserved selector mutation passed the replay gate")
        duplicate = range_trace(4, [], selector_cases[3][2])
        duplicate_fact = json.loads(json.dumps(
            duplicate["draws"][0]["predicateFacts"][-1]))
        duplicate["draws"][0]["predicateFacts"] = [duplicate_fact,
                                                    json.loads(json.dumps(duplicate_fact))]
        validate_trace(duplicate)
        print("draw-ladder reader accepted a duplicate predicate fact")
        return 1
    except TraceError as exc:
        if "duplicates a supported site fact" not in str(exc):
            print("draw-ladder predicate replay fixture failed: %s (range test %s)" %
                  (exc, (eye_index, ranges)))
            return 1

    strict_cases = []
    missing_fact = json.loads(json.dumps(vr))
    del missing_fact["draws"][0]["predicateFacts"]
    strict_cases.append(("missing", missing_fact))
    invalid_fact = json.loads(json.dumps(vr))
    invalid_fact["draws"][0]["predicateFacts"][0]["gateWanted"] = "off"
    strict_cases.append(("invalid", invalid_fact))
    invalid_delta = range_trace(1, [[1, 4]], selector_cases[0][2])
    invalid_delta["draws"][0]["predicateFacts"][-1]["censusSkippedDelta"] = 2
    strict_cases.append(("oversized counter delta", invalid_delta))
    for label, invalid_trace in strict_cases:
        try:
            validate_trace(invalid_trace)
            print("draw-ladder reader accepted %s predicate fact" % label)
            return 1
        except TraceError:
            pass

    gated_observers = json.loads(json.dumps(vr))
    # This observer-only fixture predates raw Holo/Scrim facts.
    gated_observers["predicateFactVersion"] = 4
    gated_draw = gated_observers["draws"][0]
    gated_draw["sites"] = []
    gated_sites = COMMON + EYE[:EYE.index(67) + 1]
    for site_id in gated_sites:
        kind = SITE_KINDS[site_id]
        outcome = (1 if site_id == 3 else
                   5 if site_id in NOT_ELIGIBLE_SITES else
                   (1 if kind == 1 else 2))
        flow = 1 if site_id == 67 else 0
        verdict = 0 if site_id == 67 else -1
        if site_id == 67:
            outcome = 4
        gated_draw["sites"].append({
            "id": site_id, "kind": kind, "outcome": outcome,
            "flow": flow, "subsite": 0, "verdict": verdict,
        })
    gated_draw["winnerSiteId"] = 67
    gated_draw["verdict"] = 0
    gated_draw["forwardFacts"] = None
    gated_draw["predicateFacts"] = [
        {"siteId": 3, "kind": 1, "known": "yes", "gateWanted": "yes"},
        {"siteId": 6, "kind": 4, "known": "yes", "interestMaskKnown": "yes",
         "legacyInterestMask": "0000000000000000", "helperReached": "no",
         "hiddenKnown": "yes", "hidden": "no", "contextKnown": "yes",
         "contextValid": "no", "shapeReached": "unknown",
         "shapeMatched": "unknown", "hashKnown": "unknown", "hashSource": 0,
         "vsHash": "0000000000000000", "skippedDeltaKnown": False,
         "skippedDelta": 0},
        {"siteId": 49, "kind": 2, "known": "yes", "eyeDrawIndex": 1,
         "ranges": [], "censusSkippedDeltaKnown": True,
         "censusSkippedDelta": 0},
        {"siteId": 50, "kind": 3, "known": "yes",
         "dispatchEnabled": "yes", "activeMaskKnown": "yes",
         "activePluginMask": "%016X" % (1 << 1),
         "candidateKnown": "yes", "candidatePresent": "no",
         "modeKnown": "yes", "mode": 2, "shapeReached": "no",
         "shapeMatched": "unknown", "callbackReached": "no",
         "callbackModeKnown": "unknown", "callbackMode": 0,
         "failedKnown": "unknown", "failed": "unknown"}]
    gated_draw["vsHash"] = "0000000000000000"
    gated_draw["candidateMask"] = "0000000000000000"
    try:
        summary = validate_trace(gated_observers)
        if (summary["predicateReplay"]["nightVisionStatus"] != "replayed" or
                predicate_replay_gate_failure(summary) is not None):
            raise TraceError("known shader-pair miss did not replay as a negative NV selector: %r" % summary["predicateReplay"])
        if summary["inferredUnvisitedSiteCount"] != len(COMMON + EYE) - len(gated_sites):
            raise TraceError("gated observer trace suffix was inferred incorrectly")
    except Exception as exc:
        print("draw-ladder typed NotEligible observer fixture rejected: %s" % exc)
        return 1

    dispatch_off_pair = json.loads(json.dumps(gated_observers))
    dispatch_off_draw = dispatch_off_pair["draws"][0]
    dispatch_off_fact = dispatch_off_draw["predicateFacts"][-1]
    dispatch_off_fact.update(
        dispatchEnabled="no", activeMaskKnown="unknown", activePluginMask="0000000000000000",
        candidateKnown="unknown", candidatePresent="unknown", modeKnown="unknown", mode=0)
    dispatch_off_draw["vsHash"] = "FCF7BD2896751D96"
    dispatch_off_draw["psHash"] = "F786D34B5E118D5E"
    dispatch_off_draw["candidateMask"] = "0000000000000000"
    dispatch_off_summary = validate_trace(dispatch_off_pair)
    if (dispatch_off_summary["predicateReplay"]["nightVisionStatus"] != "unreplayable" or
            dispatch_off_summary["predicateReplay"]["nightVisionReplayed"] != 0):
        print("dispatch-off exact-pair NV fact falsely proved frozen selector: %r" %
              dispatch_off_summary["predicateReplay"])
        return 1

    dispatch_off_miss = json.loads(json.dumps(dispatch_off_pair))
    dispatch_off_miss["draws"][0]["vsHash"] = "0000000000000000"
    dispatch_off_miss_summary = validate_trace(dispatch_off_miss)
    if (dispatch_off_miss_summary["predicateReplay"]["nightVisionStatus"] != "replayed" or
            predicate_replay_gate_failure(dispatch_off_miss_summary) is not None):
        print("dispatch-off known shader miss did not prove a negative frozen selector: %r" %
              dispatch_off_miss_summary["predicateReplay"])
        return 1
    candidate_drift = json.loads(json.dumps(gated_observers))
    drift_draw = candidate_drift["draws"][0]
    drift_draw["vsHash"] = "FCF7BD2896751D96"
    drift_draw["psHash"] = "F786D34B5E118D5E"
    drift_result = validate_trace(candidate_drift)
    if (drift_result["predicateReplay"]["nightVisionMismatches"] == 0 or
            predicate_replay_gate_failure(drift_result) is None):
        print("candidate cache bit was used as the NV selector oracle")
        return 1
    mandatory_observer_gated = json.loads(json.dumps(gated_observers))
    mandatory_observer_gated["draws"][0]["sites"][9]["outcome"] = 5
    try:
        validate_trace(mandatory_observer_gated)
        print("draw-ladder reader accepted NotEligible for a mandatory observer")
        return 1
    except TraceError:
        pass

    generated = json.loads(json.dumps(vr))
    generated_action = {
        "id": 6, "phase": 2, "outcome": 2, "call": 4,
        "flags": (FLAG_BITS["action"]["generatedDrawArgsUnavailable"] |
                  FLAG_BITS["action"]["issueCountUnknown"]),
        "issueCount": 0, "issueCountKnown": False,
        "count": 0, "instances": 0, "start": 0, "startInstance": 0,
        "baseVertex": 0,
    }
    generated["draws"][0]["actions"].insert(1, generated_action)
    exact_replace_issue = {
        "id": 4, "phase": 2, "outcome": 2, "call": 4,
        "flags": 0, "issueCount": 1, "issueCountKnown": True,
        "count": 240, "instances": 1, "start": 0, "startInstance": 0,
        "baseVertex": 0,
    }
    generated["draws"][0]["actions"].insert(2, exact_replace_issue)
    try:
        validate_trace(generated)
    except Exception as exc:
        print("draw-ladder generated-helper fixture rejected: %s" % exc)
        return 1
    missing_generated_flag = json.loads(json.dumps(generated))
    missing_generated_flag["draws"][0]["actions"][1]["flags"] = FLAG_BITS["action"]["issueCountUnknown"]
    try:
        validate_trace(missing_generated_flag)
        print("draw-ladder reader accepted generated helper args without the unavailable flag")
        return 1
    except TraceError:
        pass
    misleading_generated_args = json.loads(json.dumps(generated))
    misleading_generated_args["draws"][0]["actions"][1]["count"] = 240
    try:
        validate_trace(misleading_generated_args)
        print("draw-ladder reader accepted fabricated generated helper args")
        return 1
    except TraceError:
        pass

    def literal_flat_action(action_id, phase, outcome, call, flags, issue_count,
                            count, instances, start, start_instance, base_vertex):
        return {"id": action_id, "phase": phase, "outcome": outcome, "call": call,
                "flags": flags, "issueCount": issue_count, "issueCountKnown": True,
                "count": count, "instances": instances, "start": start,
                "startInstance": start_instance, "baseVertex": base_vertex}

    bypass_cases = [
        (ord("A"), 7, 5, 72, 5, 18, False, "flat bypass/actions"),
        (ord("Z"), 7, 5, 72, 6, 19, True, "flat bypass/actions"),
        (ord("Y"), 7, 5, 72, 7, 20, True, "flat bypass/actions"),
        (ord("A"), 8, 6, 73, 5, 18, False, "DrawAuto"),
        (ord("Z"), 9, 7, 74, 6, 19, True, "indirect-draw"),
        (ord("Y"), 10, 8, 75, 7, 20, True, "indirect-draw"),
        (ord("X"), 11, 9, 76, 4, 2, False, "internal/world bypass"),
    ]
    for kind, route, sequence, site_id, call, action_id, has_buffer, scope_text in bypass_cases:
        bypass = json.loads(json.dumps(base))
        draw = bypass["draws"][0]
        draw["kind"] = kind
        draw["command"] = DRAW_COMMANDS[kind]
        draw["count"] = 0
        draw["instances"] = 1 if kind == ord("X") else 0
        draw["drawParametersKnown"] = kind == ord("X")
        draw["argumentBufferKnown"] = has_buffer
        draw["argumentByteOffset"] = 64 if has_buffer else 0
        draw["resources"]["argumentBuffer"] = 5 if has_buffer else 0
        draw["route"] = route
        draw["sequence"] = sequence
        draw["sites"] = [{"id": site_id, "kind": 3, "outcome": 4,
                          "flow": 1, "subsite": 0, "verdict": 0}]
        draw["winnerSiteId"] = site_id
        draw["verdict"] = 0
        call_args = {"flags": 0, "issueCount": 0, "issueCountKnown": True,
                      "count": 0, "instances": 1, "start": 0,
                      "startInstance": 0, "baseVertex": 0}
        action_flags = (FLAG_BITS["action"]["gpuDrawArgsUnavailable"]
                        if call in (5, 6, 7) else 0)
        draw["actions"] = [
            dict({"id": 1, "phase": 1, "outcome": 2, "call": call}, **call_args),
            {"id": action_id, "phase": 2, "outcome": 2, "call": call,
             "flags": action_flags, "issueCount": 1, "issueCountKnown": True,
             "count": 0, "instances": 1 if kind == ord("X") else 0,
             "start": 0, "startInstance": 0,
             "baseVertex": 0},
            dict({"id": 17, "phase": 3, "outcome": 2, "call": call}, **call_args),
        ]
        if route == 7:
            unavailable = FLAG_BITS["action"]["gpuDrawArgsUnavailable"] if kind in (
                ord("A"), ord("Y"), ord("Z")) else 0
            action_start = draw["argumentByteOffset"] if kind in (ord("Y"), ord("Z")) else 0
            draw["actions"] = [
                literal_flat_action(1, 1, 2, call, 0, 0, draw["count"], draw["instances"],
                                    action_start, 0, 0),
                literal_flat_action(action_id, 2, 2, call, unavailable, 1,
                                    draw["count"], draw["instances"], action_start, 0, 0),
                literal_flat_action(17, 3, 2, call, 0, 0, draw["count"], draw["instances"],
                                    action_start, 0, 0),
            ]
        try:
            summary = validate_trace(bypass)
            if scope_text not in summary["scope"]:
                raise TraceError("bypass coverage label is %r" % summary["scope"])
        except Exception as exc:
            print("draw-ladder bypass fixture rejected for kind %r: %s" % (kind, exc))
            return 1
        if route == 7 and kind == ord("Y"):
            missing_gpu_args_flag = json.loads(json.dumps(bypass))
            missing_gpu_args_flag["draws"][0]["actions"][1]["flags"] = 0
            try:
                validate_trace(missing_gpu_args_flag)
                print("draw-ladder flat indirect action accepted available GPU arguments")
                return 1
            except TraceError as exc:
                if "GPU-argument availability disagrees with call kind" not in str(exc):
                    print("draw-ladder flat indirect flag failed outside its structural guard: %s" % exc)
                    return 1
        if route == 7 and kind in (ord("A"), ord("Y"), ord("Z")):
            for field in ("count", "instances"):
                bad_source_count = json.loads(json.dumps(bypass))
                bad_draw = bad_source_count["draws"][0]
                bad_draw[field] = 1
                for action in bad_draw["actions"]:
                    action[field] = 1
                try:
                    validate_trace(bad_source_count)
                    print("draw-ladder flat %s fixture accepted nonzero source %s" %
                          (chr(kind), field))
                    return 1
                except TraceError as exc:
                    if "flat Auto/indirect source counts must be zero" not in str(exc):
                        print("draw-ladder flat %s source-count guard failed unexpectedly: %s" %
                              (chr(kind), exc))
                        return 1
        if route == 7 and kind in (ord("Y"), ord("Z")):
            changed_offset_action = json.loads(json.dumps(bypass))
            changed_offset_action["draws"][0]["actions"][1]["start"] += 4
            try:
                validate_trace(changed_offset_action)
                print("draw-ladder flat indirect replay accepted a changed byte-offset action")
                return 1
            except TraceError as exc:
                if "flat bypass actions disagree" not in str(exc):
                    print("draw-ladder indirect offset failed outside replay comparison: %s" % exc)
                    return 1
        if route in (8, 9, 10):
            blocked = json.loads(json.dumps(bypass))
            blocked_draw = blocked["draws"][0]
            blocked_draw["sites"][0]["subsite"] = 1
            blocked_action = blocked_draw["actions"][1]
            blocked_action["outcome"] = 3
            blocked_action["issueCount"] = 0
            try:
                validate_trace(blocked)
            except Exception as exc:
                print("draw-ladder blocked bypass fixture rejected for kind %r: %s" %
                      (kind, exc))
                return 1
            bad_block_count = json.loads(json.dumps(blocked))
            bad_block_count["draws"][0]["actions"][1]["issueCount"] = 1
            try:
                validate_trace(bad_block_count)
                print("draw-ladder reader accepted a declined bypass with a nonzero issue count")
                return 1
            except TraceError:
                pass
            bad_block_marker = json.loads(json.dumps(blocked))
            bad_block_marker["draws"][0]["sites"][0]["subsite"] = 0
            try:
                validate_trace(bad_block_marker)
                print("draw-ladder reader accepted a declined bypass without its block marker")
                return 1
            except TraceError:
                pass

    # Frozen route-7 values from the direct hook arguments. These literals are
    # deliberately independent of _flat_bypass_action_plan.
    flat_direct_cases = (
        (ord("D"), 1, 321, 1, {"start": 0, "base": -19088744, "startInstance": 0},
         0xfedcba98, 0, 0),
        (ord("I"), 2, 321, 1, {"start": 0x87654321, "base": -123456789, "startInstance": 0},
         0x87654321, 0, -123456789),
        (ord("N"), 3, 321, 5, {"start": 0, "base": -19088744, "startInstance": 0x76543210},
         0xfedcba98, 0x76543210, 0),
        (ord("X"), 4, 321, 5, {"start": 0x87654321, "base": -123456789, "startInstance": 0x76543210},
         0x87654321, 0x76543210, -123456789),
    )
    flat_x_fixture = None
    for (kind, call, count, instances, source_args, expected_start,
         expected_start_instance, expected_base) in flat_direct_cases:
        flat = json.loads(json.dumps(base))
        draw = flat["draws"][0]
        draw.update(kind=kind, command=DRAW_COMMANDS[kind], count=count,
                    instances=instances, args=source_args)
        expected = [
            literal_flat_action(1, 1, 2, call, 0, 0, count, instances,
                                expected_start, expected_start_instance, expected_base),
            literal_flat_action(2, 2, 2, call, 0, 1, count, instances,
                                expected_start, expected_start_instance, expected_base),
            literal_flat_action(17, 3, 2, call, 0, 0, count, instances,
                                expected_start, expected_start_instance, expected_base),
        ]
        draw["actions"] = expected
        try:
            validate_trace(flat)
        except Exception as exc:
            print("draw-ladder flat direct action fixture failed for %s: %s" %
                  (chr(kind), exc))
            return 1
        if kind == ord("X"):
            flat_x_fixture = flat

    # Every original-action draw argument is checked against the captured
    # inputs, even when the mutation remains a valid serialized integer.
    for field in ("count", "instances", "start", "startInstance", "baseVertex"):
        changed = json.loads(json.dumps(flat_x_fixture))
        changed["draws"][0]["actions"][1][field] += 1
        try:
            validate_trace(changed)
            print("draw-ladder flat action replay accepted changed original %s" % field)
            return 1
        except TraceError as exc:
            if "flat bypass actions disagree" not in str(exc):
                print("draw-ladder original %s failed outside replay comparison: %s" %
                      (field, exc))
                return 1
    for action_index, field in ((0, "count"), (2, "startInstance")):
        changed = json.loads(json.dumps(flat_x_fixture))
        changed["draws"][0]["actions"][action_index][field] += 1
        try:
            validate_trace(changed)
            print("draw-ladder flat action replay accepted changed envelope %s" % field)
            return 1
        except TraceError as exc:
            if "flat bypass actions disagree" not in str(exc):
                print("draw-ladder envelope %s failed outside replay comparison: %s" %
                      (field, exc))
                return 1
    wrong_flat_call = json.loads(json.dumps(flat_x_fixture))
    wrong_flat_call["draws"][0]["actions"][1]["call"] = 3
    try:
        validate_trace(wrong_flat_call)
        print("draw-ladder flat action replay accepted a wrong original call kind")
        return 1
    except TraceError as exc:
        if "bypass original action call kind is wrong" not in str(exc):
            print("draw-ladder wrong call failed outside its structural guard: %s" % exc)
            return 1

    def reverse_vr_sites(data):
        draw = data["draws"][0]
        draw["route"] = 6
        draw["sequence"] = 4
        ids = [1, 2, 3, 4, 5, 6, 7, 8, 40, 41, 42, 43]
        draw["sites"] = []
        for site_id in ids:
            kind = SITE_KINDS[site_id]
            outcome = 3 if site_id == 43 else (1 if kind == 1 else 2)
            draw["sites"].append({
                "id": site_id, "kind": kind, "outcome": outcome,
                "flow": 1 if site_id == 43 else 0,
                "subsite": 6 if site_id == 8 else 0,
                "verdict": 7 if site_id == 43 else -1,
            })
        draw["winnerSiteId"] = 43
        draw["verdict"] = 7
        draw["predicateFacts"] = [{"siteId": 3, "kind": 1, "known": "yes",
                                   "gateWanted": "yes"}]
        draw["sites"].reverse()

    def remove_reached_vr_site(data):
        reverse_vr_sites(data)
        data["draws"][0]["sites"].reverse()
        del data["draws"][0]["sites"][10]

    def wrong_vr_site_verdict(data):
        reverse_vr_sites(data)
        draw = data["draws"][0]
        draw["sites"].reverse()
        draw["sites"][-1]["verdict"] = 6

    def wrong_vr_forward_verdict(data):
        reverse_vr_sites(data)
        draw = data["draws"][0]
        draw["sites"].reverse()
        draw["forwardFacts"] = json.loads(json.dumps(vr["draws"][0]["forwardFacts"]))
        draw["forwardFacts"]["verdictOrdinal"] = 6

    def remove_bypass_original(data):
        del data["draws"][0]["actions"][1]

    def reorder_bypass_actions(data):
        actions = data["draws"][0]["actions"]
        actions[1], actions[2] = actions[2], actions[1]

    mutations = [
        ("schema", lambda d: d.update(schemaVersion=3)),
        ("overflow", lambda d: d["footer"].update(overflow=True)),
        ("missing footer", lambda d: d.pop("footer")),
        ("unknown site", lambda d: d["draws"][0]["sites"][0].update(id=73)),
        ("terminal mismatch", lambda d: d["draws"][0].update(winnerSiteId=71)),
        ("exit verdict mismatch", lambda d: d["draws"][0].update(verdict=1)),
        ("unknown terminal verdict", lambda d: d["draws"][0]["sites"][0].update(verdict=999)),
        ("exit terminal mismatch", lambda d: d["draws"][0].update(winnerSiteId=-1)),
        ("unsafe log basename", lambda d: d.update(logFile="..\\other.log")),
        ("wrong build", lambda d: d.update(buildStamp="00000000")),
        ("predicate parity claim", lambda d: d["semantics"].update(predicateEquivalence=True)),
        ("unknown action count mismatch",
         lambda d: d["draws"][0]["actions"][1].update(flags=0x8000)),
        ("wrong-site verdict", wrong_vr_site_verdict),
        ("forward verdict mismatch", wrong_vr_forward_verdict),
        ("missing terminal", lambda d: d["draws"][0].update(sites=[])),
        ("invalid order", reverse_vr_sites),
        ("missing reached site", remove_reached_vr_site),
        ("missing action", remove_bypass_original),
        ("reordered action", reorder_bypass_actions),
        ("site overflow", lambda d: d["draws"][0].update(sites=d["draws"][0]["sites"] * 49)),
        ("forward availability mismatch",
         lambda d: d["draws"][0].update(forwardFacts=dict(
             presentMask=1, verdictOrdinal=-1, family=0, familyAvailable="unknown",
             owner="unknown", verdictForwards="yes", initialUiTake="unknown",
             afterUiTake="unknown", worldReissue="unknown", curveThisDraw="unknown",
             introCurveThisDraw="unknown", uiDepth="unknown", holoDepth="unknown",
             composite="unknown", crispPending="unknown", issueBlocked="unknown"))),
    ]
    for name, change in mutations:
        candidate = json.loads(json.dumps(base))
        change(candidate)
        try:
            validate_trace(candidate, "edvr_gfx_20261001_010203.log", "1234ABCD")
        except TraceError:
            continue
        print("draw-ladder trace self-test accepted invalid %s" % name)
        return 1
    # CLI gate and --expect-invalid remain distinct: parity failures reject a
    # valid current-schema sidecar, while the legacy unavailable status passes.
    cli_file = os.path.join(tempfile.gettempdir(), "edvr_draw_replay_cli_test.json")
    with open(cli_file, "w", encoding="utf-8") as stream:
        stream.write("{}")
    original_read_trace = read_trace
    try:
        historical = validate_trace(legacy_v1)
        for status in ("mismatch", "unreplayable", "mutation-unobserved"):
            failing = dict(historical)
            failing["predicateReplay"] = {
                "status": status, "factCount": 1, "replayed": 0,
                "unreplayable": int(status == "unreplayable"),
                "mismatches": int(status == "mismatch"),
                "mutationUnobserved": int(status == "mutation-unobserved"),
                "predicateFactVersion": 0, "nightVisionStatus": "unavailable-v1",
                "nightVisionFacts": 0, "nightVisionReplayed": 0,
                "nightVisionUnreplayable": 0, "nightVisionMismatches": 0}
            read_trace = lambda *args, _summary=failing, **kwargs: ({}, _summary)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = main(["--file", cli_file, "--dry-run"])
            if code != 1 or "predicate replay gate failed" not in output.getvalue():
                print("draw-ladder CLI accepted %s predicate replay" % status)
                return 1
        read_trace = lambda *args, **kwargs: ({}, historical)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = main(["--file", cli_file, "--dry-run"])
        if code != 0 or "unavailable" not in output.getvalue():
            print("draw-ladder CLI rejected historical v1 selector capture")
            return 1
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(["--file", cli_file, "--dry-run", "--expect-invalid"])
        if code != 1:
            print("draw-ladder --expect-invalid treated a valid v1 trace as invalid")
            return 1
        def reject_trace(*args, **kwargs):
            raise TraceError("fixture structural rejection")
        read_trace = reject_trace
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(["--file", cli_file, "--dry-run", "--expect-invalid"])
        if code != 0:
            print("draw-ladder --expect-invalid semantics changed")
            return 1
        expect_args = ["--file", cli_file, "--dry-run", "--expect-unreplayable", "2"]
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(expect_args)
        if code != 1:
            print("expected unavailable facts accepted a structurally invalid capture")
            return 1
        for count, mismatches, mutations, expected in (
                (2, 0, 0, 0), (1, 0, 0, 1), (3, 0, 0, 1),
                (2, 1, 0, 1), (2, 0, 1, 1)):
            limited = json.loads(json.dumps(historical))
            limited["predicateReplay"].update(status="unreplayable", unreplayable=count,
                mismatches=mismatches, mutationUnobserved=mutations)
            read_trace = lambda *args, _summary=limited, **kwargs: ({}, _summary)
            with contextlib.redirect_stdout(io.StringIO()):
                code = main(expect_args)
            if code != expected:
                print("expected unavailable-fact fixture gate accepted incorrect counts or failed consistency")
                return 1
        for invalid_args in (
                ["--expect-unreplayable", "0", "--dry-run"],
                ["--expect-unreplayable", "-1", "--dry-run"],
                ["--expect-unreplayable", "x", "--dry-run"],
                ["--expect-unreplayable", "2"],
                ["--expect-unreplayable", "2", "--dry-run", "--expect-invalid"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    main(["--file", cli_file] + invalid_args)
            except SystemExit as exc:
                if exc.code == 2:
                    continue
            print("expected unavailable-fact CLI accepted invalid arguments")
            return 1
        mutation_args = ["--file", cli_file, "--dry-run",
                         "--expect-mutation-unobserved", "1"]
        read_trace = reject_trace
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(mutation_args)
        if code != 1:
            print("mutation-unobserved expectation accepted a structurally rejected trace")
            return 1
        mutation_clean = json.loads(json.dumps(historical))
        mutation_clean["predicateReplay"].update(
            status="mutation-unobserved", unreplayable=0, mismatches=0,
            mutationUnobserved=1, loaderPanelFacts=1, loaderPanelReplayed=1,
            loaderPanelUnreplayable=0, loaderPanelMismatches=0,
            loaderPanelMutationUnobserved=1)
        read_trace = lambda *args, **kwargs: ({}, mutation_clean)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = main(mutation_args)
        if (code != 0 or "exactly 1 mutation-unobserved" not in output.getvalue() or
                "predicate equivalence remains false" not in output.getvalue()):
            print("mutation-unobserved expectation rejected a known selector or claimed equivalence")
            return 1
        for field, value in (("mutationUnobserved", 2),
                             ("loaderPanelMutationUnobserved", 2),
                             ("unreplayable", 1), ("loaderPanelUnreplayable", 1),
                             ("mismatches", 1), ("loaderPanelMismatches", 1)):
            bad_mutation = json.loads(json.dumps(mutation_clean))
            bad_mutation["predicateReplay"][field] = value
            read_trace = lambda *args, _summary=bad_mutation, **kwargs: ({}, _summary)
            with contextlib.redirect_stdout(io.StringIO()):
                code = main(mutation_args)
            if code != 1:
                print("mutation-unobserved expectation accepted an incorrect count or unavailable/mismatched selector")
                return 1
        for invalid_args in (
                ["--expect-mutation-unobserved", "0", "--dry-run"],
                ["--expect-mutation-unobserved", "-1", "--dry-run"],
                ["--expect-mutation-unobserved", "x", "--dry-run"],
                ["--expect-mutation-unobserved", "1"],
                ["--expect-mutation-unobserved", "1", "--dry-run", "--expect-invalid"],
                ["--expect-mutation-unobserved", "1", "--dry-run", "--expect-unreplayable", "1"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    main(["--file", cli_file] + invalid_args)
            except SystemExit as exc:
                if exc.code == 2:
                    continue
            print("mutation-unobserved CLI accepted invalid arguments")
            return 1
        forward_summary = dict(historical)
        forward_summary["forwardReplay"] = {
            "forwardInputVersion": 1, "replayed": 1, "unavailable": 0,
            "mismatches": 0, "externalCallbackWorkUnobserved": 1,
            "status": "replayed", "wholeForwardingEquivalence": False}
        read_trace = lambda *args, **kwargs: ({}, forward_summary)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = main(["--file", cli_file, "--dry-run", "--expect-forward-replayed", "1"])
        if code != 0 or "exactly 1 forwarding-local plans replayed" not in output.getvalue():
            print("forwarding replay CLI assertion rejected its exact known-plan count")
            return 1
        unavailable_summary = dict(historical)
        unavailable_summary["forwardReplay"] = dict(
            forward_summary["forwardReplay"], replayed=0, unavailable=1,
            status="unavailable")
        read_trace = lambda *args, **kwargs: ({}, unavailable_summary)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = main(["--file", cli_file, "--dry-run", "--expect-forward-unavailable", "1"])
        if code != 0 or "exactly 1 forwarding observation(s) unavailable" not in output.getvalue():
            print("forwarding replay CLI did not distinguish unavailable from malformed input")
            return 1
        # Forwarding count assertions cannot waive supported predicate failures.
        # Guard all filesystem entry points used by this tool while exercising
        # the dry-run CLI, so a rejected assertion cannot quietly write output.
        from unittest.mock import patch
        for option, clean in (("--expect-forward-replayed", forward_summary),
                              ("--expect-forward-unavailable", unavailable_summary)):
            for status, field in (("mismatch", "mismatches"),
                                  ("unreplayable", "unreplayable"),
                                  ("mutation-unobserved", "mutationUnobserved")):
                failed_forward = json.loads(json.dumps(clean))
                failed_forward["predicateReplay"].update(
                    status=status, mismatches=0, unreplayable=0, mutationUnobserved=0)
                failed_forward["predicateReplay"][field] = 1
                read_trace = lambda *args, _summary=failed_forward, **kwargs: ({}, _summary)
                output = io.StringIO()
                try:
                    with contextlib.redirect_stdout(output), \
                            patch("builtins.open", side_effect=AssertionError("dry-run opened a file")), \
                            patch("os.mkdir", side_effect=AssertionError("dry-run created a directory")), \
                            patch("os.makedirs", side_effect=AssertionError("dry-run created directories")):
                        code = main(["--file", cli_file, "--dry-run", option, "1"])
                except AssertionError as exc:
                    print("forwarding expectation dry-run changed the filesystem: %s" % exc)
                    return 1
                if (code != 1 or "predicate replay gate failed" not in output.getvalue() or
                        "fixture expectation confirmed" in output.getvalue()):
                    print("%s bypassed supported predicate %s" % (option, status))
                    return 1
        bad_forward_summary = dict(forward_summary)
        bad_forward_summary["forwardReplay"] = dict(
            forward_summary["forwardReplay"], replayed=0, unavailable=1)
        read_trace = lambda *args, **kwargs: ({}, bad_forward_summary)
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(["--file", cli_file, "--dry-run", "--expect-forward-replayed", "1"])
        if code != 1:
            print("forwarding replay CLI accepted a missing expected plan")
            return 1
        mismatch_summary = dict(forward_summary)
        mismatch_summary["forwardReplay"] = dict(
            forward_summary["forwardReplay"], mismatches=1,
            status="mismatch")
        read_trace = lambda *args, **kwargs: ({}, mismatch_summary)
        with contextlib.redirect_stdout(io.StringIO()):
            code = main(["--file", cli_file, "--dry-run"])
        if code != 1:
            print("default forwarding CLI accepted a local replay mismatch")
            return 1
        for invalid_args in (
                ["--expect-forward-replayed", "0", "--dry-run"],
                ["--expect-forward-replayed", "x", "--dry-run"],
                ["--expect-forward-replayed", "1"],
                ["--expect-forward-replayed", "1", "--dry-run", "--expect-invalid"],
                ["--expect-forward-replayed", "1", "--dry-run", "--expect-unreplayable", "1"],
                ["--expect-forward-replayed", "1", "--dry-run", "--expect-forward-unavailable", "1"],
                ["--expect-forward-unavailable", "-1", "--dry-run"],
                ["--expect-forward-unavailable", "1", "--dry-run", "--expect-mutation-unobserved", "1"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    main(["--file", cli_file] + invalid_args)
            except SystemExit as exc:
                if exc.code == 2:
                    continue
            print("forwarding replay CLI accepted invalid arguments")
            return 1
        with open(cli_file, "r", encoding="utf-8") as stream:
            if stream.read() != "{}":
                print("fixture expectation rewrote its input")
                return 1
    finally:
        read_trace = original_read_trace
        try:
            os.remove(cli_file)
        except OSError:
            pass

    def sg_read(value=None, reached=True, known=True):
        return {"reached": reached, "known": known,
                "value": value if known else None}

    def sg_unread():
        return sg_read(None, False, False)

    def sg_site(source=None, world=None, probe=None, before=None, after=None,
                billboard=None):
        return {
            "source61ActionNotStock": sg_read(source) if source is not None else sg_unread(),
            "worldValue": sg_read(world) if world is not None else sg_unread(),
            "probeValue": sg_read(probe) if probe is not None else sg_unread(),
            "clampBefore": sg_read(before) if before is not None else sg_unread(),
            "clampAfter": sg_read(after) if after is not None else sg_unread(),
            "billboardReached": sg_read(billboard) if billboard is not None else sg_unread(),
        }

    def sg_texture(match=True, width=2048, unknown_resolve=False):
        if unknown_resolve:
            return {"resolveOk": sg_read(None, True, False),
                    "isTexture2D": sg_unread(), "width": sg_unread(),
                    "height": sg_unread(), "format": sg_unread()}
        if not match:
            return {"resolveOk": sg_read(True), "isTexture2D": sg_read(True),
                    "width": sg_read(width), "height": sg_unread(),
                    "format": sg_unread()}
        return {"resolveOk": sg_read(True), "isTexture2D": sg_read(True),
                "width": sg_read(2048), "height": sg_read(1024),
                "format": sg_read(98)}

    def sg_selector(mode=2, match=True, unknown_resolve=False):
        shape = sg_read(True)
        if mode == 0:
            outer = {"outerWantsMode": sg_read(0),
                     "outerExposureDamping": sg_read(False),
                     "outerProbe": sg_unread(),
                     "outerWantsResult": sg_read(False)}
            helper = {key: sg_unread() for key in (
                "helperWantsMode", "helperExposureDamping", "helperProbe",
                "helperWantsResult")}
            helper["helperTrainShape"] = sg_unread()
            ps0 = {k: sg_unread() for k in
                   ("resolveOk", "isTexture2D", "width", "height", "format")}
            ps1 = dict(ps0)
            times = {k: sg_unread() for k in
                     ("lastSeenBeforeMs", "nowMs", "lastSeenAfterMs", "actionMode")}
            action = sg_read(0)
        else:
            outer = {"outerWantsMode": sg_read(mode),
                     "outerExposureDamping": sg_unread(),
                     "outerProbe": sg_unread(),
                     "outerWantsResult": sg_read(True)}
            helper = {"helperWantsMode": sg_read(mode),
                      "helperExposureDamping": sg_unread(),
                      "helperProbe": sg_unread(),
                      "helperWantsResult": sg_read(True),
                      "helperTrainShape": sg_read(True)}
            ps0 = sg_texture(not (not match), 1024 if not match else 2048,
                             unknown_resolve)
            ps1 = (sg_texture() if match and not unknown_resolve else
                   {k: sg_unread() for k in
                    ("resolveOk", "isTexture2D", "width", "height", "format")})
            if match and not unknown_resolve:
                times = {"lastSeenBeforeMs": sg_read(0xffffffffffffffff),
                         "nowMs": sg_read(1), "lastSeenAfterMs": sg_read(1),
                         "actionMode": sg_read(mode)}
                action = sg_read(3 if mode != 1 else 1)
            elif unknown_resolve:
                times = {k: sg_unread() for k in
                         ("lastSeenBeforeMs", "nowMs", "lastSeenAfterMs", "actionMode")}
                action = sg_read(0)
            else:
                times = {k: sg_unread() for k in
                         ("lastSeenBeforeMs", "nowMs", "lastSeenAfterMs", "actionMode")}
                action = sg_read(0)
        return dict(outer, **helper, outerTrainShape=shape, ps0=ps0, ps1=ps1,
                    **times, action=action)

    def sg_fact(site_id, kind, site, selector=None, common_before=73,
                common_after=0):
        fact = {"siteId": site_id, "kind": kind, "known": "yes", "site": site}
        if kind == 9:
            fact["selector"] = selector
            fact["common2ClampBefore"] = sg_read(common_before)
            fact["common2ClampAfter"] = sg_read(common_after)
        return fact

    sg_draw = {"kind": ord("N"), "count": 6, "instances": 2,
               "sites": [
                   {"id": 61, "kind": 2, "outcome": 2, "flow": 0,
                    "subsite": 0, "verdict": -1},
                   {"id": 62, "kind": 2, "outcome": 2, "flow": 0,
                    "subsite": 0, "verdict": -1},
                   {"id": 63, "kind": 2, "outcome": 2, "flow": 0,
                    "subsite": 0, "verdict": -1}],
               "predicateFacts": [
                   sg_fact(61, 9, sg_site(before=0, after=0),
                           sg_selector()),
                    sg_fact(62, 10, sg_site(source=True, world=0)),
                   sg_fact(63, 11, sg_site(source=True, before=0))]}
    try:
        sg_summary = _replay_predicate_facts(sg_draw, "fixture", 6)
    except Exception as exc:
        print("draw-ladder Sunglare positive fixture failed: %s" % exc)
        return 1
    if sg_summary["sunglareReplayed"] != 3 or sg_summary["sunglareMismatches"]:
        print("draw-ladder Sunglare positive selector did not replay")
        return 1

    sg_stock = {"kind": ord("N"), "count": 6, "instances": 2,
                "sites": [{"id": 61, "kind": 2, "outcome": 2, "flow": 0,
                           "subsite": 0, "verdict": -1}],
                "predicateFacts": [sg_fact(61, 9, sg_site(before=0, after=0),
                                           sg_selector(mode=0))]}
    if _replay_predicate_facts(sg_stock, "stock", 6)["sunglareReplayed"] != 1:
        print("draw-ladder Sunglare Stock short-circuit did not replay")
        return 1
    sg_claim = json.loads(json.dumps(sg_draw))
    sg_claim["sites"] = sg_claim["sites"][:2]
    sg_claim["sites"][1].update(outcome=3, flow=1, verdict=9)
    sg_claim["predicateFacts"] = sg_claim["predicateFacts"][:2]
    sg_claim["predicateFacts"][1]["site"] = sg_site(
        source=True, world=1, before=0, after=0, billboard=True)
    if _replay_predicate_facts(sg_claim, "world-claim", 6)["sunglareReplayed"] != 2:
        print("draw-ladder Sunglare world action cascade did not replay")
        return 1
    sg_short = json.loads(json.dumps(sg_stock))
    sg_short["predicateFacts"][0]["selector"] = sg_selector(match=False)
    if _replay_predicate_facts(sg_short, "resource-short", 6)["sunglareReplayed"] != 1:
        print("draw-ladder Sunglare resource short-circuit did not replay")
        return 1
    sg_unknown = json.loads(json.dumps(sg_stock))
    sg_unknown["predicateFacts"][0]["selector"] = sg_selector(unknown_resolve=True)
    if _replay_predicate_facts(sg_unknown, "resource-unknown", 6)["sunglareUnreplayable"] != 1:
        print("draw-ladder Sunglare unavailable resource was guessed")
        return 1
    # Every independently consumed selector input can be unavailable without
    # making the recorded action/result an oracle. Keep later stages present
    # to prove unknown inputs cannot be reconstructed from their outputs.
    for field in ("outerWantsMode", "outerWantsResult", "helperWantsMode",
                  "helperWantsResult", "actionMode", "action",
                  "lastSeenBeforeMs", "nowMs", "lastSeenAfterMs"):
        mutant = json.loads(json.dumps(sg_draw))
        mutant["predicateFacts"][0]["selector"][field] = sg_read(None, True, False)
        if not _replay_predicate_facts(mutant, "unknown-" + field, 6)["sunglareUnreplayable"]:
            print("draw-ladder Sunglare guessed unavailable " + field)
            return 1
    for slot in ("ps0", "ps1"):
        for field in ("resolveOk", "isTexture2D", "width", "height", "format"):
            mutant = json.loads(json.dumps(sg_draw))
            mutant["predicateFacts"][0]["selector"][slot][field] = sg_read(None, True, False)
            if not _replay_predicate_facts(mutant, "unknown-texture", 6)["sunglareUnreplayable"]:
                print("draw-ladder Sunglare guessed unavailable texture input")
                return 1
    for field in ("clampBefore", "clampAfter", "billboardReached"):
        mutant = json.loads(json.dumps(sg_claim))
        mutant["predicateFacts"][1]["site"][field] = sg_read(None, True, False)
        if not _replay_predicate_facts(mutant, "unknown-claim-mutation", 6)["sunglareUnreplayable"]:
            print("draw-ladder Sunglare accepted unavailable claim evidence")
            return 1
    sg_mode_change = json.loads(json.dumps(sg_stock))
    sg_mode_change["predicateFacts"][0]["selector"] = sg_selector()
    sg_mode_change["predicateFacts"][0]["selector"]["actionMode"] = sg_read(1)
    sg_mode_change["predicateFacts"][0]["selector"]["action"] = sg_read(1)
    sg_mode_change["sites"][0].update(outcome=4, flow=1, verdict=2)
    mode_summary = _replay_predicate_facts(sg_mode_change, "post-stamp-mode", 6)
    if mode_summary["sunglareReplayed"] != 1 or mode_summary["sunglareMismatches"]:
        print("draw-ladder Sunglare ignored independent post-stamp mode")
        return 1
    for stage in ("outer", "helper"):
        for damping, probe in ((True, False), (False, True)):
            mutant = json.loads(json.dumps(sg_draw))
            selector = mutant["predicateFacts"][0]["selector"]
            selector[stage + "WantsMode"] = sg_read(0)
            selector[stage + "ExposureDamping"] = sg_read(damping)
            selector[stage + "Probe"] = sg_unread() if damping else sg_read(probe)
            summary = _replay_predicate_facts(mutant, "lazy-wants", 6)
            if summary["sunglareReplayed"] != 3 or summary["sunglareMismatches"]:
                print("draw-ladder Sunglare lazy stock wants failed")
                return 1
            field = stage + ("ExposureDamping" if damping else "Probe")
            selector[field] = sg_read(None, True, False)
            if not _replay_predicate_facts(mutant, "unknown-wants", 6)["sunglareUnreplayable"]:
                print("draw-ladder Sunglare guessed unavailable damping/probe")
                return 1
    for world in (-2147483648, -1, 2147483647):
        mutant = json.loads(json.dumps(sg_claim))
        mutant["predicateFacts"][1]["site"]["worldValue"] = sg_read(world)
        summary = _replay_predicate_facts(mutant, "raw-world", 6)
        if summary["sunglareReplayed"] != 2 or summary["sunglareMismatches"]:
            print("draw-ladder Sunglare raw nonzero world truthiness failed")
            return 1
    sg_probe_claim = json.loads(json.dumps(sg_claim))
    sg_probe_claim["predicateFacts"][1]["site"]["worldValue"] = sg_read(0)
    sg_probe_claim["predicateFacts"][1]["site"]["probeValue"] = sg_read(True)
    if _replay_predicate_facts(sg_probe_claim, "probe-claim", 6)["sunglareReplayed"] != 2:
        print("draw-ladder Sunglare probe claim failed")
        return 1
    sg_unknown62 = json.loads(json.dumps(sg_draw))
    sg_unknown62["predicateFacts"][1]["site"]["worldValue"] = sg_read(None, True, False)
    if _replay_predicate_facts(sg_unknown62, "unknown62-suffix", 6)["sunglareUnreplayable"] != 2:
        print("draw-ladder Sunglare site63 replayed without known site62 reachability")
        return 1
    for field in ("helperWantsMode", "helperWantsResult", "ps1"):
        mutant = json.loads(json.dumps(sg_stock))
        mutant["predicateFacts"][0]["selector"][field] = None
        try:
            _replay_predicate_facts(mutant, "malformed-envelope", 6)
        except TraceError:
            pass
        else:
            print("draw-ladder Sunglare accepted malformed envelope")
            return 1
    sg_missing_stage = json.loads(json.dumps(sg_draw))
    sg_missing_stage["predicateFacts"][0]["selector"]["ps0"]["resolveOk"] = sg_read(None, True, False)
    sg_missing_stage["predicateFacts"][0]["selector"]["ps0"]["isTexture2D"] = sg_unread()
    try:
        _replay_predicate_facts(sg_missing_stage, "missing-resource-stage", 6)
    except TraceError:
        pass
    else:
        print("draw-ladder Sunglare accepted a resource stage after skipped predecessor")
        return 1
    sg_time_mutation = json.loads(json.dumps(sg_draw))
    sg_time_mutation["predicateFacts"][0]["selector"]["lastSeenAfterMs"]["value"] = 2
    if not _replay_predicate_facts(sg_time_mutation, "time-mutation", 6)["sunglareMismatches"]:
        print("draw-ladder Sunglare timestamp mutation was not detected")
        return 1
    sg_reset_mutation = json.loads(json.dumps(sg_draw))
    sg_reset_mutation["predicateFacts"][0]["common2ClampAfter"]["value"] = 4
    if not _replay_predicate_facts(sg_reset_mutation, "reset-mutation", 6)["sunglareMismatches"]:
        print("draw-ladder Sunglare common2 reset mutation was not detected")
        return 1
    sg_site61_mutation = json.loads(json.dumps(sg_draw))
    sg_site61_mutation["predicateFacts"][0]["site"]["clampAfter"]["value"] = 4
    if not _replay_predicate_facts(sg_site61_mutation, "site61-mutation", 6)["sunglareMismatches"]:
        print("draw-ladder Sunglare site61 clamp mutation was not detected")
        return 1
    sg_unreachable = json.loads(json.dumps(sg_draw))
    sg_unreachable["sites"][2].update(outcome=3, flow=1, verdict=8)
    if not _replay_predicate_facts(sg_unreachable, "site63-claim", 6)["sunglareMismatches"]:
        print("draw-ladder Sunglare site63 production claim was accepted")
        return 1
    sg_invalid = json.loads(json.dumps(sg_stock))
    sg_invalid["predicateFacts"][0]["selector"]["outerExposureDamping"] = sg_read(True)
    try:
        _replay_predicate_facts(sg_invalid, "bad-stage", 6)
    except TraceError:
        pass
    else:
        print("draw-ladder Sunglare accepted an invalid short-circuit read")
        return 1

    def fr(value=None, reached=True, known=True):
        return {"reached": reached, "known": known,
                "value": value if reached and known else None}

    def remlok_fact(outer_mode=1, mode_before_gate=1, dsv_nonnull=False,
                    resolved=True, is_texture=True, width=1024, height=512,
                    hide_mode=1, swap=False, matches_before=0, hidden_before=0,
                    pending_before=False, shape=(ord("N"), 3, 1)):
        helper_called = outer_mode != 0 and shape == (ord("N"), 3, 1)
        dsv_read = helper_called and mode_before_gate != 0
        resolve_read = dsv_read and not dsv_nonnull
        type_read = resolve_read and resolved
        desc_read = type_read and is_texture
        matching_texture = desc_read and width == 1024 and height == 512
        hide_read = matching_texture
        swap_read = matching_texture and hide_mode != 2
        helper = {
            "modeBeforeGate": fr(mode_before_gate) if helper_called else fr(None, False, False),
            "dsvNonNull": fr(dsv_nonnull) if dsv_read else fr(None, False, False),
            "resolved": fr(resolved) if resolve_read else fr(None, False, False),
            "isTexture2D": fr(is_texture) if type_read else fr(None, False, False),
            "width": fr(width) if desc_read else fr(None, False, False),
            "height": fr(height) if desc_read and width == 1024 else fr(None, False, False),
            "hideMode": fr(hide_mode) if hide_read else fr(None, False, False),
            "swap": fr(swap) if swap_read else fr(None, False, False),
        }
        if matching_texture:
            matches_after = (matches_before + 1) & 0xffffffff
        else:
            matches_after = matches_before
        hidden_after = ((hidden_before + 1) & 0xffffffffffffffff
                        if matching_texture and hide_mode == 2 else hidden_before)
        pending_after = (((matches_before & 1) != 0) != swap
                         if matching_texture and hide_mode != 2 else pending_before)
        mutation = {
            "matchesBefore": fr(matches_before) if helper_called else fr(None, False, False),
            "matchesAfter": fr(matches_after) if helper_called else fr(None, False, False),
            "hiddenBefore": fr(hidden_before) if helper_called else fr(None, False, False),
            "hiddenAfter": fr(hidden_after) if helper_called else fr(None, False, False),
            "pendingRightBefore": fr(pending_before) if helper_called else fr(None, False, False),
            "pendingRightAfter": fr(pending_after) if helper_called else fr(None, False, False),
        }
        return {"siteId": 51, "kind": 14, "known": "yes",
                "selector": {"outerMode": fr(outer_mode)},
                "helper": helper, "mutation": mutation}

    def remlok_decline51():
        return {"id": 51, "kind": 2, "outcome": 2, "flow": 0,
                "subsite": 0, "verdict": -1}

    def remlok_draw(fact, site52=None, shape=(ord("N"), 3, 1)):
        events = [remlok_decline51()]
        if site52 is None:
            site52 = {"id": 52, "kind": 2, "outcome": 2, "flow": 0,
                      "subsite": 0, "verdict": -1}
        if site52:
            events.append(site52)
        return {"kind": shape[0], "count": shape[1], "instances": shape[2],
                "sites": events, "predicateFacts": [fact]}

    def fss_v8(fact, handler_invoked=True, raw_probe_reached=False):
        result = json.loads(json.dumps(fact))
        result["handlerInvoked"] = handler_invoked
        result["rawProbeReached"] = raw_probe_reached
        if raw_probe_reached and not handler_invoked:
            for group in ("helper", "mutation"):
                result[group] = {key: fr(None, False, False) for key in result[group]}
        return result

    def fss_event(site_id, claim):
        return {"id": site_id, "kind": 2, "outcome": 3 if claim else 2,
                "flow": 1 if claim else 0, "subsite": 0,
                "verdict": (11 if site_id == 57 else 12) if claim else -1}

    def fss_helper(hash_value=None, entered=True, vs_done=True,
                   lookup_reached=True, lookup_done=True, shader=True,
                   release=True, release_done=True, callback_done=True,
                   guard_returned=True, after=None):
        helper = {"guardCallReached": fr(True)}
        helper.update({"callbackEntered": fr(entered),
                       "vsGetShaderCompleted": fr(vs_done),
                       "lookupReached": fr(lookup_reached),
                       "lookupCompleted": fr(lookup_done),
                       "releaseReached": fr(release),
                       "releaseCompleted": fr(release_done),
                       "callbackCompleted": fr(callback_done),
                       "guardReturned": fr(guard_returned),
                       "hashAfterGuard": fr((hash_value or 0) if after is None else after)})
        helper["assignedHash"] = fr(hash_value) if lookup_done else fr(None, False, False)
        helper["shaderNonNull"] = fr(shader) if lookup_done else fr(None, False, False)
        return helper

    def fss_panel_fact(outer=True, body=10, frame=12, helper_enabled=True,
                       context=True, helper=None, matched_before=0,
                       matched_after=None):
        fact = {"siteId": 57, "kind": 12, "known": "yes",
                "selector": {"outerEnabled": fr(outer),
                             "bodyFrame": fr(body) if outer else fr(None, False, False),
                             "frameNo": fr(frame) if outer and body else fr(None, False, False)},
                "helper": {"enabled": fr(helper_enabled) if outer and body and ((frame - body) & 0xffffffff) <= 2 else fr(None, False, False),
                           "contextNonNull": fr(context) if outer and body and ((frame - body) & 0xffffffff) <= 2 and helper_enabled else fr(None, False, False)},
                "mutation": {"matchedHashBefore": fr(matched_before) if outer and body and ((frame - body) & 0xffffffff) <= 2 else fr(None, False, False),
                             "matchedHashAfter": fr(matched_before if matched_after is None else matched_after) if outer and body and ((frame - body) & 0xffffffff) <= 2 else fr(None, False, False)}}
        if body == 0:
            fact["selector"]["frameNo"] = fr(None, False, False)
        stages = ("guardCallReached", "callbackEntered", "vsGetShaderCompleted",
                  "shaderNonNull", "lookupReached", "lookupCompleted", "assignedHash",
                  "releaseReached", "releaseCompleted", "callbackCompleted",
                  "guardReturned", "hashAfterGuard")
        if helper is None:
            if outer and body and ((frame - body) & 0xffffffff) <= 2 and helper_enabled and context:
                hash_value = 0xA888D51024D9798E
                helper = fss_helper(hash_value)
            else:
                helper = {key: fr(None, False, False) for key in stages}
        fact["helper"].update(helper)
        return fact

    def fss_reveal_fact(body=10, body_frame=12, jump=0, jump_frame=0,
                        outer_steady=True, outer_lockstep=False, latch=False,
                        helper_steady=True, helper_lockstep=False,
                        context=True, helper=None, arrival=True,
                        arrival_before=0, arrival_after=1):
        sel = {"outerSteady": fr(outer_steady),
               "outerLockstep": fr(outer_lockstep) if not outer_steady else fr(None, False, False),
               "bodyFrame": fr(body) if (outer_steady or outer_lockstep) else fr(None, False, False),
               "bodyFrameNo": fr(body_frame) if (outer_steady or outer_lockstep) and body else fr(None, False, False),
               "jumpFrame": fr(jump) if (outer_steady or outer_lockstep) and (not body or ((body_frame - body) & 0xffffffff) > 2) else fr(None, False, False),
               "jumpFrameNo": fr(jump_frame) if (outer_steady or outer_lockstep) and jump and (not body or ((body_frame - body) & 0xffffffff) > 2) else fr(None, False, False),
               "modeLatch": fr(latch) if (outer_steady or outer_lockstep) and jump and ((jump_frame - jump) & 0xffffffff) <= 600 and (not body or ((body_frame - body) & 0xffffffff) > 2) else fr(None, False, False)}
        helper_gate = ((outer_steady or outer_lockstep) and
                       ((body and ((body_frame - body) & 0xffffffff) <= 2) or
                        (jump and ((jump_frame - jump) & 0xffffffff) <= 600 and latch)))
        stages = ("guardCallReached", "callbackEntered", "vsGetShaderCompleted",
                  "shaderNonNull", "lookupReached", "lookupCompleted", "assignedHash",
                  "releaseReached", "releaseCompleted", "callbackCompleted",
                  "guardReturned", "hashAfterGuard")
        h = {"steady": fr(helper_steady) if helper_gate else fr(None, False, False),
             "lockstep": fr(helper_lockstep) if helper_gate and not helper_steady else fr(None, False, False),
             "contextNonNull": fr(context) if helper_gate and (helper_steady or helper_lockstep) else fr(None, False, False)}
        if helper is None:
            helper = fss_helper(0x953C8123AD8DC13B) if helper_gate and context and (helper_steady or helper_lockstep) else {key: fr(None, False, False) for key in stages}
        h.update(helper)
        lookup_completed = helper.get("lookupCompleted", fr(False))["value"]
        assigned_hash = helper.get("assignedHash", fr(0))["value"]
        recognized = helper_gate and context and (helper_steady or helper_lockstep) and lookup_completed and assigned_hash == 0x953C8123AD8DC13B
        mutation = {"arrivalOpen": fr(arrival) if recognized else fr(None, False, False),
                    "arrivalBefore": fr(arrival_before) if arrival and recognized else fr(None, False, False),
                    "arrivalAfter": fr(arrival_after) if arrival and recognized else fr(None, False, False)}
        return {"siteId": 58, "kind": 13, "known": "yes",
                "selector": sel, "helper": h, "mutation": mutation}

    def fss_draw(fact, event=None):
        site_id = fact["siteId"]
        draw = {"kind": ord("X") if site_id == 57 else ord("N"),
                "count": 6, "instances": 1,
                "sites": [event or fss_event(site_id, True)],
                "predicateFacts": [fact]}
        return draw

    panel_hash = 0xA888D51024D9798E
    panel_fact = fss_panel_fact(matched_after=panel_hash)
    panel_summary = _replay_predicate_facts(fss_draw(panel_fact), "fss-panel", 7)
    if panel_summary["fssReplayed"] != 1 or panel_summary["fssMismatches"]:
        print("draw-ladder FSS panel positive fixture failed")
        return 1
    reveal_fact = fss_reveal_fact()
    reveal_summary = _replay_predicate_facts(fss_draw(reveal_fact), "fss-reveal", 7)
    if reveal_summary["fssReplayed"] != 1 or reveal_summary["fssMismatches"]:
        print("draw-ladder FSS reveal body positive fixture failed")
        return 1
    jump_fact = fss_reveal_fact(body=0, jump=100, jump_frame=700,
                                outer_steady=False, outer_lockstep=True,
                                latch=True, helper_steady=False,
                                helper_lockstep=True, arrival_before=0xffffffff,
                                arrival_after=0)
    jump_summary = _replay_predicate_facts(fss_draw(jump_fact), "fss-jump", 7)
    if jump_summary["fssReplayed"] != 1 or jump_summary["fssMismatches"]:
        print("draw-ladder FSS reveal jump/wrap fixture failed")
        return 1

    panel_off = fss_panel_fact(outer=False)
    if _replay_predicate_facts(fss_draw(panel_off, fss_event(57, False)),
                               "fss-panel-off", 7)["fssReplayed"] != 1:
        print("draw-ladder FSS panel outer gate short circuit failed")
        return 1
    reveal_off = fss_reveal_fact(body=0, jump=0, outer_steady=False,
                                 outer_lockstep=False, helper_steady=False,
                                 helper_lockstep=False)
    if _replay_predicate_facts(fss_draw(reveal_off, fss_event(58, False)),
                               "fss-reveal-off", 7)["fssReplayed"] != 1:
        print("draw-ladder FSS reveal OR short circuit failed")
        return 1

    for body, frame, claim in ((10, 12, True), (10, 13, False),
                               (0, 12, False), (0xffffffff, 1, True)):
        fact = fss_panel_fact(body=body, frame=frame,
                              matched_after=panel_hash if claim else 0)
        result = _replay_predicate_facts(fss_draw(fact, fss_event(57, claim)),
                                         "fss-body-boundary", 7)
        if result["fssMismatches"] or result["fssReplayed"] != 1:
            print("draw-ladder FSS body-age boundary fixture failed")
            return 1
    for jump_age, claim in ((600, True), (601, False)):
        fact = fss_reveal_fact(body=0, jump=100, jump_frame=100 + jump_age,
                               outer_steady=False, outer_lockstep=True,
                               latch=True, helper_steady=False,
                               helper_lockstep=True, arrival=False)
        result = _replay_predicate_facts(fss_draw(fact, fss_event(58, claim)),
                                         "fss-jump-boundary", 7)
        if result["fssMismatches"] or result["fssReplayed"] != 1:
            print("draw-ladder FSS jump-age boundary fixture failed")
            return 1

    dead_helper = fss_helper(entered=False, vs_done=False, lookup_reached=False,
                             lookup_done=False, shader=False, release=False,
                             release_done=False, callback_done=False,
                             guard_returned=False, after=0)
    dead_fact = fss_panel_fact(helper=dead_helper, matched_after=0)
    dead_result = _replay_predicate_facts(fss_draw(dead_fact, fss_event(57, False)),
                                          "fss-budget-dead", 7)
    if dead_result["fssReplayed"] != 1 or dead_result["fssMismatches"]:
        print("draw-ladder FSS dead-budget zero-h fixture failed")
        return 1
    getter_fault = fss_helper(entered=True, vs_done=False, lookup_reached=False,
                              lookup_done=False, shader=False, release=False,
                              release_done=False, callback_done=False,
                              guard_returned=False, after=0)
    getter_fact = fss_panel_fact(helper=getter_fault, matched_after=0)
    if _replay_predicate_facts(fss_draw(getter_fact, fss_event(57, False)),
                               "fss-getter-fault", 7)["fssReplayed"] != 1:
        print("draw-ladder FSS getter-fault fixture failed")
        return 1
    lookup_fault = fss_helper(0, entered=True, vs_done=True,
                              lookup_reached=True, lookup_done=False, shader=False,
                              release=False, release_done=False, callback_done=False,
                              guard_returned=False, after=0)
    lookup_fact = fss_panel_fact(helper=lookup_fault, matched_after=0)
    if _replay_predicate_facts(fss_draw(lookup_fact, fss_event(57, False)),
                               "fss-lookup-fault", 7)["fssReplayed"] != 1:
        print("draw-ladder FSS lookup-fault fixture failed")
        return 1
    release_fault_hash = 0xB018D143700AB803
    release_fault = fss_helper(release_fault_hash, release_done=False,
                               callback_done=False, guard_returned=False,
                               after=release_fault_hash)
    release_fact = fss_panel_fact(helper=release_fault,
                                  matched_after=release_fault_hash)
    if _replay_predicate_facts(fss_draw(release_fact), "fss-release-fault", 7)["fssReplayed"] != 1:
        print("draw-ladder FSS post-assignment Release fault was not replayed")
        return 1

    changed_panel_mutation = json.loads(json.dumps(panel_fact))
    changed_panel_mutation["mutation"]["matchedHashAfter"] = fr(0)
    if not _replay_predicate_facts(fss_draw(changed_panel_mutation),
                                   "fss-panel-mutation", 7)["fssMismatches"]:
        print("draw-ladder FSS matched-hash mutation was not checked")
        return 1
    changed_arrival = json.loads(json.dumps(reveal_fact))
    changed_arrival["mutation"]["arrivalAfter"] = fr(2)
    if not _replay_predicate_facts(fss_draw(changed_arrival),
                                   "fss-arrival-mutation", 7)["fssMismatches"]:
        print("draw-ladder FSS arrival counter mutation was not checked")
        return 1

    unknown_fact = fss_panel_fact()
    unknown_fact["selector"]["outerEnabled"] = fr(None, True, False)
    unknown_fact["selector"]["bodyFrame"] = fr(None, False, False)
    unknown_fact["selector"]["frameNo"] = fr(None, False, False)
    unknown_fact["helper"] = {key: fr(None, False, False) for key in
                               ("enabled", "contextNonNull", "guardCallReached",
                                "callbackEntered", "vsGetShaderCompleted", "shaderNonNull",
                                "lookupReached", "lookupCompleted", "assignedHash",
                                "releaseReached", "releaseCompleted", "callbackCompleted",
                                "guardReturned", "hashAfterGuard")}
    unknown_fact["mutation"] = {key: fr(None, False, False)
                                 for key in ("matchedHashBefore", "matchedHashAfter")}
    unknown_result = _replay_predicate_facts(fss_draw(unknown_fact), "fss-unknown", 7)
    if unknown_result["fssUnreplayable"] != 1 or unknown_result["fssReplayed"]:
        print("draw-ladder FSS unknown source was inferred from observed result")
        return 1
    unknown_hash = json.loads(json.dumps(panel_fact))
    unknown_hash["helper"]["lookupCompleted"] = fr(None, True, False)
    unknown_hash["helper"]["assignedHash"] = fr(None, False, False)
    unknown_hash["helper"]["shaderNonNull"] = fr(None, False, False)
    unknown_hash["helper"]["releaseReached"] = fr(False)
    unknown_hash["helper"]["releaseCompleted"] = fr(False)
    unknown_hash["helper"]["callbackCompleted"] = fr(False)
    unknown_hash["helper"]["guardReturned"] = fr(False)
    unknown_hash["helper"]["hashAfterGuard"] = fr(panel_hash)
    unknown_hash["mutation"]["matchedHashAfter"] = fr(panel_hash)
    unknown_hash_result = _replay_predicate_facts(fss_draw(unknown_hash),
                                                  "fss-unknown-hash", 7)
    if unknown_hash_result["fssUnreplayable"] != 1 or unknown_hash_result["fssReplayed"]:
        print("draw-ladder FSS unknown lookup assignment was inferred from result")
        return 1

    # Concrete registry zero and null shader are known negatives. A late
    # Release fault leaves the assigned hash intact, including on reveal.
    for shader in (False, True):
        zero_helper = fss_helper(0, shader=shader, release=shader,
                                 release_done=shader)
        for site_id in (57, 58):
            zero_fact = (fss_panel_fact(helper=zero_helper, matched_before=123,
                                         matched_after=123) if site_id == 57 else
                         fss_reveal_fact(helper=zero_helper))
            result = _replay_predicate_facts(
                fss_draw(zero_fact, fss_event(site_id, False)), "fss-known-zero", 7)
            if result["fssReplayed"] != 1 or result["fssMismatches"]:
                print("draw-ladder FSS concrete zero was not a known decline")
                return 1
    late_reveal = fss_reveal_fact(helper=fss_helper(
        0x953C8123AD8DC13B, release_done=False, callback_done=False,
        guard_returned=False), arrival_before=0xffffffff, arrival_after=0)
    result = _replay_predicate_facts(fss_draw(late_reveal), "fss-late-reveal", 7)
    if result["fssReplayed"] != 1 or result["fssMismatches"]:
        print("draw-ladder FSS late Release fault lost reveal claim/arrival wrap")
        return 1
    for fact in (fss_panel_fact(helper_enabled=False, matched_before=17,
                                 matched_after=17),
                 fss_panel_fact(context=False, matched_before=17,
                                 matched_after=17),
                 fss_reveal_fact(helper_steady=False, helper_lockstep=False),
                 fss_reveal_fact(context=False),
                 fss_reveal_fact(body=0, jump=1, jump_frame=601, latch=False)):
        result = _replay_predicate_facts(fss_draw(fact, fss_event(fact["siteId"], False)),
                                         "fss-helper-decline", 7)
        if result["fssReplayed"] != 1 or result["fssMismatches"]:
            print("draw-ladder FSS helper/latch short circuit failed")
            return 1
    # Every unavailable consumed raw source remains unknown even if all
    # observed outputs continue to assert the original positive claim.
    for base in (panel_fact, reveal_fact):
        for group in ("selector", "helper", "mutation"):
            for key, envelope in base[group].items():
                if not envelope["reached"]:
                    continue
                unavailable = json.loads(json.dumps(base))
                unavailable[group][key] = fr(None, True, False)
                result = _replay_predicate_facts(fss_draw(unavailable), "fss-unavailable-read", 7)
                if result["fssUnreplayable"] != 1 or result["fssReplayed"]:
                    print("draw-ladder FSS unavailable read was guessed: %s.%s" % (group, key))
                    return 1
    malformed_fss = []
    for group, key, value in (
            ("selector", "outerEnabled", {"reached": False, "known": True, "value": None}),
            ("selector", "bodyFrame", fr(-1)),
            ("selector", "frameNo", fr(0x100000000)),
            ("helper", "assignedHash", fr(0x10000000000000000)),
            ("helper", "callbackEntered", fr(False)),
            ("helper", "lookupReached", fr(False)),
            ("helper", "releaseReached", fr(False)),
            ("helper", "shaderNonNull", fr(False)),
            ("helper", "callbackEntered", {"reached": True, "known": False, "value": False})):
        bad = json.loads(json.dumps(panel_fact)); bad[group][key] = value
        malformed_fss.append(bad)
    bad = json.loads(json.dumps(reveal_fact)); bad["selector"]["jumpFrame"] = fr(100)
    malformed_fss.append(bad)
    bad = json.loads(json.dumps(panel_off)); bad["selector"]["bodyFrame"] = fr(0)
    malformed_fss.append(bad)
    bad = json.loads(json.dumps(panel_fact)); bad["known"] = "unknown"
    malformed_fss.append(bad)
    bad = json.loads(json.dumps(dead_fact)); bad["helper"]["assignedHash"] = fr(0)
    malformed_fss.append(bad)
    for bad in malformed_fss:
        try:
            _replay_predicate_facts(fss_draw(bad), "fss-malformed", 7)
        except TraceError:
            pass
        else:
            print("draw-ladder FSS accepted malformed availability/lazy/progress facts")
            return 1
    for group, key, value in (("helper", "hashAfterGuard", fr(0)),
                              ("helper", "guardReturned", fr(False)),
                              ("mutation", "matchedHashAfter", fr(0))):
        changed = json.loads(json.dumps(panel_fact)); changed[group][key] = value
        result = _replay_predicate_facts(fss_draw(changed), "fss-evidence-mutant", 7)
        if result["fssReplayed"] != 1 or not result["fssMismatches"]:
            print("draw-ladder FSS postguard/mutation evidence became an oracle")
            return 1
    wrong_event = _replay_predicate_facts(fss_draw(panel_fact, fss_event(57, False)),
                                          "fss-event-mutant", 7)
    independently_expected, _, _, _ = _replay_fss_fact(panel_fact, fss_draw(panel_fact), "fss-independent")
    if independently_expected != fss_event(57, True) or not wrong_event["fssMismatches"]:
        print("draw-ladder FSS event output became an oracle")
        return 1
    bad_release_prefix = json.loads(json.dumps(release_fact))
    bad_release_prefix["helper"]["releaseCompleted"] = fr(True)
    try:
        _replay_predicate_facts(fss_draw(bad_release_prefix), "fss-bad-prefix", 7)
    except TraceError:
        pass
    else:
        print("draw-ladder FSS accepted invalid helper stage prefix")
        return 1
    old_v6 = json.loads(json.dumps(sg_stock))
    if _replay_predicate_facts(old_v6, "old-v6", 6)["sunglareReplayed"] != 1:
        print("draw-ladder FSS reader regressed v6 compatibility")
        return 1
    for base in (panel_fact, reveal_fact):
        empty = json.loads(json.dumps(base))
        for group in ("selector", "helper", "mutation"):
            empty[group] = {key: fr(None, False, False) for key in empty[group]}
        not_eligible = {"id": empty["siteId"], "kind": 2, "outcome": 5,
                        "flow": 0, "subsite": 0, "verdict": -1}
        result = _replay_predicate_facts(fss_draw(empty, not_eligible), "fss-empty-noteligible", 7)
        if result["fssUnreplayable"] != 1 or result["fssReplayed"] or result["fssMismatches"]:
            print("draw-ladder FSS staged absence became a known selector result")
            return 1
        for event in (fss_event(empty["siteId"], True), fss_event(empty["siteId"], False)):
            try:
                _replay_predicate_facts(fss_draw(empty, event), "fss-empty-handler", 7)
            except TraceError:
                pass
            else:
                print("draw-ladder FSS accepted unfinished invoked-handler observations")
                return 1
        partial = json.loads(json.dumps(empty))
        del partial["helper"]["lookupCompleted"]
        try:
            _replay_predicate_facts(fss_draw(partial, not_eligible), "fss-missing-staged-field", 7)
        except TraceError:
            pass
        else:
            print("draw-ladder FSS staged absence weakened exact payload coverage")
            return 1
        malformed = json.loads(json.dumps(empty))
        next(iter(malformed["helper"].values()))["known"] = 0
        try:
            _replay_predicate_facts(fss_draw(malformed, not_eligible), "fss-staged-numeric-flag", 7)
        except TraceError:
            pass
        else:
            print("draw-ladder FSS staged absence accepted numeric availability flags")
            return 1

    # Version 8 carries explicit handler/probe provenance. The old v7 empty
    # NotEligible fixture above remains unavailable, while a raw outer miss is
    # independently replayable without consulting that event.
    panel_v8 = fss_v8(panel_fact)
    if _replay_predicate_facts(fss_draw(panel_v8), "fss-v8-handler", 8)["fssReplayed"] != 1:
        print("draw-ladder FSS v8 invoked-handler provenance failed")
        return 1
    panel_raw_off = fss_v8(fss_panel_fact(outer=False), False, True)
    panel_not_eligible = {"id": 57, "kind": 2, "outcome": 5,
                          "flow": 0, "subsite": 0, "verdict": -1}
    raw_off = _replay_predicate_facts(fss_draw(panel_raw_off, panel_not_eligible),
                                      "fss-v8-raw-off", 8)
    if raw_off["fssReplayed"] != 1 or raw_off["fssMismatches"]:
        print("draw-ladder FSS v8 raw outer decline was not replayed")
        return 1
    wrong_raw_off = _replay_predicate_facts(fss_draw(panel_raw_off, fss_event(57, True)),
                                            "fss-v8-raw-off-mutant", 8)
    if not wrong_raw_off["fssMismatches"]:
        print("draw-ladder FSS v8 used NotEligible output as selector input")
        return 1
    panel_raw_stale = fss_v8(fss_panel_fact(body=10, frame=13), False, True)
    stale_decline = _replay_predicate_facts(
        fss_draw(panel_raw_stale, panel_not_eligible), "fss-v8-raw-stale", 8)
    if stale_decline["fssReplayed"] != 1 or stale_decline["fssMismatches"]:
        print("draw-ladder FSS v8 raw age boundary failed")
        return 1
    panel_raw_on = fss_v8(panel_fact, False, True)
    raw_on = _replay_predicate_facts(fss_draw(panel_raw_on, panel_not_eligible),
                                     "fss-v8-raw-on", 8)
    if raw_on["fssUnreplayable"] != 1 or raw_on["fssReplayed"] or raw_on["fssMismatches"]:
        print("draw-ladder FSS v8 probed outer-true path invented helper evidence")
        return 1
    reveal_raw_off = fss_v8(fss_reveal_fact(outer_steady=False,
                                             outer_lockstep=False), False, True)
    reveal_not_eligible = {"id": 58, "kind": 2, "outcome": 5,
                           "flow": 0, "subsite": 0, "verdict": -1}
    if _replay_predicate_facts(fss_draw(reveal_raw_off, reveal_not_eligible),
                               "fss-v8-reveal-off", 8)["fssReplayed"] != 1:
        print("draw-ladder FSS v8 reveal short-circuit was not replayed")
        return 1

    malformed_v8 = []
    contradictory = fss_v8(panel_fact, True, True)
    malformed_v8.append(contradictory)
    raw_progress = json.loads(json.dumps(panel_raw_off))
    raw_progress["helper"]["callbackEntered"] = fr(False)
    malformed_v8.append(raw_progress)
    raw_mutation = json.loads(json.dumps(panel_raw_off))
    raw_mutation["mutation"]["matchedHashAfter"] = fr(0)
    malformed_v8.append(raw_mutation)
    bad_bool = json.loads(json.dumps(panel_raw_off))
    bad_bool["rawProbeReached"] = 1
    malformed_v8.append(bad_bool)
    missing_raw = json.loads(json.dumps(panel_raw_off))
    missing_raw["selector"]["outerEnabled"] = fr(None, False, False)
    malformed_v8.append(missing_raw)
    for bad in malformed_v8:
        try:
            _replay_predicate_facts(fss_draw(bad, panel_not_eligible), "fss-v8-malformed", 8)
        except TraceError:
            pass
        else:
            print("draw-ladder FSS v8 accepted contradictory or malformed provenance")
            return 1
    empty_v8 = fss_v8(fss_panel_fact(outer=False), False, False)
    try:
        _replay_predicate_facts(fss_draw(empty_v8, panel_not_eligible), "fss-v8-empty", 8)
    except TraceError:
        pass
    else:
        print("draw-ladder FSS v8 accepted missing handler/probe provenance")
        return 1

    rem_swapped = remlok_fact(outer_mode=1, mode_before_gate=2, swap=True,
                              matches_before=0xffffffff, pending_before=True)
    rem_claim = {"id": 52, "kind": 2, "outcome": 3, "flow": 1,
                 "subsite": 0, "verdict": 3}
    rem_summary = _replay_predicate_facts(
        remlok_draw(rem_swapped, rem_claim), "remlok-swapped-wrap", 8)
    if (rem_summary["remlokReplayed"] != 2 or rem_summary["remlokMismatches"] or
            rem_summary["remlokUnreplayable"]):
        print("draw-ladder RemLok differing modes/parity wrap fixture failed")
        return 1
    rem_hide = remlok_fact(outer_mode=2, mode_before_gate=1, hide_mode=2,
                           matches_before=0xffffffff, hidden_before=0xffffffffffffffff,
                           pending_before=True)
    rem_hide_event = {"id": 51, "kind": 2, "outcome": 4, "flow": 1,
                      "subsite": 0, "verdict": 2}
    rem_hide_summary = _replay_predicate_facts(
        {"kind": ord("N"), "count": 3, "instances": 1,
         "sites": [rem_hide_event], "predicateFacts": [rem_hide]},
        "remlok-hide-wrap", 8)
    if (rem_hide_summary["remlokReplayed"] != 1 or
            rem_hide_summary["remlokMismatches"] or
            rem_hide_summary["remlokUnreplayable"]):
        print("draw-ladder RemLok hide/count wrap fixture failed")
        return 1
    for declined in (remlok_fact(outer_mode=1, dsv_nonnull=True),
                     remlok_fact(outer_mode=1, resolved=False),
                     remlok_fact(outer_mode=1, is_texture=False),
                     remlok_fact(outer_mode=1, width=1023),
                     remlok_fact(outer_mode=1, mode_before_gate=0)):
        result = _replay_predicate_facts(remlok_draw(declined), "remlok-known-decline", 8)
        if result["remlokReplayed"] != 2 or result["remlokMismatches"]:
            print("draw-ladder RemLok known decline path failed")
            return 1
    rem_shape_miss = remlok_fact(outer_mode=1, shape=(ord("N"), 4, 1))
    unknown_modes = remlok_fact(outer_mode=91, mode_before_gate=7, hide_mode=7)
    unknown_result = _replay_predicate_facts(
        remlok_draw(unknown_modes, rem_claim), "remlok-raw-mode-values", 8)
    if unknown_result["remlokMismatches"] or unknown_result["remlokReplayed"] != 2:
        print("draw-ladder RemLok raw uint32 mode values changed frozen semantics")
        return 1
    bad_height = remlok_fact(width=1023)
    bad_height["helper"]["height"] = fr(512)
    try:
        _replay_predicate_facts(remlok_draw(bad_height), "remlok-height-after-width-miss", 8)
    except TraceError:
        pass
    else:
        print("draw-ladder RemLok read height after a width rejection")
        return 1
    if _replay_predicate_facts(remlok_draw(rem_shape_miss, shape=(ord("N"), 4, 1)),
                               "remlok-shape-miss", 8)["remlokReplayed"] != 2:
        print("draw-ladder RemLok shape short-circuit failed")
        return 1
    rem_bad_query = json.loads(json.dumps(remlok_fact(outer_mode=1, mode_before_gate=0)))
    rem_bad_query["helper"]["dsvNonNull"] = fr(False)
    try:
        _replay_predicate_facts(remlok_draw(rem_bad_query), "remlok-read-past-gate", 8)
    except TraceError:
        pass
    else:
        print("draw-ladder RemLok accepted a query past the stock-mode gate")
        return 1
    rem_bad_mutation = json.loads(json.dumps(rem_swapped))
    for unknown_key in ("width", "hideMode", "swap"):
        partial_rem = json.loads(json.dumps(rem_swapped))
        partial_rem["helper"][unknown_key] = fr(None, True, False)
        partial_result = _replay_predicate_facts(
            remlok_draw(partial_rem, rem_claim), "remlok-partial-" + unknown_key, 8)
        if partial_result["remlokMismatches"] or not partial_result["mutationUnobserved"]:
            print("draw-ladder RemLok unavailable trigger manufactured mutation evidence")
            return 1
    rem_bad_mutation["mutation"]["matchesAfter"] = fr(0xffffffff)
    if not _replay_predicate_facts(remlok_draw(rem_bad_mutation, rem_claim),
                                   "remlok-mutation", 8)["remlokMismatches"]:
        print("draw-ladder RemLok counter mutation was not detected")
        return 1
    try:
        _replay_predicate_facts({"kind": ord("N"), "count": 3, "instances": 1,
                                 "sites": [rem_claim], "predicateFacts": []},
                                "remlok-missing-predecessor", 8)
    except TraceError:
        pass
    else:
        print("draw-ladder RemLok derived site lacked its source fact")
        return 1
    try:
        _replay_predicate_facts(remlok_draw(rem_swapped, rem_claim),
                               "remlok-v7-unavailable", 7)
    except TraceError:
        pass
    else:
        print("draw-ladder predicate version 7 unexpectedly accepted RemLok kind 14")
        return 1

    def basic_read(value=None, reached=True, known=True):
        return {"reached": reached, "known": known,
                "value": value if known else None}

    def basic_context_fact(self_id=0, owner_id=0, before=7, after=0,
                           identity_known=True, reset_before_known=True,
                           reset_after_known=True, distance_reached=False):
        context = {
            "contextIdentity": basic_read(self_id if identity_known else None, True, identity_known),
            "ownerContextIdentity": basic_read(owner_id if identity_known else None, True, identity_known),
            "glareClampBefore": basic_read(before if reset_before_known else None, True, reset_before_known),
            "glareClampAfter": basic_read(after if reset_after_known else None, True, reset_after_known),
        }
        distance = {"distanceEnabled": basic_read(None, distance_reached, False)}
        return {"siteId": 2, "kind": 15, "known": "yes",
                "context": context, "distance": distance}

    def basic_distance_fact(enabled=None, known=True, context_reached=False):
        context = {name: basic_read(None, context_reached, False) for name in
                   ("contextIdentity", "ownerContextIdentity",
                    "glareClampBefore", "glareClampAfter")}
        return {"siteId": 67, "kind": 16, "known": "yes",
                "context": context,
                "distance": {"distanceEnabled": basic_read(enabled if known else None,
                                                            True, known)}}

    def basic_draw(site_id, outcome, flow, verdict, fact):
        return {"kind": ord("N"), "count": 1, "instances": 1,
                "sites": [{"id": site_id, "kind": 3, "outcome": outcome,
                           "flow": flow, "subsite": 0, "verdict": verdict}],
                "predicateFacts": [fact]}

    def forward_observation(ordinal, callback=True, family=-1):
        names = set(_FORWARD_INPUT_READS)
        fact = {"kind": 1, "version": 1}
        for name in names:
            fact[name] = basic_read(None, False, False)
        def put(name, value):
            fact[name] = basic_read(value)
        put("verdictOrdinal", ordinal)
        put("issueBlockedEntry", False)
        put("objectProbeLedgerOn", False)
        put("uiDepthThisDraw", False)
        put("holoDepthThisDraw", False)
        put("compositeThisDraw", False)
        if ordinal == 2:
            put("curveThisDrawBeforeSkipClear", False)
            return fact
        put("seedDiagnostics", False)
        put("uiLayerLiveEyeGate", False)
        put("uiLayerLiveFallbackGate", False)
        put("uiLayerWatchingGate", False)
        put("curveThisDrawCurveGate", False)
        put("engineVelocityCacheFamily", family)
        put("introCurveThisDrawStripGate", False)
        put("issueBlockedBeforeOriginal", False)
        put("originalCallReturned", callback)
        if callback:
            put("crispPendingAfterOriginal", False)
        put("issueBlockedAfterOriginal", False)
        put("planetPending", False)
        put("planetSolarPending", False)
        return fact

    def forward_trace(ordinal, callback=True, family=-1):
        trace = json.loads(json.dumps(stars_trace(claimed=True) if ordinal == 2 else vr))
        trace["predicateFactVersion"] = 13
        trace["forwardInputVersion"] = 1
        draw = trace["draws"][0]
        draw["predicateFacts"] = [fact for fact in draw["predicateFacts"]
                                  if fact.get("siteId") != 2]
        draw["predicateFacts"].append(basic_context_fact(0, 0))
        for event in draw["sites"]:
            if event["id"] == 2:
                event.update(outcome=1, flow=0, verdict=-1)
        draw["forwardFacts"] = None
        draw["forwardInputsReached"] = True
        draw["forwardInputs"] = forward_observation(ordinal, callback, family)
        expected = (_forward_draw_envelope(draw) +
                    ([_forward_action(3, 2, 2, draw, 0)] if ordinal == 2 else
                     [_forward_original_action(draw, callback)]) +
                    _forward_draw_end(draw))
        draw["actions"] = expected
        return trace

    # Forward-input replay has an independent raw plan and treats serialized
    # actions/ForwardFacts only as comparison targets.
    for ordinal, callback, family in ((0, True, -1), (0, False, 0), (2, True, -1)):
        forward_fixture = forward_trace(ordinal, callback, family)
        forward_result = validate_trace(forward_fixture)["forwardReplay"]
        if (forward_result["replayed"] != 1 or forward_result["unavailable"] or
                forward_result["mismatches"] or forward_result["wholeForwardingEquivalence"]):
            print("forwarding-local None/Skip fixture failed independent replay: %r" % forward_result)
            return 1
    def literal_forward_action(action_id, phase, outcome, draw_call, issues,
                               count, instances, start, start_instance, base_vertex):
        return {"id": action_id, "phase": phase, "outcome": outcome,
                "call": draw_call, "flags": 0, "issueCount": issues,
                "issueCountKnown": True, "count": count, "instances": instances,
                "start": start, "startInstance": start_instance,
                "baseVertex": base_vertex}
    for draw_kind, draw_call, action_start, action_base in (
            (ord("D"), 1, 27, 0), (ord("I"), 2, 89, 12),
            (ord("N"), 3, 12, 0), (ord("X"), 4, 89, 12)):
        tuple_fixture = forward_trace(0)
        tuple_draw = tuple_fixture["draws"][0]
        tuple_draw.update(kind=draw_kind, command=DRAW_COMMANDS[draw_kind],
                          count=6, instances=4)
        tuple_draw["args"] = {"start": 89, "base": 27 if draw_kind == ord("D") else
                               (12 if draw_kind == ord("N") else 12),
                               "startInstance": 11}
        # Explicit per-command expected tuples, independent of _forward_action.
        expected_tuple_actions = [
            literal_forward_action(1, 1, 2, draw_call, 0, 6, 4,
                                   action_start, 11, action_base),
            literal_forward_action(2, 2, 2, draw_call, 1, 6, 4,
                                   action_start, 11, action_base),
            literal_forward_action(17, 3, 2, draw_call, 0, 6, 4,
                                   action_start, 11, action_base)]
        tuple_draw["actions"] = expected_tuple_actions
        tuple_result = validate_trace(tuple_fixture)["forwardReplay"]
        if tuple_result["replayed"] != 1 or tuple_result["mismatches"]:
            print("forwarding replay mismatched literal normalized %s argument tuples" %
                  chr(draw_kind))
            return 1
    for signed_base, unsigned_start in ((-2147483648, 0x80000000), (-1, 0xffffffff)):
        for draw_kind, draw_call in ((ord("D"), 1), (ord("I"), 2),
                                     (ord("N"), 3), (ord("X"), 4)):
            for returned in (True, False):
                edge_fixture = forward_trace(0, returned)
                edge_draw = edge_fixture["draws"][0]
                edge_draw.update(kind=draw_kind, command=DRAW_COMMANDS[draw_kind],
                                 count=6, instances=4)
                edge_draw["args"] = {"start": 89, "base": signed_base, "startInstance": 11}
                # The nonindexed UINT StartVertexLocation survives its signed
                # DrawFacts carrier; indexed BaseVertexLocation stays signed.
                expected_start = unsigned_start if draw_kind in (ord("D"), ord("N")) else 89
                expected_base = 0 if draw_kind in (ord("D"), ord("N")) else signed_base
                edge_draw["actions"] = [
                    literal_forward_action(1, 1, 2, draw_call, 0, 6, 4,
                                           expected_start, 11, expected_base),
                    literal_forward_action(2, 2, 2 if returned else 3, draw_call,
                                           1 if returned else 0, 6, 4,
                                           expected_start, 11, expected_base),
                    literal_forward_action(17, 3, 2, draw_call, 0, 6, 4,
                                           expected_start, 11, expected_base)]
                edge_result = validate_trace(edge_fixture)["forwardReplay"]
                if edge_result["replayed"] != 1 or edge_result["mismatches"]:
                    print("forwarding replay lost signed/unsigned command argument bits")
                    return 1
    changed_action = forward_trace(0, False)
    changed_action["draws"][0]["actions"][1]["outcome"] = 2
    changed_action["draws"][0]["actions"][1]["issueCount"] = 1
    if not validate_trace(changed_action)["forwardReplay"]["observedActionMismatches"]:
        print("forwarding replay let a valid action mutation alter its expected plan")
        return 1
    changed_facts = forward_trace(0, True)
    changed_facts["draws"][0]["forwardFacts"] = {
        "presentMask": 3, "verdictOrdinal": 7, "family": 0,
        "familyAvailable": "unknown", "owner": "no", "verdictForwards": "unknown",
        "initialUiTake": "unknown", "afterUiTake": "unknown", "worldReissue": "unknown",
        "curveThisDraw": "unknown", "introCurveThisDraw": "unknown", "uiDepth": "unknown",
        "holoDepth": "unknown", "composite": "unknown", "crispPending": "unknown",
        "issueBlocked": "unknown"}
    if not validate_trace(changed_facts)["forwardReplay"]["forwardFactsMismatches"]:
        print("forwarding replay let a valid ForwardFacts mutation alter its expected plan")
        return 1
    changed_depth_fact = forward_trace(0, True)
    changed_depth_fact["draws"][0]["forwardFacts"] = {
        "presentMask": 1 << 9, "verdictOrdinal": -1, "family": 0,
        "familyAvailable": "unknown", "owner": "unknown", "verdictForwards": "unknown",
        "initialUiTake": "unknown", "afterUiTake": "unknown", "worldReissue": "unknown",
        "curveThisDraw": "unknown", "introCurveThisDraw": "unknown", "uiDepth": "yes",
        "holoDepth": "unknown", "composite": "unknown", "crispPending": "unknown",
        "issueBlocked": "unknown"}
    if not validate_trace(changed_depth_fact)["forwardReplay"]["forwardFactsMismatches"]:
        print("forwarding replay ignored a valid depth-field ForwardFacts mutation")
        return 1
    if validate_trace(forward_trace(0, True, -1))["forwardReplay"]["poolFamilyDraws"] != 0 or \
            validate_trace(forward_trace(0, True, 0))["forwardReplay"]["poolFamilyDraws"] != 1:
        print("forwarding replay conflated engine-velocity family -1 and family 0")
        return 1
    family_edge = forward_trace(0, True, 0x7fffffff)
    if (validate_trace(family_edge)["forwardReplay"]["poolFamilyDraws"] != 1 or
            validate_trace(forward_trace(0, True, -0x80000000))["forwardReplay"]["poolFamilyDraws"] != 0):
        print("forwarding replay did not preserve the full signed engine-family domain")
        return 1
    other_verdict = forward_trace(0)
    other_verdict["draws"][0]["forwardInputs"]["verdictOrdinal"] = basic_read(19)
    if validate_trace(other_verdict)["forwardReplay"]["unavailable"] != 1:
        print("writer-valid non-None/Skip verdict was not explicit unavailable")
        return 1
    lazy_skip = forward_trace(2)
    lazy_skip["draws"][0]["forwardInputs"]["seedDiagnostics"] = basic_read(False)
    try:
        validate_trace(lazy_skip)
    except TraceError:
        pass
    else:
        print("forwarding Skip path accepted a None-only seed read")
        return 1
    missing_input = forward_trace(0)
    del missing_input["draws"][0]["forwardInputs"]
    try:
        validate_trace(missing_input)
    except TraceError:
        pass
    else:
        print("forwarding v1 accepted a missing input key as an unavailable observation")
        return 1
    unknown_family = forward_trace(0)
    unknown_family["draws"][0]["forwardInputs"]["engineVelocityCacheFamily"] = basic_read(None, True, False)
    unknown_family_result = validate_trace(unknown_family)["forwardReplay"]
    if unknown_family_result["unavailable"] != 1 or unknown_family_result["mismatches"]:
        print("unknown forwarding family was not distinguished from mismatch")
        return 1
    repeated_live = forward_trace(0)
    repeated_live["draws"][0]["forwardInputs"]["uiLayerLiveFallbackGate"] = basic_read(True)
    repeated_live_result = validate_trace(repeated_live)["forwardReplay"]
    if repeated_live_result["unavailable"] != 1 or repeated_live_result["mismatches"]:
        print("forwarding reader compared separate live-gate reads as if cached")
        return 1
    post_callback_gate = forward_trace(0)
    post_callback_gate["draws"][0]["forwardInputs"]["issueBlockedAfterOriginal"] = basic_read(True)
    post_callback_gate["draws"][0]["forwardInputs"]["planetPending"] = basic_read(None, False, False)
    post_callback_gate["draws"][0]["forwardInputs"]["planetSolarPending"] = basic_read(None, False, False)
    post_callback_result = validate_trace(post_callback_gate)["forwardReplay"]
    if post_callback_result["unavailable"] != 1 or post_callback_result["mismatches"]:
        print("forwarding reader treated post-callback gates as cached pre-callback reads")
        return 1
    active_planet = forward_trace(0)
    active_planet["draws"][0]["forwardInputs"]["planetPending"] = basic_read(True)
    active_planet["draws"][0]["forwardInputs"]["planetSolarPending"] = basic_read(None, False, False)
    active_planet_result = validate_trace(active_planet)["forwardReplay"]
    if active_planet_result["unavailable"] != 1 or active_planet_result["mismatches"]:
        print("active callback-time planet work was not marked unavailable")
        return 1
    illegal_planet_or_read = forward_trace(0)
    illegal_planet_or_read["draws"][0]["forwardInputs"]["planetPending"] = basic_read(True)
    illegal_planet_or_read["draws"][0]["forwardInputs"]["planetSolarPending"] = basic_read(False)
    try:
        validate_trace(illegal_planet_or_read)
    except TraceError:
        pass
    else:
        print("forwarding reader accepted a solar-pending read short-circuited by planet-pending")
        return 1
    null_forward = forward_trace(0)
    null_forward["draws"][0].update(forwardInputs=None, forwardInputsReached=False)
    null_forward_result = validate_trace(null_forward)["forwardReplay"]
    if null_forward_result["unavailable"] != 1 or null_forward_result["mismatches"]:
        print("explicit null forwarding observation was not reported as unavailable")
        return 1
    malformed_marker = forward_trace(0)
    malformed_marker["draws"][0].update(forwardInputs=None, forwardInputsReached=True)
    try:
        validate_trace(malformed_marker)
    except TraceError:
        pass
    else:
        print("forwarding reached marker accepted a missing fact")
        return 1
    for field, value in (("kind", True), ("version", True)):
        malformed_kind = forward_trace(0)
        malformed_kind["draws"][0]["forwardInputs"][field] = value
        try:
            validate_trace(malformed_kind)
        except TraceError:
            pass
        else:
            print("forwarding reader accepted boolean %s as integer one" % field)
            return 1
    foreign_raw = forward_observation(0)
    for name in ("issueBlockedEntry", "objectProbeLedgerOn", "seedDiagnostics",
                 "uiLayerLiveEyeGate", "uiLayerLiveFallbackGate", "uiLayerWatchingGate",
                 "issueBlockedBeforeOriginal", "issueBlockedAfterOriginal",
                 "planetPending", "planetSolarPending"):
        foreign_raw[name] = basic_read(None, False, False)
    foreign_draw = json.loads(json.dumps(forward_trace(0)["draws"][0]))
    foreign_draw["predicateFacts"] = [basic_context_fact(10, 11)]
    foreign_result = _replay_forward_inputs(foreign_raw, foreign_draw, "forward-foreign")
    if foreign_result["status"] != "unavailable":
        print("foreign-context forwarding was included in owner-context replay scope")
        return 1
    skip_curve_true = forward_trace(2)
    skip_curve_true["draws"][0]["forwardInputs"]["curveThisDrawBeforeSkipClear"] = basic_read(True)
    if validate_trace(skip_curve_true)["forwardReplay"]["unavailable"] != 1:
        print("Skip with a true curve-clear mutation was replayed outside the bounded scope")
        return 1
    explicit_legacy_header = json.loads(json.dumps(legacy_v1))
    explicit_legacy_header["forwardInputVersion"] = 1
    try:
        validate_trace(explicit_legacy_header)
    except TraceError:
        pass
    else:
        print("schemaVersion 1 accepted a forwarding-input version header")
        return 1
    for old_forward_version in range(1, 14):
        old_forward = _fixture()
        old_forward["predicateFactVersion"] = old_forward_version
        old_forward.pop("forwardInputVersion", None)
        old_forward_summary = validate_trace(old_forward)["forwardReplay"]
        if (old_forward_summary["forwardInputVersion"] != 0 or
                old_forward_summary["status"] != "unavailable"):
            print("legacy predicate version %d inferred forwarding inputs" % old_forward_version)
            return 1
    empty_forward = _fixture()
    empty_forward.update(predicateFactVersion=13, forwardInputVersion=1)
    empty_forward["draws"] = []
    empty_forward["footer"]["drawCount"] = 0
    empty_forward_result = validate_trace(empty_forward)["forwardReplay"]
    if (empty_forward_result["replayed"] or empty_forward_result["unavailable"] or
            empty_forward_result["mismatches"] or empty_forward_result["status"] != "not-visited"):
        print("empty forwarding-input capture lacks zeroed replay defaults")
        return 1
    mixed_forward = forward_trace(0)
    second_draw = json.loads(json.dumps(mixed_forward["draws"][0]))
    second_draw.update(index=1, forwardInputs=None, forwardInputsReached=False)
    mixed_forward["draws"].append(second_draw)
    mixed_forward["footer"]["drawCount"] = 2
    mixed_forward_result = validate_trace(mixed_forward)["forwardReplay"]
    if (mixed_forward_result["replayed"] != 1 or
            mixed_forward_result["unavailable"] != 1 or
            mixed_forward_result["mismatches"]):
        print("mixed known/null forwarding observations lost independent status counts")
        return 1

    basic_equal = _replay_predicate_facts(
        basic_draw(2, 1, 0, -1, basic_context_fact(0, 0)), "basic-equal", 9)
    if (basic_equal["basicDrawReplayed"] != 1 or
            basic_equal["basicDrawMismatches"] or basic_equal["mutationUnobserved"]):
        print("BasicDraw site 2 failed equal/null identity replay")
        return 1
    basic_foreign = _replay_predicate_facts(
        basic_draw(2, 4, 1, 0, basic_context_fact(10, 11)), "basic-foreign", 9)
    if basic_foreign["basicDrawReplayed"] != 1:
        print("BasicDraw site 2 failed foreign identity exit replay")
        return 1
    wrong_foreign = _replay_predicate_facts(
        basic_draw(2, 1, 0, -1, basic_context_fact(10, 11)), "basic-wrong-outcome", 9)
    if not wrong_foreign["basicDrawMismatches"]:
        print("BasicDraw site 2 ignored an actual outcome mismatch")
        return 1
    bad_reset = _replay_predicate_facts(
        basic_draw(2, 1, 0, -1, basic_context_fact(after=9)), "basic-reset-mismatch", 9)
    if not bad_reset["mismatches"] or not bad_reset["basicDrawMismatches"]:
        print("BasicDraw site 2 failed to report a nonzero clamp after reset")
        return 1
    unknown_reset = _replay_predicate_facts(
        basic_draw(2, 1, 0, -1, basic_context_fact(reset_before_known=False)),
        "basic-reset-unknown", 9)
    if (unknown_reset["mutationUnobserved"] != 1 or
            unknown_reset["basicDrawMismatches"]):
        print("BasicDraw site 2 guessed unavailable reset evidence")
        return 1
    unknown_identity = _replay_predicate_facts(
        basic_draw(2, 1, 0, -1,
                   basic_context_fact(identity_known=False)),
        "basic-identity-unknown", 9)
    if unknown_identity["basicDrawUnreplayable"] != 1:
        print("BasicDraw site 2 guessed unavailable context identity")
        return 1
    if (_replay_predicate_facts(
            basic_draw(67, 2, 0, -1, basic_distance_fact(True)),
            "basic-distance-on", 9)["basicDrawReplayed"] != 1 or
            _replay_predicate_facts(
                basic_draw(67, 4, 1, 0, basic_distance_fact(False)),
                "basic-distance-off", 9)["basicDrawReplayed"] != 1):
        print("BasicDraw site 67 failed enabled/disabled distance replay")
        return 1
    wrong_distance = _replay_predicate_facts(
        basic_draw(67, 2, 0, -1, basic_distance_fact(False)),
        "basic-distance-wrong-outcome", 9)
    if not wrong_distance["basicDrawMismatches"]:
        print("BasicDraw site 67 ignored an actual outcome mismatch")
        return 1
    unknown_distance = _replay_predicate_facts(
        basic_draw(67, 2, 0, -1, basic_distance_fact(known=False)),
        "basic-distance-unknown", 9)
    if unknown_distance["basicDrawUnreplayable"] != 1:
        print("BasicDraw site 67 guessed an unavailable distance read")
        return 1

    # A site61 reset fact may confirm site2's independent after-read, but it
    # cannot provide the identity inputs used to predict site2.
    cross_draw = json.loads(json.dumps(sg_stock))
    cross_draw["sites"].insert(0, {"id": 2, "kind": 3, "outcome": 1,
                                   "flow": 0, "subsite": 0, "verdict": -1})
    cross_draw["predicateFacts"].append(basic_context_fact(after=1))
    cross_reset = _replay_predicate_facts(cross_draw, "basic-cross-reset", 9)
    if not cross_reset["basicDrawMismatches"]:
        print("BasicDraw site 2 ignored an inconsistent Sunglare reset observation")
        return 1

    old_basic = {"kind": ord("N"), "count": 1, "instances": 1,
                 "sites": [{"id": 2, "kind": 3, "outcome": 1, "flow": 0,
                            "subsite": 0, "verdict": -1}],
                 "predicateFacts": []}
    if _replay_predicate_facts(old_basic, "basic-v8", 8)["factCount"] != 0:
        print("predicate fact version 8 fabricated site-2 coverage")
        return 1
    earlier_exit = {"kind": ord("N"), "count": 1, "instances": 1,
                    "sites": [{"id": 1, "kind": 2, "outcome": 4, "flow": 1,
                               "subsite": 0, "verdict": 2}],
                    "predicateFacts": []}
    if _replay_predicate_facts(earlier_exit, "basic-prefix-absence", 9)["factCount"] != 0:
        print("BasicDraw facts were required beyond an earlier terminal site")
        return 1
    try:
        earlier_exit["predicateFacts"] = [basic_context_fact()]
        _replay_predicate_facts(earlier_exit, "basic-unvisited-fact", 9)
    except TraceError:
        pass
    else:
        print("BasicDraw accepted a site-2 fact after an earlier terminal site")
        return 1
    malformed_unused = basic_distance_fact(True)
    malformed_unused["context"]["contextIdentity"] = basic_read(1)
    try:
        _replay_predicate_facts(basic_draw(67, 2, 0, -1, malformed_unused),
                                "basic-unused-group", 9)
    except TraceError:
        pass
    else:
        print("BasicDraw accepted a reached nonapplicable context read")
        return 1
    malformed_missing = basic_context_fact()
    del malformed_missing["context"]["contextIdentity"]
    try:
        _replay_predicate_facts(basic_draw(2, 1, 0, -1, malformed_missing),
                                "basic-missing-read", 9)
    except TraceError:
        pass
    else:
        print("BasicDraw accepted a missing consumed read")
        return 1

    # Exercise the complete reader status path with the existing valid route
    # fixture so the version bump reports coverage without changing parity.
    v9_basic = json.loads(json.dumps(vr))
    v9_basic["predicateFactVersion"] = 9
    v9_draw = v9_basic["draws"][0]
    v9_draw["sites"][1].update(outcome=1, flow=0, verdict=-1)
    v9_draw["predicateFacts"].append(basic_context_fact())
    v9_summary = validate_trace(v9_basic)["predicateReplay"]
    if v9_summary["basicDrawStatus"] != "replayed" or v9_summary["basicDrawReplayed"] != 1:
        print("predicate fact version 9 did not expose BasicDraw replay status")
        return 1

    def eye_read(value=None, reached=False, known=False):
        return {"reached": reached, "known": known,
                "value": value if known else None}

    eye_rule_fields = ("loopCount", "vsHashGate", "heldVsHash",
                       "vsHashCompareExpected", "ruleKind", "countHighGate",
                       "countMinimum", "countHighBound", "exactCount")
    eye_filter_fields = ("modeOffGate", "modeAnyGate", "boundNonNull",
                         "modeAfterBound", "resolved", "isTexture2D", "width",
                         "height", "modeAfterResolve", "configuredWidth",
                         "configuredHeight", "eyeSizeAvailable", "eyeWidth",
                         "eyeHeight")

    def eye_fact():
        rules = []
        for _ in range(8):
            rule = {name: eye_read() for name in eye_rule_fields}
            rule["filters"] = [{name: eye_read() for name in eye_filter_fields}
                               for _ in range(4)]
            rules.append(rule)
        return {"siteId": 48, "kind": 17, "known": "yes",
                "skipCountGate": eye_read(8, True, True), "rules": rules,
                "terminalLoopCount": eye_read(),
                "mutation": {"censusSkippedBefore": eye_read(),
                             "censusSkippedAfter": eye_read()}}

    def eye_set(obj, name, value):
        obj[name] = eye_read(value, True, True)

    def eye_expected(fact, count=17, kind=ord("N")):
        return _replay_eye_census_fact(fact,
                                      {"count": count, "kind": kind},
                                      "eye-self-test")

    eye_empty = eye_fact()
    eye_empty["skipCountGate"] = eye_read(0, True, True)
    if eye_expected(eye_empty)[0] != _eye_site_event(False, 0):
        print("EyeCensus zero skip gate did not decline")
        return 1

    eye_hash = eye_fact()
    rule = eye_hash["rules"][0]
    eye_set(rule, "loopCount", 1)
    eye_set(rule, "vsHashGate", 1)
    eye_set(rule, "heldVsHash", 55)
    eye_set(rule, "vsHashCompareExpected", 55)
    eye_set(eye_hash["mutation"], "censusSkippedBefore", 0xffffffffffffffff)
    eye_set(eye_hash["mutation"], "censusSkippedAfter", 0)
    if eye_expected(eye_hash)[0] != _eye_site_event(True, 0):
        print("EyeCensus hash hit did not claim before count and filter reads")
        return 1
    eye_hash["rules"][1]["loopCount"] = eye_read(0, True, True)
    try:
        eye_expected(eye_hash)
    except TraceError:
        pass
    else:
        print("EyeCensus hash hit accepted a later loop read")
        return 1

    eye_hash_miss = eye_fact()
    rule = eye_hash_miss["rules"][0]
    eye_set(rule, "loopCount", 1); eye_set(rule, "vsHashGate", 1)
    eye_set(rule, "heldVsHash", 54); eye_set(rule, "vsHashCompareExpected", 55)
    eye_set(eye_hash_miss, "terminalLoopCount", 1)
    if eye_expected(eye_hash_miss)[0] != _eye_site_event(False, 0):
        print("EyeCensus hash miss did not continue to the natural loop exit")
        return 1

    def eye_count_rule(fact, index=0, draw_count=17, rule_count=1,
                       high_gate=0, minimum=None, high=None, exact=None,
                       rule_kind=ord("N")):
        rule = fact["rules"][index]
        eye_set(rule, "loopCount", rule_count)
        eye_set(rule, "vsHashGate", 0)
        eye_set(rule, "countHighGate", high_gate)
        if high_gate:
            eye_set(rule, "countMinimum", minimum)
            if draw_count >= minimum:
                eye_set(rule, "countHighBound", high)
        else:
            eye_set(rule, "exactCount", exact if exact is not None else draw_count)
        eye_set(rule, "ruleKind", rule_kind)
        return rule

    eye_zero_draw = eye_fact()
    eye_count_rule(eye_zero_draw, draw_count=0, exact=17)
    eye_set(eye_zero_draw, "terminalLoopCount", 1)
    if eye_expected(eye_zero_draw, count=0)[0] != _eye_site_event(False, 0):
        print("EyeCensus raw zero draw count did not replay a known negative")
        return 1

    eye_exact = eye_fact()
    erule = eye_count_rule(eye_exact)
    for filt in erule["filters"]:
        eye_set(filt, "modeOffGate", 0)
    eye_set(eye_exact["mutation"], "censusSkippedBefore", 0)
    eye_set(eye_exact["mutation"], "censusSkippedAfter", 1)
    if eye_expected(eye_exact)[0] != _eye_site_event(True, 0):
        print("EyeCensus exact-count and four off filters did not claim")
        return 1

    eye_range_short = eye_fact()
    erule = eye_count_rule(eye_range_short, high_gate=1, minimum=10, high=16)
    eye_set(eye_range_short, "terminalLoopCount", 1)
    if eye_expected(eye_range_short)[0] != _eye_site_event(False, 0):
        print("EyeCensus high range miss did not stop before exact-count")
        return 1

    eye_kind_miss = eye_fact()
    erule = eye_count_rule(eye_kind_miss, rule_kind=ord("D"))
    eye_set(eye_kind_miss, "terminalLoopCount", 1)
    if eye_expected(eye_kind_miss)[0] != _eye_site_event(False, 0):
        print("EyeCensus kind mismatch did not decline after the count reads")
        return 1

    eye_filter_modes = eye_fact()
    erule = eye_count_rule(eye_filter_modes)
    f0, f1, f2, f3 = erule["filters"]
    eye_set(f0, "modeOffGate", 0)
    eye_set(f1, "modeOffGate", 1); eye_set(f1, "modeAnyGate", 4)
    eye_set(f2, "modeOffGate", 1); eye_set(f2, "modeAnyGate", 2)
    eye_set(f2, "boundNonNull", False); eye_set(f2, "modeAfterBound", 3)
    eye_set(f3, "modeOffGate", 1); eye_set(f3, "modeAnyGate", 2)
    eye_set(f3, "boundNonNull", True); eye_set(f3, "modeAfterBound", 2)
    eye_set(f3, "resolved", True); eye_set(f3, "isTexture2D", True)
    eye_set(f3, "width", 2048); eye_set(f3, "height", 2048)
    eye_set(f3, "modeAfterResolve", 2); eye_set(f3, "eyeSizeAvailable", True)
    eye_set(f3, "eyeWidth", 2048); eye_set(f3, "eyeHeight", 2048)
    if eye_expected(eye_filter_modes)[0] != _eye_site_event(True, 0):
        print("EyeCensus off, any, none, and eye-size filters failed")
        return 1

    eye_size_unavailable = eye_fact()
    erule = eye_count_rule(eye_size_unavailable)
    filt = erule["filters"][0]
    for field, value in (("modeOffGate", 1), ("modeAnyGate", 2),
                         ("boundNonNull", True), ("modeAfterBound", 2),
                         ("resolved", True), ("isTexture2D", True),
                         ("modeAfterResolve", 2), ("eyeSizeAvailable", False)):
        eye_set(filt, field, value)
    eye_set(eye_size_unavailable, "terminalLoopCount", 1)
    if eye_expected(eye_size_unavailable)[0] != _eye_site_event(False, 0):
        print("EyeCensus unavailable eye size did not short-circuit before dimensions")
        return 1

    eye_none_bound = eye_fact()
    erule = eye_count_rule(eye_none_bound)
    filt = erule["filters"][0]
    eye_set(filt, "modeOffGate", 1); eye_set(filt, "modeAnyGate", 2)
    eye_set(filt, "boundNonNull", True); eye_set(filt, "modeAfterBound", 3)
    eye_set(eye_none_bound, "terminalLoopCount", 1)
    if eye_expected(eye_none_bound)[0] != _eye_site_event(False, 0):
        print("EyeCensus None mode accepted a non-null binding")
        return 1

    eye_resolver_false = eye_fact()
    erule = eye_count_rule(eye_resolver_false)
    filt = erule["filters"][0]
    for field, value in (("modeOffGate", 1), ("modeAnyGate", 2),
                         ("boundNonNull", True), ("modeAfterBound", 1),
                         ("resolved", False)):
        eye_set(filt, field, value)
    eye_set(eye_resolver_false, "terminalLoopCount", 1)
    if eye_expected(eye_resolver_false)[0] != _eye_site_event(False, 0):
        print("EyeCensus known resolver failure did not replay as a miss")
        return 1

    eye_nontexture = eye_fact()
    erule = eye_count_rule(eye_nontexture)
    filt = erule["filters"][0]
    for field, value in (("modeOffGate", 1), ("modeAnyGate", 2),
                         ("boundNonNull", True), ("modeAfterBound", 1),
                         ("resolved", True), ("isTexture2D", False)):
        eye_set(filt, field, value)
    eye_set(eye_nontexture, "terminalLoopCount", 1)
    if eye_expected(eye_nontexture)[0] != _eye_site_event(False, 0):
        print("EyeCensus non-texture resource did not replay as a miss")
        return 1

    eye_exact_width = eye_fact()
    erule = eye_count_rule(eye_exact_width)
    filt = erule["filters"][0]
    for field, value in (("modeOffGate", 1), ("modeAnyGate", 2),
                         ("boundNonNull", True), ("modeAfterBound", 1),
                         ("resolved", True), ("isTexture2D", True),
                         ("width", 1024),
                         ("modeAfterResolve", 1), ("configuredWidth", 2048)):
        eye_set(filt, field, value)
    eye_set(eye_exact_width, "terminalLoopCount", 1)
    if eye_expected(eye_exact_width)[0] != _eye_site_event(False, 0):
        print("EyeCensus exact-width mismatch did not short-circuit before height")
        return 1

    eye_changed_bound = eye_fact()
    eye_count_rule(eye_changed_bound, index=0, exact=0, rule_count=1)
    eye_count_rule(eye_changed_bound, index=1, exact=17, rule_count=2)
    for filt in eye_changed_bound["rules"][1]["filters"]:
        eye_set(filt, "modeOffGate", 0)
    if eye_expected(eye_changed_bound)[0] != _eye_site_event(True, 1):
        print("EyeCensus per-iteration loop bound change did not reach rule one")
        return 1

    eye_order_seven = eye_fact()
    for i in range(8):
        count = i + 1
        rule = eye_count_rule(eye_order_seven, index=i,
                              exact=(0 if i < 7 else 17), rule_count=count)
        if i == 7:
            for filt in rule["filters"]:
                eye_set(filt, "modeOffGate", 0)
    if eye_expected(eye_order_seven)[0] != _eye_site_event(True, 7):
        print("EyeCensus did not preserve winning subsite seven")
        return 1

    eye_unknown = eye_fact()
    eye_unknown["skipCountGate"] = eye_read(None, True, False)
    try:
        eye_expected(eye_unknown)
    except _EyeCensusUnavailable:
        pass
    else:
        print("EyeCensus unknown consumed gate became a negative result")
        return 1

    eye_unknown_dispatch = {"kind": ord("N"), "count": 17,
                             "sites": [_eye_site_event(False, 0)],
                             "predicateFacts": [eye_unknown]}
    unknown_eye_replay = _replay_predicate_facts(
        eye_unknown_dispatch, "eye-unknown-dispatch", 10)
    if (unknown_eye_replay["eyeCensusUnreplayable"] != 1 or
            unknown_eye_replay["mutationUnobserved"] != 0):
        print("EyeCensus lazy unknown incorrectly became a miss or mutation warning")
        return 1

    eye_null = json.loads(json.dumps(eye_resolver_false))
    eye_set(eye_null["rules"][0]["filters"][0], "boundNonNull", False)
    if eye_expected(eye_null)[0] != _eye_site_event(False, 0):
        print("EyeCensus null view did not consume known-false resolver return")
        return 1
    eye_null["rules"][0]["filters"][0]["resolved"] = eye_read(None, True, False)
    try:
        eye_expected(eye_null)
    except _EyeCensusUnavailable:
        pass
    else:
        print("EyeCensus null view fabricated a negative from unknown resolver return")
        return 1

    eye_bad_shapes = []
    bad = json.loads(json.dumps(eye_unknown)); bad["rules"] = []; eye_bad_shapes.append(bad)
    bad = json.loads(json.dumps(eye_unknown)); bad["rules"][0]["filters"][0]["width"]["known"] = 1; eye_bad_shapes.append(bad)
    bad = json.loads(json.dumps(eye_unknown)); eye_set(bad["rules"][7], "loopCount", 9); eye_bad_shapes.append(bad)
    bad = json.loads(json.dumps(eye_unknown)); eye_set(bad, "skipCountGate", 9); eye_bad_shapes.append(bad)
    bad = json.loads(json.dumps(eye_exact)); eye_set(bad["rules"][0], "heldVsHash", 0); eye_bad_shapes.append(bad)
    bad = eye_fact(); eye_set(bad, "skipCountGate", 1); eye_set(bad, "terminalLoopCount", 0)
    eye_set(bad["rules"][0]["filters"][0], "modeOffGate", 0); eye_bad_shapes.append(bad)
    bad = json.loads(json.dumps(eye_unknown)); bad["rules"][7]["filters"][3]["extra"] = 1; eye_bad_shapes.append(bad)
    for bad in eye_bad_shapes:
        try:
            eye_expected(bad)
        except TraceError:
            pass
        else:
            print("EyeCensus accepted malformed envelope or short-circuit reads")
            return 1

    eye_unknown_counter = json.loads(json.dumps(eye_exact))
    eye_unknown_counter["mutation"]["censusSkippedAfter"] = eye_read(None, True, False)
    counter_dispatch = {"kind": ord("N"), "count": 17,
                        "sites": [_eye_site_event(True, 0)],
                        "predicateFacts": [eye_unknown_counter]}
    counter_replay = _replay_predicate_facts(counter_dispatch, "eye-unknown-counter", 10)
    if (counter_replay["eyeCensusReplayed"] != 1 or
            counter_replay["eyeCensusUnreplayable"] != 0 or
            counter_replay["mutationUnobserved"] != 1):
        print("EyeCensus known winner did not distinguish unavailable counter observation")
        return 1

    eye_dispatch = {"kind": ord("N"), "count": 17,
                    "sites": [_eye_site_event(True, 0)],
                    "predicateFacts": [eye_exact]}
    eye_replay = _replay_predicate_facts(eye_dispatch, "eye-dispatch", 10)
    if (eye_replay["eyeCensusFacts"] != 1 or
            eye_replay["eyeCensusReplayed"] != 1 or
            eye_replay["eyeCensusMismatches"] or
            eye_replay["mutationUnobserved"]):
        print("EyeCensus kind17 did not pass supported-fact replay")
        return 1
    full_eye_trace = json.loads(json.dumps(vr))
    full_eye_trace["predicateFactVersion"] = 10
    full_eye_draw = full_eye_trace["draws"][0]
    full_eye_draw["sites"][-1].update(outcome=2, flow=0, verdict=-1)
    for site_id in (44, 45, 71, 46, 47):
        full_eye_draw["sites"].append({
            "id": site_id, "kind": SITE_KINDS[site_id], "outcome": 1,
            "flow": 0, "subsite": 0, "verdict": -1})
    full_eye_draw["sites"].append(_eye_site_event(True, 0))
    full_eye_draw["winnerSiteId"] = 48
    full_eye_draw["verdict"] = 2
    full_eye_draw["forwardFacts"]["verdictOrdinal"] = 2
    full_eye_selector = eye_fact()
    erule = eye_count_rule(full_eye_selector,
                           exact=full_eye_draw["count"],
                           rule_kind=full_eye_draw["kind"])
    for filt in erule["filters"]:
        eye_set(filt, "modeOffGate", 0)
    eye_set(full_eye_selector["mutation"], "censusSkippedBefore", 7)
    eye_set(full_eye_selector["mutation"], "censusSkippedAfter", 8)
    full_eye_draw["predicateFacts"].append(full_eye_selector)
    full_eye_draw["predicateFacts"].append(basic_context_fact())
    full_eye_summary = validate_trace(full_eye_trace)["predicateReplay"]
    if (full_eye_summary["eyeCensusStatus"] != "replayed" or
            full_eye_summary["eyeCensusReplayed"] != 1):
        print("schema10 did not expose EyeCensus status through full validation")
        return 1
    if (validate_trace(v9_basic)["predicateReplay"]["eyeCensusStatus"] !=
            "unavailable-before-v10"):
        print("schema9 did not report EyeCensus as unavailable")
        return 1
    eye_dispatch_v9 = {"kind": ord("N"), "count": 17,
                       "sites": [_eye_site_event(True, 0)],
                       "predicateFacts": []}
    if _replay_predicate_facts(eye_dispatch_v9, "eye-v9-unavailable", 9)["factCount"]:
        print("predicate fact version 9 treated site48 as supported")
        return 1
    eye_bad_fact = json.loads(json.dumps(eye_dispatch))
    del eye_bad_fact["predicateFacts"][0]["skipCountGate"]
    try:
        _replay_predicate_facts(eye_bad_fact, "eye-missing-consumed-read", 10)
    except TraceError:
        pass
    else:
        print("EyeCensus accepted a missing consumed read")
        return 1
    eye_bad_fact = json.loads(json.dumps(eye_dispatch))
    eye_bad_fact["predicateFacts"][0]["rules"] = []
    try:
        _replay_predicate_facts(eye_bad_fact, "eye-wrong-cardinality", 10)
    except TraceError:
        pass
    else:
        print("EyeCensus accepted a malformed fixed rule table")
        return 1
    eye_duplicate = json.loads(json.dumps(eye_dispatch))
    eye_duplicate["predicateFacts"].append(json.loads(json.dumps(eye_exact)))
    try:
        _replay_predicate_facts(eye_duplicate, "eye-duplicate-fact", 10)
    except TraceError:
        pass
    else:
        print("EyeCensus accepted a duplicate site fact")
        return 1

    eye_bad_order = eye_fact()
    eye_bad_order["rules"][0]["loopCount"] = eye_read(1, True, True)
    eye_set(eye_bad_order["rules"][0], "vsHashGate", 0)
    eye_set(eye_bad_order["rules"][0], "countHighGate", 0)
    eye_set(eye_bad_order["rules"][0], "exactCount", 17)
    eye_set(eye_bad_order["rules"][0], "ruleKind", ord("N"))
    eye_set(eye_bad_order["rules"][0]["filters"][0], "modeOffGate", 1)
    eye_set(eye_bad_order["rules"][0]["filters"][0], "modeAnyGate", 2)
    eye_set(eye_bad_order["rules"][0]["filters"][0], "boundNonNull", True)
    eye_set(eye_bad_order["rules"][0]["filters"][0], "modeAfterBound", 1)
    eye_set(eye_bad_order["rules"][0]["filters"][0], "resolved", False)
    eye_set(eye_bad_order["rules"][0]["filters"][0], "isTexture2D", False)
    try:
        eye_expected(eye_bad_order)
    except TraceError:
        pass
    else:
        print("EyeCensus accepted reads after resolver failure")
        return 1

    eye_mismatch = eye_fact()
    eye_count_rule(eye_mismatch)
    for filt in eye_mismatch["rules"][0]["filters"]:
        eye_set(filt, "modeOffGate", 0)
    eye_set(eye_mismatch["mutation"], "censusSkippedBefore", 100)
    eye_set(eye_mismatch["mutation"], "censusSkippedAfter", 100)
    if eye_expected(eye_mismatch)[1] != 1:
        print("EyeCensus missed a recorded counter increment mismatch")
        return 1

    legacy_eye_trace = json.loads(json.dumps(earlier_exit))
    legacy_eye_trace["predicateFacts"] = []
    if _replay_predicate_facts(legacy_eye_trace, "eye-v9-compat", 9)["eyeCensusFacts"] != 0:
        print("predicate fact version 9 fabricated EyeCensus coverage")
        return 1

    def rb_read(value=None, reached=False, known=False):
        return {"reached": reached, "known": known,
                "value": value if known else None}

    rb_outer_fields = ("wants", "psPresent", "psHash")
    rb_helper_fields = (
        "wants", "contextNonNull", "psPresent", "psHash", "lambdaEntered",
        "psGetReached", "psGetCompleted", "shaderNonNull", "lookupReached",
        "lookupCompleted", "lookupHash", "cacheBeforePresent",
        "cacheBeforeHash", "cacheAfterPresent", "cacheAfterHash",
        "releaseReached", "releaseCompleted", "guardReturned")

    def rb_fact():
        return {"siteId": 60, "kind": 18, "known": "yes",
                "outer": {name: rb_read() for name in rb_outer_fields},
                "helper": {name: rb_read() for name in rb_helper_fields}}

    def rb_set(fact, group, name, value):
        fact[group][name] = rb_read(value, True, True)

    def rb_default_fallback():
        fact = rb_fact()
        for name, value in (("wants", True), ("psPresent", False), ("psHash", 0)):
            rb_set(fact, "outer", name, value)
        for name, value in (("wants", True), ("contextNonNull", True),
                            ("psPresent", False), ("psHash", 0)):
            rb_set(fact, "helper", name, value)
        return fact

    def rb_fallback_entered(fact, guard_returned):
        rb_set(fact, "helper", "lambdaEntered", True)
        rb_set(fact, "helper", "guardReturned", guard_returned)
        rb_set(fact, "helper", "psGetReached", True)

    rb_outer_off = rb_fact()
    rb_set(rb_outer_off, "outer", "wants", False)
    if _replay_resolve_bind_fact(rb_outer_off, "resolve-outer-off")[0] != _resolve_bind_event(False):
        print("ResolveBind outer gate off did not short-circuit")
        return 1

    rb_outer_no = rb_fact()
    for name, value in (("wants", True), ("psPresent", True),
                        ("psHash", RESOLVE_BIND_PS_HASH ^ 1)):
        rb_set(rb_outer_no, "outer", name, value)
    if _replay_resolve_bind_fact(rb_outer_no, "resolve-outer-no")[0] != _resolve_bind_event(False):
        print("ResolveBind known outer shadow miss did not decline")
        return 1

    rb_reentry = rb_fact()
    for name, value in (("wants", True), ("psPresent", True),
                        ("psHash", RESOLVE_BIND_PS_HASH)):
        rb_set(rb_reentry, "outer", name, value)
    rb_set(rb_reentry, "helper", "wants", False)
    if _replay_resolve_bind_fact(rb_reentry, "resolve-reentry-off")[0] != _resolve_bind_event(False):
        print("ResolveBind failed independent repeated wants replay")
        return 1

    rb_inner_yes = rb_default_fallback()
    rb_set(rb_inner_yes, "helper", "psPresent", True)
    rb_set(rb_inner_yes, "helper", "psHash", RESOLVE_BIND_PS_HASH)
    if _replay_resolve_bind_fact(rb_inner_yes, "resolve-inner-yes")[0] != _resolve_bind_event(True):
        print("ResolveBind inner cached match did not claim")
        return 1

    rb_inner_no = rb_default_fallback()
    rb_set(rb_inner_no, "helper", "psPresent", True)
    rb_set(rb_inner_no, "helper", "psHash", RESOLVE_BIND_PS_HASH ^ 2)
    if _replay_resolve_bind_fact(rb_inner_no, "resolve-inner-no")[0] != _resolve_bind_event(False):
        print("ResolveBind inner cached miss did not decline")
        return 1

    rb_null_context = rb_fact()
    for name, value in (("wants", True), ("psPresent", False), ("psHash", 0)):
        rb_set(rb_null_context, "outer", name, value)
    rb_set(rb_null_context, "helper", "wants", True)
    rb_set(rb_null_context, "helper", "contextNonNull", False)
    if _replay_resolve_bind_fact(rb_null_context, "resolve-null-context")[0] != _resolve_bind_event(False):
        print("ResolveBind null context reached helper inputs")
        return 1

    rb_denied = rb_default_fallback()
    rb_set(rb_denied, "helper", "lambdaEntered", False)
    rb_set(rb_denied, "helper", "guardReturned", False)
    if _replay_resolve_bind_fact(rb_denied, "resolve-budget-denied")[0] != _resolve_bind_event(False):
        print("ResolveBind denied fallback budget did not preserve initialized miss")
        return 1

    rb_ps_fault = rb_default_fallback()
    rb_fallback_entered(rb_ps_fault, False)
    if _replay_resolve_bind_fact(rb_ps_fault, "resolve-psget-fault")[0] != _resolve_bind_event(False):
        print("ResolveBind PSGet fault did not preserve initialized miss")
        return 1

    rb_null_ps = rb_default_fallback()
    rb_fallback_entered(rb_null_ps, True)
    for name, value in (("psGetCompleted", True), ("shaderNonNull", False)):
        rb_set(rb_null_ps, "helper", name, value)
    if _replay_resolve_bind_fact(rb_null_ps, "resolve-null-ps")[0] != _resolve_bind_event(False):
        print("ResolveBind null shader did not replay as a known miss")
        return 1

    rb_lookup_fault = rb_default_fallback()
    rb_fallback_entered(rb_lookup_fault, False)
    for name, value in (("psGetCompleted", True), ("shaderNonNull", True),
                        ("lookupReached", True)):
        rb_set(rb_lookup_fault, "helper", name, value)
    if _replay_resolve_bind_fact(rb_lookup_fault, "resolve-lookup-fault")[0] != _resolve_bind_event(False):
        print("ResolveBind lookup fault did not preserve initialized miss")
        return 1

    rb_lookup_zero = rb_default_fallback()
    rb_fallback_entered(rb_lookup_zero, True)
    for name, value in (("psGetCompleted", True), ("shaderNonNull", True),
                        ("lookupReached", True), ("lookupCompleted", True),
                        ("lookupHash", 0), ("releaseReached", True),
                        ("releaseCompleted", True)):
        rb_set(rb_lookup_zero, "helper", name, value)
    if _replay_resolve_bind_fact(rb_lookup_zero, "resolve-lookup-zero")[0] != _resolve_bind_event(False):
        print("ResolveBind successful zero lookup was not a known miss")
        return 1

    def rb_nonzero_fallback(hash_value, release_complete=True,
                            include_cache=True):
        fact = rb_default_fallback()
        rb_fallback_entered(fact, release_complete)
        for name, value in (("psGetCompleted", True), ("shaderNonNull", True),
                            ("lookupReached", True), ("lookupCompleted", True),
                            ("lookupHash", hash_value)):
            rb_set(fact, "helper", name, value)
        if include_cache:
            for name, value in (("cacheBeforePresent", False),
                                ("cacheBeforeHash", 0),
                                ("cacheAfterPresent", True),
                                ("cacheAfterHash", hash_value)):
                rb_set(fact, "helper", name, value)
        rb_set(fact, "helper", "releaseReached", True)
        if release_complete:
            rb_set(fact, "helper", "releaseCompleted", True)
        return fact

    rb_fallback_hit = rb_nonzero_fallback(RESOLVE_BIND_PS_HASH)
    if _replay_resolve_bind_fact(rb_fallback_hit, "resolve-fallback-hit") != (_resolve_bind_event(True), 0, False):
        print("ResolveBind nonzero matching fallback did not claim")
        return 1

    rb_absent_nonzero = json.loads(json.dumps(rb_fallback_hit))
    rb_set(rb_absent_nonzero, "helper", "cacheBeforeHash", RESOLVE_BIND_PS_HASH ^ 7)
    if _replay_resolve_bind_fact(rb_absent_nonzero, "resolve-absent-nonzero") != (_resolve_bind_event(True), 0, False):
        print("ResolveBind discarded an unconditional hash snapshot for absent shadow")
        return 1
    rb_absent_unknown = json.loads(json.dumps(rb_fallback_hit))
    rb_absent_unknown["helper"]["cacheBeforeHash"] = rb_read(None, True, False)
    if _replay_resolve_bind_fact(rb_absent_unknown, "resolve-absent-unknown") != (_resolve_bind_event(True), 0, True):
        print("ResolveBind treated unknown absent-shadow hash as observed repair")
        return 1
    rb_after_absent = json.loads(json.dumps(rb_fallback_hit))
    rb_set(rb_after_absent, "helper", "cacheAfterPresent", False)
    if _replay_resolve_bind_fact(rb_after_absent, "resolve-after-absent") != (_resolve_bind_event(True), 1, False):
        print("ResolveBind failed to flag observed cache repair mismatch")
        return 1

    rb_release_fault = rb_nonzero_fallback(RESOLVE_BIND_PS_HASH, False)
    if _replay_resolve_bind_fact(rb_release_fault, "resolve-release-fault")[0] != _resolve_bind_event(True):
        print("ResolveBind Release fault erased an already assigned match")
        return 1

    rb_fallback_miss = rb_nonzero_fallback(RESOLVE_BIND_PS_HASH ^ 4)
    if _replay_resolve_bind_fact(rb_fallback_miss, "resolve-fallback-miss")[0] != _resolve_bind_event(False):
        print("ResolveBind nonmatching fallback hash did not decline")
        return 1

    rb_repair_fault = rb_default_fallback()
    rb_fallback_entered(rb_repair_fault, False)
    for name, value in (("psGetCompleted", True), ("shaderNonNull", True),
                        ("lookupReached", True), ("lookupCompleted", True),
                        ("lookupHash", RESOLVE_BIND_PS_HASH),
                        ("cacheBeforePresent", False)):
        rb_set(rb_repair_fault, "helper", name, value)
    repair_fault = _replay_resolve_bind_fact(rb_repair_fault, "resolve-repair-fault")
    if repair_fault != (_resolve_bind_event(False), 0, True):
        print("ResolveBind repair fault did not preserve initialized miss and unobserved boundary")
        return 1

    rb_missing_cache = rb_nonzero_fallback(RESOLVE_BIND_PS_HASH,
                                           include_cache=False)
    missing_cache = _replay_resolve_bind_fact(rb_missing_cache,
                                              "resolve-missing-cache")
    if missing_cache[0] != _resolve_bind_event(True) or not missing_cache[2]:
        print("ResolveBind missing repair boundary altered selector replay")
        return 1

    rb_unknown = rb_default_fallback()
    rb_unknown["outer"]["wants"] = rb_read(None, True, False)
    try:
        _replay_resolve_bind_fact(rb_unknown, "resolve-unknown-input")
    except _ResolveBindUnavailable:
        pass
    else:
        print("ResolveBind unknown consumed input became a negative result")
        return 1

    rb_dispatch = {"kind": ord("N"), "count": 17,
                   "sites": [_resolve_bind_event(True)],
                   "predicateFacts": [rb_fallback_hit]}
    rb_replay = _replay_predicate_facts(rb_dispatch, "resolve-dispatch", 11)
    if (rb_replay["resolveBindFacts"] != 1 or
            rb_replay["resolveBindReplayed"] != 1 or
            rb_replay["resolveBindMismatches"] or
            rb_replay["mutationUnobserved"]):
        print("ResolveBind kind18 did not pass supported-fact replay")
        return 1

    rb_hidden = {"kind": ord("N"), "count": 17,
                 "sites": [_resolve_bind_event(False)],
                 "predicateFacts": [json.loads(json.dumps(rb_outer_off))]}
    rb_hidden["predicateFacts"][0]["helper"]["releaseCompleted"] = rb_read(True, True, True)
    try:
        _replay_predicate_facts(rb_hidden, "resolve-hidden-read", 11)
    except TraceError:
        pass
    else:
        print("ResolveBind accepted a hidden Release read after an unreached release")
        return 1

    malformed_resolve_bind = []
    rb_missing_field = json.loads(json.dumps(rb_outer_off))
    del rb_missing_field["helper"]["guardReturned"]
    malformed_resolve_bind.append(rb_missing_field)
    rb_extra_field = json.loads(json.dumps(rb_outer_off))
    rb_extra_field["helper"]["shadowMatch"] = rb_read()
    malformed_resolve_bind.append(rb_extra_field)
    rb_bad_boolean = json.loads(json.dumps(rb_outer_off))
    rb_bad_boolean["outer"]["wants"] = rb_read(1, True, True)
    malformed_resolve_bind.append(rb_bad_boolean)
    rb_bad_integer = json.loads(json.dumps(rb_outer_off))
    rb_bad_integer["outer"]["psHash"] = rb_read(True, True, True)
    malformed_resolve_bind.append(rb_bad_integer)
    rb_bad_read = json.loads(json.dumps(rb_outer_off))
    rb_bad_read["outer"]["wants"] = {"reached": 1, "known": False, "value": None}
    malformed_resolve_bind.append(rb_bad_read)
    for malformed in malformed_resolve_bind:
        try:
            _replay_resolve_bind_fact(malformed, "resolve-malformed-shape")
        except TraceError:
            pass
        else:
            print("ResolveBind accepted a malformed read shape or hidden field")
            return 1

    rb_duplicate = json.loads(json.dumps(rb_dispatch))
    rb_duplicate["predicateFacts"].append(json.loads(json.dumps(rb_fallback_hit)))
    try:
        _replay_predicate_facts(rb_duplicate, "resolve-duplicate", 11)
    except TraceError:
        pass
    else:
        print("ResolveBind accepted duplicate site60 facts")
        return 1
    rb_v10_trace = {"kind": ord("N"), "count": 17,
                    "sites": [_resolve_bind_event(True)], "predicateFacts": []}
    if _replay_predicate_facts(rb_v10_trace, "resolve-v10-compat", 10)["factCount"]:
        print("predicate fact version 10 treated ResolveBind as supported")
        return 1

    # Tuple slot six is a legacy claim boolean for these older fact types,
    # but a mutation-availability flag for EyeCensus. Never conflate them.
    positive_claims_v10 = (
        (6, stars_trace(hidden="yes", hash_source=1,
                        vs_hash="9AEC596A2B036EA6", claimed=True)),
        (50, range_trace(10, [], selector_cases[3][2])),
        (53, holo_positive), (55, scrim_positive))
    for site_id, trace in positive_claims_v10:
        draw = json.loads(json.dumps(trace["draws"][0]))
        draw["sites"] = [event for event in draw["sites"] if event["id"] == site_id]
        draw["predicateFacts"] = [fact for fact in draw["predicateFacts"]
                                  if fact["siteId"] == site_id]
        replay = _replay_predicate_facts(draw, "positive-claim-v10", 10)
        if (replay["replayed"] != 1 or replay["unreplayable"] != 0 or
                replay["mismatches"] != 0 or replay["mutationUnobserved"] != 0):
            print("schema10 confused a known legacy claim with an unobserved mutation", site_id)
            return 1

    def lp_read(value=None, reached=False, known=False):
        return {"reached": reached, "known": known, "value": value}

    def lp_set(group, name, value):
        group[name] = lp_read(value, True, True)

    def lp_unread(group, names):
        for name in names:
            group[name] = lp_read()

    lp_outer_names = ("wants", "eyeDrawsLastFrame", "rtvPresent", "resolved",
                      "isTexture2D", "targetWidth", "targetHeight", "qsStartIndex",
                      "qsBaseVertex", "textured")
    lp_sequence_names = ("position", "hashBeforeCount", "hashAfterCount",
                         "hashBeforeWidth", "hashAfterWidth", "hashBeforeHeight",
                         "hashAfterHeight", "slotCount", "slotWidth", "slotHeight",
                         "lenBefore", "lenAfter")
    lp_panel_names = ("frameAnyBefore", "frameAnyAfter", "frameFirstPanelDoneBefore",
                      "frameFirstPanelDoneAfter", "chainOnBeforeCollection", "chainWidth",
                      "chainHeight", "frameChainPanelBefore", "frameChainPanelAfter",
                      "panelOrdinalBefore", "panelOrdinalAfter", "localOrdinal")
    lp_collection_names = ("collecting", "capCountGate", "guardEntered", "guardReturned",
                           "capCountBeforeWrite", "capCountAfterWrite", "ibFillBeforeWrite",
                           "ibFillAfterWrite", "capDroppedBeforeWrite", "capDroppedAfterWrite")
    lp_withhold_names = ("subArmBeforeClear", "subArmAfterClear", "subArmBeforeWithhold",
                         "subArmAfterWithhold", "chainOrdCountGate", "terminalChainOrdCount",
                         "specDoneGate", "chainOnSpecGate", "retiredGate", "frameWithheldBefore",
                         "frameWithheldAfter", "dimLiveBefore", "dimLiveAfter")

    def loader_panel_fact(claim=True, collection_warning=True, first_panel=False):
        outer = {name: lp_read() for name in lp_outer_names}
        for name, value in (("wants", True), ("eyeDrawsLastFrame", 0), ("rtvPresent", True),
                            ("resolved", True), ("isTexture2D", True), ("targetWidth", 1024),
                            ("targetHeight", 512), ("qsStartIndex", 13), ("qsBaseVertex", -7),
                            ("textured", False)):
            lp_set(outer, name, value)
        seq = {name: lp_read() for name in lp_sequence_names}
        h0 = 0xfffffff0
        h1 = ((h0 ^ 30) * 16777619) & 0xffffffff
        h2 = ((h1 ^ 1024) * 16777619) & 0xffffffff
        h3 = ((h2 ^ 512) * 16777619) & 0xffffffff
        for name, value in (("position", 0), ("hashBeforeCount", h0), ("hashAfterCount", h1),
                            ("hashBeforeWidth", h1), ("hashAfterWidth", h2),
                            ("hashBeforeHeight", h2), ("hashAfterHeight", h3),
                            ("slotCount", 30), ("slotWidth", 1024), ("slotHeight", 512),
                            ("lenBefore", 0), ("lenAfter", 1)):
            lp_set(seq, name, value)
        panel = {name: lp_read() for name in lp_panel_names}
        for name, value in (("frameAnyBefore", first_panel), ("frameAnyAfter", True),
                            ("frameFirstPanelDoneBefore", not first_panel),
                            ("frameFirstPanelDoneAfter", True), ("chainOnBeforeCollection", claim),
                            ("localOrdinal", 2 if claim else 0xffffffff)):
            lp_set(panel, name, value)
        if claim:
            for name, value in (("chainWidth", 1024), ("chainHeight", 512),
                                ("frameChainPanelBefore", False), ("frameChainPanelAfter", True),
                                ("panelOrdinalBefore", 2), ("panelOrdinalAfter", 3)):
                lp_set(panel, name, value)
        collection = {name: lp_read() for name in lp_collection_names}
        lp_set(collection, "collecting", collection_warning)
        if collection_warning:
            for name, value in (("capCountGate", 0), ("guardEntered", True),
                                ("guardReturned", True), ("capDroppedBeforeWrite", 8),
                                ("capDroppedAfterWrite", 9)):
                lp_set(collection, name, value)
        withhold = {name: lp_read() for name in lp_withhold_names}
        lp_set(withhold, "subArmBeforeClear", True)
        lp_set(withhold, "subArmAfterClear", False)
        chain_scan = [{"loopCount": lp_read(), "chainOrd": lp_read()} for _ in range(4)]
        if claim:
            lp_set(withhold, "chainOrdCountGate", 1)
            lp_set(chain_scan[0], "loopCount", 1)
            lp_set(chain_scan[0], "chainOrd", 2)
            lp_set(withhold, "subArmBeforeWithhold", False)
            lp_set(withhold, "subArmAfterWithhold", True)
            lp_set(withhold, "frameWithheldBefore", False)
            lp_set(withhold, "frameWithheldAfter", True)
            lp_set(withhold, "dimLiveBefore", False)
            lp_set(withhold, "dimLiveAfter", True)
        else:
            lp_set(withhold, "specDoneGate", True)
        return {"siteId": 25, "kind": 19, "known": "yes", "outer": outer,
                "helper": {"wants": lp_read(True, True, True),
                           "contextNonNull": lp_read(True, True, True),
                           "sequence": seq, "panel": panel,
                           "collection": dict(collection,
                               collectionMutationUnobserved=collection_warning),
                           "withhold": dict(withhold, chainScan=chain_scan)}}

    lp_draw = {"kind": ord("X"), "count": 30, "instances": 1}
    lp_positive = loader_panel_fact()
    if _replay_loader_panel_fact(lp_positive, lp_draw, "loader-positive") != \
            (_loader_panel_event(True), 0, True):
        print("LoaderPanel raw chain ordinal did not replay independently with an opaque collection warning")
        return 1
    lp_dispatch = {"kind": ord("X"), "count": 30, "instances": 1,
                   "sites": [_loader_panel_event(True)], "predicateFacts": [lp_positive]}
    lp_replay = _replay_predicate_facts(lp_dispatch, "loader-dispatch", 12)
    if (lp_replay["loaderPanelReplayed"] != 1 or lp_replay["loaderPanelUnreplayable"] != 0 or
            lp_replay["loaderPanelMismatches"] != 0 or
            lp_replay["loaderPanelMutationUnobserved"] != 1 or
            lp_replay["mutationUnobserved"] != 1):
        print("LoaderPanel selector replay did not separate the collection-mutation warning")
        return 1
    lp_false = loader_panel_fact(claim=False, collection_warning=False)
    if _replay_loader_panel_fact(lp_false, lp_draw, "loader-known-miss") != \
            (_loader_panel_event(False), 0, False):
        print("LoaderPanel known first-verdict miss did not replay as a decline")
        return 1
    # SiteEvents are outputs only: changing one cannot change the raw selector result.
    lp_wrong_output = json.loads(json.dumps(lp_dispatch))
    lp_wrong_output["sites"] = [_loader_panel_event(False)]
    lp_mismatch = _replay_predicate_facts(lp_wrong_output, "loader-output-check", 12)
    if lp_mismatch["loaderPanelReplayed"] != 0 or lp_mismatch["loaderPanelMismatches"] != 1:
        print("LoaderPanel used its observed SiteEvent as a selector input")
        return 1
    # A cap-full collection drops the qualifying draw but cannot alter the withhold decision.
    lp_cap = loader_panel_fact(collection_warning=False)
    lp_col = lp_cap["helper"]["collection"]
    lp_set(lp_col, "collecting", True)
    lp_set(lp_col, "capCountGate", 24)
    lp_set(lp_col, "capDroppedBeforeWrite", 2)
    lp_set(lp_col, "capDroppedAfterWrite", 3)
    if _replay_loader_panel_fact(lp_cap, lp_draw, "loader-cap-full") != \
            (_loader_panel_event(True), 0, False):
        print("LoaderPanel cap-full collection changed the independent selector")
        return 1
    # Outer range gates short-circuit lazily; 1023 does not consume target height or helper reads.
    lp_range = loader_panel_fact()
    for name in lp_outer_names[6:]:
        lp_range["outer"][name] = lp_read()
    lp_set(lp_range["outer"], "targetWidth", 1023)
    lp_range["helper"]["wants"] = lp_read()
    lp_range["helper"]["contextNonNull"] = lp_read()
    lp_unread(lp_range["helper"]["sequence"], lp_sequence_names)
    lp_unread(lp_range["helper"]["panel"], lp_panel_names)
    lp_unread(lp_range["helper"]["collection"], lp_collection_names)
    lp_range["helper"]["collection"]["collectionMutationUnobserved"] = False
    lp_unread(lp_range["helper"]["withhold"], lp_withhold_names)
    lp_range["helper"]["withhold"]["chainScan"] = [
        {"loopCount": lp_read(), "chainOrd": lp_read()} for _ in range(4)]
    if _replay_loader_panel_fact(lp_range, lp_draw, "loader-width-range")[0] != _loader_panel_event(False):
        print("LoaderPanel accepted a target below the 1024-pixel gate")
        return 1
    lp_outer_off = json.loads(json.dumps(lp_positive))
    lp_set(lp_outer_off["outer"], "wants", False)
    lp_unread(lp_outer_off["outer"], lp_outer_names[1:])
    lp_outer_off["helper"]["wants"] = lp_read()
    lp_outer_off["helper"]["contextNonNull"] = lp_read()
    lp_unread(lp_outer_off["helper"]["sequence"], lp_sequence_names)
    lp_unread(lp_outer_off["helper"]["panel"], lp_panel_names)
    lp_unread(lp_outer_off["helper"]["collection"], lp_collection_names)
    lp_outer_off["helper"]["collection"]["collectionMutationUnobserved"] = False
    lp_unread(lp_outer_off["helper"]["withhold"], lp_withhold_names)
    lp_outer_off["helper"]["withhold"]["chainScan"] = [
        {"loopCount": lp_read(), "chainOrd": lp_read()} for _ in range(4)]
    if _replay_loader_panel_fact(lp_outer_off, lp_draw, "loader-outer-off")[0] != _loader_panel_event(False):
        print("LoaderPanel outer-off short circuit did not decline")
        return 1
    # COM reentry may change the post-collection chain between repeated bound reads.
    lp_reentry = json.loads(json.dumps(lp_positive))
    lp_set(lp_reentry["helper"]["withhold"]["chainScan"][0], "chainOrd", 99)
    lp_set(lp_reentry["helper"]["withhold"], "terminalChainOrdCount", 0)
    lp_set(lp_reentry["helper"]["withhold"], "specDoneGate", False)
    lp_set(lp_reentry["helper"]["withhold"], "chainOnSpecGate", False)
    lp_set(lp_reentry["helper"]["withhold"], "retiredGate", False)
    lp_set(lp_reentry["helper"]["withhold"], "subArmBeforeWithhold", False)
    lp_set(lp_reentry["helper"]["withhold"], "subArmAfterWithhold", True)
    lp_set(lp_reentry["helper"]["panel"], "frameFirstPanelDoneBefore", False)
    lp_set(lp_reentry["helper"]["withhold"], "frameWithheldBefore", False)
    lp_set(lp_reentry["helper"]["withhold"], "frameWithheldAfter", True)
    lp_set(lp_reentry["helper"]["withhold"], "dimLiveBefore", False)
    lp_set(lp_reentry["helper"]["withhold"], "dimLiveAfter", True)
    if _replay_loader_panel_fact(lp_reentry, lp_draw, "loader-reentry") != \
            (_loader_panel_event(True), 0, True):
        print("LoaderPanel did not replay from post-collection reentry reads")
        return 1
    lp_bad = json.loads(json.dumps(lp_positive))
    lp_bad["helper"]["withhold"]["chainScan"] = lp_bad["helper"]["withhold"]["chainScan"][:3]
    try:
        _replay_loader_panel_fact(lp_bad, lp_draw, "loader-short-chain-array")
    except TraceError:
        pass
    else:
        print("LoaderPanel accepted a truncated fixed chain array")
        return 1
    lp_bad = json.loads(json.dumps(lp_positive))
    lp_bad["helper"]["withhold"]["chainOrdCountGate"] = lp_read(5, True, True)
    try:
        _replay_loader_panel_fact(lp_bad, lp_draw, "loader-chain-bound")
    except TraceError:
        pass
    else:
        print("LoaderPanel accepted a chain count outside its fixed capacity")
        return 1
    lp_unknown = json.loads(json.dumps(lp_positive))
    lp_unknown["outer"]["wants"] = lp_read(None, True, False)
    lp_unknown["helper"]["sequence"]["extra"] = lp_read(1, True, True)
    try:
        _replay_loader_panel_fact(lp_unknown, lp_draw, "loader-unknown-gate-shape")
    except TraceError:
        pass
    else:
        print("LoaderPanel unknown outer gate hid an invalid nested read envelope")
        return 1
    lp_bad_height = loader_panel_fact(claim=False, collection_warning=False)
    lp_set(lp_bad_height["helper"]["panel"], "chainOnBeforeCollection", True)
    lp_set(lp_bad_height["helper"]["panel"], "chainWidth", 2048)
    lp_set(lp_bad_height["helper"]["panel"], "chainHeight", 512)
    try:
        _replay_loader_panel_fact(lp_bad_height, lp_draw, "loader-unconsumed-height")
    except TraceError:
        pass
    else:
        print("LoaderPanel accepted height after a failed width gate")
        return 1

    # Writes can finish before a later Release fault; callback admission is
    # required, successful guard completion is not a stored-draw oracle.
    lp_stored = loader_panel_fact()
    stored_collection = lp_stored["helper"]["collection"]
    for name in ("capDroppedBeforeWrite", "capDroppedAfterWrite"):
        stored_collection[name] = lp_read()
    for name, value in (("capCountBeforeWrite", 23), ("capCountAfterWrite", 24),
                        ("ibFillBeforeWrite", 0), ("ibFillAfterWrite", 60),
                        ("guardReturned", False)):
        lp_set(stored_collection, name, value)
    if _replay_loader_panel_fact(lp_stored, lp_draw, "loader-late-release-fault") != \
            (_loader_panel_event(True), 0, True):
        print("LoaderPanel rejected completed writes before a later guard fault")
        return 1
    lp_no_entry = json.loads(json.dumps(lp_stored))
    lp_set(lp_no_entry["helper"]["collection"], "guardEntered", False)
    lp_no_entry["helper"]["collection"]["collectionMutationUnobserved"] = False
    try:
        _replay_loader_panel_fact(lp_no_entry, lp_draw, "loader-writes-without-entry")
    except TraceError:
        pass
    else:
        print("LoaderPanel accepted capture writes without callback entry")
        return 1
    lp_budget_skip = loader_panel_fact()
    skipped_collection = lp_budget_skip["helper"]["collection"]
    lp_set(skipped_collection, "guardEntered", False)
    lp_set(skipped_collection, "guardReturned", False)
    skipped_collection["collectionMutationUnobserved"] = False
    lp_set(skipped_collection, "capDroppedBeforeWrite", 0)
    lp_set(skipped_collection, "capDroppedAfterWrite", 1)
    if _replay_loader_panel_fact(lp_budget_skip, lp_draw, "loader-budget-skip-drop") != \
            (_loader_panel_event(True), 0, False):
        print("LoaderPanel lost legal outer drop mutation after a skipped callback")
        return 1

    # A large nonpanel count divisible by six has a genuinely small wrapped
    # 32-bit-index byte requirement; derive it from raw count, not output.
    lp_large_draw = dict(lp_draw, count=1073741826)
    lp_large = loader_panel_fact(claim=False)
    large_seq = lp_large["helper"]["sequence"]
    h0 = large_seq["hashBeforeCount"]["value"]
    h1 = ((h0 ^ lp_large_draw["count"]) * 16777619) & 0xffffffff
    h2 = ((h1 ^ 1024) * 16777619) & 0xffffffff
    h3 = ((h2 ^ 512) * 16777619) & 0xffffffff
    for name, value in (("hashAfterCount", h1), ("hashBeforeWidth", h1),
                        ("hashAfterWidth", h2), ("hashBeforeHeight", h2),
                        ("hashAfterHeight", h3), ("slotCount", lp_large_draw["count"])):
        lp_set(large_seq, name, value)
    for name in lp_panel_names:
        lp_large["helper"]["panel"][name] = lp_read()
    lp_set(lp_large["helper"]["panel"], "localOrdinal", 0xffffffff)
    large_collection = lp_large["helper"]["collection"]
    for name in ("capDroppedBeforeWrite", "capDroppedAfterWrite"):
        large_collection[name] = lp_read()
    for name, value in (("capCountBeforeWrite", 0), ("capCountAfterWrite", 1),
                        ("ibFillBeforeWrite", 0), ("ibFillAfterWrite", 8)):
        lp_set(large_collection, name, value)
    if _replay_loader_panel_fact(lp_large, lp_large_draw, "loader-wrapped-byte-count") != \
            (_loader_panel_event(False), 0, True):
        print("LoaderPanel did not preserve UINT byte-count arithmetic")
        return 1
    lp_set(large_collection, "ibFillAfterWrite", 9)
    if _replay_loader_panel_fact(lp_large, lp_large_draw, "loader-invalid-byte-count")[1] != 1:
        print("LoaderPanel accepted a byte-count delta unsupported by either index width")
        return 1
    # Version 11 remains readable and cannot claim the new site is supported.
    lp_v11 = {"kind": ord("X"), "count": 30, "instances": 1,
              "sites": [_loader_panel_event(True)], "predicateFacts": []}
    if _replay_predicate_facts(lp_v11, "loader-v11-compat", 11)["loaderPanelFacts"]:
        print("predicate fact version 11 treated site25 as supported")
        return 1

    def dump_read(value=None):
        return {"reached": True, "known": True, "value": value}

    def dump_unread():
        return {"reached": False, "known": False, "value": None}

    def dump_wants(enabled=True):
        if enabled:
            return {"frame": dump_read(1), "done": dump_read(False),
                    "seriesWant": dump_unread(), "seriesDone": dump_unread()}
        return {"frame": dump_read(0), "done": dump_unread(),
                "seriesWant": dump_read(0), "seriesDone": dump_unread()}

    def dump_fact(shader_hash=0x7E38A6AA1269C901, counter_before=0,
                  counter_after=None, dumping=True, release_ok=True,
                  outer_wants=True, helper_wants=True):
        if counter_after is None:
            counter_after = (counter_before + 1) & 0xff
        helper = {"wants": dump_wants(helper_wants),
                  "contextNonNull": dump_read(True) if helper_wants else dump_unread(),
                  "guardCallReached": dump_unread(), "callbackEntered": dump_unread(),
                  "vsGetShaderReached": dump_unread(), "vsGetShaderCompleted": dump_unread(),
                  "shaderNonNull": dump_unread(), "lookupReached": dump_unread(),
                  "lookupCompleted": dump_unread(), "lookupHash": dump_unread(),
                  "releaseReached": dump_unread(), "releaseCompleted": dump_unread(),
                  "callbackCompleted": dump_unread(), "guardReturned": dump_unread(),
                  "hashAfterGuard": dump_unread(), "dumping": dump_unread(),
                  "counters": {name: dump_unread() for name in
                               ("ringBefore", "ringAfter", "compositeBefore", "compositeAfter",
                                "tonemapBefore", "tonemapAfter")},
                  "pendingKindBefore": dump_unread(), "pendingKindAfter": dump_unread(),
                  "pendingEyeBefore": dump_unread(), "pendingEyeAfter": dump_unread()}
        outer = {"wants": dump_wants(outer_wants),
                 "bodyFrame": dump_read(1) if outer_wants else dump_unread(),
                 "frameNo": dump_read(2) if outer_wants else dump_unread()}
        fact = {"siteId": 59, "kind": 20, "known": "yes", "handlerInvoked": True,
                "outer": outer, "helper": helper}
        if outer_wants and helper_wants:
            helper.update({"guardCallReached": dump_read(True),
                           "callbackEntered": dump_read(True),
                           "vsGetShaderReached": dump_read(True),
                           "vsGetShaderCompleted": dump_read(True),
                           "shaderNonNull": dump_read(True),
                           "lookupReached": dump_read(True),
                           "lookupCompleted": dump_read(True),
                           "lookupHash": dump_read(shader_hash),
                           "releaseReached": dump_read(True),
                           "releaseCompleted": dump_read(release_ok),
                           "callbackCompleted": dump_read(release_ok),
                           "guardReturned": dump_read(release_ok),
                           "hashAfterGuard": dump_read(shader_hash),
                           "dumping": dump_read(dumping) if counter_after <= 2 else dump_unread()})
            counters = helper["counters"]
            counters["ringBefore"] = dump_read(counter_before)
            counters["ringAfter"] = dump_read(counter_after)
            if dumping and counter_after <= 2:
                helper.update({"pendingKindBefore": dump_read(8),
                               "pendingKindAfter": dump_read(0),
                               "pendingEyeBefore": dump_read(7),
                               "pendingEyeAfter": dump_read((counter_after - 1) & 0xffffffff)})
        return fact

    dump_draw = {"kind": ord("N"), "count": 4, "instances": 1}
    dump_claim = dump_fact()
    if _replay_fss_dump_fact(dump_claim, dump_draw, "fss-dump-claim", True)[0] != \
            _fss_dump_event(True):
        print("FSS dump raw category did not replay a claim")
        return 1
    dump_release_fault = dump_fact(release_ok=False)
    # The real helper calls lookupShaderHash even after a successful null VS
    # getter; only Release is conditional on pointer presence.
    dump_null = json.loads(json.dumps(dump_claim))
    null_helper = dump_null["helper"]
    null_helper["shaderNonNull"] = dump_read(False)
    null_helper["lookupHash"] = dump_read(0)
    null_helper["hashAfterGuard"] = dump_read(0)
    for name in ("releaseReached", "releaseCompleted", "dumping", "pendingKindBefore",
                 "pendingKindAfter", "pendingEyeBefore", "pendingEyeAfter"):
        null_helper[name] = dump_unread()
    null_helper["counters"] = {name: dump_unread() for name in null_helper["counters"]}
    if _replay_fss_dump_fact(dump_null, dump_draw, "fss-dump-real-null-shader", True)[0] != \
            _fss_dump_event(False):
        print("FSS dump did not retain the consumed null-shader hash lookup")
        return 1
    dump_missing_lookup = json.loads(json.dumps(dump_null))
    dump_missing_lookup["helper"]["lookupReached"] = dump_unread()
    dump_forged_hash = json.loads(json.dumps(dump_null))
    dump_forged_hash["helper"]["hashAfterGuard"] = dump_read(0x7E38A6AA1269C901)
    dump_nonzero_null_lookup = json.loads(json.dumps(dump_null))
    dump_nonzero_null_lookup["helper"]["lookupHash"] = dump_read(1)
    for bad_dump in (dump_missing_lookup, dump_forged_hash, dump_nonzero_null_lookup):
        try:
            _replay_fss_dump_fact(bad_dump, dump_draw, "fss-dump-null-provenance", True)
        except TraceError:
            pass
        else:
            print("FSS dump accepted missing lookup or manufactured post-guard hash")
            return 1
    dump_unknown_null_lookup = json.loads(json.dumps(dump_null))
    dump_unknown_null_lookup["helper"]["lookupHash"] = {
        "reached": True, "known": False, "value": None}
    try:
        _replay_fss_dump_fact(dump_unknown_null_lookup, dump_draw,
                              "fss-dump-unknown-null-lookup", True)
    except _FssDumpUnavailable:
        pass
    else:
        print("FSS dump manufactured zero from an unknown consumed null lookup")
        return 1
    dump_denied = json.loads(json.dumps(dump_null))
    denied_helper = dump_denied["helper"]
    denied_helper["callbackEntered"] = dump_read(False)
    denied_helper["guardReturned"] = dump_read(False)
    for name in ("vsGetShaderReached", "vsGetShaderCompleted", "shaderNonNull",
                 "lookupReached", "lookupCompleted", "lookupHash", "callbackCompleted"):
        denied_helper[name] = dump_unread()
    if _replay_fss_dump_fact(dump_denied, dump_draw, "fss-dump-real-budget-denial", True)[0] != \
            _fss_dump_event(False):
        print("FSS dump rejected the real unused callback checkpoints on budget denial")
        return 1
    dump_getter_fault = json.loads(json.dumps(dump_null))
    fault_helper = dump_getter_fault["helper"]
    for name in ("vsGetShaderCompleted", "callbackCompleted", "guardReturned"):
        fault_helper[name] = dump_read(False)
    for name in ("shaderNonNull", "lookupReached", "lookupCompleted", "lookupHash"):
        fault_helper[name] = dump_unread()
    if _replay_fss_dump_fact(dump_getter_fault, dump_draw, "fss-dump-real-getter-fault", True)[0] != \
            _fss_dump_event(False):
        print("FSS dump rejected a real getter fault prefix")
        return 1
    if _replay_fss_dump_fact(dump_release_fault, dump_draw, "fss-dump-release-fault", True)[0] != \
            _fss_dump_event(True):
        print("FSS dump lost the raw hash assignment across a Release fault")
        return 1
    dump_wrap = dump_fact(counter_before=255, counter_after=0)
    if _replay_fss_dump_fact(dump_wrap, dump_draw, "fss-dump-counter-wrap", True)[0] != \
            _fss_dump_event(True) or dump_wrap["helper"]["pendingEyeAfter"]["value"] != 0xffffffff:
        print("FSS dump did not preserve uint8 counter wrap / UINT32 pending-eye arithmetic")
        return 1
    dump_third = dump_fact(counter_before=2, counter_after=3)
    if _replay_fss_dump_fact(dump_third, dump_draw, "fss-dump-third-occurrence", True)[0] != \
            _fss_dump_event(False):
        print("FSS dump did not decline the third category occurrence")
        return 1
    dump_off = dump_fact(dumping=False)
    if _replay_fss_dump_fact(dump_off, dump_draw, "fss-dump-disabled", True)[0] != \
            _fss_dump_event(False):
        print("FSS dump did not decline after the counter write when dumping is disabled")
        return 1
    dump_mismatch = dump_fact(shader_hash=0x7E38A6AA1269C901)
    dump_mismatch["helper"]["counters"]["ringAfter"] = dump_read(2)
    try:
        _replay_fss_dump_fact(dump_mismatch, dump_draw, "fss-dump-counter-delta", True)
    except TraceError:
        pass
    else:
        print("FSS dump accepted a counter delta that does not match the raw category")
        return 1
    dump_unknown = dump_fact()
    dump_unknown["outer"]["wants"]["frame"] = {
        "reached": True, "known": False, "value": None}
    try:
        _replay_fss_dump_fact(dump_unknown, dump_draw, "fss-dump-unknown-gate", True)
    except _FssDumpUnavailable:
        pass
    else:
        print("FSS dump converted an unknown lazy gate into a known selector result")
        return 1
    dump_cached = dump_fact()
    dump_cached["helper"]["helperReturnedClaim"] = dump_read(True)
    try:
        _replay_fss_dump_fact(dump_cached, dump_draw, "fss-dump-cached-result", True)
    except TraceError:
        pass
    else:
        print("FSS dump accepted a serialized helper-result oracle")
        return 1

    dump_no_interest = dump_fact(outer_wants=False)
    dump_no_interest["handlerInvoked"] = False
    for group in (dump_no_interest["outer"], dump_no_interest["helper"]):
        for key, item in group.items():
            if key == "wants":
                continue
            if isinstance(item, dict) and set(item) == {"reached", "known", "value"}:
                group[key] = dump_unread()
    dump_no_interest["outer"]["wants"] = {
        "frame": dump_unread(), "done": dump_unread(),
        "seriesWant": dump_unread(), "seriesDone": dump_unread()}
    dump_no_interest["helper"]["wants"] = {
        "frame": dump_unread(), "done": dump_unread(),
        "seriesWant": dump_unread(), "seriesDone": dump_unread()}
    if _replay_fss_dump_fact(dump_no_interest, dump_draw, "fss-dump-not-eligible", False)[0] != \
            {"id": 59, "kind": 2, "outcome": 5, "flow": 0,
             "subsite": 0, "verdict": -1}:
        print("FSS dump not-eligible fact did not replay the raw interest mask")
        return 1

    dump_site6 = {"siteId": 6, "kind": 4, "known": "yes",
                  "interestMaskKnown": "yes", "legacyInterestMask": "%016X" % (1 << 4),
                  "helperReached": "no", "hiddenKnown": "yes", "hidden": "no",
                  "contextKnown": "yes", "contextValid": "no",
                  "shapeReached": "unknown", "shapeMatched": "unknown",
                  "hashKnown": "unknown", "hashSource": 0,
                  "vsHash": "0000000000000000", "skippedDeltaKnown": False,
                  "skippedDelta": 0}
    dump_dispatch = {"kind": ord("N"), "count": 4, "instances": 1,
                     "sites": [{"id": 6, "kind": 2, "outcome": 5, "flow": 0,
                                "subsite": 0, "verdict": -1}, _fss_dump_event(True)],
                     "predicateFacts": [dump_site6, dump_claim]}
    dump_summary = _replay_predicate_facts(dump_dispatch, "fss-dump-dispatch", 13)
    if dump_summary["fssDumpReplayed"] != 1 or dump_summary["fssDumpFacts"] != 1:
        print("FSS dump aggregate did not derive site59 admission from site6 raw mask")
        return 1
    dump_excluded_dispatch = json.loads(json.dumps(dump_dispatch))
    dump_excluded_dispatch["predicateFacts"][0]["legacyInterestMask"] = "0000000000000000"
    dump_excluded_dispatch["predicateFacts"][1] = dump_no_interest
    dump_excluded_dispatch["sites"][1] = {
        "id": 59, "kind": 2, "outcome": 5, "flow": 0, "subsite": 0, "verdict": -1}
    excluded_summary = _replay_predicate_facts(dump_excluded_dispatch,
                                              "fss-dump-production-not-eligible", 13)
    if (excluded_summary["fssDumpReplayed"] != 1 or
            excluded_summary["fssDumpUnreplayable"] != 0 or
            excluded_summary["mismatches"] != 0 or excluded_summary["mutationUnobserved"] != 0):
        print("FSS dump aggregate changed raw-mask exclusion into a handler decline")
        return 1
    # Only independently established source mutation phases may add a warning.
    for subgroup, field, warning in (("counters", "ringBefore", True),
                                     ("counters", "ringAfter", True),
                                     (None, "pendingKindBefore", True),
                                     (None, "pendingEyeAfter", True),
                                     (None, "lookupHash", False)):
        unavailable_dispatch = json.loads(json.dumps(dump_dispatch))
        unavailable_helper = unavailable_dispatch["predicateFacts"][1]["helper"]
        destination = unavailable_helper[subgroup] if subgroup else unavailable_helper
        destination[field] = {"reached": True, "known": False, "value": None}
        unavailable_metrics = _replay_predicate_facts(unavailable_dispatch,
                                                     "fss-dump-phase-unknown", 13)
        if (unavailable_metrics["fssDumpUnreplayable"] != 1 or
                unavailable_metrics["unreplayable"] != 1 or
                unavailable_metrics["mutationUnobserved"] != int(warning) or
                unavailable_metrics["mismatches"] != 0):
            print("FSS dump unknown ledger lost its independently established mutation phase")
            return 1
    outer_unknown_dispatch = json.loads(json.dumps(dump_dispatch))
    outer_unknown_dispatch["predicateFacts"][1]["outer"]["wants"]["frame"] = {
        "reached": True, "known": False, "value": None}
    outer_unknown_metrics = _replay_predicate_facts(outer_unknown_dispatch,
                                                   "fss-dump-unknown-outer-phase", 13)
    if (outer_unknown_metrics["unreplayable"] != 1 or
            outer_unknown_metrics["mutationUnobserved"] != 0 or
            outer_unknown_metrics["mismatches"] != 0):
        print("FSS dump unknown outer selector manufactured a mutation warning")
        return 1
    missing_counter = json.loads(json.dumps(dump_claim))
    missing_counter["helper"]["counters"]["ringAfter"] = dump_unread()
    try:
        _replay_fss_dump_fact(missing_counter, dump_draw, "fss-dump-missing-counter", True)
    except TraceError:
        pass
    else:
        print("FSS dump treated a missing consumed counter checkpoint as mere unavailability")
        return 1

    # Schema12 remains supported and initializes all schema13 metrics in empty
    # and bypass traces; version13 adds one bounded fact slot for site59.
    if _replay_predicate_facts({"kind": ord("D"), "count": 0, "instances": 1,
                                "sites": [], "predicateFacts": []},
                               "fss-dump-v12", 12)["fssDumpFacts"] != 0:
        print("predicate fact version 12 claimed schema13 FSS dump support")
        return 1

    def target_sharp_positive():
        raw_names = ("outerSharp", "outerFailed", "helperSharp", "helperFailed",
                     "srv0Present", "srv0Resolved", "srv0Texture2D", "srv0Width",
                     "srv0Height", "aux1Present", "aux2Present", "aux3Present",
                     "vsPresent", "queriedShaderHash", "configuredShaderHash")
        values = {"outerSharp": True, "outerFailed": False,
                  "helperSharp": True, "helperFailed": False,
                  "srv0Present": True, "srv0Resolved": True,
                  "srv0Texture2D": True, "srv0Width": 2048,
                  "srv0Height": 1024, "aux1Present": False,
                  "aux2Present": False, "aux3Present": False,
                  "vsPresent": True, "queriedShaderHash": 0,
                  "configuredShaderHash": 0}
        return {"siteId": 54, "kind": 21, "known": "yes",
                "handlerInvoked": True,
                "inputs": {name: {"reached": True, "known": True,
                                   "value": values[name]} for name in raw_names},
                "eyeSize": {"reached": "yes", "statePresent": "no",
                            "result": "no", "readMask": 0,
                            "depthW": 0, "depthH": 0, "eyeW": 0,
                            "eyeH": 0, "renderW": 0, "renderH": 0}}

    target_draw = {"kind": ord("X"), "count": 6, "instances": 1}
    target = target_sharp_positive()
    target_claim, target_eye_mismatch = _target_sharp_fact(
        target, target_draw, "targetsharp-positive", True)
    if target_claim is not True or target_eye_mismatch:
        print("TargetSharp positive raw inputs did not derive the zero-hash claim")
        return 1
    target_site6 = json.loads(json.dumps(dump_site6))
    target_site6["legacyInterestMask"] = "%016X" % 1
    target_site6["interestMaskKnown"] = "yes"
    target_claim_event = {"id": 54, "kind": 2, "outcome": 3,
                          "flow": 1, "subsite": 0, "verdict": 5}
    target_dispatch = {"kind": ord("X"), "count": 6, "instances": 1,
                       "sites": [{"id": 6, "kind": 2, "outcome": 5,
                                  "flow": 0, "subsite": 0, "verdict": -1},
                                 target_claim_event],
                       "predicateFacts": [target_site6, target]}
    target_summary = _replay_predicate_facts(target_dispatch,
                                            "targetsharp-v14-positive", 14)
    if (target_summary["targetSharpFacts"] != 1 or
            target_summary["targetSharpReplayed"] != 1 or
            target_summary["targetSharpMismatches"] or
            target_summary["targetSharpUnreplayable"] or
            target_summary["mismatches"]):
        print("TargetSharp v14 did not replay admission and claim from raw inputs")
        return 1
    lazy_decline = {"siteId": 54, "kind": 21, "known": "yes",
                    "handlerInvoked": True, "inputs": {
                        name: {"reached": False, "known": False, "value": None}
                        for name in target["inputs"]},
                    "eyeSize": {"reached": "unknown", "statePresent": "unknown",
                                "result": "unknown", "readMask": 0,
                                "depthW": 0, "depthH": 0, "eyeW": 0,
                                "eyeH": 0, "renderW": 0, "renderH": 0}}
    lazy_decline["inputs"]["outerSharp"] = {"reached": True, "known": True,
                                               "value": True}
    lazy_decline["inputs"]["outerFailed"] = {"reached": True, "known": True,
                                                "value": True}
    if _target_sharp_fact(lazy_decline, target_draw,
                          "targetsharp-lazy-decline", True) != (False, False):
        print("TargetSharp lazy failed-gate decline did not preserve raw short-circuit order")
        return 1
    target_lazy_dispatch = json.loads(json.dumps(target_dispatch))
    target_lazy_dispatch["sites"][1] = {"id": 54, "kind": 2, "outcome": 2,
                                        "flow": 0, "subsite": 0, "verdict": -1}
    target_lazy_dispatch["predicateFacts"][1] = lazy_decline
    target_lazy_summary = _replay_predicate_facts(
        target_lazy_dispatch, "targetsharp-v14-lazy-decline", 14)
    if (target_lazy_summary["targetSharpReplayed"] != 1 or
            target_lazy_summary["targetSharpMismatches"] or
            target_lazy_summary["mismatches"]):
        print("TargetSharp lazy failed-gate did not replay its Declined event")
        return 1
    target_noteligible = {"siteId": 54, "kind": 21, "known": "yes",
                          "handlerInvoked": False,
                          "inputs": {name: {"reached": False, "known": False,
                                             "value": None} for name in target["inputs"]},
                          "eyeSize": {"reached": "unknown", "statePresent": "unknown",
                                      "result": "unknown", "readMask": 0,
                                      "depthW": 0, "depthH": 0, "eyeW": 0,
                                      "eyeH": 0, "renderW": 0, "renderH": 0}}
    if _target_sharp_fact(target_noteligible, target_draw,
                          "targetsharp-noteligible", False) != (None, False):
        print("TargetSharp not-eligible observation was not independently represented")
        return 1
    target_off_site6 = json.loads(json.dumps(target_site6))
    target_off_site6["legacyInterestMask"] = "%016X" % 0
    target_off_event = {"id": 54, "kind": 2, "outcome": 5,
                        "flow": 0, "subsite": 0, "verdict": -1}
    target_off_dispatch = {"kind": ord("X"), "count": 6, "instances": 1,
                           "sites": [target_dispatch["sites"][0], target_off_event],
                           "predicateFacts": [target_off_site6, target_noteligible]}
    target_off_summary = _replay_predicate_facts(
        target_off_dispatch, "targetsharp-v14-noteligible", 14)
    if (target_off_summary["targetSharpFacts"] != 1 or
            target_off_summary["targetSharpNotEligible"] != 1 or
            target_off_summary["targetSharpReplayed"] != 1 or
            target_off_summary["mismatches"]):
        print("TargetSharp v14 not-eligible trace lost independent mask evidence")
        return 1
    bad_noteligible = json.loads(json.dumps(target_noteligible))
    bad_noteligible["inputs"]["outerSharp"] = {
        "reached": False, "known": True, "value": None}
    try:
        _target_sharp_fact(bad_noteligible, target_draw,
                           "targetsharp-noteligible-malformed", False)
    except TraceError:
        pass
    else:
        print("TargetSharp NotEligible accepted malformed unread-read envelope")
        return 1
    bad_skipped_eye = json.loads(json.dumps(lazy_decline))
    bad_skipped_eye["inputs"]["outerSharp"]["value"] = False
    bad_skipped_eye["inputs"]["outerFailed"] = {
        "reached": False, "known": False, "value": None}
    bad_skipped_eye["eyeSize"]["depthW"] = 8
    try:
        _target_sharp_fact(bad_skipped_eye, target_draw,
                           "targetsharp-skipped-eye-mutant", True)
    except TraceError:
        pass
    else:
        print("TargetSharp lazy decline accepted consumed dimensions in skipped eye helper")
        return 1
    unknown_target = json.loads(json.dumps(lazy_decline))
    for name in unknown_target["inputs"]:
        unknown_target["inputs"][name] = {
            "reached": False, "known": False, "value": None}
    unknown_target["inputs"]["outerSharp"] = {
        "reached": True, "known": False, "value": None}
    missing_outer_target = json.loads(json.dumps(unknown_target))
    missing_outer_target["inputs"]["outerSharp"]["reached"] = False
    try:
        _target_sharp_fact(missing_outer_target, target_draw,
                           "targetsharp-missing-first-read", True)
    except TraceError:
        pass
    else:
        print("TargetSharp invoked handler accepted an unconsumed first read")
        return 1
    if _target_sharp_fact(unknown_target, target_draw,
                          "targetsharp-unknown", True) != (None, False):
        print("TargetSharp reached-unknown input became a selector result")
        return 1
    target_unknown_dispatch = json.loads(json.dumps(target_lazy_dispatch))
    target_unknown_dispatch["predicateFacts"][1] = unknown_target
    unknown_summary = _replay_predicate_facts(
        target_unknown_dispatch, "targetsharp-v14-unknown", 14)
    if (unknown_summary["targetSharpUnreplayable"] != 1 or
            unknown_summary["targetSharpReplayed"] or
            unknown_summary["targetSharpMismatches"]):
        print("TargetSharp reached-unknown input was not reported as unavailable")
        return 1
    target_mutations = []
    shape_miss = json.loads(json.dumps(target))
    shape_miss_draw = {"kind": ord("N"), "count": 6, "instances": 1}
    for name in target["inputs"]:
        shape_miss["inputs"][name] = {"reached": False, "known": False,
                                       "value": None}
    shape_miss["eyeSize"] = {"reached": "unknown", "statePresent": "unknown",
                              "result": "unknown", "readMask": 0,
                              "depthW": 0, "depthH": 0, "eyeW": 0,
                              "eyeH": 0, "renderW": 0, "renderH": 0}
    for name, value in (("outerSharp", True), ("outerFailed", False),
                        ("helperSharp", True), ("helperFailed", False)):
        shape_miss["inputs"][name] = {"reached": True, "known": True,
                                      "value": value}
    target_mutations.append((shape_miss, shape_miss_draw))
    for name, value in (("outerSharp", False), ("outerFailed", True),
                        ("helperSharp", False), ("helperFailed", True),
                        ("srv0Present", False), ("srv0Resolved", False),
                        ("srv0Texture2D", False), ("aux1Present", True),
                        ("aux2Present", True), ("aux3Present", True),
                        ("vsPresent", False), ("queriedShaderHash", 1),
                        ("configuredShaderHash", 1)):
        mutant = json.loads(json.dumps(target))
        mutant["inputs"][name]["value"] = value
        target_mutations.append((mutant, target_draw))
    eye_mutant = json.loads(json.dumps(target))
    eye_mutant["eyeSize"]["result"] = "yes"
    target_mutations.append((eye_mutant, target_draw))
    dimension_mutant = json.loads(json.dumps(target))
    dimension_mutant["eyeSize"].update({"reached": "yes", "statePresent": "yes",
                                       "result": "no", "readMask": 31,
                                       "depthW": 2048, "depthH": 1024,
                                       "eyeW": 1920, "eyeH": 0,
                                       "renderW": 1920, "renderH": 0})
    dimension_mutant["inputs"]["srv0Width"]["value"] = 1920
    dimension_mutant["eyeSize"].update({"result": "yes", "readMask": 15,
                                        "depthW": 1920, "eyeW": 1920,
                                        "eyeH": 1024})
    for name in ("aux1Present", "aux2Present", "aux3Present", "vsPresent",
                 "queriedShaderHash", "configuredShaderHash"):
        dimension_mutant["inputs"][name] = {"reached": False, "known": False,
                                              "value": None}
    target_mutations.append((dimension_mutant, target_draw))
    for mutant, mutant_draw in target_mutations:
        try:
            derived, eye_mismatch = _target_sharp_fact(
                mutant, mutant_draw, "targetsharp-mutant", True)
        except TraceError:
            continue
        if derived is True and not eye_mismatch:
            print("TargetSharp mutation retained a claimed selector without evidence")
            return 1
    try:
        null_resolve = json.loads(json.dumps(target))
        null_resolve["inputs"]["srv0Present"]["value"] = False
        _target_sharp_fact(null_resolve, target_draw, "targetsharp-null-resolve", True)
    except TraceError:
        pass
    else:
        print("TargetSharp accepted a resolved null SRV0")
        return 1
    for old_version in range(1, 14):
        if _replay_predicate_facts({"kind": ord("D"), "count": 0,
                                    "instances": 1, "sites": [],
                                    "predicateFacts": []},
                                   "targetsharp-legacy", old_version).get("targetSharpFacts") != 0:
            print("legacy predicate schema claimed TargetSharp availability")
            return 1
    nominal_draw = {"kind": ord("X"), "count": 10001, "instances": 1}
    nomination = {
        "siteId": 45, "kind": 22, "known": "yes",
        "inputs": {
            "worldMode": {"reached": True, "known": True, "value": 1},
            "boundCbIdentity": {"reached": True, "known": True, "value": 2},
            "nominatedBeforeIdentity": {"reached": True, "known": True, "value": 1},
            "resourceResolved": {"reached": True, "known": True, "value": True},
            "isBuffer": {"reached": True, "known": True, "value": True},
            "byteWidth": {"reached": True, "known": True, "value": 208},
            "callbackInvoked": {"reached": True, "known": True, "value": True},
            "nominatedAfterIdentity": {"reached": True, "known": True, "value": 2},
            "callbackTargetAfterIdentity": {"reached": True, "known": True, "value": 2},
        },
    }
    nomination_event, nomination_mismatch, nomination_unavailable = \
        _sunglare_nomination_fact(nomination, nominal_draw, "sungnom-positive")
    if nomination_event["id"] != 45 or nomination_mismatch or nomination_unavailable:
        print("Sunglare nomination positive raw inputs did not replay")
        return 1
    nomination_trace_draw = dict(nominal_draw, sites=[nomination_event],
                                predicateFacts=[nomination])
    nomination_summary = _replay_predicate_facts(
        nomination_trace_draw, "sungnom-trace", 15)
    if (nomination_summary["sunglareNominationFacts"] != 1 or
            nomination_summary["sunglareNominationReplayed"] != 1 or
            nomination_summary["sunglareNominationMismatches"]):
        print("Sunglare nomination schema15 fact did not join the observed site")
        return 1
    empty_v15 = _fixture()
    empty_v15["predicateFactVersion"] = 15
    empty_v15["draws"] = []
    empty_v15["footer"]["drawCount"] = 0
    try:
        empty_v15_summary = validate_trace(empty_v15)
    except TraceError as exc:
        print("Sunglare nomination empty schema15 trace failed: %s" % exc)
        return 1
    if empty_v15_summary["predicateReplay"]["sunglareNominationFacts"] != 0:
        print("Sunglare nomination empty schema15 trace reported unexpected facts")
        return 1
    cutoff = json.loads(json.dumps(nomination))
    cutoff_draw = dict(nominal_draw, count=10000)
    for name in ("boundCbIdentity", "nominatedBeforeIdentity", "resourceResolved",
                 "isBuffer", "byteWidth", "nominatedAfterIdentity", "callbackTargetAfterIdentity"):
        cutoff["inputs"][name] = {"reached": False, "known": False, "value": None}
    cutoff["inputs"]["callbackInvoked"] = {"reached": True, "known": True, "value": False}
    _sunglare_nomination_fact(cutoff, cutoff_draw, "sungnom-cutoff")
    malformed_cutoff = json.loads(json.dumps(cutoff))
    malformed_cutoff["inputs"]["resourceResolved"] = {"reached": True, "known": False, "value": None}
    try:
        _sunglare_nomination_fact(malformed_cutoff, cutoff_draw, "sungnom-bad-cutoff")
    except TraceError:
        pass
    else:
        print("Sunglare nomination cutoff accepted a consumed unknown suffix")
        return 1
    unknown_mode = json.loads(json.dumps(cutoff))
    unknown_mode_draw = dict(nominal_draw)
    unknown_mode["inputs"]["worldMode"] = {"reached": True, "known": False, "value": None}
    _, _, unknown_unavailable = _sunglare_nomination_fact(
        unknown_mode, unknown_mode_draw, "sungnom-unknown")
    if not unknown_unavailable:
        print("Sunglare nomination unknown mode was guessed")
        return 1
    for malformed_prefix in (unknown_mode, nomination):
        mutant = json.loads(json.dumps(malformed_prefix))
        if malformed_prefix is unknown_mode:
            mutant["inputs"]["resourceResolved"] = {
                "reached": True, "known": True, "value": True}
        else:
            mutant["inputs"]["nominatedBeforeIdentity"] = {
                "reached": True, "known": False, "value": None}
            mutant["inputs"]["resourceResolved"] = {
                "reached": False, "known": False, "value": None}
            mutant["inputs"]["isBuffer"] = {
                "reached": True, "known": True, "value": False}
        try:
            _sunglare_nomination_fact(mutant, nominal_draw, "sungnom-orphan-read")
        except TraceError:
            continue
        print("Sunglare nomination accepted a read without its lazy predecessor")
        return 1
    unknown_event = {"id": 45, "kind": 1, "outcome": 1, "flow": 0,
                     "subsite": 0, "verdict": -1}
    unknown_summary = _replay_predicate_facts(
        dict(unknown_mode_draw, sites=[unknown_event], predicateFacts=[unknown_mode]),
        "sungnom-unknown-trace", 15)
    if (unknown_summary["sunglareNominationUnreplayable"] != 1 or
            unknown_summary["sunglareNominationReplayed"]):
        print("Sunglare nomination reached-unknown mode was not reported unavailable")
        return 1

    chrome_draw_input = {"kind": ord("X"), "count": 6}
    chrome_positive = _fss_chrome_skip_fact_fixture(chrome_draw_input)
    chrome_event, chrome_mismatches = _legacy14a_fss_chrome_skip(
        chrome_positive, chrome_draw_input, "fss-chrome-positive")
    if chrome_event["verdict"] != 2 or chrome_mismatches:
        print("FSS chrome positive did not replay the terminal Skip")
        return 1
    second_hash_fact = _fss_chrome_skip_fact_fixture(
        chrome_draw_input, vsHash=0xB018D143700AB803)
    second_hash_event, _ = _legacy14a_fss_chrome_skip(
        second_hash_fact, chrome_draw_input, "fss-chrome-second-hash")
    if second_hash_event["verdict"] != 2:
        print("FSS chrome second frozen hash was not accepted")
        return 1
    for gate_case, changes in (
            ("census", {"outerHeal": False, "outerCensus": True}),
            ("temporal", {"outerHeal": False, "outerCensus": False,
                          "outerTemporal": True})):
        fact = _fss_chrome_skip_fact_fixture(chrome_draw_input, **changes)
        event, _ = _legacy14a_fss_chrome_skip(
            fact, chrome_draw_input, "fss-chrome-outer-" + gate_case)
        if event["verdict"] != 2:
            print("FSS chrome outer %s gate did not admit the selector" % gate_case)
            return 1
    entered_fault_fact = _fss_chrome_skip_fact_fixture(
        chrome_draw_input, budgetResult=False)
    entered_fault_event, _ = _legacy14a_fss_chrome_skip(
        entered_fault_fact, chrome_draw_input, "fss-chrome-caught-fault-after-match")
    if (entered_fault_fact["budgetEntered"] != "yes" or
            entered_fault_fact["budgetResult"] != "no" or
            entered_fault_event["verdict"] != 2):
        print("FSS chrome guarded-budget entry was conflated with its caught-fault result")
        return 1
    for ordinal in range(32):
        fact = _fss_chrome_skip_fact_fixture(
            chrome_draw_input, frameNo=12, priorFrameNo=12,
            ordinal=ordinal, mask=1 << ordinal)
        event, _ = _legacy14a_fss_chrome_skip(
            fact, chrome_draw_input, "fss-chrome-ordinal-%d" % ordinal)
        if event["verdict"] != 2:
            print("FSS chrome ordinal %d did not select the corresponding mask bit" % ordinal)
            return 1
    chrome_negative_inputs = [
        ("outer gate", {"outerHeal": False, "outerCensus": False, "outerTemporal": False}, chrome_draw_input),
        ("kind", {"drawKind": ord("N")}, dict(chrome_draw_input, kind=ord("N"))),
        ("count", {"drawCount": 5}, dict(chrome_draw_input, count=5)),
        ("hash A miss", {"vsHash": 0}, chrome_draw_input),
        ("hash B miss", {"vsHash": 0x1111111111111111}, chrome_draw_input),
        ("budget declined before lambda", {"budgetEntered": False,
                                             "budgetResult": False}, chrome_draw_input),
        ("null SRV", {"srvNonNull": False}, chrome_draw_input),
        ("missing resource", {"resourceNonNull": False}, chrome_draw_input),
        ("failed QI", {"texture2D": False}, chrome_draw_input),
        ("width 1999", {"width": 1999}, chrome_draw_input),
        ("height 999", {"height": 999}, chrome_draw_input),
        ("equal dimensions", {"width": 2000, "height": 2000}, chrome_draw_input),
        ("heal off", {"healForSkip": False}, chrome_draw_input),
        ("latch off", {"latchOn": False}, chrome_draw_input),
        ("ordinal 32", {"frameNo": 12, "priorFrameNo": 12,
                         "ordinal": 32, "mask": 0xffffffff}, chrome_draw_input),
        ("mask clear", {"mask": 0}, chrome_draw_input),
    ]
    for case_name, changes, case_draw in chrome_negative_inputs:
        fact = _fss_chrome_skip_fact_fixture(case_draw, **changes)
        event, _ = _legacy14a_fss_chrome_skip(
            fact, case_draw, "fss-chrome-" + case_name)
        if event["verdict"] != -1:
            print("FSS chrome negative control matched unexpectedly: %s" % case_name)
            return 1
    for width, height, expected in ((2000, 1000, 2), (1999, 1000, -1),
                                    (2000, 999, -1), (2000, 2000, -1)):
        fact = _fss_chrome_skip_fact_fixture(
            chrome_draw_input, width=width, height=height)
        event, _ = _legacy14a_fss_chrome_skip(
            fact, chrome_draw_input, "fss-chrome-size-%dx%d" % (width, height))
        if event["verdict"] != expected:
            print("FSS chrome dimension boundary failed: %dx%d" % (width, height))
            return 1

    chrome_terminal_trace = _fixture()
    chrome_terminal_trace["predicateFactVersion"] = 16
    chrome_terminal_draw = chrome_terminal_trace["draws"][0]
    chrome_terminal_draw.update(route=6, sequence=4, count=6, instances=1,
                                winnerSiteId=1, verdict=2,
                                sites=[chrome_event], predicateFacts=[chrome_positive])
    chrome_terminal_draw["args"] = {"start": 19, "base": -7, "startInstance": 3}
    issue_blocked_entry = False
    chrome_terminal_draw["actions"] = _fss_chrome_skip_action_plan(
        chrome_terminal_draw, issue_blocked_entry)
    try:
        _fss_chrome_skip_action_plan(chrome_terminal_draw, True)
    except TraceError:
        pass
    else:
        print("FSS chrome terminal action plan accepted issueBlockedEntry true")
        return 1
    try:
        chrome_terminal_summary = validate_trace(chrome_terminal_trace)
    except TraceError as exc:
        print("FSS chrome terminal/action roundtrip failed: %s" % exc)
        return 1
    chrome_replay_summary = chrome_terminal_summary["predicateReplay"]
    if (chrome_replay_summary["fssChromeSkipStatus"] != "replayed" or
            chrome_replay_summary["fssChromeSkipTerminal"] != 1 or
            chrome_terminal_draw["actions"][1]["id"] != 3 or
            chrome_terminal_draw["actions"][1]["phase"] != 2 or
            chrome_terminal_draw["actions"][1]["outcome"] != 2 or
            chrome_terminal_draw["actions"][1]["issueCount"] != 0 or
            chrome_terminal_draw["actions"][1]["call"] != 4 or
            chrome_terminal_draw["actions"][1]["count"] != 6 or
            chrome_terminal_draw["actions"][1]["instances"] != 1 or
            chrome_terminal_draw["actions"][1]["start"] != 19 or
            chrome_terminal_draw["actions"][1]["baseVertex"] != -7 or
            chrome_terminal_draw["actions"][1]["startInstance"] != 3):
        print("FSS chrome terminal/action plan did not match applied swallowed original")
        return 1

    base_capacity_ids = (1, 3, 6, 49, 50, 53, 55)
    if len(set(base_capacity_ids)) != 7 or PREDICATE_FACT_VERSION != 16:
        print("FSS chrome owner-context decline does not pin the reviewed seven-fact base set")
        return 1

    for field, changes in (
            ("hash", {"vsHash": 0x1111111111111111}),
            ("threshold", {"width": 1999}),
            ("ordinal", {"frameNo": 12, "priorFrameNo": 12,
                         "ordinal": 1, "mask": 1}),
            ("mask", {"mask": 0})):
        mutant = _fss_chrome_skip_fact_fixture(chrome_draw_input, **changes)
        mutant_trace = json.loads(json.dumps(chrome_terminal_trace))
        mutant_trace["draws"][0]["predicateFacts"][0] = mutant
        mutant_summary = validate_trace(mutant_trace)["predicateReplay"]
        if not mutant_summary["mismatches"]:
            print("FSS chrome %s mutation did not mismatch the captured terminal result" % field)
            return 1
    gate_order = json.loads(json.dumps(chrome_positive))
    gate_order["outerCensus"] = "yes"
    try:
        _legacy14a_fss_chrome_skip(gate_order, chrome_draw_input,
                                   "fss-chrome-gate-order")
    except TraceError:
        pass
    else:
        print("FSS chrome gate-order mutation did not fail reachability validation")
        return 1
    terminal_mutation = json.loads(json.dumps(chrome_terminal_trace))
    terminal_mutation["draws"][0]["actions"][1]["id"] = 2
    try:
        validate_trace(terminal_mutation)
    except TraceError:
        pass
    else:
        print("FSS chrome swallowed-action mutation was not detected")
        return 1

    old_v15_site1 = json.loads(json.dumps(chrome_positive))
    try:
        _replay_predicate_facts(dict(chrome_draw_input,
                                     sites=[chrome_event],
                                     predicateFacts=[old_v15_site1]),
                                "fss-chrome-old-v15", 15)
    except TraceError:
        pass
    else:
        print("predicate-fact version 15 accepted the new site-1 pair")
        return 1
    print("draw-ladder-replay self-test: ok")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Validate and summarize EDVR draw-ladder traces.")
    parser.add_argument("--file", help="read one exact draw-ladder sidecar")
    parser.add_argument("--expected-log", help="require this associated graphics-log basename")
    parser.add_argument("--expected-build-stamp", help="require the associated PE build stamp")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate and report only; writes nothing")
    parser.add_argument("--expect-invalid", action="store_true",
                        help="succeed only if this sidecar is rejected (requires --dry-run)")
    parser.add_argument("--expect-unreplayable", type=int, metavar="COUNT",
                        help="assert exactly COUNT unavailable fixture facts with no mismatches or unobserved mutations (requires --dry-run)")
    parser.add_argument("--expect-mutation-unobserved", type=int, metavar="COUNT",
                        help="assert exactly COUNT known LoaderPanel selector mutation warnings with no unavailable predicates or selector mismatches (requires --dry-run)")
    parser.add_argument("--expect-forward-replayed", type=int, metavar="COUNT",
                        help="assert exactly COUNT independently replayed forwarding-local plans and no unavailable plans or mismatches (requires --dry-run)")
    parser.add_argument("--expect-forward-unavailable", type=int, metavar="COUNT",
                        help="assert exactly COUNT unavailable forwarding observations and no mismatches (requires --dry-run)")
    parser.add_argument("--self-test", action="store_true", help="run fixture checks and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.file:
        parser.error("--file is required unless --self-test is used")
    if args.expect_invalid and not args.dry_run:
        parser.error("--expect-invalid requires --dry-run")
    if args.expect_unreplayable is not None:
        if args.expect_unreplayable <= 0:
            parser.error("--expect-unreplayable requires a positive count")
        if not args.dry_run:
            parser.error("--expect-unreplayable requires --dry-run")
        if args.expect_invalid:
            parser.error("--expect-unreplayable cannot be combined with --expect-invalid")
    if args.expect_mutation_unobserved is not None:
        if args.expect_mutation_unobserved <= 0:
            parser.error("--expect-mutation-unobserved requires a positive count")
        if not args.dry_run:
            parser.error("--expect-mutation-unobserved requires --dry-run")
        if args.expect_invalid or args.expect_unreplayable is not None:
            parser.error("--expect-mutation-unobserved cannot be combined with --expect-invalid or --expect-unreplayable")
    if args.expect_forward_replayed is not None or args.expect_forward_unavailable is not None:
        for value, option in ((args.expect_forward_replayed, "--expect-forward-replayed"),
                              (args.expect_forward_unavailable, "--expect-forward-unavailable")):
            if value is not None and value <= 0:
                parser.error(option + " requires a positive count")
        if args.expect_forward_replayed is not None and args.expect_forward_unavailable is not None:
            parser.error("forward replay expectations cannot be combined")
        if not args.dry_run:
            parser.error("forward replay expectations require --dry-run")
        if (args.expect_invalid or args.expect_unreplayable is not None or
                args.expect_mutation_unobserved is not None):
            parser.error("forward replay expectations cannot be combined with other fixture expectations")
    try:
        _, summary = read_trace(args.file, args.expected_log, args.expected_build_stamp)
    except OSError as exc:
        print("[edvr] draw-ladder trace I/O failed: %s" % exc)
        return 1
    except (ValueError, TraceError) as exc:
        if args.expect_invalid:
            print("[edvr] expected rejection confirmed: %s" % exc)
            return 0
        print("[edvr] draw-ladder trace rejected: %s" % exc)
        return 1
    if args.expect_invalid:
        print("[edvr] expected rejection failed: sidecar is valid")
        return 1
    print(format_summary(summary, os.path.abspath(args.file)))
    if args.expect_unreplayable is not None:
        replay = summary["predicateReplay"]
        if (replay["unreplayable"] != args.expect_unreplayable or
                replay["mismatches"] != 0 or replay["mutationUnobserved"] != 0):
            print("[edvr] expected unavailable-fact count or clean consistency checks failed")
            return 1
        print("[edvr] fixture expectation confirmed: exactly %d unreplayable facts; predicate equivalence remains unestablished" % args.expect_unreplayable)
        print("  dry-run: no files or directories were written")
        return 0
    if args.expect_mutation_unobserved is not None:
        replay = summary["predicateReplay"]
        expected_count = args.expect_mutation_unobserved
        if (replay.get("mutationUnobserved") != expected_count or
                replay.get("loaderPanelMutationUnobserved") != expected_count or
                replay.get("unreplayable") != 0 or replay.get("mismatches") != 0 or
                replay.get("loaderPanelUnreplayable") != 0 or
                replay.get("loaderPanelMismatches") != 0 or
                replay.get("loaderPanelReplayed", 0) < expected_count):
            print("[edvr] expected mutation-warning count or known-selector replay checks failed")
            return 1
        print("[edvr] fixture expectation confirmed: exactly %d mutation-unobserved warning(s) on known LoaderPanel selectors; predicate equivalence remains false" % expected_count)
        print("  dry-run: no files or directories were written")
        return 0
    # Only the explicit predicate expectation modes above may accept their
    # declared unavailable facts or mutation warnings. Forwarding assertions
    # must still pass the normal supported-predicate correctness gate.
    gate_failure = predicate_replay_gate_failure(summary)
    if gate_failure:
        print("[edvr] predicate replay gate failed: %s" % gate_failure)
        return 1
    if args.expect_forward_replayed is not None:
        replay = summary["forwardReplay"]
        if (replay["replayed"] != args.expect_forward_replayed or
                replay["unavailable"] != 0 or replay["mismatches"] != 0):
            print("[edvr] expected forwarding replay count or clean local comparisons failed")
            return 1
        print("[edvr] fixture expectation confirmed: exactly %d forwarding-local plans replayed; whole forwarding equivalence remains false" % args.expect_forward_replayed)
        print("  dry-run: no files or directories were written")
        return 0
    if args.expect_forward_unavailable is not None:
        replay = summary["forwardReplay"]
        if (replay["unavailable"] != args.expect_forward_unavailable or
                replay["mismatches"] != 0):
            print("[edvr] expected forwarding unavailable count or clean local comparisons failed")
            return 1
        print("[edvr] fixture expectation confirmed: exactly %d forwarding observation(s) unavailable" % args.expect_forward_unavailable)
        print("  dry-run: no files or directories were written")
        return 0
    if summary["forwardReplay"]["mismatches"]:
        print("[edvr] forwarding replay gate failed: %d local mismatch(es)" %
              summary["forwardReplay"]["mismatches"])
        return 1
    if args.dry_run:
        print("  dry-run: no files or directories were written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
