import argparse
from pathlib import Path
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
                return source[opening + 1:pos]
    raise AssertionError(f"unterminated function {signature}")


def ordered(source: str, *needles: str) -> None:
    positions = [source.index(needle) for needle in needles]
    assert positions == sorted(positions), f"wrong order: {needles}"


def check(lifecycle: str, vscreen: str) -> None:
    configure = body(lifecycle, "void configure(void* opaqueConfig)")
    frame = body(lifecycle, "void frame(void*, uint32_t sceneFrame)")
    shutdown = body(lifecycle, "void shutdown(void*)")
    configure_stage = body(lifecycle, "void configureStage(")
    frame_stage = body(lifecycle, "void frameStage(")
    shutdown_stage = body(lifecycle, "void shutdownStage(")
    ops = body(lifecycle, "const EdvrPluginLifecycleOps kIntroVideoLifecycleOps =")

    assert configure.index("introSkipConfigure(cfg)") < configure.index("introUpscaleConfigure(cfg)")
    assert "introSkipTick(sceneFrame != 0)" in frame
    assert shutdown.index("introUpscaleShutdown()") < shutdown.index("introSkipShutdown()")
    for stage, call in (("kIntroLifecycleLoader", "loaderPanelConfigure(cfg)"),
                        ("kIntroLifecycleSplashDim", "splashDimConfigure(cfg)"),
                        ("kIntroLifecyclePanel", "introPanelConfigure(cfg)"),
                        ("kIntroLifecycleBackdrop", "backdropConfigure(cfg)")):
        assert f"case {stage}:" in configure_stage and call in configure_stage
    assert "if (!opaqueConfig) return;" in configure_stage
    assert "default: break;" in configure_stage
    for stage, call in (("kIntroLifecycleLoader", "loaderPanelTick(context, scene)"),
                        ("kIntroLifecyclePanel", "introPanelTick(context, scene)"),
                        ("kIntroLifecycleCurve", "introCurveTick(context, scene)")):
        assert f"case {stage}:" in frame_stage and call in frame_stage
    assert "const bool scene = sceneFrame != 0;" in frame_stage
    assert "default: break;" in frame_stage
    for stage, call in (("kIntroLifecycleLoader", "loaderPanelShutdown()"),
                        ("kIntroLifecycleSplashDim", "splashDimShutdown()"),
                        ("kIntroLifecyclePanel", "introPanelShutdown()"),
                        ("kIntroLifecycleCurve", "introCurveShutdown()"),
                        ("kIntroLifecycleBackdrop", "backdropShutdown()")):
        assert f"case {stage}:" in shutdown_stage and call in shutdown_stage
    assert "default: break;" in shutdown_stage
    for field in ("sizeof(EdvrPluginLifecycleOps)", "plugins::kPluginIntro", '"intro"',
                  "configure,", "frame,", "shutdown,", "configureStage,",
                  "frameStage,", "shutdownStage,"):
        assert field in ops

    ensure = body(vscreen, "bool ensureIntroVideoLifecycle()")
    assert "if (!g_state) return false;" in ensure
    assert "pluginRegistryRegisterLifecycle(introVideoLifecycleOps())" in ensure
    assert "pluginRegistryHasLifecycle" not in ensure
    select = body(vscreen, "void selectIntroVideoLifecycle()")
    assert "g_introLifecycleRegistered = ensureIntroVideoLifecycle();" in select

    configure_stage_dispatch = body(vscreen, "void configureIntroLifecycleStage(")
    ordered(configure_stage_dispatch,
            "pluginRegistryConfigureLifecycleStage(kIntroPluginIndex, stage, &cfg)",
            "fallback(cfg)")
    assert "        return;\n    fallback(cfg);" in configure_stage_dispatch
    assert "if (g_introLifecycleRegistered &&" in configure_stage_dispatch
    assert "pluginRegistryRegisterLifecycle" not in configure_stage_dispatch
    configure_dispatch = body(vscreen, "void configureIntroVideoLifecycle(Config& cfg)")
    ordered(configure_dispatch, "pluginRegistryConfigureLifecycle(kIntroPluginIndex, &cfg)",
            "introSkipConfigure(cfg)", "introUpscaleConfigure(cfg)")
    assert "if (g_introLifecycleRegistered)" in configure_dispatch
    assert "pluginRegistryRegisterLifecycle" not in configure_dispatch

    for signature in ("void vScreenRefreshConfig()", "void installVScreenFixes("):
        site = body(vscreen, signature)
        assert site.count("selectIntroVideoLifecycle()") == 1
        assert site.count("configureIntroLifecycleStage(kIntroLifecycleLoader, cfg,") == 1
        assert site.count("configureIntroLifecycleStage(kIntroLifecycleSplashDim, cfg,") == 1
        assert site.count("configureIntroLifecycleStage(kIntroLifecyclePanel, cfg,") == 1
        assert site.count("configureIntroVideoLifecycle(cfg)") == 1
        assert site.count("configureIntroLifecycleStage(kIntroLifecycleBackdrop, cfg,") == 1
        ordered(site, "selectIntroVideoLifecycle()",
                "configureIntroLifecycleStage(kIntroLifecycleLoader, cfg,",
                "configureIntroLifecycleStage(kIntroLifecycleSplashDim, cfg,",
                "configureIntroLifecycleStage(kIntroLifecyclePanel, cfg,",
                "configureIntroVideoLifecycle(cfg)", "sharpenPassConfigure(cfg)",
                "temporalPassConfigure(cfg)", "pluginRegistryConfigure(&cfg)",
                "depthProbeConfigure(cfg)",
                "configureIntroLifecycleStage(kIntroLifecycleBackdrop, cfg,")
        for stage, fallback in (("Loader", "loaderPanelConfigure"),
                                ("SplashDim", "splashDimConfigure"),
                                ("Panel", "introPanelConfigure"),
                                ("Backdrop", "backdropConfigure")):
            assert f"configureIntroLifecycleStage(kIntroLifecycle{stage}, cfg,\n                                 {fallback});" in site

    frame_dispatch = body(vscreen, "void frameIntroVideoLifecycle(uint32_t sceneFrame)")
    assert "g_introLifecycleRegistered" in frame_dispatch
    assert "pluginRegistryRegisterLifecycle" not in frame_dispatch
    assert "sceneFrame != 0" in frame_dispatch
    frame_stage_dispatch = body(vscreen, "void frameIntroLifecycleStage(")
    ordered(frame_stage_dispatch,
            "pluginRegistryFrameLifecycleStage(kIntroPluginIndex, stage, context,",
            "fallback(context, sceneFrame != 0)")
    assert "        return;\n    fallback(context, sceneFrame != 0);" in frame_stage_dispatch
    assert "pluginRegistryRegisterLifecycle" not in frame_stage_dispatch

    frame_body = body(vscreen, "void vScreenFrameBoundary()")
    panel = body(frame_body, "tkIntroPanel.run([&] {")
    curve = body(frame_body, "tkIntroCurve.run([&] {")
    skip = body(frame_body, "tkIntroSkip.run([&] {")
    loader = body(frame_body, "tkLoaderPanel.run([&] {")
    ordered(frame_body, "tkIntroPanel.run([&] {", "tkIntroCurve.run([&] {",
            "tkIntroSkip.run([&] {", "tkLoaderPanel.run([&] {")
    for site, stage, tick in ((panel, "kIntroLifecyclePanel", "introPanelTick"),
                              (curve, "kIntroLifecycleCurve", "introCurveTick"),
                              (loader, "kIntroLifecycleLoader", "loaderPanelTick")):
        assert "frameIntroLifecycleStage(" in site and stage in site
        assert tick in site
        assert "g_state->ownerCtx" in site
        assert "g_state->eyeDrawsLastFrame >= kSceneEyeDraws" in site
    ordered(curve, "frameIntroLifecycleStage(", "introCurveNoteRetired()")
    assert "if (sceneFrame) introCurveNoteRetired();" in curve
    assert "frameIntroVideoLifecycle(" in skip
    assert "g_state->eyeDrawsLastFrame >= kSceneEyeDraws" in skip
    for wrapper in ("void configureIntroVideoLifecycle(Config& cfg)",
                    "void frameIntroVideoLifecycle(uint32_t sceneFrame)",
                    "void frameIntroLifecycleStage("):
        assert "pluginRegistryRegisterLifecycle" not in body(vscreen, wrapper)

    shutdown_stage_dispatch = body(vscreen, "void shutdownIntroLifecycleStage(")
    ordered(shutdown_stage_dispatch,
            "pluginRegistryShutdownLifecycleStage(kIntroPluginIndex, stage)",
            "fallback()")
    assert "        return;\n    fallback();" in shutdown_stage_dispatch
    assert "pluginRegistryRegisterLifecycle" not in shutdown_stage_dispatch
    shutdown_dispatch = body(vscreen, "void shutdownIntroVideoLifecycle()")
    ordered(shutdown_dispatch, "const bool registered = g_introLifecycleRegistered;",
            "g_introLifecycleRegistered = false;",
            "pluginRegistryShutdownLifecycle(kIntroPluginIndex)",
            "introUpscaleShutdown()", "introSkipShutdown()")
    assert "pluginRegistryRegisterLifecycle" not in shutdown_dispatch
    shutdown_site = body(vscreen, "void shutdownVScreenFixes()")
    ordered(shutdown_site,
            "pluginRegistryShutdown();",
            "shutdownIntroLifecycleStage(kIntroLifecycleLoader, loaderPanelShutdown)",
            "shutdownIntroLifecycleStage(kIntroLifecycleSplashDim, splashDimShutdown)",
            "shutdownIntroLifecycleStage(kIntroLifecycleBackdrop, backdropShutdown)",
            "shutdownIntroLifecycleStage(kIntroLifecyclePanel, introPanelShutdown)",
            "shutdownIntroLifecycleStage(kIntroLifecycleCurve, introCurveShutdown)",
            "shutdownIntroVideoLifecycle()", "temporalPassShutdown()",
            "g_state->hook.uninstall();", "targetSharpShutdown();")
    assert shutdown_site.count("shutdownIntroVideoLifecycle()") == 1
    assert "loaderPanelShutdown();" not in shutdown_site
    assert "splashDimShutdown();" not in shutdown_site
    assert "backdropShutdown();" not in shutdown_site
    assert "introPanelShutdown();" not in shutdown_site
    assert "introCurveShutdown();" not in shutdown_site

    # The stage registry owns lifecycle dispatch only. Draw recognition and
    # per-draw work stay at their existing ladder and forwarder sites.
    for required in ("SiteId::kOffscreenBackdropBlit", "backdropOnDraw(self",
                     "SiteId::kEyeBackdropComposite", "backdropOnComposite(self",
                     "SiteId::kOffscreenLoaderPanel", "SiteId::kIntroPanelClaim",
                     "SiteId::kIntroCurveObserve", "introPanelEndDraw(self)",
                     "introCurveEndDraw()"):
        assert required in vscreen
    backdrop_begin = body(vscreen, "__declspec(noinline) void forwardVerdictBegin(")
    assert "case DrawVerdict::kBackdrop:     backdropBegin(self);" in backdrop_begin
    draw_forwarder = body(vscreen, "void forwardWithVerdict(")
    ordered(draw_forwarder, "splashDimBegin(self)", "splashDimEnd(self)")
    assert "DrawVerdict::kIntroPanel" in draw_forwarder
    assert "pluginRegistryConfigureLifecycleStage" not in draw_forwarder
    assert "pluginRegistryFrameLifecycleStage" not in draw_forwarder
    assert "pluginRegistryShutdownLifecycleStage" not in draw_forwarder
    for api, owner in (("pluginRegistryConfigureLifecycleStage", configure_stage_dispatch),
                       ("pluginRegistryFrameLifecycleStage", frame_stage_dispatch),
                       ("pluginRegistryShutdownLifecycleStage", shutdown_stage_dispatch)):
        assert vscreen.count(api) == 1
        assert owner.count(api) == 1


