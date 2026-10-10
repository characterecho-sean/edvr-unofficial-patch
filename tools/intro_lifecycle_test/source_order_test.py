import argparse
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
LIFECYCLE_PATH = ROOT / "src/plugins/intro/intro_lifecycle.cpp"
VSCREEN_PATH = ROOT / "src/d3d11/vscreen.cpp"


def body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function {signature}")
    opening = source.find("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : pos]
    raise AssertionError(f"unterminated function {signature}")


def check(lifecycle: str, vscreen: str) -> None:
    configure = body(lifecycle, "void configure(void* opaqueConfig)")
    frame = body(lifecycle, "void frame(void*, uint32_t sceneFrame)")
    shutdown = body(lifecycle, "void shutdown(void*)")
    ops = body(lifecycle, "const EdvrPluginLifecycleOps kIntroVideoLifecycleOps =")

    assert configure.index("introSkipConfigure(cfg)") < configure.index("introUpscaleConfigure(cfg)")
    assert "introSkipTick(sceneFrame != 0)" in frame
    assert shutdown.index("introUpscaleShutdown()") < shutdown.index("introSkipShutdown()")
    for field in ("sizeof(EdvrPluginLifecycleOps)", "plugins::kPluginIntro", '"intro"',
                  "configure,", "frame,", "shutdown,"):
        assert field in ops

    ensure = body(vscreen, "bool ensureIntroVideoLifecycle()")
    assert "if (!g_state) return false;" in ensure
    assert "pluginRegistryRegisterLifecycle(introVideoLifecycleOps())" in ensure
    assert "pluginRegistryHasLifecycle" not in ensure

    configure_dispatch = body(vscreen, "void configureIntroVideoLifecycle(Config& cfg)")
    assert configure_dispatch.index("pluginRegistryConfigureLifecycle") < \
        configure_dispatch.index("introSkipConfigure(cfg)") < \
        configure_dispatch.index("introUpscaleConfigure(cfg)")
    assert "g_introLifecycleRegistered = ensureIntroVideoLifecycle();" in configure_dispatch
    for signature in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        site = body(vscreen, signature)
        assert site.count("configureIntroVideoLifecycle(cfg)") == 1
        assert site.index("introPanelConfigure(cfg)") < \
            site.index("configureIntroVideoLifecycle(cfg)") < \
            site.index("sharpenPassConfigure(cfg)") < \
            site.index("temporalPassConfigure(cfg)")

    frame_dispatch = body(vscreen, "void frameIntroVideoLifecycle(uint32_t sceneFrame)")
    assert "g_introLifecycleRegistered" in frame_dispatch
    assert "pluginRegistryRegisterLifecycle" not in frame_dispatch
    frame_body = body(vscreen, "void vScreenFrameBoundary()")
    assert frame_body.index("tkIntroCurve.run([&] {") < \
        frame_body.index("tkIntroSkip.run([&] {") < \
        frame_body.index("tkLoaderPanel.run([&] {")
    frame_site = re.search(r'tkIntroSkip\.run\(\[&\] \{(?P<body>.*?)\}\);', frame_body, re.S)
    assert frame_site
    assert "frameIntroVideoLifecycle(" in frame_site.group("body")
    assert "eyeDrawsLastFrame >= kSceneEyeDraws" in frame_site.group("body")

    shutdown_dispatch = body(vscreen, "void shutdownIntroVideoLifecycle()")
    assert shutdown_dispatch.index("g_introLifecycleRegistered = false;") < \
        shutdown_dispatch.index("pluginRegistryShutdownLifecycle")
    assert shutdown_dispatch.index("pluginRegistryShutdownLifecycle") < \
        shutdown_dispatch.index("introUpscaleShutdown()") < \
        shutdown_dispatch.index("introSkipShutdown()")
    assert "pluginRegistryRegisterLifecycle" not in shutdown_dispatch
    shutdown_site = body(vscreen, "void shutdownVScreenFixes()")
    assert shutdown_site.count("shutdownIntroVideoLifecycle()") == 1
    assert shutdown_site.index("introPanelShutdown()") < \
        shutdown_site.index("introCurveShutdown()") < \
        shutdown_site.index("shutdownIntroVideoLifecycle()") < \
        shutdown_site.index("temporalPassShutdown()")


def expect_rejected(label: str, lifecycle: str, vscreen: str) -> None:
    try:
        check(lifecycle, vscreen)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"source-order mutation was not detected: {label}")


