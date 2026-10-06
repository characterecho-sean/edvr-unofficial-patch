"""Run the production proxy's offline flat-AA scenarios in isolated processes.

Actual shader bytes are hash-pinned; draw state and workloads are reconstructed.
Scenario FAIL is a renderer failure, even when the bench's self-test succeeds.
--dry-run creates no directories, files, processes, devices or DLL loads.
"""
import argparse
import contextlib
import hashlib
import io
import json
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
                 "state_partial_mask", "state_blended")
CASE_PURPOSES = {"smoke": "entry", "unsupported_host": "guard", "scene": "renderer", "inert_no_write": "renderer",
                 "inert_depth_write": "guard", "inert_color_write": "guard", "state_partial_mask": "guard",
                 "state_blended": "guard"}
SDK_CASES = frozenset(("inert_no_write", "inert_depth_write", "inert_color_write", "state_partial_mask",
                       "state_blended"))
PRODUCTION_CASES = frozenset(("unsupported_host",))
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
    if r["verdict"] == "PASS" and r["purpose"] == "guard":
        checked(obs.get("guardConfirmed") is True, "guard PASS requires a measured expected refusal")
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
        outputs = [stage, pathlib.Path(run["staged_proxy"]), stage / "nvngx_dlss.dll",
                   stage / "edvr_profile.ini", stage / "edvr-flat.ini"]
        checked(all(p.resolve().is_relative_to((root / "build").resolve()) for p in outputs),
                "staging output escapes build/")
        stage.mkdir(parents=True, exist_ok=True)
        shutil.copy2(run["source_proxy"], run["staged_proxy"])
        dlss = root / "build/nvngx_dlss.dll"
        if dlss.is_file():
            shutil.copy2(dlss, stage / dlss.name)
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
        r["proxy_sha256"] = hashlib.sha256(pathlib.Path(run["staged_proxy"]).read_bytes()).hexdigest()
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
        matrix = make_plan(root, DEFAULT_CASES, MODES, "warp")
        assert len(matrix["runs"]) == len(DEFAULT_CASES)*len(MODES)-len(SDK_CASES)
        assert not any(r["mode"] == "taa" and r["case"] in SDK_CASES for r in matrix["runs"])
        # Exercise the real CLI branch, not just its planning helper. Any
        # subprocess would load a DLL and may write runtime settings/logs.
        with mock.patch.dict(globals(), ROOT=root), mock.patch("sys.argv", ["flat_sdk_bench.py",
                "--dry-run", "--mode", "dlaa", "--report", "build/results/new.json"]), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("dry-run launched a process")), \
                contextlib.redirect_stdout(io.StringIO()):
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
                         "rasterPixels": 1352, "namedWorld": 1, "resolverCalls": 1,
                         "owner": {"gpuWorldPixelsAtH": 556, "gpuForeignPixelsAtH": 218}}
        assert parse_result(MARKER + json.dumps(r), "scene", "dlaa")["verdict"] == "PASS"
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
        hist = {"schema": "edvr-flat-sdk-history-bench", "version": 1, "verdict": "PASS",
                "provenance": "reconstructed GPU workload", "cases": [
                    {"name": name, "passed": True, "observed": {"records": 0, "bytes": 0,
                     "admissions": 0, "priorCandidates": 0, "retired": 0, "firstRefusal": ""}}
                    for name in sorted(HISTORY_CASES)]}
        assert parse_history_result(HISTORY_MARKER + json.dumps(hist))["verdict"] == "PASS"
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
    print("flat_sdk_bench: self-test passed (write-free plans, pinned fixtures, honest verdicts)")


def main():
    p = argparse.ArgumentParser(description=__doc__, epilog=(
        "Example: python tools/flat_sdk_bench.py --report build/flat-aa-report.json. "
        "Use --adapter hardware for the local D3D11 adapter; WARP cannot qualify NVIDIA "
        "image quality. Exit 1 means FAIL and exit 2 means UNSUPPORTED; neither is a green "
        "renderer result. Staging and reports stay under build/, without a game install."))
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
