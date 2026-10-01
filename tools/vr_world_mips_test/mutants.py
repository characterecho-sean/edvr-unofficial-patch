#!/usr/bin/env python3
"""The mutation proof for tools\\vr_world_mips_test: the rig fails when a rule of the production module is flipped.

The rig (vr_world_mips_test.cpp) pins eleven rules of src\\d3d11\\vr_world_mips.cpp, R1..R11 (its header says which). A rig
that passes proves little until it is seen to FAIL on a module that breaks the rule it pins. This tool does that: for each
mutation below it copies the production source into a temp directory OUTSIDE the repo, applies one textual edit (or a few
that belong together), compiles the copy, links it with the rig's own object, runs the rig on the rule's cases, and requires
the rig to fail on the check that belongs to the rule (the first line it prints after FAIL: starts with the label prefix
the mutation names). Nothing is written inside the repo; the temp directory is removed at the end.

  python tools\\vr_world_mips_test\\mutants.py --self-test          text only: every anchor is found exactly once in the
                                                                  source as it is now, every label named is in the rig, and
                                                                  build.bat compiles the rig the way this tool does
  python tools\\vr_world_mips_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\vr_world_mips_test\\mutants.py --list
  python tools\\vr_world_mips_test\\mutants.py --run --dry-run      the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes about a
minute. --self-test runs in build.bat's rig and is what keeps an edit of the module from silently orphaning a mutation: if
an anchor stops matching, the build fails and this file says which.
"""
import argparse
import concurrent.futures
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROD = ROOT / "src" / "d3d11" / "vr_world_mips.cpp"
RIG = HERE / "vr_world_mips_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_vr_world_mips_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_LIBS = ["d3dcompiler.lib"]
RUN_TIMEOUT = 90.0


