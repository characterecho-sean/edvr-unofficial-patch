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
                 "settlement_prepass", "predicted_world_mismatch")
# "renderer" cases must complete the production HDR resolve; "guard" cases must keep a refusal.
CASE_PURPOSES = {"smoke": "entry", "unsupported_host": "guard", "scene": "renderer", "inert_no_write": "renderer",
                 "inert_depth_write": "guard", "inert_color_write": "renderer", "state_partial_mask": "renderer",
                 "state_blended": "renderer", "state_blended_no_depth": "renderer", "state_blended_hdr": "guard",
                 "settlement_prepass": "renderer", "predicted_world_mismatch": "guard"}
SDK_CASES = frozenset(("inert_no_write", "inert_depth_write", "inert_color_write", "state_partial_mask",
                       "state_blended", "state_blended_no_depth", "state_blended_hdr", "settlement_prepass",
                       "predicted_world_mismatch"))
PRODUCTION_CASES = frozenset(("unsupported_host",))
# What a rule-bearing PASS must have measured over its own frame (observed.measuredFrame), as (min, max) with None
# for unbounded. The scenario computes the same claims; this is the independent check that its PASS is believed.
#  - inert_color_write, state_blended_no_depth: draws that cannot write depth are counted as forwarded, never refused.
#  - state_partial_mask, state_blended: a depth-writing Gbuffer draw is admitted and captured, not forwarded.
#  - settlement_prepass: the world camera's prepass run is planned as the predicted world, and is not captured.
# Every one of them also ends with an empty refusal inventory (failureKinds 0).
RULE_COUNTERS = {
    "inert_color_write": {"surfacePreserving": (1, None), "failureKinds": (0, 0)},
    "state_partial_mask": {"surfacePreserving": (0, 0), "captured": (2, 2), "failureKinds": (0, 0)},
    "state_blended": {"surfacePreserving": (0, 0), "captured": (2, 2), "failureKinds": (0, 0)},
    "state_blended_no_depth": {"surfacePreserving": (1, None), "surfacePreservingForeign": (1, None),
                               "captured": (1, 1), "failureKinds": (0, 0)},
    "settlement_prepass": {"predictedWorld": (70, None), "foreignSeen": (0, 4), "captured": (0, 4),
                           "failureKinds": (0, 0)},
}
# The exact first-failure reason a guard that keeps its refusal must still report.
GUARD_REASONS = {"state_blended_hdr": "foreground-mixed-component-writer",
                 "predicted_world_mismatch": "foreground-pending-null-not-selected-world"}
