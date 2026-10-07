"""Run the production proxy's offline flat-AA scenarios in isolated processes.

Actual shader bytes are hash-pinned; draw state and workloads are reconstructed.
Scenario FAIL is a renderer failure, even when the bench's self-test succeeds.
--dry-run creates no directories, files, links, deletes, processes, devices or DLL loads.
Each scenario keeps its own directory, but its DLLs are hard links to the build's (a copy only where the
volume cannot link), and a scenario that PASSes drops them again: a matrix then costs no disk to speak of.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = pathlib.Path("tools/flat_sdk_integration_test/fixtures_manifest.json")
MARKER = "EDVR_BENCH_RESULT "
HISTORY_MARKER = "EDVR_BENCH_HISTORY_RESULT "
MODES = ("taa", "dlaa", "dlss", "fsr")
DEFAULT_CASES = ("unsupported_host", "scene", "inert_no_write", "inert_depth_write", "inert_color_write",
                 "state_partial_mask", "state_blended", "state_blended_no_depth", "state_blended_hdr",
                 "settlement_prepass", "predicted_world_mismatch", "predicted_world_close_scale", "state_blended_hdr_world",
                 "inert_depth_write_world", "stale_foreign_mark", "first_person_slot_repacked", "first_person_pieces",
                 "supersampled_scene", "supersampled_scene_4k")
# "renderer" cases must complete the production HDR resolve; "guard" cases must keep a refusal.
CASE_PURPOSES = {"smoke": "entry", "unsupported_host": "guard", "scene": "renderer", "inert_no_write": "renderer",
                 "inert_depth_write": "guard", "inert_color_write": "renderer", "state_partial_mask": "renderer",
                 "state_blended": "renderer", "state_blended_no_depth": "renderer", "state_blended_hdr": "guard",
                 "settlement_prepass": "renderer", "predicted_world_mismatch": "renderer", "predicted_world_close_scale": "guard",
                 "state_blended_hdr_world": "renderer", "inert_depth_write_world": "renderer",
                 "stale_foreign_mark": "renderer", "first_person_slot_repacked": "renderer", "first_person_pieces": "renderer",
                 "supersampled_scene": "renderer",
                 "supersampled_scene_4k": "renderer"}
SDK_CASES = frozenset(("inert_no_write", "inert_depth_write", "inert_color_write", "state_partial_mask",
                       "state_blended", "state_blended_no_depth", "state_blended_hdr", "settlement_prepass",
                       "predicted_world_mismatch", "predicted_world_close_scale", "state_blended_hdr_world", "inert_depth_write_world",
                       "stale_foreign_mark", "first_person_slot_repacked", "first_person_pieces", "supersampled_scene",
                       "supersampled_scene_4k"))
PRODUCTION_CASES = frozenset(("unsupported_host",))
# The real-size supersampled case (5760x3240 into 3840x2160: H alone is 75 MB at the render size, the owner plane and the foreground map
# 300 MB each) is not run on WARP: it reports UNSUPPORTED there, with this cause, and no process starts. It runs for longer than the
# others (the scene reads whole planes back several times), so it has a timeout of its own.
HARDWARE_ONLY_CASES = frozenset(("supersampled_scene_4k",))
UNSUPPORTED_ON_WARP = "case-needs-hardware-adapter-at-real-size"
LARGE_CASE_TIMEOUT = 300
# What a rule-bearing PASS must have measured over its own frame (observed.measuredFrame, or a path from observed
# itself when it starts with "@"), as (min, max) with None for unbounded. The scenario computes the same claims;
# this is the independent check that its PASS is believed. Cameras (section 104): camera 0 is the world's, camera 1
# the first person's. Only a camera-1 depth writer is planned (foreignSeen), captured and marked; a camera-0 draw is
# counted worldUnmarked (or, before the world is named at the last world's near, predictedWorld); the domain's own
# world-marker count (worldMarkers) is zero in every case.
#  - inert_no_write, inert_color_write, state_blended_no_depth: camera-1 draws that cannot write depth are forwarded
#    unchanged (surfacePreserving), never refused.
#  - state_partial_mask, state_blended: a camera-1 depth-writing Gbuffer draw is admitted and captured, not forwarded.
#  - state_blended_hdr_world, inert_depth_write_world: camera-0 depth writers (a blended HDR mesh; a full-screen inert
#    pair) are never planned, marked or refused.
#  - settlement_prepass: the camera-0 prepass run is planned as the predicted world and is neither captured nor marked.
#  - predicted_world_mismatch: the first-person camera takes the world's near plane (aiming down sights) and keeps a projection scale 25%
#    off the world's: not predicted (flatDomainPredictsWorld asks the scale as well as the near), so its pre-naming draw is a first-person
#    draw like the one after naming (two foreign, two captured), the 136 world prepass draws are the predicted ones, and H qualifies.
#    Guard predicted_world_close_scale: the same camera 3% off is still the world's, so it refuses as the near-only rule always did.
#  - stale_foreign_mark: a camera-0 depth writer outside the producer's families, drawn over a camera-1 surface in
#    front of it, leaves the owner plane byte-identical: the marks it covers are stale (their depth is no longer the
#    pixel's), stay in the plane, and the frame still qualifies.
# Every one of them also ends with an empty refusal inventory (failureKinds 0).
FIRST_PERSON_ONLY = {"worldMarkers": (0, 0), "failureKinds": (0, 0)}
RULE_COUNTERS = {
    "inert_no_write": {"surfacePreserving": (1, 1), "surfacePreservingForeign": (1, 1), "foreignSeen": (1, 1),
                       "captured": (1, 1), "worldUnmarked": (2, 2), **FIRST_PERSON_ONLY},
    "inert_color_write": {"surfacePreserving": (1, 1), "surfacePreservingForeign": (1, 1), "foreignSeen": (1, 1),
                          "captured": (1, 1), "worldUnmarked": (2, 2), **FIRST_PERSON_ONLY},
    "state_partial_mask": {"surfacePreserving": (0, 0), "foreignSeen": (2, 2), "captured": (2, 2),
                           "worldUnmarked": (2, 2), **FIRST_PERSON_ONLY},
    "state_blended": {"surfacePreserving": (0, 0), "foreignSeen": (2, 2), "captured": (2, 2),
                      "worldUnmarked": (2, 2), **FIRST_PERSON_ONLY},
    "state_blended_no_depth": {"surfacePreserving": (1, 1), "surfacePreservingForeign": (1, 1), "foreignSeen": (2, 2),
                               "captured": (1, 1), "worldUnmarked": (2, 2), **FIRST_PERSON_ONLY},
    "state_blended_hdr_world": {"surfacePreserving": (0, 0), "foreignSeen": (1, 1), "captured": (1, 1),
                                "worldUnmarked": (2, 2), **FIRST_PERSON_ONLY},
    "inert_depth_write_world": {"surfacePreserving": (0, 0), "foreignSeen": (1, 1), "captured": (1, 1),
                                "worldUnmarked": (3, 3), **FIRST_PERSON_ONLY},
    "settlement_prepass": {"predictedWorld": (134, None), "foreignSeen": (0, 4), "captured": (0, 4),
                           "worldUnmarked": (2, 2), "prepass.worldMarkers": (0, 0), "prepass.worldUnmarked": (0, 0),
                           **FIRST_PERSON_ONLY},
    "predicted_world_mismatch": {"predictedWorld": (136, 136), "surfacePreserving": (0, 0), "foreignSeen": (2, 2), "captured": (2, 2),
                                 "worldUnmarked": (2, 2), "hQualified": (1, None), "prepass.worldMarkers": (0, 0),
                                 "prepass.worldUnmarked": (0, 0), **FIRST_PERSON_ONLY},
    "stale_foreign_mark": {"surfacePreserving": (0, 0), "foreignSeen": (1, 1), "captured": (1, 1),
                           "worldUnmarked": (3, 3), "hQualified": (1, 1),
                           "@staleMark.worldWriterDepthPixels": (1, None), "@staleMark.before.foreign": (1, None),
                           "@staleMark.before.stale": (0, 0), "@staleMark.after.stale": (1, None),
                           "@staleMark.atH.stale": (1, None), "@staleMark.atH.fresh": (1, None), **FIRST_PERSON_ONLY},
}
# The supersampled on-foot frame (the HDR route at R > D, section 104): the plain scene's draws (a camera-0 world draw and a
# camera-1 first-person one: a mixed camera) with the scene targets 1.5x the swap chain per axis. H must qualify and the frame must
# be the backend's: its calls rise, and neither the spatial recovery nor a backend failure does. The sizes are the targets' own,
# read by the scenario, not the case's name.
SUPERSAMPLED_FRAME = {"surfacePreserving": (0, 0), "foreignSeen": (1, 1), "captured": (1, 1), "worldUnmarked": (2, 2),
                      "hAttempts": (1, None), "hQualified": (1, None), "backendCalls": (1, None), "hdrSpatial": (0, 0),
                      "backendFailures": (0, 0), **FIRST_PERSON_ONLY}
SUPERSAMPLED_SIZES = {"supersampled_scene": (96, 96, 64, 64), "supersampled_scene_4k": (5760, 3240, 3840, 2160)}
RULE_COUNTERS.update({
    case: {**SUPERSAMPLED_FRAME, "@sizes.renderWidth": (rw, rw), "@sizes.renderHeight": (rh, rh),
           "@sizes.outputWidth": (ow, ow), "@sizes.outputHeight": (oh, oh)}
    for case, (rw, rh, ow, oh) in SUPERSAMPLED_SIZES.items()})
# The first person's pool slot moves every frame (first_person_slot_repacked). The measured frame is the ordinary scene frame (a camera-0 world draw
# and a camera-1 first-person one, H qualified, the backend's frame, no refusal). After it the proxy's first-person map, the one it hands the resolver
# (w = 1 a sample with its motion, w = 2 a rejected one with its reason in x), is read after the H of every frame of three runs of 32 ordinary frames and
# its samples counted. (The resolver's refusal census is not the witness: every bench frame is a reset frame for the resolver, the offline proxy having no
# camera injector for the jitter's phase machine to call a frame clean, and a reset frame is neither asked for the census nor sampled.)
# A frame is counted only if its map is its own and a history could exist (its H qualified, and its first-person draw and the frame before's were captured:
# the offline proxy cannot land the jitter's phase on every frame, and a frame whose phase fails is neither captured nor treated); at least six of the 32 are.
#   steady    slot 9 every frame: every sample matched, none rejected.
#   moved     slots 9 and 10 alternating, two pool records of one identity (the pool repacked): still none rejected. The flat map matches a draw to the
#             frame before's by the record's identity (flat_foreground_motion_shader.h), not its slot; the slot compared rejected every sample of every
#             frame after the first of this run.
#   different slots 9 and 12 alternating, two records with another bone base: from the second frame on (31 of 32 are drawn) nothing matched and every
#             rejected sample says the identity differs (the scene's own rule says so: here the counts): the control that this scene's map rejects at all.
RULE_COUNTERS["first_person_slot_repacked"] = {
    "surfacePreserving": (0, 0), "foreignSeen": (1, 1), "captured": (1, 1), "worldUnmarked": (2, 2), "hAttempts": (1, None),
    "hQualified": (1, None), "backendCalls": (1, None), "hdrSpatial": (0, 0), "backendFailures": (0, 0), **FIRST_PERSON_ONLY,
    "@repack.steady.drawn": (32, 32), "@repack.steady.frames": (6, 32), "@repack.steady.matched": (1, None), "@repack.steady.rejected": (0, 0),
    "@repack.moved.drawn": (32, 32), "@repack.moved.frames": (6, 32), "@repack.moved.matched": (1, None), "@repack.moved.rejected": (0, 0),
    "@repack.different.drawn": (31, 31), "@repack.different.frames": (6, 31), "@repack.different.matched": (0, 0),
    "@repack.different.rejected": (1, None), "@repack.different.identityDiffers": (1, None)}
# The grenade hold (first_person_pieces): seventy draws of one first-person mesh in one frame. The flat adapter captures 64 of a key
# (AnimatedVertexHistory::maxExtendedOccurrences); the six past it are refused their capture and covered per pixel (their owner marks stand,
# the map holds no sample for them), so the frame stays qualified: H qualified with the covered draws counted, the backend's frame, and no
# refusal anywhere (failureKinds 0: the covered ones are kept apart from the frame refusals).
RULE_COUNTERS["first_person_pieces"] = {
    "surfacePreserving": (0, 0), "foreignSeen": (70, 70), "captured": (64, 64), "coveredDraws": (6, 6), "hCoveredFrames": (1, None),
    "worldUnmarked": (2, 2), "hAttempts": (1, None), "hQualified": (1, None), "backendCalls": (1, None), "hdrSpatial": (0, 0),
    "backendFailures": (0, 0), **FIRST_PERSON_ONLY}
# Facts a rule PASS must report as the boolean true.
RULE_TRUE = {"stale_foreign_mark": ("@staleMark.planeUntouched",), "first_person_slot_repacked": ("@repack.ran",)}
# The exact first-failure reason a guard that keeps its refusal must still report.
GUARD_REASONS = {"predicted_world_close_scale": "foreground-pending-null-not-selected-world"}
# A guard whose refusal is the selector's, before any H attempt: the verdict it must report, with no H attempt.
#  - state_blended_hdr: a camera-1 depth writer into the HDR light target. The domain admits and captures it, but the
#    prefix model refuses a second camera on H (conflicting-hdr-target-or-camera) before H is attempted.
GUARD_VERDICTS = {"state_blended_hdr": "conflicting-hdr-target-or-camera"}
HISTORY_CASES = frozenset(("provisional_before_foreign", "byte_budget", "invalidated_byte_occupancy",
                            "transient_churn", "mutation_reset", "adapter_cross_frame_record_budget",
                            "adapter_invalidated_prior_occupancy", "invalidated_capture_snapshot_lifetime",
                            "outstanding_capture_record_index", "stale_capture_epoch_guard",
                            "duplicate_occurrence_cap", "extended_occurrence_window",
                            "extended_spent_record_reclaim", "adapter_covered_refusals"))
MAX_JSON = 1 << 20


def checked(condition, message):
    if not condition:
        raise ValueError(message)


def positive_counter(value):
    return type(value) is int and value > 0


def inside(root, relative):
    p = pathlib.Path(relative)
    checked(not p.is_absolute() and ".." not in p.parts, "unsafe fixture path")
    result = (root / p).resolve()
    checked(result.is_relative_to(root.resolve()), "fixture path escapes repository")
    return result


DLSS_RUNTIME = "nvngx_dlss.dll"


def stages_dlss(mode):
    # NGX, which loads nvngx_dlss.dll, is initialised only by the dlaa and dlss backends (flat_mono_resolve.cpp
    # backendAvailable: fsr asks AMD's library, linked into the proxy; taa asks no vendor). Measured 2026-10-06 on
    # hardware, scene with the file absent from the scenario directory: taa and fsr PASS exactly as with it, dlaa
    # and dlss FAIL (NGX does not initialise, backend failure, spatial fallback). The copy beside the test exe
    # is not what NGX finds.
    return mode in ("dlaa", "dlss")


def link_or_copy(source, dest):
    """Stage source at dest as a hard link (no bytes of its own on the same volume), else as a copy.

    A file already at dest is replaced by name, never written through: it may be a link to an older build that
    a kept failing scenario still holds as its evidence. Returns "link" or "copy"."""
    source, dest = pathlib.Path(source), pathlib.Path(dest)
    if os.path.lexists(dest):
        dest.unlink()
    try:
        os.link(source, dest)
        return "link"
    except OSError:
        shutil.copy2(source, dest)
        return "copy"


def unstage(*paths):
    """Remove staged names (links or copies); the files they were staged from are never touched.

    Returns the paths that could not be removed. A link costs no disk, so a leftover is harmless."""
    kept = []
    for path in paths:
        try:
            pathlib.Path(path).unlink()
        except FileNotFoundError:
            pass
        except OSError:
            kept.append(path)
    return kept


def shader_hash(blob):
    h = 1469598103934665603
    for b in blob:
        h = ((h ^ b) * 1099511628211) & ((1 << 64) - 1)
    return f"{h:016X}"


def config_mode(mode):
    # Native TAA is the existing public "on" setting. "taa" is only the
    # bench's readable mode label and is not accepted by the production INI.
    checked(mode in MODES, "invalid AA mode")
    return "on" if mode == "taa" else mode


def audit_fixtures(root, manifest):
    checked(manifest.get("schema") == "edvr-flat-sdk-bench-fixtures" and
            manifest.get("version") == 1, "unsupported fixture manifest")
    checked(manifest.get("evidence", {}).get("exact_scene_replay") is False,
            "fixture manifest must disclose reconstruction")
    shaders = manifest.get("shaders")
    checked(isinstance(shaders, list) and 0 < len(shaders) <= 32, "invalid shader list")
    seen = set()
    for entry in shaders:
        p = inside(root, entry["path"])
        checked(p not in seen, "duplicate fixture path")
        seen.add(p)
        checked(p.stat().st_size == entry["bytes"] and 32 <= entry["bytes"] <= MAX_JSON,
                f"fixture size mismatch: {p.name}")
        blob = p.read_bytes()
        checked(blob[:4] == b"DXBC" and struct.unpack_from("<I", blob, 24)[0] == len(blob),
                f"invalid DXBC container: {p.name}")
        checked(hashlib.sha256(blob).hexdigest() == entry["sha256"].lower() and
                shader_hash(blob) == entry["hash"].upper(), f"fixture identity mismatch: {p.name}")
        checked(entry["stage"] in ("vs", "ps"), "invalid fixture stage")
    return manifest["evidence"]


def make_plan(root, cases, modes, adapter, report=None):
    checked(cases and all(c in CASE_PURPOSES for c in cases),
            "invalid scenario name")
    checked(modes and all(m in MODES for m in modes), "invalid AA mode")
    checked(adapter in ("warp", "hardware"), "invalid adapter")
    build = (root / "build").resolve()
    checked(build.is_relative_to(root.resolve()), "build path escapes repository")
    if report:
        report = report.resolve()
        checked(report.is_relative_to(build) and report != build, "report must be a file under build/")
    exe = build / "flat_sdk_integration_test.exe"
    proxy = build / "flat_sdk_bench_proxy.dll"
    fixtures = root / "tools/flat_temporal_test/fixtures"
    entries = []
    for mode in dict.fromkeys(modes):
        for case in dict.fromkeys(cases):
            if mode == "taa" and case in SDK_CASES:
                continue
            stage = build / "flat_sdk_bench" / mode / case
            checked(stage.resolve().is_relative_to(build), "staging path escapes build/")
            source = build / "d3d11.dll" if case in PRODUCTION_CASES else proxy
            command = [str(exe), "--case", case, "--proxy", str(stage / source.name),
                       "--fixtures", str(fixtures), "--adapter", adapter]
            entries.append({"case": case, "mode": mode, "stage": str(stage), "command": command,
                            "source_proxy": str(source), "staged_proxy": str(stage / source.name),
                            "source_dlss": str(build / DLSS_RUNTIME),
                            "staged_dlss": str(stage / DLSS_RUNTIME) if stages_dlss(mode) else None,
                            "proxy_flavor": "production" if case in PRODUCTION_CASES else "reconstructed-emit",
                            # Not run at all where the adapter cannot do it: the entry says so, and no process starts.
                            "unsupported": UNSUPPORTED_ON_WARP if adapter == "warp" and case in HARDWARE_ONLY_CASES else None,
                            "timeout": LARGE_CASE_TIMEOUT if case in HARDWARE_ONLY_CASES else None})
    checked(entries, "selected cases do not apply to the selected AA mode")
    return {"schema": "edvr-flat-sdk-bench-plan", "version": 1, "adapter": adapter,
            "proxy": str(proxy), "executable": str(exe), "report": str(report) if report else None,
            "runs": entries, "history_command": [str(build / "obj/weaponmotion/weapon_motion_test.exe"),
                                                     "--bench-history"]}


def rule_value(obs, path):
    """A counter by dotted path: from observed.measuredFrame, or from observed itself when the path starts with @."""
    node = obs if path.startswith("@") else obs.get("measuredFrame")
    for part in path.lstrip("@").split("."):
        if not isinstance(node, dict):
            return None
        node = node.get(part)
    return node


def parse_result(stdout, case, mode):
    checked(len(stdout) <= MAX_JSON, "scenario output exceeds cap")
    lines = [line[len(MARKER):] for line in stdout.splitlines() if line.startswith(MARKER)]
    checked(len(lines) == 1, "scenario must emit exactly one result")
    r = json.loads(lines[0])
    checked(r.get("schema") == "edvr-flat-sdk-bench" and r.get("version") == 1,
            "unsupported scenario result")
    checked(r.get("case") == case and r.get("mode") == mode, "scenario/mode result mismatch")
    checked(r.get("verdict") in ("PASS", "FAIL", "UNSUPPORTED"), "invalid scenario verdict")
    checked(r.get("purpose") == CASE_PURPOSES[case], "scenario purpose mismatch")
    checked(isinstance(r.get("observed"), dict) and isinstance(r.get("limitations"), list),
            "scenario observation/limitations missing")
    obs = r["observed"]
    if r["verdict"] == "PASS" and r["purpose"] == "renderer":
        checked(positive_counter(obs.get("rasterPixels")) and type(obs.get("namedWorld")) is int and
                obs["namedWorld"] == 1 and positive_counter(obs.get("resolverCalls")),
                "renderer PASS requires real raster and resolver evidence")
        if mode != "taa":
            h = obs.get("lastH", {})
            owner = obs.get("owner", {})
            checked(isinstance(h, dict) and positive_counter(h.get("qualified")) and
                    positive_counter(obs.get("backendCalls")) and isinstance(owner, dict) and
                    positive_counter(owner.get("gpuWorldPixelsAtH")) and
                    positive_counter(owner.get("gpuForeignPixelsAtH")),
                    "SDK PASS requires visible world and foreground, qualified original draws, and a completed backend call")
        checked(obs.get("ruleConfirmed") is True, "renderer PASS requires its expected rule to be measured")
        counters = RULE_COUNTERS.get(case)
        if counters:
            frame = obs.get("measuredFrame")
            failure = obs.get("firstFailure")
            checked(isinstance(frame, dict) and isinstance(failure, dict) and failure.get("frame") == 0,
                    "rule PASS requires measured frame counters and a frame with no refusal")
            for counter, (low, high) in counters.items():
                value = rule_value(obs, counter)
                checked(type(value) is int and (low is None or value >= low) and (high is None or value <= high),
                        f"rule PASS needs {counter} within [{low}, {high}], saw {value}")
        for path in RULE_TRUE.get(case, ()):
            checked(rule_value(obs, path) is True, f"rule PASS needs {path} to be true, saw {rule_value(obs, path)}")
    if r["verdict"] == "PASS" and r["purpose"] == "guard":
        checked(obs.get("guardConfirmed") is True, "guard PASS requires a measured expected refusal")
        reason = GUARD_REASONS.get(case)
        if reason:
            frame = obs.get("measuredFrame")
            failure = obs.get("firstFailure")
            checked(isinstance(frame, dict) and isinstance(failure, dict) and failure.get("reason") == reason and
                    frame.get("hQualified") == 0 and frame.get("backendCalls") == 0,
                    f"guard PASS requires first failure {reason} with no qualified H and no backend call")
        verdict = GUARD_VERDICTS.get(case)
        if verdict:
            frame = obs.get("measuredFrame")
            checked(isinstance(frame, dict) and obs.get("hdrVerdict") == verdict and frame.get("hAttempts") == 0 and
                    frame.get("hQualified") == 0 and frame.get("backendCalls") == 0,
                    f"guard PASS requires the selector verdict {verdict} with no H attempt and no backend call")
    if r["verdict"] == "PASS" and r["purpose"] == "entry":
        scope = obs.get("scope", {})
        checked(mode == "taa" and isinstance(scope, dict) and
                positive_counter(scope.get("drawAfter")) and type(scope.get("drawBefore")) is int and
                scope["drawAfter"] > scope["drawBefore"],
                "entry-only case cannot qualify an SDK renderer")
    return r


def parse_history_result(stdout):
    checked(len(stdout) <= MAX_JSON, "history output exceeds cap")
    lines = [line[len(HISTORY_MARKER):] for line in stdout.splitlines() if line.startswith(HISTORY_MARKER)]
    checked(len(lines) == 1, "history bench must emit exactly one result")
    r = json.loads(lines[0])
    checked(r.get("schema") == "edvr-flat-sdk-history-bench" and r.get("version") == 1,
            "unsupported history result")
    cases = r.get("cases")
    checked(r.get("verdict") in ("PASS", "FAIL") and isinstance(cases, list) and
            r.get("provenance"), "history coverage/provenance missing")
    names = [c.get("name") for c in cases if isinstance(c, dict)]
    checked(len(names) == len(cases) and len(names) == len(set(names)) and
            set(names) == HISTORY_CASES, "history case coverage missing or duplicated")
    for case in cases:
        obs = case.get("observed", {})
        checked(isinstance(obs, dict) and isinstance(case.get("passed"), bool),
                "history case observation/verdict missing")
        checked(all(type(obs.get(k)) is int and obs[k] >= 0 for k in
                    ("records", "bytes", "admissions", "priorCandidates", "retired")) and
                isinstance(obs.get("firstRefusal"), str), "invalid history counters")
    checked(r["verdict"] != "PASS" or all(c["passed"] for c in cases),
            "history PASS contradicts failed case")
    return r


def execute_plan(root, plan, evidence, timeout):
    checked(all(pathlib.Path(r["source_proxy"]).is_file() for r in plan["runs"]) and
            pathlib.Path(plan["executable"]).is_file(),
            "build the graphics DLL and integration bench first")
    results = []
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    for run in plan["runs"]:
        if run.get("unsupported"):
            # A case the adapter cannot run: UNSUPPORTED with its cause, never a PASS, and nothing is staged or started.
            r = {"schema": "edvr-flat-sdk-bench", "version": 1, "case": run["case"], "mode": run["mode"],
                 "purpose": CASE_PURPOSES[run["case"]], "verdict": "UNSUPPORTED", "observed": {},
                 "cause": run["unsupported"], "limitations": ["Not run on this adapter: the case needs the hardware adapter"],
                 "proxy_flavor": run["proxy_flavor"], "proxy_sha256": None,
                 "staging": {"proxy": "not-staged", "dlss": "not-staged", "removed": True}}
            results.append(r)
            print(f"{run['case']} / {run['mode']}: UNSUPPORTED ({r['cause']})")
            continue
        stage = pathlib.Path(run["stage"])
        staged_proxy = pathlib.Path(run["staged_proxy"])
        staged_dlss = pathlib.Path(run["staged_dlss"]) if run["staged_dlss"] else None
        outputs = [stage, staged_proxy, stage / DLSS_RUNTIME, stage / "edvr_profile.ini", stage / "edvr-flat.ini"]
        checked(all(p.resolve().is_relative_to((root / "build").resolve()) for p in outputs),
                "staging output escapes build/")
        stage.mkdir(parents=True, exist_ok=True)
        staging = {"proxy": link_or_copy(run["source_proxy"], staged_proxy), "dlss": "not-staged"}
        dlss = pathlib.Path(run["source_dlss"])
        if staged_dlss and dlss.is_file():
            staging["dlss"] = link_or_copy(dlss, staged_dlss)
        else:
            # Absent by plan: a copy an earlier matrix left here must not be a hidden input of this run.
            unstage(stage / DLSS_RUNTIME)
        (stage / "edvr_profile.ini").write_text("[install]\nschema = 1\nprofile = flat\n", encoding="utf-8")
        (stage / "edvr-flat.ini").write_text(
            f"[fix]\ntemporal_aa = {config_mode(run['mode'])}\n[log]\nenabled = on\n", encoding="utf-8")
        p = None
        try:
            p = subprocess.run(run["command"], cwd=stage, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=max(timeout, run.get("timeout") or 0),
                               creationflags=flags)
            r = parse_result(p.stdout, run["case"], run["mode"])
            checked(p.returncode in (0, 1, 2), "unexpected scenario process exit")
            checked(p.returncode == {"PASS": 0, "FAIL": 1, "UNSUPPORTED": 2}[r["verdict"]],
                    "scenario exit and verdict disagree")
        except (ValueError, json.JSONDecodeError, subprocess.TimeoutExpired, OSError) as error:
            r = {"schema": "edvr-flat-sdk-bench", "version": 1, "case": run["case"],
                 "mode": run["mode"], "purpose": CASE_PURPOSES[run["case"]], "verdict": "FAIL", "observed": {},
                 "cause": str(error), "limitations": ["Scenario did not produce a valid complete result"]}
            if p is not None:
                r["process_exit"] = p.returncode
                r["stderr"] = p.stderr[-4096:]
        results.append(r)
        r["proxy_flavor"] = run["proxy_flavor"]
        r["proxy_sha256"] = hashlib.sha256(staged_proxy.read_bytes()).hexdigest()
        # A scenario that PASSes needs no evidence directory beyond its settings: its DLL names go. FAIL and
        # UNSUPPORTED keep theirs, so the directory still holds exactly what the failing process ran.
        staging["removed"] = r["verdict"] == "PASS" and not unstage(staged_proxy, stage / DLSS_RUNTIME)
        r["staging"] = staging
        print(f"{run['case']} / {run['mode']}: {r['verdict']}" +
              (f" ({r['cause']})" if r.get("cause") else ""))
    try:
        p = subprocess.run(plan["history_command"], cwd=root, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout, creationflags=flags)
        history = parse_history_result(p.stdout)
        checked(p.returncode == (0 if history["verdict"] == "PASS" else 1),
                "history exit and verdict disagree")
    except (ValueError, json.JSONDecodeError, subprocess.TimeoutExpired, OSError) as error:
        history = {"verdict": "FAIL", "cause": str(error), "cases": []}
    print(f"GPU history pressure: {history['verdict']}" +
          (f" ({history['cause']})" if history.get("cause") else ""))
    verdict = "FAIL" if history["verdict"] == "FAIL" or any(r["verdict"] == "FAIL" for r in results) else (
        "UNSUPPORTED" if any(r["verdict"] == "UNSUPPORTED" for r in results) else "PASS")
    renderer = [r for r in results if r["purpose"] == "renderer"]
    renderer_verdict = "NOT_RUN" if not renderer else (
        "FAIL" if any(r["verdict"] == "FAIL" for r in renderer) else (
        "UNSUPPORTED" if any(r["verdict"] == "UNSUPPORTED" for r in renderer) else "PASS"))
    summary = {"schema": "edvr-flat-sdk-bench-report", "version": 1, "verdict": verdict,
               "renderer_verdict": renderer_verdict,
               "adapter": plan["adapter"], "evidence": evidence,
               "fixtures": plan["fixtures"],
               "runs": results, "history": history}
    if plan["report"]:
        report = pathlib.Path(plan["report"])
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def self_test():
    assert config_mode("taa") == "on"
    assert all(config_mode(m) == m for m in MODES if m != "taa")
    with tempfile.TemporaryDirectory(prefix="edvr-flat-sdk-bench-") as temp:
        root = pathlib.Path(temp)
        blob = bytearray(32)
        blob[:4] = b"DXBC"
        struct.pack_into("<I", blob, 24, len(blob))
        (root / "a.dxbc").write_bytes(blob)
        fixture = {"schema": "edvr-flat-sdk-bench-fixtures", "version": 1,
                   "evidence": {"exact_scene_replay": False}, "shaders": [{"path": "a.dxbc",
                   "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
                   "hash": shader_hash(blob), "stage": "vs"}]}
        audit_fixtures(root, fixture)
        manifest_path = root / MANIFEST
        manifest_path.parent.mkdir(parents=True)
        manifest_path.write_text(json.dumps(fixture), encoding="utf-8")
        before = sorted(p.relative_to(root) for p in root.rglob("*"))
        plan = make_plan(root, ["smoke", "unsupported_host"], ["dlaa", "fsr"], "warp",
                         root / "build/results/new.json")
        assert {r["proxy_flavor"] for r in plan["runs"]} == {"production", "reconstructed-emit"}
        for run in plan["runs"]:
            name = "d3d11.dll" if run["case"] == "unsupported_host" else "flat_sdk_bench_proxy.dll"
            assert pathlib.Path(run["staged_proxy"]).name == name
            # DLSS's runtime is planned for the two NGX modes only, and only ever inside the scenario's own directory.
            assert (run["staged_dlss"] is not None) == (run["mode"] == "dlaa")
            assert run["staged_dlss"] is None or pathlib.Path(run["staged_dlss"]) == pathlib.Path(run["stage"]) / DLSS_RUNTIME
        assert [stages_dlss(m) for m in MODES] == [False, True, True, False]
        matrix = make_plan(root, DEFAULT_CASES, MODES, "warp")
        assert len(matrix["runs"]) == len(DEFAULT_CASES)*len(MODES)-len(SDK_CASES)
        assert not any(r["mode"] == "taa" and r["case"] in SDK_CASES for r in matrix["runs"])
        assert all((r["staged_dlss"] is not None) == (r["mode"] in ("dlaa", "dlss")) for r in matrix["runs"])
        # The real-size case is the hardware adapter's: on WARP its entries are UNSUPPORTED with a cause (no process, no staging),
        # on hardware they run, with a timeout of their own.
        hardware_matrix = make_plan(root, DEFAULT_CASES, MODES, "hardware")
        assert len(hardware_matrix["runs"]) == len(matrix["runs"])
        for planned, adapter in ((matrix, "warp"), (hardware_matrix, "hardware")):
            for run in planned["runs"]:
                big = run["case"] in HARDWARE_ONLY_CASES
                assert run["unsupported"] == (UNSUPPORTED_ON_WARP if big and adapter == "warp" else None), (adapter, run["case"])
                assert run["timeout"] == (LARGE_CASE_TIMEOUT if big else None), (adapter, run["case"])
        assert {r["mode"] for r in matrix["runs"] if r["case"] == "supersampled_scene_4k"} == {"dlaa", "dlss", "fsr"}
        # Exercise the real CLI branch, not just its planning helper. Any subprocess would load a DLL and may write
        # runtime settings/logs. A dry run also stages nothing: no link, copy, delete, directory or file.
        writers = [(os, "link"), (os, "unlink"), (os, "remove"), (shutil, "copy2"), (shutil, "rmtree"),
                   (pathlib.Path, "mkdir"), (pathlib.Path, "unlink"), (pathlib.Path, "write_text"),
                   (pathlib.Path, "write_bytes")]
        for dry_mode in ("dlaa", "all"):
            with contextlib.ExitStack() as forbidden:
                for owner, attribute in writers:
                    forbidden.enter_context(mock.patch.object(
                        owner, attribute, side_effect=AssertionError(f"dry-run called {attribute}")))
                forbidden.enter_context(mock.patch.dict(globals(), ROOT=root))
                forbidden.enter_context(mock.patch("sys.argv", ["flat_sdk_bench.py", "--dry-run", "--mode", dry_mode,
                                                                 "--report", "build/results/new.json"]))
                forbidden.enter_context(mock.patch.object(
                    subprocess, "run", side_effect=AssertionError("dry-run launched a process")))
                forbidden.enter_context(contextlib.redirect_stdout(io.StringIO()))
                assert main() == 0
            assert sorted(p.relative_to(root) for p in root.rglob("*")) == before
        try:
            make_plan(root, ["smoke"], ["dlaa"], "warp", root / "source.json")
            raise AssertionError("report outside build accepted")
        except ValueError:
            pass
        for bad in ("../a.dxbc", str(root / "a.dxbc")):
            try:
                inside(root, bad)
                raise AssertionError("unsafe path accepted")
            except ValueError:
                pass
        (root / "a.dxbc").write_bytes(bytes(blob[:-1]) + b"\1")
        try:
            audit_fixtures(root, fixture)
            raise AssertionError("corrupted fixture accepted")
        except ValueError:
            pass
        r = {"schema": "edvr-flat-sdk-bench", "version": 1, "case": "scene", "purpose": "renderer", "mode": "dlaa",
             "verdict": "FAIL", "observed": {"lastH": {"qualified": 0}}, "limitations": []}
        assert parse_result(MARKER + json.dumps(r), "scene", "dlaa")["verdict"] == "FAIL"
        r["verdict"] = "PASS"
        try:
            parse_result(MARKER + json.dumps(r), "scene", "dlaa")
            raise AssertionError("false SDK pass accepted")
        except ValueError:
            pass
        r["observed"] = {"lastH": {"qualified": 1}, "backendCalls": 1,
                         "rasterPixels": 1352, "namedWorld": 1, "resolverCalls": 1, "ruleConfirmed": True,
                         "owner": {"gpuWorldPixelsAtH": 556, "gpuForeignPixelsAtH": 218}}
        assert parse_result(MARKER + json.dumps(r), "scene", "dlaa")["verdict"] == "PASS"
        for unproven in (False, None, "true"):
            r["observed"]["ruleConfirmed"] = unproven
            try:
                parse_result(MARKER + json.dumps(r), "scene", "dlaa")
                raise AssertionError("renderer pass without its rule measured accepted")
            except ValueError:
                pass
        r["observed"]["ruleConfirmed"] = True
        for counter, bad in (("qualified", 0), ("qualified", "1"), ("backendCalls", 0),
                             ("backendCalls", True)):
            where = r["observed"]["lastH"] if counter == "qualified" else r["observed"]
            good = where[counter]
            where[counter] = bad
            try:
                parse_result(MARKER + json.dumps(r), "scene", "dlaa")
                raise AssertionError("invalid SDK completion evidence accepted")
            except ValueError:
                pass
            where[counter] = good
        for counter, bad in (("gpuWorldPixelsAtH", 0), ("gpuForeignPixelsAtH", 0),
                             ("gpuForeignPixelsAtH", True), ("gpuForeignPixelsAtH", "218")):
            good = r["observed"]["owner"][counter]
            r["observed"]["owner"][counter] = bad
            try:
                parse_result(MARKER + json.dumps(r), "scene", "dlaa")
                raise AssertionError("empty or invalid GPU ownership accepted")
            except ValueError:
                pass
            r["observed"]["owner"][counter] = good
        try:
            parse_result(MARKER + json.dumps(r), "scene", "fsr")
            raise AssertionError("wrong mode accepted")
        except ValueError:
            pass
        r["observed"]["rasterPixels"] = 0
        try:
            parse_result(MARKER + json.dumps(r), "scene", "dlaa")
            raise AssertionError("empty raster passed as working renderer")
        except ValueError:
            pass
        # The case tables agree with each other, and every rule or guard check names a case that can carry it.
        assert set(DEFAULT_CASES) <= set(CASE_PURPOSES) and SDK_CASES <= set(DEFAULT_CASES)
        assert set(RULE_COUNTERS) <= SDK_CASES and all(CASE_PURPOSES[c] == "renderer" for c in RULE_COUNTERS)
        assert set(RULE_TRUE) <= set(RULE_COUNTERS)
        guards = {**GUARD_REASONS, **GUARD_VERDICTS}
        assert set(guards) <= SDK_CASES and all(CASE_PURPOSES[c] == "guard" for c in guards)
        assert not set(GUARD_REASONS) & set(GUARD_VERDICTS)
        assert {"state_blended_no_depth", "state_blended_hdr", "settlement_prepass", "predicted_world_mismatch",
                "predicted_world_close_scale", "state_blended_hdr_world", "inert_depth_write_world", "stale_foreign_mark",
                "supersampled_scene", "supersampled_scene_4k"} <= set(DEFAULT_CASES)
        # The two weapon-camera cases (aiming down sights): the 25% one is admitted, the 3% one keeps the old refusal.
        assert CASE_PURPOSES["predicted_world_mismatch"] == "renderer" and CASE_PURPOSES["predicted_world_close_scale"] == "guard"
        assert "predicted_world_mismatch" in RULE_COUNTERS and "predicted_world_close_scale" in GUARD_REASONS
        assert CASE_PURPOSES["state_blended_hdr"] == "guard" and CASE_PURPOSES["state_blended_hdr_world"] == "renderer"
        # The supersampled cases are SDK renderer cases (EDVR's TAA above the output stays on the copy route), the real-size one is the
        # hardware adapter's alone, and their sizes are a uniform 1.5x per axis: render above output, the same factor on both axes.
        assert {"supersampled_scene", "supersampled_scene_4k"} <= SDK_CASES and HARDWARE_ONLY_CASES == {"supersampled_scene_4k"}
        assert all(CASE_PURPOSES[c] == "renderer" for c in SUPERSAMPLED_SIZES) and set(SUPERSAMPLED_SIZES) <= set(RULE_COUNTERS)
        assert all(rw * 2 == ow * 3 and rh * 2 == oh * 3 for rw, rh, ow, oh in SUPERSAMPLED_SIZES.values())
        assert SUPERSAMPLED_SIZES["supersampled_scene_4k"] == (5760, 3240, 3840, 2160) and 5760 * 3240 > 16 * 1024 * 1024

        # Synthetic observations: what a PASS of each rule case looks like, then every table entry broken in turn.
        stale_good = {"planeUntouched": True, "worldWriterDepthPixels": 50, "before": {"foreign": 338, "stale": 0, "fresh": 338},
                      "after": {"foreign": 338, "stale": 50, "fresh": 288}, "atH": {"foreign": 218, "stale": 50, "fresh": 168}}
        good_frames = {
            "inert_no_write": {"surfacePreserving": 1, "surfacePreservingForeign": 1, "foreignSeen": 1, "captured": 1,
                               "worldUnmarked": 2},
            "state_partial_mask": {"surfacePreserving": 0, "foreignSeen": 2, "captured": 2, "worldUnmarked": 2},
            "state_blended_no_depth": {"surfacePreserving": 1, "surfacePreservingForeign": 1, "foreignSeen": 2,
                                       "captured": 1, "worldUnmarked": 2},
            "state_blended_hdr_world": {"surfacePreserving": 0, "foreignSeen": 1, "captured": 1, "worldUnmarked": 2},
            "inert_depth_write_world": {"surfacePreserving": 0, "foreignSeen": 1, "captured": 1, "worldUnmarked": 3},
            "settlement_prepass": {"predictedWorld": 137, "foreignSeen": 3, "captured": 3, "worldUnmarked": 2,
                                   "prepass": {"worldMarkers": 0, "worldUnmarked": 0}},
            "predicted_world_mismatch": {"predictedWorld": 136, "surfacePreserving": 0, "foreignSeen": 2, "captured": 2,
                                         "worldUnmarked": 2, "hQualified": 1, "prepass": {"worldMarkers": 0, "worldUnmarked": 0}},
            "stale_foreign_mark": {"surfacePreserving": 0, "foreignSeen": 1, "captured": 1, "worldUnmarked": 3,
                                   "hQualified": 1},
            "first_person_slot_repacked": {"surfacePreserving": 0, "foreignSeen": 1, "captured": 1, "worldUnmarked": 2,
                                           "hAttempts": 1, "hQualified": 1, "backendCalls": 1, "hdrSpatial": 0,
                                           "backendFailures": 0},
            "first_person_pieces": {"surfacePreserving": 0, "foreignSeen": 70, "captured": 64, "coveredDraws": 6,
                                    "hCoveredFrames": 1, "worldUnmarked": 2, "hAttempts": 1, "hQualified": 1,
                                    "backendCalls": 1, "hdrSpatial": 0, "backendFailures": 0},
        }
        supersampled_frame = {"surfacePreserving": 0, "foreignSeen": 1, "captured": 1, "worldUnmarked": 2, "hAttempts": 1,
                              "hQualified": 1, "backendCalls": 1, "hdrSpatial": 0, "backendFailures": 0}
        kept_run = {"drawn": 32, "treated": 21, "frames": 10, "matched": 2180, "rejected": 0, "identityDiffers": 0}
        good_extras = {"first_person_slot_repacked": {"repack": {"ran": True, "steady": kept_run, "moved": kept_run,
                       "different": {"drawn": 31, "treated": 21, "frames": 10, "matched": 0, "rejected": 2180, "identityDiffers": 2180}}}}
        for case, (rw, rh, ow, oh) in SUPERSAMPLED_SIZES.items():
            good_frames[case] = supersampled_frame
            good_extras[case] = {"sizes": {"renderWidth": rw, "renderHeight": rh, "outputWidth": ow, "outputHeight": oh}}
        good_frames["inert_color_write"] = good_frames["inert_no_write"]
        good_frames["state_blended"] = good_frames["state_partial_mask"]
        assert set(good_frames) == set(RULE_COUNTERS)

        def sdk_observed(frame, reason="", **extra):
            seen = {"lastH": {"qualified": 1}, "backendCalls": 1, "rasterPixels": 1352, "namedWorld": 1,
                    "resolverCalls": 1, "ruleConfirmed": True, "guardConfirmed": True, "hdrVerdict": "",
                    "owner": {"gpuWorldPixelsAtH": 556, "gpuForeignPixelsAtH": 218},
                    "firstFailure": {"frame": 6 if reason else 0, "reason": reason},
                    "staleMark": json.loads(json.dumps(stale_good)),
                    "sizes": {"renderWidth": 64, "renderHeight": 64, "outputWidth": 64, "outputHeight": 64},
                    "measuredFrame": {"worldMarkers": 0, "failureKinds": 1 if reason else 0, **frame}}
            seen.update(extra)
            return seen

        def good_observed(case, reason="", **extra):
            return sdk_observed(good_frames[case], reason, **{**json.loads(json.dumps(good_extras.get(case, {}))), **extra})

        def accepted(case, purpose, observed):
            result = {"schema": "edvr-flat-sdk-bench", "version": 1, "case": case, "mode": "dlaa",
                      "purpose": purpose, "verdict": "PASS", "observed": observed, "limitations": []}
            try:
                return parse_result(MARKER + json.dumps(result), case, "dlaa")["verdict"] == "PASS"
            except ValueError:
                return False

        def with_value(observed, path, value):
            node = observed if path.startswith("@") else observed["measuredFrame"]
            parts = path.lstrip("@").split(".")
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = value
            return observed

        for case, bounds in RULE_COUNTERS.items():
            assert accepted(case, "renderer", good_observed(case)), f"{case}: the good frame is refused"
            for counter, (low, high) in bounds.items():
                for outside in ([low - 1] if low is not None else []) + ([high + 1] if high is not None else []):
                    broken = with_value(good_observed(case), counter, outside)
                    assert not accepted(case, "renderer", broken), f"{case}: {counter}={outside} accepted"
                for bad in ("1", None, True):
                    broken = with_value(good_observed(case), counter, bad)
                    assert not accepted(case, "renderer", broken), f"{case}: {counter}={bad!r} accepted"
            for path in RULE_TRUE.get(case, ()):
                for bad in (False, 1, None):
                    assert not accepted(case, "renderer", with_value(good_observed(case), path, bad)), \
                        f"{case}: {path}={bad!r} accepted"
            assert not accepted(case, "renderer", good_observed(case, reason="foreground-draw-bound"))
            assert not accepted(case, "renderer", good_observed(case, ruleConfirmed=False))
            assert not accepted(case, "renderer", good_observed(case, measuredFrame=None))
        # A supersampled PASS is the backend's frame at the measured sizes: a size the case did not run at (native, or 1.4x), H not
        # qualified in the frame, or the frame the spatial recovery's or a failed backend's, is no PASS.
        for case, (rw, rh, ow, oh) in SUPERSAMPLED_SIZES.items():
            for sizes in ({"renderWidth": ow, "renderHeight": oh, "outputWidth": ow, "outputHeight": oh},
                          {"renderWidth": rw - 1, "renderHeight": rh, "outputWidth": ow, "outputHeight": oh},
                          {"renderWidth": rw, "renderHeight": rh, "outputWidth": ow + 1, "outputHeight": oh}):
                assert not accepted(case, "renderer", good_observed(case, sizes=sizes)), f"{case}: {sizes} accepted"
            assert not accepted(case, "renderer", good_observed(case, sizes={}))
            assert not accepted(case, "renderer", good_observed(case, sizes=None))
            for counter, bad in (("hQualified", 0), ("backendCalls", 0), ("hdrSpatial", 1), ("backendFailures", 1)):
                assert not accepted(case, "renderer", with_value(good_observed(case), counter, bad)), f"{case}: {counter}={bad}"
        # The camera-0 prepass run is not marked and not captured, whatever the numbers elsewhere say.
        assert not accepted("settlement_prepass", "renderer", with_value(
            sdk_observed(good_frames["settlement_prepass"]), "predictedWorld", 133))
        assert not accepted("settlement_prepass", "renderer", with_value(
            sdk_observed(good_frames["settlement_prepass"]), "captured", 136))
        # A first-person mark that was never fresh, or a plane the world writer changed, is not the stale-mark proof.
        stale_frame = good_frames["stale_foreign_mark"]
        assert not accepted("stale_foreign_mark", "renderer", sdk_observed(
            stale_frame, staleMark={**stale_good, "planeUntouched": False}))
        assert not accepted("stale_foreign_mark", "renderer", sdk_observed(
            stale_frame, staleMark={**stale_good, "before": {"foreign": 338, "stale": 3, "fresh": 335}}))
        assert not accepted("stale_foreign_mark", "renderer", sdk_observed(
            stale_frame, staleMark={**stale_good, "atH": {"foreign": 218, "stale": 218, "fresh": 0}}))
        assert not accepted("stale_foreign_mark", "renderer", sdk_observed(
            stale_frame, staleMark={**stale_good, "atH": {"foreign": 218, "stale": 0, "fresh": 218}}))

        for case, reason in GUARD_REASONS.items():
            refusal = sdk_observed({"hQualified": 0, "backendCalls": 0}, reason=reason)
            assert accepted(case, "guard", refusal)
            assert not accepted(case, "guard", sdk_observed({"hQualified": 0, "backendCalls": 0}, reason="other-refusal"))
            assert not accepted(case, "guard", sdk_observed({"hQualified": 1, "backendCalls": 0}, reason=reason))
            assert not accepted(case, "guard", sdk_observed({"hQualified": 0, "backendCalls": 1}, reason=reason))
            assert not accepted(case, "guard", sdk_observed({"hQualified": 0, "backendCalls": 0}, reason=reason,
                                                            guardConfirmed=False))
        for case, verdict in GUARD_VERDICTS.items():
            refused = {"hAttempts": 0, "hQualified": 0, "backendCalls": 0}
            assert accepted(case, "guard", sdk_observed(refused, hdrVerdict=verdict))
            assert not accepted(case, "guard", sdk_observed(refused, hdrVerdict="selected"))
            assert not accepted(case, "guard", sdk_observed(refused, hdrVerdict=""))
            for counter in refused:
                assert not accepted(case, "guard", sdk_observed({**refused, counter: 1}, hdrVerdict=verdict)), \
                    f"{case}: {counter}=1 accepted"
            assert not accepted(case, "guard", sdk_observed(refused, hdrVerdict=verdict, guardConfirmed=False))
        # A case that is not a rule case still passes on the generic renderer evidence alone.
        assert accepted("scene", "renderer", sdk_observed({}))
        hist = {"schema": "edvr-flat-sdk-history-bench", "version": 1, "verdict": "PASS",
                "provenance": "reconstructed GPU workload", "cases": [
                    {"name": name, "passed": True, "observed": {"records": 0, "bytes": 0,
                     "admissions": 0, "priorCandidates": 0, "retired": 0, "firstRefusal": ""}}
                    for name in sorted(HISTORY_CASES)]}
        assert parse_history_result(HISTORY_MARKER + json.dumps(hist))["verdict"] == "PASS"
        good_history = HISTORY_MARKER + json.dumps(hist)
        hist["cases"][0]["passed"] = False
        try:
            parse_history_result(HISTORY_MARKER + json.dumps(hist))
            raise AssertionError("contradictory history pass accepted")
        except ValueError:
            pass
        hist["cases"] = []
        try:
            parse_history_result(HISTORY_MARKER + json.dumps(hist))
            raise AssertionError("empty history coverage accepted")
        except ValueError:
            pass

        # Staging: a hard link where the volume allows one, a copy where it does not; a name already there is
        # replaced and never written through; unstaging touches only the staged names.
        def links_here(directory):
            one, two = directory / "probe.one", directory / "probe.two"
            one.write_bytes(b"x")
            try:
                os.link(one, two)
            except OSError:
                return False
            finally:
                one.unlink()
                if two.exists():
                    two.unlink()
            return True

        work = root / "staging"
        work.mkdir()
        hard_links = links_here(work)
        new, old, dest = work / "new.dll", work / "old.dll", work / "stage.dll"
        new.write_bytes(b"new build")
        old.write_bytes(b"old build")
        os.link(old, dest) if hard_links else shutil.copy2(old, dest)   # what a kept failing scenario leaves behind
        assert link_or_copy(new, dest) == ("link" if hard_links else "copy")
        assert dest.read_bytes() == b"new build" and old.read_bytes() == b"old build"   # never written through
        if hard_links:
            assert os.path.samefile(new, dest) and new.stat().st_nlink == 2
        with mock.patch.object(os, "link", side_effect=OSError("cross-volume")):
            other = work / "other.dll"
            assert link_or_copy(new, other) == "copy"
            assert other.read_bytes() == b"new build" and not os.path.samefile(new, other)
        assert unstage(dest, other, work / "never-staged.dll") == []
        assert not dest.exists() and not other.exists() and new.read_bytes() == b"new build" and new.stat().st_nlink == 1
        assert unstage(work) == [work] and work.is_dir()   # a name that cannot be removed is reported, not raised

        # The flow end to end with the scenario process stubbed: what the process finds staged while it runs, and
        # what a PASS, a FAIL and a stale leftover leave behind.
        from types import SimpleNamespace
        xroot = root / "xroot"
        (xroot / "build").mkdir(parents=True)
        payload = {name: name.encode() * 64 for name in
                   ("flat_sdk_integration_test.exe", "flat_sdk_bench_proxy.dll", "d3d11.dll", DLSS_RUNTIME)}
        for name, data in payload.items():
            (xroot / "build" / name).write_bytes(data)
        stages = xroot / "build/flat_sdk_bench"
        (stages / "fsr/unsupported_host").mkdir(parents=True)
        (stages / "fsr/unsupported_host" / DLSS_RUNTIME).write_bytes(b"left by an older matrix")
        observed_at_run = {}

        def fake_process(command, **kwargs):
            if not str(command[0]).endswith("flat_sdk_integration_test.exe"):
                return SimpleNamespace(stdout=good_history, stderr="", returncode=0)
            case = command[command.index("--case") + 1]
            staged = pathlib.Path(command[command.index("--proxy") + 1])
            where = pathlib.Path(kwargs["cwd"])
            mode = where.parent.name
            observed_at_run[(case, mode)] = {
                "proxy": staged.is_file() and staged.read_bytes() == payload[staged.name],
                "dlss": (where / DLSS_RUNTIME).is_file(),
                "ini": (where / "edvr-flat.ini").is_file() and (where / "edvr_profile.ini").is_file()}
            failing = (case, mode) == ("scene", "fsr")
            result = {"schema": "edvr-flat-sdk-bench", "version": 1, "case": case, "mode": mode,
                      "purpose": CASE_PURPOSES[case], "verdict": "FAIL" if failing else "PASS",
                      "observed": good_observed(case) if case in good_frames else sdk_observed({}), "limitations": []}
            return SimpleNamespace(stdout=MARKER + json.dumps(result), stderr="", returncode=int(failing))

        def run_plan(cases, modes):
            staged_plan = make_plan(xroot, cases, modes, "warp")
            staged_plan["fixtures"] = []
            with mock.patch.object(subprocess, "run", side_effect=fake_process), \
                    contextlib.redirect_stdout(io.StringIO()):
                summary = execute_plan(xroot, staged_plan, {}, 10)
            return {(r["case"], r["mode"]): r for r in summary["runs"]}, summary

        results, summary = run_plan(["scene", "unsupported_host"], ["taa", "dlaa", "fsr"])
        assert summary["verdict"] == "FAIL" and len(results) == 6
        assert all(seen["proxy"] and seen["ini"] for seen in observed_at_run.values())
        # DLSS's runtime is in the directory for the NGX modes and absent for taa and fsr (a stale copy included).
        assert {key: seen["dlss"] for key, seen in observed_at_run.items()} == {
            (case, mode): mode == "dlaa" for case in ("scene", "unsupported_host") for mode in ("taa", "dlaa", "fsr")}
        for (case, mode), r in results.items():
            where = stages / mode / case
            names = sorted(p.name for p in where.iterdir())
            if r["verdict"] == "PASS":   # no DLL name left, the settings stay, and the sources are untouched
                assert names == ["edvr-flat.ini", "edvr_profile.ini"] and r["staging"]["removed"] is True
            else:                         # a failing scenario keeps what its process ran
                assert r["staging"]["removed"] is False
                assert "flat_sdk_bench_proxy.dll" in names and DLSS_RUNTIME not in names
            assert r["staging"]["dlss"] == ("not-staged" if mode != "dlaa" else r["staging"]["proxy"])
            assert r["staging"]["proxy"] == ("link" if hard_links else "copy")
        assert all((xroot / "build" / name).read_bytes() == data for name, data in payload.items())
        # A volume that cannot link still stages, by copy, and a PASS still drops its copies.
        with mock.patch.object(os, "link", side_effect=OSError("cross-volume")):
            results, summary = run_plan(["scene"], ["dlaa"])
        assert summary["verdict"] == "PASS" and results[("scene", "dlaa")]["staging"] == {
            "proxy": "copy", "dlss": "copy", "removed": True}
        assert sorted(p.name for p in (stages / "dlaa/scene").iterdir()) == ["edvr-flat.ini", "edvr_profile.ini"]
        # The real-size case on WARP starts no process and stages nothing (UNSUPPORTED, with its cause); on hardware it runs, with a
        # timeout of its own that no shorter --timeout can undercut.
        started = []

        def spying_process(command, **kwargs):
            started.append((command, kwargs.get("timeout")))
            return fake_process(command, **kwargs)

        for adapter in ("warp", "hardware"):
            started.clear()
            staged_plan = make_plan(xroot, ["supersampled_scene_4k", "supersampled_scene"], ["dlss"], adapter)
            staged_plan["fixtures"] = []
            with mock.patch.object(subprocess, "run", side_effect=spying_process), contextlib.redirect_stdout(io.StringIO()):
                summary = execute_plan(xroot, staged_plan, {}, 10)
            by_case = {r["case"]: r for r in summary["runs"]}
            ran = [(c[c.index("--case") + 1], t) for c, t in started if str(c[0]).endswith("flat_sdk_integration_test.exe")]
            if adapter == "warp":
                big = by_case["supersampled_scene_4k"]
                assert big["verdict"] == "UNSUPPORTED" and big["cause"] == UNSUPPORTED_ON_WARP and big["proxy_sha256"] is None
                assert ran == [("supersampled_scene", 10)] and not (stages / "dlss/supersampled_scene_4k").exists()
                assert summary["verdict"] == "UNSUPPORTED" and by_case["supersampled_scene"]["verdict"] == "PASS"
            else:
                assert ran == [("supersampled_scene_4k", LARGE_CASE_TIMEOUT), ("supersampled_scene", 10)]
                assert summary["verdict"] == "PASS" and all(r["verdict"] == "PASS" for r in summary["runs"])
    print("flat_sdk_bench: self-test passed (write-free plans, pinned fixtures, linked staging, honest verdicts)")


def main():
    p = argparse.ArgumentParser(description=__doc__, epilog=(
        "Example: python tools/flat_sdk_bench.py --report build/flat-aa-report.json. "
        "Use --adapter hardware for the local D3D11 adapter; WARP cannot qualify NVIDIA "
        "image quality. Exit 1 means FAIL and exit 2 means UNSUPPORTED; neither is a green "
        "renderer result. Staging and reports stay under build/, without a game install; a scenario's DLLs "
        "are hard links, dropped again once it PASSes (FAIL and UNSUPPORTED keep theirs as evidence)."))
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--case", choices=tuple(CASE_PURPOSES), action="append", help="scenario name; repeat for a matrix")
    p.add_argument("--mode", choices=MODES + ("all",), default="all")
    p.add_argument("--adapter", choices=("warp", "hardware"), default="warp")
    p.add_argument("--report", type=pathlib.Path, help="write JSON under build/ (optional)")
    p.add_argument("--timeout", type=int, default=60, help="seconds per isolated scenario")
    args = p.parse_args()
    if args.self_test:
        self_test()
        return 0
    checked(1 <= args.timeout <= 600, "invalid scenario timeout")
    manifest = json.loads((ROOT / MANIFEST).read_text(encoding="utf-8"))
    evidence = audit_fixtures(ROOT, manifest)
    modes = MODES if args.mode == "all" else (args.mode,)
    report = args.report if not args.report or args.report.is_absolute() else ROOT / args.report
    plan = make_plan(ROOT, args.case or DEFAULT_CASES, modes, args.adapter, report)
    plan["fixtures"] = manifest["shaders"]
    if args.dry_run:
        print(json.dumps(plan, indent=2))
        return 0
    summary = execute_plan(ROOT, plan, evidence, args.timeout)
    print(f"Overall: {summary['verdict']}; renderer: {summary['renderer_verdict']}; "
          "state/workloads reconstructed, exact shaders captured.")
    return {"PASS": 0, "FAIL": 1, "UNSUPPORTED": 2}[summary["verdict"]]


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f"flat_sdk_bench: {error}")
        raise SystemExit(1)