class Mutant:
    def __init__(self, name, caught, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why
        self.rule = re.match(r"R\d+", self.caught[0]).group(0)                  # the rig's case to run


def M(name, caught, edits, why):
    return Mutant(name, caught, edits, why)


# ---- the anchors: lines of the production source, verbatim (the self-test finds each exactly once) -----------------------
COPY = "        ctx->CopySubresourceRegion(g_mips.texture, 0, 0, 0, 0, screen, 0, nullptr);\n"
GEN = "        ctx->GenerateMips(g_mips.srgbView);\n"
CENSUS = "        GpuCensusScope census(ctx, GpuCensusSection::FrameWorldMips);\n"
CACHE_IF = "    if (g_mips.unormView && g_mips.frameDone && g_mips.frame == frame && g_mips.source == screen) {\n"
HIT_RETURN = "        ++g_stats.frameHits;\n        return g_mips.unormView;\n"
END_RETURN = "    g_mips.frameDone = true;\n    return g_mips.unormView;\n"
SIZE_IF = ("    if (g_mips.texture && (g_mips.width != sd.Width || g_mips.height != sd.Height || g_mips.sourceFormat != sd.Format))\n"
           "        releaseMips();\n")
DEVICE_IF = "    if (g_mips.texture && g_mips.device != device.Get()) releaseMips();\n"
INTERNAL_SCREEN = "    // Everything from here reaches the device or the context.\n    VrWorldInternalScope internal;\n"
INTERNAL_SAMPLER = "        VrWorldInternalScope internal;\n        if (FAILED(device->CreateSamplerState(&want, &state)) || !state) {\n"
SAMPLER_DEVICE_IF = "    if (g_samplers.device && g_samplers.device != device) releaseSamplers();\n"
SAMPLER_HIT_IF = "        if (s.state && std::memcmp(&s.key, &want, sizeof want) == 0) {\n"
SAMPLER_HIT_BODY = "            ++g_stats.samplerHits;\n            return s.state;\n"
SAMPLER_OVERRIDES = ("    want.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;\n"
                     "    want.MipLODBias = 0.0f;\n"
                     "    want.MinLOD = 0.0f;\n"
                     "    want.MaxLOD = D3D11_FLOAT32_MAX;\n"
                     "    want.MaxAnisotropy = 1;\n"
                     "    want.ComparisonFunc = D3D11_COMPARISON_NEVER;\n")
EVICT = ("        take = &g_samplers.slot[0];\n"
         "        for (SamplerSlot& s : g_samplers.slot)\n"
         "            if (s.serial < take->serial) take = &s;\n"
         "        releaseAndNull(take->state);\n"
         "        ++g_stats.samplerEvictions;\n")
CREATION_NOTE = ('        Log::get().note("vr world mips: mipped screen %ux%u, %u levels, %.1f MB (linear-light mips through an sRGB view, "\n'
                 '                        "sampled through the UNORM view)",\n'
                 "                        d.Width, d.Height, d.MipLevels,\n"
                 "                        static_cast<double>(chainBytes(d.Width, d.Height, d.MipLevels)) / 1.0e6);\n")
LEAK = "        volatile char* leak = new char[16];\n        leak[0] = 1;\n"


def drop(old):
    return [(old, "")]


MUTANTS = [
    # ---- R1: mip 0 is a byte copy ----------------------------------------------------------------------------------------
    M("copy-skipped", "R1c", [(COPY, "        (void)screen;\n")], "the copy of mip 0 never happens"),
    M("copy-drops-the-last-row", "R1c",
      [(COPY, "        const D3D11_BOX box = {0, 0, 0, sd.Width, sd.Height - 1, 1};\n"
              "        ctx->CopySubresourceRegion(g_mips.texture, 0, 0, 0, 0, screen, 0, &box);\n")],
      "the copy leaves out the screen's last row"),
    # ---- R2: linear light -------------------------------------------------------------------------------------------------
    M("generate-skipped", "R2b", [(GEN, "")], "GenerateMips is never called"),
    M("generate-through-unorm-view", "R2b", [(GEN, "        ctx->GenerateMips(g_mips.unormView);\n")], "GenerateMips is called with the UNORM view"),
    M("srgb-view-is-unorm", "R2b", [("        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;\n", "        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;\n")],
      "the view GenerateMips reads through is made UNORM"),
    # ---- R3: the UNORM view, over every mip ---------------------------------------------------------------------------------
    M("returns-the-srgb-view", "R3b", [(END_RETURN, "    g_mips.frameDone = true;\n    return g_mips.srgbView;\n")], "the work path answers with the sRGB view"),
    M("unorm-view-is-srgb", "R3b", [("            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;\n", "            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;\n")],
      "the view the caller samples through is made sRGB"),
    M("unorm-view-one-mip", "R3d", [("            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;\n", "            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;\n            v.Texture2D.MipLevels = 1;\n")],
      "the UNORM view covers mip 0 only"),
    M("frame-hit-answers-srgb", "R5d", [(HIT_RETURN, "        ++g_stats.frameHits;\n        return g_mips.srgbView;\n")], "the cached answer is the sRGB view"),
    # ---- R4: the full chain -----------------------------------------------------------------------------------------------
    M("one-level", "R4b", [("    d.MipLevels = 0;                                // the full chain, down to the last level\n",
                            "    d.MipLevels = 1;\n")], "the texture has no mips"),
    M("four-levels", "R4b", [("    d.MipLevels = 0;                                // the full chain, down to the last level\n",
                              "    d.MipLevels = 4;\n")], "the chain stops at 4 levels"),
    M("width-rounded-down", ["R4c", "R4d"], [("    d.Width = src.Width;\n", "    d.Width = src.Width - 1;\n")], "the mipped texture is a texel narrower than the screen"),
    # ---- R5: once a frame, and what remakes the texture -----------------------------------------------------------------------
    M("frame-cache-never-hits", "R5e", [(CACHE_IF, CACHE_IF.replace("if (g_mips.unormView", "if (false && g_mips.unormView"))], "a second call redoes the work"),
    M("frame-cache-ignores-frame", "R5h", [(CACHE_IF, CACHE_IF.replace(" && g_mips.frame == frame", ""))], "a new frame is answered from the last one"),
    M("frame-cache-ignores-screen", "R5k", [(CACHE_IF, CACHE_IF.replace(" && g_mips.source == screen", ""))], "another screen in the same frame is answered from the first"),
    M("frame-not-recorded", "R5e", [("    g_mips.frame = frame;\n", "")], "the frame is never remembered"),
    M("frame-done-not-set", "R5e", [("    g_mips.frameDone = true;\n    return g_mips.unormView;\n", "    return g_mips.unormView;\n")], "the frame is never marked done"),
    M("source-not-recorded", "R5e", [("    g_mips.source = screen;\n", "")], "the screen is never remembered"),
    M("frame-hit-not-counted", "R5e", [(HIT_RETURN, "        return g_mips.unormView;\n")], "a frame hit is not counted"),
    M("copy-not-counted", "R5b", [("        ++g_stats.copies;\n", "")], "the copy is not counted"),
    M("generate-not-counted", "R5b", [("        ++g_stats.generates;\n", "")], "GenerateMips is not counted"),
    M("creation-not-counted", "R5b", [("    ++g_stats.creations;\n", "")], "a creation is not counted"),
    M("width-change-ignored", "R5p", [(SIZE_IF, SIZE_IF.replace("g_mips.width != sd.Width || ", ""))], "a new width does not remake the texture"),
    M("height-change-ignored", "R5p", [(SIZE_IF, SIZE_IF.replace("g_mips.height != sd.Height || ", ""))], "a new height does not remake the texture"),
    M("format-change-ignored", "R5p", [(SIZE_IF, SIZE_IF.replace(" || g_mips.sourceFormat != sd.Format", ""))], "a new source format does not remake the texture"),
    M("new-screen-remakes", ["R5j", "R5k"], [(SIZE_IF, SIZE_IF.replace("|| g_mips.sourceFormat != sd.Format", "|| g_mips.sourceFormat != sd.Format || g_mips.source != screen"))],
      "another screen of the same size remakes the texture (a game that alternates two would allocate 76 MB a frame)"),
    M("device-not-recorded", ["R5g", "R5h"], [("    g_mips.device = device;\n", "")], "the device is never remembered, so every new frame remakes the texture"),
    M("release-not-counted", "R5p", [("    if (g_mips.texture) ++g_stats.releases;\n", "")], "a release is not counted"),
    # ---- R6: refusals ---------------------------------------------------------------------------------------------------------
    M("no-srgb-rule", "R6b", [("    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return VrWorldMipsRefusal::SrgbTyped;\n", "")],
      "an sRGB texture is refused as any other format, not by its own reason"),
    M("srgb-accepted", "R6a", [("    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return VrWorldMipsRefusal::SrgbTyped;\n", ""),
                               ("    if (d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM)\n",
                                "    if (d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&\n"
                                "        d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)\n")],
      "an sRGB texture is taken"),
    M("multisampled-accepted", "R6a", drop("    if (d.SampleDesc.Count != 1) return VrWorldMipsRefusal::Multisampled;\n"), "a multisampled texture is taken"),
    M("array-accepted", "R6a", drop("    if (d.ArraySize != 1) return VrWorldMipsRefusal::TextureArray;\n"), "an array texture is taken"),
    M("mipped-accepted", "R6a", drop("    if (d.MipLevels != 1) return VrWorldMipsRefusal::AlreadyMipped;\n"), "an already-mipped texture is taken"),
    M("any-format-accepted", "R6a", [("    if (d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM)\n        return VrWorldMipsRefusal::NotRgba8;\n", "")],
      "any format is taken"),
    M("uint-accepted", "R6a", [("d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM)\n",
                                "d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UINT)\n")],
      "an R8G8B8A8_UINT texture is taken"),
    M("null-arguments-unchecked", "R6j", [("    if (!ctx || !screen) return refuse(VrWorldMipsRefusal::NullArgument, nullptr, S_OK);\n", "")], "a null context or texture is dereferenced"),
    M("refusal-not-counted", ["R6l", "R6b"], [("    ++g_stats.refusals[static_cast<int>(why)];\n", "")], "a refusal is not counted"),
    M("refusal-logged-every-time", "R6h",
      [("    for (uint32_t i = 0; i < g_seenCount; ++i)\n        if (std::memcmp(&g_seen[i], &key, sizeof key) == 0) return nullptr;\n", "")],
      "a repeated refusal is logged again"),
    M("refusal-cap-twelve", "R6s", [("constexpr uint32_t kRefusalLines = 8;\n", "constexpr uint32_t kRefusalLines = 12;\n")], "twelve distinct refusals are logged, not eight"),
    M("refusal-cap-four", ["R6f", "R6s"], [("constexpr uint32_t kRefusalLines = 8;\n", "constexpr uint32_t kRefusalLines = 4;\n")], "four distinct refusals are logged, not eight"),
    M("refusal-keyed-by-reason-only", ["R6f", "R6s"],
      [("        key.format = static_cast<uint32_t>(d->Format);\n        key.width = d->Width;\n        key.height = d->Height;\n", "        key.format = 0;\n")],
      "refusals of different textures for the same reason are one refusal"),
    M("refusal-lines-not-counted", "R6s", [("    ++g_stats.refusalLines;\n", "")], "the logged-refusal count is not kept"),
    M("refusal-line-missing", "R6f",
      [('    Log::get().note("vr world mips: refused the screen texture (%s: %s): %ux%u, DXGI format %u, %u samples, %u slices, "\n'
        '                    "%u levels%s; the eye route keeps the eye.",\n'
        "                    vrWorldMipsRefusalName(why), refusalExplanation(why), key.width, key.height, key.format, key.samples,\n"
        "                    key.slices, key.levels, failed);\n", "")],
      "a refusal is counted but never logged"),
    M("create-failure-not-refused", "R6u", [("        if (!createMips(device.Get(), sd, &failure)) return refuse(VrWorldMipsRefusal::CreateFailed, &sd, failure);\n",
                                             "        if (!createMips(device.Get(), sd, &failure)) return nullptr;\n")],
      "a failed creation is neither counted nor logged"),
    M("create-failure-leaks-srgb-view", "R6y", [("        releaseAndNull(unorm);\n        releaseAndNull(srgb);\n        releaseAndNull(texture);\n",
                                                 "        releaseAndNull(unorm);\n        releaseAndNull(texture);\n")],
      "a failed creation leaves the sRGB view behind"),
    M("create-failure-leaks-texture", "R6y", [("        releaseAndNull(unorm);\n        releaseAndNull(srgb);\n        releaseAndNull(texture);\n",
                                               "        releaseAndNull(unorm);\n        releaseAndNull(srgb);\n")],
      "a failed creation leaves the texture behind"),
    # ---- R7: the sampler ----------------------------------------------------------------------------------------------------
    M("sampler-filter-point", "R7c", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("D3D11_FILTER_MIN_MAG_MIP_LINEAR", "D3D11_FILTER_MIN_MAG_MIP_POINT"))], "the filter is point"),
    M("sampler-filter-kept", "R7c", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("    want.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;\n", ""))], "the game's filter is kept"),
    M("sampler-lod-bias-kept", "R7f", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("    want.MipLODBias = 0.0f;\n", ""))], "the game's LOD bias is kept"),
    M("sampler-min-lod-kept", "R7g", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("    want.MinLOD = 0.0f;\n", ""))], "the game's MinLOD is kept"),
    M("sampler-max-lod-zero", "R7h", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("D3D11_FLOAT32_MAX", "0.0f"))], "the LOD range ends at 0"),
    M("sampler-max-lod-kept", "R7h", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("    want.MaxLOD = D3D11_FLOAT32_MAX;\n", ""))], "the game's MaxLOD is kept"),
    M("sampler-anisotropy-kept", "R7i", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("    want.MaxAnisotropy = 1;\n", ""))], "the game's MaxAnisotropy is kept"),
    M("sampler-comparison-kept", "R7j", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES.replace("    want.ComparisonFunc = D3D11_COMPARISON_NEVER;\n", ""))], "the game's comparison function is kept"),
    M("sampler-address-u-clamped", "R7d", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES + "    want.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;\n")], "address mode U is forced to clamp"),
    M("sampler-address-v-wrapped", "R7d", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES + "    want.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;\n")], "address mode V is forced to wrap"),
    M("sampler-address-w-clamped", "R7d", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES + "    want.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;\n")], "address mode W is forced to clamp"),
    M("sampler-border-dropped", "R7e", [(SAMPLER_OVERRIDES, SAMPLER_OVERRIDES + "    std::memset(want.BorderColor, 0, sizeof want.BorderColor);\n")], "the border colour is cleared"),
    M("sampler-no-cache", "R7r", [(SAMPLER_HIT_IF, "        if (false && s.state && std::memcmp(&s.key, &want, sizeof want) == 0) {\n")], "the table never hits"),
    M("sampler-hit-not-counted", "R7r", [(SAMPLER_HIT_BODY, "            return s.state;\n")], "a table hit is not counted"),
    M("sampler-keyed-by-game-desc", "R7t",
      [("    D3D11_SAMPLER_DESC want = game;\n", "    D3D11_SAMPLER_DESC want = game;\n    const D3D11_SAMPLER_DESC asGiven = game;\n"),
       (SAMPLER_HIT_IF, "        if (s.state && std::memcmp(&s.key, &asGiven, sizeof asGiven) == 0) {\n"),
       ("    take->key = want;\n", "    take->key = asGiven;\n")],
      "the table is keyed by the game's desc as given, so a game sampler that differs only in what the route overrides is another entry"),
    M("evict-newest", "R7z", [(EVICT, EVICT.replace("s.serial < take->serial", "s.serial > take->serial"))], "the newest entry is evicted, not the oldest"),
    M("evict-leaks", "R7z", [(EVICT, EVICT.replace("        releaseAndNull(take->state);\n", "        take->state = nullptr;\n"))], "an evicted sampler is forgotten without being released"),
    M("evict-not-counted", "R7y", [(EVICT, EVICT.replace("        ++g_stats.samplerEvictions;\n", ""))], "an eviction is not counted"),
    M("table-of-sixteen", "R7y", [("constexpr int kSamplerSlots = 8;\n", "constexpr int kSamplerSlots = 16;\n")], "the table holds sixteen samplers"),
    M("table-of-four", "R7w", [("constexpr int kSamplerSlots = 8;\n", "constexpr int kSamplerSlots = 4;\n")], "the table holds four samplers"),
    M("sampler-failure-not-counted", "R7ai", [("            ++g_stats.samplerFailures;\n", "")], "a failed sampler creation is not counted"),
    M("sampler-device-change-ignored", "R9y", [(SAMPLER_DEVICE_IF, "")], "a sampler for another device is answered from the first device's table"),
    # ---- R8: the hooks ---------------------------------------------------------------------------------------------------------
    M("no-internal-scope-on-screen", "R8t", [(INTERNAL_SCREEN, "")], "the D3D calls run outside a VrWorldInternalScope"),
    M("internal-scope-only-on-the-work", "R8t",
      [(INTERNAL_SCREEN, ""), (CENSUS, CENSUS + "        VrWorldInternalScope internal;\n")],
      "the scope covers the copy and GenerateMips but not the creation"),
    M("no-internal-scope-on-sampler", "R8aj", [(INTERNAL_SAMPLER, INTERNAL_SAMPLER.replace("        VrWorldInternalScope internal;\n", ""))], "CreateSamplerState runs outside the scope"),
    M("no-census-scope", ["R8u", "R8w"], [(CENSUS, "")], "the GPU work is not timed"),
    M("census-on-the-wrong-section", "R8x", [(CENSUS, CENSUS.replace("FrameWorldMips", "FrameWorldResolve"))], "the work is timed on another section"),
    M("census-opens-after-the-copy", "R8u", [(CENSUS + "        // Mip 0 is a byte copy; the format family is shared, so typeless or UNORM in, typeless out.\n" + COPY,
                                              "        // Mip 0 is a byte copy; the format family is shared, so typeless or UNORM in, typeless out.\n" + COPY + CENSUS)],
      "the census scope brackets GenerateMips but not the copy"),
    M("generate-before-copy", "R8s", [(COPY + "        ++g_stats.copies;\n" + "        // The box filter averages in linear light through the sRGB view and stores the result re-encoded.\n" + GEN,
                                       "        // The box filter averages in linear light through the sRGB view and stores the result re-encoded.\n" + GEN + COPY + "        ++g_stats.copies;\n")],
      "GenerateMips runs before the copy, on last frame's mip 0"),
    M("generate-mips-flag-missing", "R8j", [("    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;\n", "")], "the texture is not made with GENERATE_MIPS"),
    M("render-target-bind-missing", ["R8b", "R8i"], [("    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;   // GenerateMips renders the levels\n",
                                              "    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;\n")], "the texture cannot be rendered to, so GenerateMips cannot fill its levels"),
    # ---- R9: lifetime ----------------------------------------------------------------------------------------------------------
    M("resize-forgets-without-releasing", "R9c", [(SIZE_IF, SIZE_IF.replace("        releaseMips();\n", "        g_mips = Mips{};\n"))], "a new size drops the old objects without releasing them"),
    M("release-misses-srgb-view", "R9d", [("    releaseAndNull(g_mips.srgbView);\n", "")], "a release leaves the sRGB view"),
    M("release-misses-unorm-view", "R9c", [("    releaseAndNull(g_mips.unormView);\n", "")], "a release leaves the UNORM view"),
    M("release-misses-texture", "R9e", [("    releaseAndNull(g_mips.texture);\n", "")], "a release leaves the texture"),
    M("device-change-ignored", "R9r", [(DEVICE_IF, "")], "a screen on another device is served from the first device's texture"),
    M("reset-keeps-the-mips", "R9h", [("void vrWorldMipsReset() {\n    releaseMips();\n    releaseSamplers();\n}\n", "void vrWorldMipsReset() {\n    releaseSamplers();\n}\n")],
      "a reset leaves the texture"),
    M("reset-keeps-the-samplers", "R9h", [("void vrWorldMipsReset() {\n    releaseMips();\n    releaseSamplers();\n}\n", "void vrWorldMipsReset() {\n    releaseMips();\n}\n")],
      "a reset leaves the samplers"),
    M("samplers-forgotten-not-released", "R9l", [("    for (SamplerSlot& s : g_samplers.slot) releaseAndNull(s.state);\n", "    for (SamplerSlot& s : g_samplers.slot) s.state = nullptr;\n")],
      "releasing the sampler table forgets the samplers without releasing them"),
    # ---- R10: no heap allocation on the warm paths ---------------------------------------------------------------------------------
    M("allocates-on-frame-hit", "R10d", [(HIT_RETURN, "        ++g_stats.frameHits;\n" + LEAK + "        return g_mips.unormView;\n")], "a frame hit allocates"),
    M("allocates-on-new-frame", "R10e", [("    g_mips.source = screen;\n", LEAK.replace("        ", "    ") + "    g_mips.source = screen;\n")], "the copy and GenerateMips path allocates"),
    M("allocates-on-repeated-refusal", "R10f",
      [("        if (std::memcmp(&g_seen[i], &key, sizeof key) == 0) return nullptr;\n",
        "        if (std::memcmp(&g_seen[i], &key, sizeof key) == 0) {\n" + LEAK.replace("        ", "            ") + "            return nullptr;\n        }\n")],
      "a repeated refusal allocates"),
    M("allocates-on-sampler-hit", "R10g", [(SAMPLER_HIT_BODY, "            ++g_stats.samplerHits;\n" + LEAK.replace("        ", "            ") + "            return s.state;\n")], "a table hit allocates"),
    # ---- R11: the log -------------------------------------------------------------------------------------------------------------
    M("creation-line-missing", "R11c", [(CREATION_NOTE, "")], "a creation writes no line"),
    M("creation-line-twice", "R11c", [(CREATION_NOTE, CREATION_NOTE + CREATION_NOTE)], "a creation writes its line twice"),
    M("creation-megabytes-are-mebibytes", "R11d", [(CREATION_NOTE, CREATION_NOTE.replace("1.0e6", "1048576.0"))], "the size is in 2^20 bytes, not 10^6"),
    M("creation-line-words", "R11d", [(CREATION_NOTE, CREATION_NOTE.replace("mipped screen", "mip screen"))], "the creation line says something else"),
    M("creation-cap-gone", "R11h", [("constexpr uint32_t kCreationLines = 8;\n", "constexpr uint32_t kCreationLines = 100;\n")], "every creation is logged"),
    M("creation-lines-not-counted", "R11h", [("        ++g_stats.creationLines;\n", "")], "the logged-creation count is not kept"),
]


