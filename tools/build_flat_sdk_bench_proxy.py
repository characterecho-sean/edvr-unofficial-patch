"""Link an offline-only proxy from production objects with a reconstructed emit hook.

The shipping d3d11.dll is built normally.  Only engine_velocity.cpp is
recompiled with its existing rig guard; three hook entry points are redirected
to this bench's inert fixture object.  The runtime draw, selection, owner and
resolver objects are the same objects linked into the shipping proxy.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def plan(root: Path, env: dict[str, str]) -> tuple[list[str], Path]:
    build = Path(env.get("BUILD", str(root / "build")))
    objdir = Path(env.get("OBJ", str(build / "obj")))
    generated = Path(env.get("GEN", str(build / "gen")))
    checkout = root.resolve()
    build_resolved = build.resolve()
    if (build_resolved != (root / "build").resolve() or
            not build_resolved.is_relative_to(checkout) or
            objdir.resolve() != (build / "obj").resolve() or
            not objdir.resolve().is_relative_to(build_resolved) or
            generated.resolve() != (build / "gen").resolve() or
            not generated.resolve().is_relative_to(build_resolved)):
        raise RuntimeError("all bench build targets must be the checkout's build directory")
    prod = objdir / "d3d11"
    plugin_specs = (
        ("cockpit visuals", objdir / "plugins" / "cockpit_visuals" / "plugin_cockpit_visuals.lib"),
        ("intro", objdir / "plugins" / "intro" / "plugin_intro.lib"),
        ("exposure", objdir / "plugins" / "exposure" / "plugin_exposure.lib"),
        ("on-foot-panel", objdir / "plugins" / "on_foot_panel" / "plugin_on_foot_panel.lib"),
        ("scanners", objdir / "plugins" / "scanners" / "plugin_scanners.lib"),
        ("comfort", objdir / "plugins" / "comfort" / "plugin_comfort.lib"),
        ("temporal-aa", objdir / "plugins" / "temporal_aa" / "plugin_temporal_aa.lib"),
        ("diagnostics", objdir / "plugins" / "diagnostics" / "plugin_diagnostics.lib"),
    )
    bench = objdir / "flat_sdk_bench_proxy"
    output = build / "flat_sdk_bench_proxy.dll"
    response = bench / "link.rsp"
    if any(not path.resolve().is_relative_to(build_resolved)
           for path in (prod, bench, output, response, build / "flat_sdk_bench_proxy.pdb",
                        build / "flat_sdk_bench_proxy.lib", build / "flat_sdk_bench_proxy.exp",
                        bench / "engine_velocity.obj", bench / "offline_emit_fixture.obj")):
        raise RuntimeError("bench output resolves outside the checkout's build directory")
    objects = sorted(prod.glob("*.obj"))
    if not objects or sum(p.name.lower() == "engine_velocity.obj" for p in objects) != 1:
        raise RuntimeError("production objects, including one engine_velocity.obj, are required")
    common = [p for p in objects if p.name.lower() != "engine_velocity.obj"]
    if len(common) < 100:
        raise RuntimeError("production object set is incomplete")
    # Reconstruct the feature DLL's module-library inputs. The registry object
    # is the feature-build marker: legacy main-only builds contain the same
    # vscreen, intro_panel and exposure_fix object names but compile their
    # owner implementations into the core object set. A missing or escaped
    # required archive fails before any compiler command runs.
    object_names = {p.name.lower() for p in objects}
    feature_build = "plugin_registry.obj" in object_names
    required_plugins = []
    for label, library in plugin_specs:
        if feature_build:
            if not library.is_file():
                raise RuntimeError(f"required {label} plugin library is missing: {library}")
            if not library.resolve().is_relative_to(build_resolved):
                raise RuntimeError(f"{label} plugin library resolves outside the checkout's build directory")
            required_plugins.append(library)
    cflags = env.get("CFLAGS", "")
    if not cflags or "/EHs" not in cflags or "/MT" not in cflags:
        raise RuntimeError("invoke from the build.bat compiler environment")
    compile_flags = " ".join((cflags, env.get("NGXFLAGS", ""), env.get("FSRFLAGS", "")))
    engine = root / "src" / "d3d11" / "engine_velocity.cpp"
    stub = root / "tools" / "flat_sdk_integration_test" / "offline_emit_fixture.cpp"
    compile_engine = (
        f'cl.exe {compile_flags} /DEDVR_ENGINE_VELOCITY_RIG '
        '/DkinematicEvalEmitHookLive=edvrOfflineEmitHookLive '
        '/DkinematicEvalEmitAttach=edvrOfflineEmitAttach '
        '/DkinematicEvalEmitDetach=edvrOfflineEmitDetach '
        f'/Fo"{bench / "engine_velocity.obj"}" "{engine}"'
    )
    compile_stub = f'cl.exe {compile_flags} /Fo"{bench / "offline_emit_fixture.obj"}" "{stub}"'
    inputs = [*common, bench / "engine_velocity.obj", bench / "offline_emit_fixture.obj"]
    object_args = " ".join(f'"{p}"' for p in inputs)
    resources = f'"{prod / "dxbc_notice.res"}" "{prod / "version.res"}"'
    libraries = " ".join(("kernel32.lib user32.lib gdi32.lib version.lib d3dcompiler.lib",
                          env.get("NGXLIB", ""), env.get("FSRLIB", "")))
    plugin_libraries = " ".join(f'"{library}"' for library in required_plugins)
    link = (
        'link.exe /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO '
        f'{env.get("EDVR_CPU_LINK", "")} /PDB:"{build / "flat_sdk_bench_proxy.pdb"}" '
        f'/DEF:"{generated / "edvr_d3d11.def"}" /OUT:"{output}" '
        f'{object_args} {plugin_libraries} {resources} {libraries}'
    )
    return [compile_engine, compile_stub, link], output


def self_test() -> None:
    import tempfile
    from unittest.mock import patch

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        prod = root / "build" / "obj" / "d3d11"
        fake = ([prod / f"part_{i}.obj" for i in range(100)] +
                [prod / "plugin_registry.obj", prod / "engine_velocity.obj"])
        env = {"CFLAGS": "/nologo /c /O2 /MT /EHs", "BUILD": str(root / "build"),
               "OBJ": str(root / "build" / "obj"), "GEN": str(root / "build" / "gen")}
        plugin_libs = {
            "cockpit visuals": root / "build" / "obj" / "plugins" / "cockpit_visuals" / "plugin_cockpit_visuals.lib",
            "intro": root / "build" / "obj" / "plugins" / "intro" / "plugin_intro.lib",
            "exposure": root / "build" / "obj" / "plugins" / "exposure" / "plugin_exposure.lib",
            "on-foot-panel": root / "build" / "obj" / "plugins" / "on_foot_panel" / "plugin_on_foot_panel.lib",
            "scanners": root / "build" / "obj" / "plugins" / "scanners" / "plugin_scanners.lib",
            "comfort": root / "build" / "obj" / "plugins" / "comfort" / "plugin_comfort.lib",
            "temporal-aa": root / "build" / "obj" / "plugins" / "temporal_aa" / "plugin_temporal_aa.lib",
            "diagnostics": root / "build" / "obj" / "plugins" / "diagnostics" / "plugin_diagnostics.lib",
        }
        for library in plugin_libs.values():
            library.parent.mkdir(parents=True)
            library.touch()
        with patch.object(Path, "glob", return_value=fake):
            commands, output = plan(root, env)
        assert len(commands) == 3 and output.name == "flat_sdk_bench_proxy.dll"
        assert "/DEDVR_ENGINE_VELOCITY_RIG" in commands[0]
        assert "/DkinematicEvalEmitHookLive=edvrOfflineEmitHookLive" in commands[0]
        assert '"' + str(prod / "engine_velocity.obj") + '"' not in commands[2]
        assert commands[2].count("engine_velocity.obj") == 1
        for library in plugin_libs.values():
            assert commands[2].count(f'"{library}"') == 1
        assert "flat_runtime.obj" not in commands[0]
        main_objects = ([p for p in fake if p.name.lower() != "plugin_registry.obj"] +
                        [prod / name for name in ("vscreen.obj", "intro_panel.obj",
                                                   "exposure_fix.obj", "intro_skip.obj",
                                                   "intro_upscale.obj")])
        with patch.object(Path, "glob", return_value=main_objects):
            main_only_commands, _ = plan(root, env)
        assert all(str(library) not in main_only_commands[2]
                   for library in plugin_libs.values())
        for library in plugin_libs.values():
            library.unlink()
        with patch.object(Path, "glob", return_value=main_objects):
            absent_optional_commands, _ = plan(root, env)
        assert all(str(library) not in absent_optional_commands[2]
                   for library in plugin_libs.values())
        for library in plugin_libs.values():
            library.touch()
        for label, library in plugin_libs.items():
            library.unlink()
            with patch.object(Path, "glob", return_value=fake):
                try:
                    plan(root, env)
                except RuntimeError as exc:
                    assert f"required {label} plugin library is missing" in str(exc)
                else:
                    raise AssertionError(f"missing required {label} library was accepted")
            library.touch()
        real_resolve = Path.resolve
        for escaped in (root / "build", root / "build" / "obj" / "flat_sdk_bench_proxy",
                        *plugin_libs.values()):
            def fake_resolve(path: Path, *args, **kwargs):
                return root.parent / "outside" if path == escaped else real_resolve(path, *args, **kwargs)
            with patch.object(Path, "glob", return_value=fake), \
                 patch.object(Path, "resolve", autospec=True, side_effect=fake_resolve):
                try:
                    plan(root, env)
                except RuntimeError as exc:
                    for label, library in plugin_libs.items():
                        if escaped == library:
                            assert f"{label} plugin library resolves outside" in str(exc)
                else:
                    raise AssertionError(f"escaped bench path accepted: {escaped}")
        with patch.object(Path, "glob", return_value=fake), \
             patch.dict(os.environ, env), patch.dict(globals(), {"ROOT": root}), \
             patch.object(sys, "argv", ["build_flat_sdk_bench_proxy.py", "--dry-run"]), \
             patch.object(Path, "mkdir", side_effect=AssertionError("dry-run mkdir")), \
             patch.object(Path, "write_text", side_effect=AssertionError("dry-run write")), \
             patch("subprocess.run", side_effect=AssertionError("dry-run process")):
            assert main() == 0
    print("build_flat_sdk_bench_proxy: self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    commands, output = plan(ROOT, os.environ)
    if args.dry_run:
        print(f"Would compile offline emit fixture and variant engine object, then link {output}; writes nothing.")
        return 0
    output.parent.mkdir(parents=True, exist_ok=True)
    (output.parent / "obj" / "flat_sdk_bench_proxy").mkdir(parents=True, exist_ok=True)
    response = output.parent / "obj" / "flat_sdk_bench_proxy" / "link.rsp"
    response.write_text(commands[2].removeprefix("link.exe "), encoding="utf-8")
    commands[2] = f'link.exe @"{response}"'
    for command in commands:
        done = subprocess.run(command, cwd=ROOT, shell=True, check=False)
        if done.returncode:
            return done.returncode
    print(f"[edvr] offline-only flat SDK proxy: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
