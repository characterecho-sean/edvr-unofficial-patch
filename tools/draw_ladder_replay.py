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
PREDICATE_FACT_VERSION = 4
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


def _replay_predicate_facts(draw, label, predicate_fact_version=1):
    facts = draw.get("predicateFacts")
    maximum = 4 if predicate_fact_version >= 3 else 3 if predicate_fact_version >= 2 else 2
    if not isinstance(facts, list) or len(facts) > maximum:
        raise TraceError(label + ".predicateFacts must be a bounded array (type %s, count %s)" %
                         (type(facts).__name__, len(facts) if isinstance(facts, list) else "n/a"))
    supported_ids = ((3, 6, 24, 26, 49, 50) if predicate_fact_version >= 4 else
                     (3, 6, 49, 50) if predicate_fact_version >= 3 else
                     (3, 49, 50) if predicate_fact_version >= 2 else (3, 49))
    expected = {event["id"] for event in draw["sites"] if event["id"] in supported_ids}
    by_site = {}
    for index, fact in enumerate(facts):
        fact_label = "%s.predicateFacts[%d]" % (label, index)
        if not isinstance(fact, dict):
            raise TraceError(fact_label + " must be an object")
        site_id = _integer(fact.get("siteId"), fact_label + ".siteId", 1, 76)
        kind = _integer(fact.get("kind"), fact_label + ".kind", 1,
                        6 if predicate_fact_version >= 4 else
                        4 if predicate_fact_version >= 3 else
                        3 if predicate_fact_version >= 2 else 2)
        if site_id in by_site:
            raise TraceError(fact_label + " duplicates a supported site fact")
        supported_pairs = ((3, 1), (6, 4), (24, 5), (26, 6), (49, 2), (50, 3)) if predicate_fact_version >= 4 else (
            (3, 1), (6, 4), (49, 2), (50, 3)) if predicate_fact_version >= 3 else (
            (3, 1), (49, 2), (50, 3)) if predicate_fact_version >= 2 else ((3, 1), (49, 2))
        if (site_id, kind) not in supported_pairs:
            raise TraceError(fact_label + " has an unsupported site/kind pair")
        known = fact.get("known")
        if known not in TRI_STATES:
            raise TraceError(fact_label + ".known is invalid")
        if known == "no":
            raise TraceError(fact_label + ".known must be yes or unknown")
        if kind == 3:
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
        if kind not in (3, 4, 5, 6):
            by_site[site_id] = (expected_event, expected_delta,
                                fact.get("censusSkippedDeltaKnown", True),
                                fact.get("censusSkippedDelta", 0), 0, None)
    if set(by_site) != expected:
        raise TraceError(label + ".predicateFacts do not exactly cover visited supported sites")
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
    for site_id, (expected_event, expected_delta, delta_known,
                  observed_delta, cache_mismatches, legacy_claim) in by_site.items():
        mismatches += cache_mismatches
        site_unreplayable = expected_event is None or (
            site_id in (6, 50) and legacy_claim is None)
        if site_unreplayable:
            unreplayable += 1
        actual = next(event for event in draw["sites"] if event["id"] == site_id)
        if expected_event is not None:
            if any(actual[key] != expected_event[key]
                   for key in ("id", "kind", "outcome", "flow", "subsite", "verdict")):
                mismatches += 1
            else:
                replayed += 1
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
            "offscreenQuadMismatches": quad_mismatches}


def _integer(value, label, low=0, high=0xffffffff):
    if type(value) is not int or value < low or value > high:
        raise TraceError("%s must be an integer in %d..%d" % (label, low, high))
    return value


def validate_trace(data, expected_log=None, expected_build_stamp=None):
    if not isinstance(data, dict):
        raise TraceError("sidecar root must be an object")
    if data.get("format") != FORMAT:
        raise TraceError("unknown trace format")
    if type(data.get("schemaVersion")) is not int or data["schemaVersion"] not in (1, SCHEMA_VERSION):
        raise TraceError("unsupported schemaVersion")
    schema_version = data["schemaVersion"]
    if schema_version == SCHEMA_VERSION:
        if type(data.get("predicateFactVersion")) is not int or data["predicateFactVersion"] not in (1, 2, 3, PREDICATE_FACT_VERSION):
            raise TraceError("unsupported predicateFactVersion")
        predicate_fact_version = data["predicateFactVersion"]
    elif "predicateFactVersion" in data:
        raise TraceError("schemaVersion 1 cannot declare predicate facts")
    else:
        predicate_fact_version = 0
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
                        "offscreenQuadMismatches": 0}
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
            if present & verdict_bit and facts_verdict != verdict:
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
        if facts is not None and facts.get("issueBlocked") == "yes":
            blocked = [action for action in actions
                       if action["id"] == 3 and action["phase"] == 2]
            if (len(blocked) != 1 or blocked[0]["outcome"] != 3 or
                    blocked[0]["issueCount"] != 0 or not blocked[0]["issueCountKnown"]):
                raise TraceError(label + " issueBlocked fact lacks its declined swallow action")

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
             "offscreenQuadUnreplayable": 0, "offscreenQuadMismatches": 0}),
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
        "predicateFactVersion": PREDICATE_FACT_VERSION,
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
        try:
            summary = validate_trace(bypass)
            if scope_text not in summary["scope"]:
                raise TraceError("bypass coverage label is %r" % summary["scope"])
        except Exception as exc:
            print("draw-ladder bypass fixture rejected for kind %r: %s" % (kind, exc))
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
    finally:
        read_trace = original_read_trace
        try:
            os.remove(cli_file)
        except OSError:
            pass
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
    parser.add_argument("--self-test", action="store_true", help="run fixture checks and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.file:
        parser.error("--file is required unless --self-test is used")
    if args.expect_invalid and not args.dry_run:
        parser.error("--expect-invalid requires --dry-run")
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
    gate_failure = predicate_replay_gate_failure(summary)
    if gate_failure:
        print("[edvr] predicate replay gate failed: %s" % gate_failure)
        return 1
    if args.dry_run:
        print("  dry-run: no files or directories were written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