# ---- applying an edit ----------------------------------------------------------------------------------------------------
def apply_edits(text, edits, name="?"):
    """The text with each (old, new) applied in order; each `old` must occur exactly once in the text as it stands then."""
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise ValueError("mutation %s: an anchor occurs %d times (want 1): %r" % (name, count, old[:90]))
        if old == new:
            raise ValueError("mutation %s: an edit changes nothing: %r" % (name, old[:90]))
        text = text.replace(old, new)
    return text


def read_source(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def parse_fail(output):
    """The text after the last 'FAIL: ' line of the rig's output, or None when it printed none."""
    found = None
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            found = line[len("FAIL: "):]
    return found


def rig_cases(rig_text):
    return set(re.findall(r'\{"(R\d+)", case\w+\}', rig_text))


def rig_label_exists(rig_text, prefix):
    return ('"%s:' % prefix) in rig_text


def label_block(bat_text, label):
    """The lines of one build.bat subroutine, continuation lines (a trailing ^) joined; None when the label is absent."""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.startswith(":") and not line.startswith("::"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the
    environment they need: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

    def __init__(self):
        found = shutil.which("cl.exe")
        if found:
            self.env = os.environ.copy()
        else:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
            bat = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if not vs or not bat.is_file():
                raise RuntimeError("cl.exe is not on PATH and no Visual Studio with the x64 C++ tools was found")
            dump = subprocess.run('cmd.exe /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True).stdout
            self.env = {}
            for line in dump.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    self.env[key] = value
            path = next((v for k, v in self.env.items() if k.upper() == "PATH"), "")
            found = shutil.which("cl.exe", path=path)
            if not found:
                raise RuntimeError("vcvars64.bat did not put cl.exe on PATH")
        self.cl = str(found)
        self.link = str(Path(found).with_name("link.exe"))


def cl_compile(tc, source, outdir, includes, extra=()):
    cmd = [tc.cl] + CL_FLAGS + ["/c"] + ["/I" + str(i) for i in includes] + list(extra) + ["/Fo" + str(outdir) + os.sep, str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace")
    return done.returncode, (done.stdout + done.stderr)


def link_exe(tc, exe, objs):
    cmd = [tc.link, "/nologo", "/INCREMENTAL:NO", "/OUT:" + str(exe)] + [str(o) for o in objs] + LINK_LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace")
    return done.returncode, (done.stdout + done.stderr)


def run_rig(exe, rule, tc):
    """(outcome, detail) of one run of the rig: 'pass', 'fail' (detail = the label), 'crash', 'timeout'."""
    cmd = [str(exe), "--self-test"] + (["--only", rule] if rule else [])
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    if done.returncode == 0:
        return "pass", ""
    label = parse_fail(done.stdout + "\n" + done.stderr)
    if label is None:
        return "crash", "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", label


# ---- the run ---------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each is compiled in a temp directory outside the repo, linked with the rig's object and run on its rule's cases:" % len(mutants)]
    for m in mutants:
        lines.append("  %-38s rule %-4s caught by %-14s %s" % (m.name, m.rule, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS) + "; libs: " + " ".join(LINK_LIBS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    prod = read_source(PROD)
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="vr_world_mips_mutants_"))
    includes = [ROOT / "src" / "d3d11"]
    try:
        rigdir = work / "rig"
        rigdir.mkdir()
        code, text = cl_compile(tc, RIG, rigdir, [])
        if code != 0:
            print(text, file=out)
            print("the rig does not compile", file=out)
            return 1
        rig_obj = rigdir / "vr_world_mips_test.obj"

        def build_and_run(name, source_text, rule):
            d = work / name
            d.mkdir()
            src = d / "vr_world_mips.cpp"
            src.write_text(source_text, encoding="utf-8", newline="\n")
            code, text = cl_compile(tc, src, d, includes)
            if code != 0:
                return "nocompile", text.strip().splitlines()[-1] if text.strip() else ""
            exe = d / "rig.exe"
            code, text = link_exe(tc, exe, [rig_obj, d / "vr_world_mips.obj"])
            if code != 0:
                return "nocompile", "link: " + (text.strip().splitlines()[-1] if text.strip() else "")
            return run_rig(exe, rule, tc)

        control, detail = build_and_run("control", prod, None)
        print("control (the unmutated source, every case): %s %s" % (control, detail), file=out)
        if control != "pass":
            print("the rig does not pass on the unmutated module when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                text = apply_edits(prod, m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(m.name, text, m.rule)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p + ":") or detail.startswith(p) for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-38s %-4s %s" % (verdict, m.name, m.rule, detail[:150]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own rule, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(parse_fail("a\nFAIL: R1c: x\nb\n") == "R1c: x" and parse_fail("PASS: 3\n") is None, "parse_fail reads the last FAIL: line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"R1", caseR1}, {"R10", caseR10}') == {"R1", "R10"}, "rig_cases reads the case table")

    # every mutation against the source as it is now, and against the rig
    prod = read_source(PROD)
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 60, "the mutation list did not shrink (%d)" % len(MUTANTS))
    for m in MUTANTS:
        try:
            mutated = apply_edits(prod, m.edits, m.name)
            check(mutated != prod, "%s changes the source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(rig_label_exists(rig, p), "%s: the rig has no check labelled %s" % (m.name, p))
        check(m.rule in cases, "%s: the rig has no case %s" % (m.name, m.rule))
    rules = {m.rule for m in MUTANTS}
    check(rules == cases, "every rule of the rig has a mutation, and only rules of the rig: %s vs %s" % (sorted(rules), sorted(cases)))

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\vr_world_mips_test\\vr_world_mips_test.cpp" in cl and "src\\d3d11\\vr_world_mips.cpp" in cl, "build.bat compiles the rig and the production module")
        check("d3d11.lib" not in cl.replace("d3dcompiler.lib", ""), "the rig does not link d3d11.lib (it takes System32's device through system_d3d11.h)")
        for lib in LINK_LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("vr_world_mips_test.exe\" --dry-run" in text and "vr_world_mips_test.exe\" --self-test" in text, "build.bat runs the rig's --dry-run and --self-test")
        check("mutants.py\" --self-test" in text, "build.bat runs this tool's --self-test")

    # --dry-run starts nothing and writes nothing
    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: vr_world_mips_test mutants.py self-test (%d mutations over %d rules, every anchor found once, build.bat wired)" % (len(MUTANTS), len(rules)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