def expect_rejected(label: str, lifecycle: str, vscreen: str) -> None:
    try:
        check(lifecycle, vscreen)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"source-order mutation was not detected: {label}")


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise AssertionError(f"expected one mutation target: {old}")
    return source.replace(old, new, 1)


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
    changed = replace_once(function, old, new)
    return source[:opening + 1] + changed + source[closing:]


def self_test(lifecycle: str, vscreen: str) -> None:
    expect_rejected("legacy configure order", replace_once(
        lifecycle, "introSkipConfigure(cfg);\n    introUpscaleConfigure(cfg);",
        "introUpscaleConfigure(cfg);\n    introSkipConfigure(cfg);"), vscreen)
    expect_rejected("legacy frame truth mapping", replace_once(
        lifecycle, "introSkipTick(sceneFrame != 0)",
        "introSkipTick(sceneFrame == 1)"), vscreen)
    expect_rejected("stage configure fallback", lifecycle, replace_in_function(
        vscreen, "void configureIntroLifecycleStage(",
        "pluginRegistryConfigureLifecycleStage(kIntroPluginIndex, stage, &cfg)",
        "pluginRegistryConfigureLifecycleStage(kIntroPluginIndex, stage + 1, &cfg)"))
    expect_rejected("configure success must return before fallback", lifecycle,
                    replace_in_function(
        vscreen, "void configureIntroLifecycleStage(",
        "        return;\n    fallback(cfg);",
        "        ;\n    fallback(cfg);"))
    expect_rejected("frame success must return before fallback", lifecycle,
                    replace_in_function(
        vscreen, "void frameIntroLifecycleStage(",
        "        return;\n    fallback(context, sceneFrame != 0);",
        "        ;\n    fallback(context, sceneFrame != 0);"))
    expect_rejected("shutdown success must return before fallback", lifecycle,
                    replace_in_function(
        vscreen, "void shutdownIntroLifecycleStage(",
        "        return;\n    fallback();",
        "        ;\n    fallback();"))
    expect_rejected("refresh registration before stages", lifecycle, replace_in_function(
        vscreen, "void vScreenRefreshConfig()",
        "selectIntroVideoLifecycle();\n    configureIntroLifecycleStage(kIntroLifecycleLoader, cfg,",
        "configureIntroLifecycleStage(kIntroLifecycleLoader, cfg,"))
    expect_rejected("install registration before stages", lifecycle, replace_in_function(
        vscreen, "void installVScreenFixes(",
        "selectIntroVideoLifecycle();\n    configureIntroLifecycleStage(kIntroLifecycleLoader, cfg,",
        "configureIntroLifecycleStage(kIntroLifecycleLoader, cfg,"))
    expect_rejected("install stage order", lifecycle, replace_in_function(
        vscreen, "void installVScreenFixes(",
        "configureIntroLifecycleStage(kIntroLifecyclePanel, cfg,\n                                 introPanelConfigure);",
        "configureIntroLifecycleStage(kIntroLifecycleSplashDim, cfg,\n                                 splashDimConfigure);"))
    expect_rejected("panel/curve frame order", lifecycle, replace_in_function(
        vscreen, "void vScreenFrameBoundary()",
        "tkIntroPanel.run([&] {", "tkIntroCurve.run([&] {"))
    expect_rejected("curve retirement note follows tick", lifecycle, replace_in_function(
        vscreen, "void vScreenFrameBoundary()",
        "frameIntroLifecycleStage(kIntroLifecycleCurve, g_state->ownerCtx,\n                                     sceneFrame ? 1u : 0u, introCurveTick);\n            if (sceneFrame) introCurveNoteRetired();",
        "if (sceneFrame) introCurveNoteRetired();\n            frameIntroLifecycleStage(kIntroLifecycleCurve, g_state->ownerCtx,\n                                     sceneFrame ? 1u : 0u, introCurveTick);"))
    expect_rejected("shutdown stage fallback", lifecycle, replace_in_function(
        vscreen, "void shutdownIntroLifecycleStage(",
        "pluginRegistryShutdownLifecycleStage(kIntroPluginIndex, stage)",
        "pluginRegistryShutdownLifecycleStage(kIntroPluginIndex, stage + 1)"))
    expect_rejected("shutdown order", lifecycle, replace_once(
        vscreen,
        "shutdownIntroLifecycleStage(kIntroLifecyclePanel, introPanelShutdown);\n    shutdownIntroLifecycleStage(kIntroLifecycleCurve, introCurveShutdown);",
        "shutdownIntroLifecycleStage(kIntroLifecycleCurve, introCurveShutdown);\n    shutdownIntroLifecycleStage(kIntroLifecyclePanel, introPanelShutdown);"))
    expect_rejected("final base detach order", lifecycle, replace_in_function(
        vscreen, "void shutdownVScreenFixes()",
        "g_state->hook.uninstall();", "targetSharpShutdown();"))
    expect_rejected("draw action moved to lifecycle dispatch", lifecycle,
                    vscreen.replace("backdropOnDraw(self, kind, count, instances)",
                                    "pluginRegistryFrameLifecycleStage(kIntroPluginIndex, 5, self, 1)"))
    expect_rejected("stage API inserted in a draw handler", lifecycle, replace_once(
        vscreen, "backdropOnDraw(self, kind, count, instances)",
        "backdropOnDraw(self, kind, count, instances) ||\n                pluginRegistryFrameLifecycleStage(kIntroPluginIndex, 5, self, 1)"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Pin staged intro lifecycle and VScreen source order")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="read and check source only")
    mode.add_argument("--self-test", action="store_true", help="run source pins and in-memory mutations")
    args = parser.parse_args()
    lifecycle = LIFECYCLE_PATH.read_text(encoding="utf-8")
    vscreen = VSCREEN_PATH.read_text(encoding="utf-8")
    check(lifecycle, vscreen)
    if args.self_test:
        self_test(lifecycle, vscreen)
        print("PASS: staged intro lifecycle source order and mutation pins")
    else:
        print("DRY RUN: staged intro lifecycle source pins passed; no files were written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