def swap_calls(source: str, first: str, second: str) -> str:
    first_start = source.index(first)
    first_end = source.index("});", first_start) + len("});")
    second_start = source.index(second, first_end)
    second_end = source.index("});", second_start) + len("});")
    first_block = source[first_start:first_end]
    gap = source[first_end:second_start]
    second_block = source[second_start:second_end]
    return source[:first_start] + second_block + gap + first_block + source[second_end:]


def replace_in_function(source: str, signature: str, old: str, new: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function {signature}")
    opening = source.find("{", start)
    depth = 0
    closing = -1
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                closing = pos
                break
    if closing < 0:
        raise AssertionError(f"unterminated function {signature}")
    function = source[opening + 1:closing]
    if function.count(old) != 1:
        raise AssertionError(f"expected one mutation target in {signature}")
    changed = function.replace(old, new, 1)
    return source[:opening + 1] + changed + source[closing:]


def self_test(lifecycle: str, vscreen: str) -> None:
    expect_rejected("configure order", lifecycle.replace(
        "introSkipConfigure(cfg);\n    introUpscaleConfigure(cfg);",
        "introUpscaleConfigure(cfg);\n    introSkipConfigure(cfg);", 1), vscreen)
    expect_rejected("frame truth mapping", lifecycle.replace(
        "introSkipTick(sceneFrame != 0)", "introSkipTick(sceneFrame == 1)"), vscreen)
    expect_rejected("install callsite", lifecycle, vscreen.replace(
        "configureIntroVideoLifecycle(cfg);", "introSkipConfigure(cfg);", 1))
    expect_rejected("refresh configure order", lifecycle, vscreen.replace(
        "configureIntroVideoLifecycle(cfg);\n    sharpenPassConfigure(cfg);",
        "sharpenPassConfigure(cfg);\n    configureIntroVideoLifecycle(cfg);", 1))
    expect_rejected("install configure order", lifecycle, replace_in_function(
        vscreen, "void installVScreenFixes(",
        "configureIntroVideoLifecycle(cfg);\n    sharpenPassConfigure(cfg);",
        "sharpenPassConfigure(cfg);\n    configureIntroVideoLifecycle(cfg);"))
    expect_rejected("intro tick order", lifecycle, swap_calls(
        vscreen, "tkIntroCurve.run([&] {", "tkIntroSkip.run([&] {"))
    expect_rejected("shutdown selection reset", lifecycle, vscreen.replace(
        "const bool registered = g_introLifecycleRegistered;\n    g_introLifecycleRegistered = false;",
        "const bool registered = g_introLifecycleRegistered;\n    g_introLifecycleRegistered = true;", 1))
    expect_rejected("shutdown order", lifecycle, vscreen.replace(
        "introCurveShutdown();\n    shutdownIntroVideoLifecycle();",
        "shutdownIntroVideoLifecycle();\n    introCurveShutdown();", 1))


def main() -> int:
    parser = argparse.ArgumentParser(description="Pin intro lifecycle callback and VScreen source order")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="read and check source only")
    mode.add_argument("--self-test", action="store_true", help="run source pins and in-memory mutations")
    args = parser.parse_args()
    lifecycle = LIFECYCLE_PATH.read_text(encoding="utf-8")
    vscreen = VSCREEN_PATH.read_text(encoding="utf-8")
    check(lifecycle, vscreen)
    if args.self_test:
        self_test(lifecycle, vscreen)
        print("PASS: intro lifecycle source order and mutation pins")
    else:
        print("DRY RUN: intro lifecycle source pins passed; no files were written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