HISTORY_CASES = frozenset(("provisional_before_foreign", "byte_budget", "invalidated_byte_occupancy",
                            "transient_churn", "mutation_reset", "adapter_cross_frame_record_budget",
                            "adapter_invalidated_prior_occupancy", "invalidated_capture_snapshot_lifetime",
                            "outstanding_capture_record_index", "stale_capture_epoch_guard",
                            "duplicate_occurrence_cap"))
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
                            "proxy_flavor": "production" if case in PRODUCTION_CASES else "reconstructed-emit"})
    checked(entries, "selected cases do not apply to the selected AA mode")
    return {"schema": "edvr-flat-sdk-bench-plan", "version": 1, "adapter": adapter,
            "proxy": str(proxy), "executable": str(exe), "report": str(report) if report else None,
            "runs": entries, "history_command": [str(build / "obj/weaponmotion/weapon_motion_test.exe"),
                                                     "--bench-history"]}


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
                value = frame.get(counter)
                checked(type(value) is int and (low is None or value >= low) and (high is None or value <= high),
                        f"rule PASS needs {counter} within [{low}, {high}], saw {value}")
    if r["verdict"] == "PASS" and r["purpose"] == "guard":
        checked(obs.get("guardConfirmed") is True, "guard PASS requires a measured expected refusal")
        reason = GUARD_REASONS.get(case)
        if reason:
            frame = obs.get("measuredFrame")
            failure = obs.get("firstFailure")
            checked(isinstance(frame, dict) and isinstance(failure, dict) and failure.get("reason") == reason and
                    frame.get("hQualified") == 0 and frame.get("backendCalls") == 0,
                    f"guard PASS requires first failure {reason} with no qualified H and no backend call")
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
                               encoding="utf-8", errors="replace", timeout=timeout, creationflags=flags)
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
        assert set(GUARD_REASONS) <= SDK_CASES and all(CASE_PURPOSES[c] == "guard" for c in GUARD_REASONS)
        assert {"state_blended_no_depth", "state_blended_hdr", "settlement_prepass",
                "predicted_world_mismatch"} <= set(DEFAULT_CASES)

        def sdk_observed(frame, reason="", **extra):
            seen = {"lastH": {"qualified": 1}, "backendCalls": 1, "rasterPixels": 1352, "namedWorld": 1,
                    "resolverCalls": 1, "ruleConfirmed": True, "guardConfirmed": True,
                    "owner": {"gpuWorldPixelsAtH": 556, "gpuForeignPixelsAtH": 218},
                    "firstFailure": {"frame": 6 if reason else 0, "reason": reason},
                    "measuredFrame": {"failureKinds": 1 if reason else 0, **frame}}
            seen.update(extra)
            return seen

        def accepted(case, purpose, observed):
            result = {"schema": "edvr-flat-sdk-bench", "version": 1, "case": case, "mode": "dlaa",
                      "purpose": purpose, "verdict": "PASS", "observed": observed, "limitations": []}
            try:
                return parse_result(MARKER + json.dumps(result), case, "dlaa")["verdict"] == "PASS"
            except ValueError:
                return False

        settlement = {"predictedWorld": 72, "foreignSeen": 2, "captured": 2, "surfacePreserving": 0}
        assert accepted("settlement_prepass", "renderer", sdk_observed(settlement))
        for counter, bad in (("predictedWorld", 69), ("predictedWorld", "72"), ("captured", 72), ("foreignSeen", 72),
                             ("predictedWorld", None)):
            assert not accepted("settlement_prepass", "renderer", sdk_observed({**settlement, counter: bad})), \
                f"settlement pass accepted with {counter}={bad!r}"
        assert not accepted("settlement_prepass", "renderer", sdk_observed(settlement, reason="foreground-draw-bound"))
        assert not accepted("settlement_prepass", "renderer", sdk_observed({**settlement, "failureKinds": 1}))
        assert not accepted("settlement_prepass", "renderer", sdk_observed(settlement, ruleConfirmed=False))
        assert not accepted("settlement_prepass", "renderer", sdk_observed(settlement, measuredFrame=None))
        forwarded = {"surfacePreserving": 1, "surfacePreservingForeign": 1, "foreignSeen": 2, "captured": 1}
        assert accepted("state_blended_no_depth", "renderer", sdk_observed(forwarded))
        assert not accepted("state_blended_no_depth", "renderer", sdk_observed({**forwarded, "surfacePreserving": 0}))
        assert not accepted("state_blended_no_depth", "renderer", sdk_observed({**forwarded, "captured": 2}))
        assert accepted("inert_color_write", "renderer", sdk_observed({"surfacePreserving": 1}))
        assert not accepted("inert_color_write", "renderer", sdk_observed({"surfacePreserving": 0}))
        admitted = {"surfacePreserving": 0, "foreignSeen": 2, "captured": 2}
        for case in ("state_partial_mask", "state_blended"):
            assert accepted(case, "renderer", sdk_observed(admitted))
            assert not accepted(case, "renderer", sdk_observed({**admitted, "surfacePreserving": 1}))
            assert not accepted(case, "renderer", sdk_observed({**admitted, "captured": 1}))
        for case, reason in GUARD_REASONS.items():
            refusal = sdk_observed({"hQualified": 0, "backendCalls": 0}, reason=reason)
            assert accepted(case, "guard", refusal)
            assert not accepted(case, "guard", sdk_observed({"hQualified": 0, "backendCalls": 0}, reason="other-refusal"))
            assert not accepted(case, "guard", sdk_observed({"hQualified": 1, "backendCalls": 0}, reason=reason))
            assert not accepted(case, "guard", sdk_observed({"hQualified": 0, "backendCalls": 1}, reason=reason))
            assert not accepted(case, "guard", sdk_observed({"hQualified": 0, "backendCalls": 0}, reason=reason,
                                                            guardConfirmed=False))
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
                      "observed": sdk_observed({}), "limitations": []}
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
