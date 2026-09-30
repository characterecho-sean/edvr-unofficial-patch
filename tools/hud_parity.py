#!/usr/bin/env python3
"""Gate G-F parity error: |T(F*(1-a)+L) - (T(F)*(1-a)+T(L))| on HUD pixels.

    python tools/hud_parity.py <ledger_dir>            # auto-find panels_*/tonemap_* pair
    python tools/hud_parity.py panels.bin tonemap.bin  # explicit pair (order sniffed by magic)
    python tools/hud_parity.py <dir> --verbose --budget 2.0 --bright-luma 1.0

Stock Elite composites the cockpit HUD into the HDR target BEFORE exposure
and tonemap, so a pixel computes T(F*(1-a) + L): T the game's tonemap, F the
HDR background, L the HUD's premultiplied HDR radiance, a coverage. The
crisp-HUD design (docs/cockpit-hud-layer-design-2026-09-27.md, "Parity",
design point 5) tonemaps the HUD separately and composites after:
T(F)*(1-a) + T(L). The two agree where the HUD is opaque or the background
dark; they differ where a translucent HUD crosses a bright background.
Gate G-F measures that error on HUD pixels (p50/p99, in 8-bit steps) from
one in-sim capture BEFORE Phase 1 may ship.

Inputs are the two bins the game-side capture writes on an F10 eye-run
ledger (src/d3d11/object_probe.cpp:934-937):

  * panels_<stamp>.bin  (EDVRPNL1, src/d3d11/eye_panel_snapshot.h:452-506):
    per holo-panel draw (vs 81216C77F90DEDD6 / ps A2965EC2931A39C8,
    premultiplied ONE/INV_SRC_ALPHA): rtv_before = central crop of the HDR
    target BEFORE the draw (= F), rtv_after = the same crop AFTER
    (= F*(1-a)+L), ps_t2 = the interface surface the draw samples, plus
    geometry (ib/vb*, layout), constant buffers and blend/depth descs.
    Crops are R11G11B10_FLOAT (eye_panel_snapshot.h:211-212), tightly
    packed per mip row (eye_panel_snapshot.h:495-499).
  * tonemap_<stamp>.bin (EDVRTON1, src/d3d11/eye_tonemap_snapshot.h:226-253):
    blobs per tonemap draw: [0] exposure scalar (R32), [1] colour LUT (3D),
    [2] HDR source (central crop, <=1400^2), [3] tonemap output (same-region
    crop, RGBA8), [4] PS b2, [5] VB0, plus VS/PS bytecode.

How T is recovered: the HDR crop (blob 2) and the output crop (blob 3) are
the same screen region (meta origin/size are compared and the pairing
intersected if they only partially overlap), so per-pixel pairs
(hdr_rgb, out_rgb) exist. T is fit per channel as a MONOTONE
non-decreasing interpolation of out vs hdr, pooling all captured pixels.
Assumptions this leans on, and their failure modes:

  * SPATIAL UNIFORMITY: the tonemap is a full-screen pass with no spatial
    terms, so T pooled over the captured region is valid everywhere in the
    eye. Pooling stays valid even if the panel crops fall outside the
    tonemap crop; per-channel RANGE extrapolation beyond the fitted HDR
    span is clamped and counted (oor_share).
  * PER-CHANNEL MONOTONICITY: a 3D colour LUT (EDHM) can mix channels; a
    per-channel fit then misstates T. The fit residual (p50/p99 against the
    captured out bytes) is reported; treat the verdict as unreliable when
    it is large.
  * EXPOSURE: whatever scalar exposure was bound is already baked into the
    captured hdr->out pairs, so the fitted T is the exposure-applied T of
    THIS frame, which is exactly the T the parity question needs. The
    exposure scalar (blob 0, first element) is reported for the record.

How a and L are recovered per panel draw, per pixel inside the draw's
footprint on the rtv crop:

  * PREFERRED (route=geometry): the draw is replayed offline. clip position
    = dp4 of the vertex POSITION with vs_b0 rows 4..7 (the ForwardDp4
    recipe for vs 81216C77F90DEDD6, src/d3d11/flat_projection_recipes.h:93
    and flat_projection_math.h:66-78), NDC through the captured viewport,
    triangles rasterized, surface UV interpolated perspective-correctly
    from TEXCOORD0, then a = surface alpha and L = surface rgb at that UV
    (ps_t2, nearest texel; sRGB views decoded to linear). Instanced draws
    or layouts without POSITION/TEXCOORD0 float elements are not replayed.
  * CROSS-CHECK: the blend identity after = before*(1-a) + L must hold on
    the captured pair. The residual |after - (before*(1-a)+L)| is reported
    per draw (p50/p99/max, HDR units); a large residual invalidates the
    whole a/L chain (wrong UV mapping, glow alpha the surface does not
    carry, depth-rejected pixels misread). The rasterized footprint alone
    is the coverage evidence: stock before/after equality NEVER excludes a
    pixel, because a translucent draw can leave the stock HDR exactly
    unchanged (F*(1-a)+L == F) while the separated composition differs by
    tens of steps. Genuinely depth-rejected pixels surface through the
    cross-check residual (L != a*F there) and the ceiling below, instead of
    being silently dropped.
  * FALLBACK (route=luminance): where geometry is not decodable, a is
    solved per pixel from luma mixing against a single estimated HUD
    radiance Lbar (median of after over the darkest-decile footprint
    pixels) and L = after - before*(1-a) exactly. This assumes a spatially
    constant HUD radiance and F != Lbar; it is wrong for multi-coloured
    panels. The limitation is printed whenever this route is used. Pixels
    whose solved a claims coverage but that stock left unchanged cannot be
    resolved on this route (after == before makes the luma equation
    degenerate); they are counted and printed as an AMBIGUOUS class with
    the possible parity error bounded per pixel by
    max_a |T(F) - (1-a)*T(F) - T(a*F)| (L == a*F is forced there), never
    silently dropped.

Metric: err = |T(F*(1-a)+L) - (T(F)*(1-a)+T(L))| per HUD pixel (a > eps),
max over RGB, in 8-bit steps (x255; note T(after) is the stock term since
after = F*(1-a)+L by capture). Reported pooled and per draw: p50/p99/max
and counts; plus the share of HUD pixels in the regime that matters
(0 < a < 1 AND background luma above --bright-luma, default 1.0 HDR) and
the p99 restricted to that regime. VERDICT: a PASS requires ALL of

  1. a PROVEN PAIR: the capture stamps match (when both file names carry
     one), the tonemap draw's frame equals the panels frame, and the
     tonemap draw's HDR resource equals the panels target identity. No
     unique compatible draw is INVALID; --allow-mismatched measures anyway
     under a banner and caps the outcome at non-PASS.
  2. RESIDUALS UNDER CEILING: T fit residual p99 <= 4.0 steps and every
     geometry draw's blend cross-check p99 <= 0.25 HDR units. The ceilings
     come from the flown ledgers: flight 2's fit residual p99 ran
     1.45-2.53 steps with a clean cross-check, so 4.0 keeps ~1.6x headroom
     over the worst valid flight while a cross-channel EDHM LUT or a
     corrupt capture lands far past it; 0.25 is 5x the R11G11B10 rounding
     scale of consistent captures (<0.05) and a quarter of the corrupt
     blend-chain fixture (1.03 HDR units).
  3. the restricted p99 (or the overall p99 when the regime has <32
     samples) under --budget (default 2.0 8-bit steps, the design doc's
     G-F budget), as before.

Anything else is INVALID with the reason printed. Exit 0 PASS, 1 FAIL
budget, 2 data/INVALID.

What this does NOT prove: T here is an empirical per-channel replay, not
the game's tonemap shader (EDHM 3D-LUT cross-channel terms degrade it;
watch the fit residual); a and L come from the sampled surface, not the
pixel shader's exact output (the holo PS adds a smear of the surface alpha
and t0 grain terms, so the cross-check residual is the chain's honesty
check); geometry replay assumes the dp4-rows convention and pass-through
TEXCOORD0 for the surface UV, which a game update could change; the
verdict covers the captured frame only, and says nothing about bloom,
SMAA, or the look Sean actually ships.
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import eye_panel_snapshot as pnl
import eye_tonemap_snapshot as ton

# DXGI formats used here.
R32G32B32A32_FLOAT, R16G16B16A16_FLOAT = 2, 10
R11G11B10_FLOAT = 26
RGBA8_TYPELESS, RGBA8_UNORM, RGBA8_SRGB = 27, 28, 29
R32_FLOAT, R8_UNORM = 41, 61
BC1_UNORM, BC1_SRGB = 71, 72
R16_UINT, R32_UINT = 57, 42

EPS_A = 0.5 / 255.0           # coverage below this is not a HUD pixel
REGIME_MIN = 32               # samples before the restricted p99 drives the verdict
LUMA = (0.2126, 0.7152, 0.0722)

# Residual ceilings for the INVALID verdict (rationale in the docstring):
# flight 2's ledgers ran a T-fit residual p99 of 1.45-2.53 steps and a clean
# cross-check, so PASS allows ~1.6x headroom on the fit and 5x the R11G11B10
# rounding scale (<0.05 HDR units) on the cross-check.
FIT_P99_CEILING = 4.0       # 8-bit steps
XCHECK_P99_CEILING = 0.25   # HDR units


def fail(message):
    raise ValueError(message)


class InvalidData(ValueError):
    """The captures cannot support a gate verdict; reported as INVALID (exit 2)."""


# ---------------------------------------------------------------- formats

def unpack_r11g11b10(words):
    """uint32 array -> (..., 3) float64. 5-bit exp (bias 15), 6/6/5 mantissa."""
    w = np.asarray(words, dtype=np.uint32)

    def chan(bits, mbits):
        exp = (bits >> mbits).astype(np.float64)
        mant = (bits & ((1 << mbits) - 1)).astype(np.float64)
        normal = (1.0 + mant / (1 << mbits)) * np.power(2.0, exp - 15.0)
        sub = mant * 2.0 ** (-14 - mbits)
        return np.where(exp == 0.0, sub, normal)

    return np.stack([chan(w & 0x7FF, 6), chan((w >> 11) & 0x7FF, 6), chan(w >> 22, 5)], axis=-1)


def _srgb_to_linear(c):
    return np.where(c > 0.04045, ((c + 0.055) / 1.055) ** 2.4, c / 12.92)


def _decode_bc1(data, width, height):
    raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, 8)
    c0 = raw[:, 0].astype(np.uint16) | (raw[:, 1].astype(np.uint16) << 8)
    c1 = raw[:, 2].astype(np.uint16) | (raw[:, 3].astype(np.uint16) << 8)
    bits = raw[:, 4:].reshape(-1, 4).astype(np.uint32)
    idx = np.zeros((len(raw), 16), dtype=np.uint8)
    for i in range(4):
        idx[:, i * 4:(i + 1) * 4] = ((bits[:, i:i + 1] >> (2 * np.arange(4))) & 3).astype(np.uint8)

    def rgb565(c):
        r = ((c >> 11) & 31) / 31.0
        g = ((c >> 5) & 63) / 63.0
        b = (c & 31) / 31.0
        return np.stack([r, g, b], axis=-1)

    p0, p1 = rgb565(c0), rgb565(c1)
    pal = np.zeros((len(raw), 4, 4))
    pal[:, 0, :3], pal[:, 0, 3] = p0, 1.0
    pal[:, 1, :3], pal[:, 1, 3] = p1, 1.0
    four = (c0 > c1)[:, None]
    pal[:, 2, :3] = np.where(four, (2 * p0 + p1) / 3.0, (p0 + p1) / 2.0)
    pal[:, 2, 3] = 1.0
    pal[:, 3, :3] = np.where(four, (p0 + 2 * p1) / 3.0, 0.0)
    pal[:, 3, 3] = np.where(four, 1.0, 0.0)
    px = pal[np.arange(len(raw))[:, None], idx]
    bw, bh = (width + 3) // 4, (height + 3) // 4
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:height, :width]


def decode_texture(fmt, data, width, height):
    """Tightly packed texture payload -> (H, W, 4) float64 in 0..1 (or HDR)."""
    if fmt == R11G11B10_FLOAT:
        words = np.frombuffer(data, dtype='<u4').reshape(height, width)
        rgb = unpack_r11g11b10(words)
        return np.concatenate([rgb, np.ones((*rgb.shape[:2], 1))], axis=-1)
    if fmt in (RGBA8_TYPELESS, RGBA8_UNORM, RGBA8_SRGB):
        rgba = np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4) / 255.0
        if fmt == RGBA8_SRGB:
            rgba = rgba.copy()
            rgba[..., :3] = _srgb_to_linear(rgba[..., :3])
        return rgba
    if fmt == R16G16B16A16_FLOAT:
        return np.frombuffer(data, dtype='<f2').reshape(height, width, 4).astype(np.float64)
    if fmt == R32G32B32A32_FLOAT:
        return np.frombuffer(data, dtype='<f4').reshape(height, width, 4).astype(np.float64)
    if fmt in (BC1_UNORM, BC1_SRGB):
        rgba = _decode_bc1(data, width, height)
        if fmt == BC1_SRGB:
            rgba[..., :3] = _srgb_to_linear(rgba[..., :3])
        return rgba
    if fmt == R8_UNORM:
        g = np.frombuffer(data, dtype=np.uint8).reshape(height, width, 1) / 255.0
        return np.concatenate([g, g, g, np.ones_like(g)], axis=-1)
    if fmt == R32_FLOAT:
        g = np.frombuffer(data, dtype='<f4').reshape(height, width, 1).astype(np.float64)
        return np.concatenate([g, g, g, np.ones_like(g)], axis=-1)
    fail(f'unsupported texture format {fmt}')


def panel_texture(blob, mip_level=None):
    """A rtv/ps_t blob -> (H, W, 4) float64, using the real parser's checks."""
    pnl.verify_blob(blob)
    m = blob.meta
    level = m.get('view_first_mip', 0) if mip_level is None else mip_level
    mip = m['mips'][level]
    data = blob.payload[mip['offset']:mip['offset'] + mip['size']]
    return decode_texture(m['storage_format'], data, mip['width'], mip['height'])


# ------------------------------------------------------------------ T fit

class Curve:
    def __init__(self, xs, ys):
        self.x, self.y = xs, ys
        self.xmax = float(xs[-1]) if len(xs) else 0.0

    def __call__(self, v):
        return np.interp(np.clip(np.asarray(v, dtype=np.float64), 0.0, None), self.x, self.y)

    def oor(self, v):
        v = np.asarray(v, dtype=np.float64)
        return float(np.mean(v > self.xmax)) if v.size else 0.0


def fit_channel(h, o, bins=1024):
    """Monotone (non-decreasing) interpolation of out byte vs hdr, log-spaced bins."""
    h = np.clip(h, 0.0, None)
    xmax = float(h.max()) if h.size else 0.0
    if not h.size or xmax <= 0.0:
        mean = float(o.mean()) if o.size else 0.0
        return Curve(np.array([0.0, 1.0]), np.array([mean, mean]))
    umax = math.log2(1.0 + xmax)
    edges = np.linspace(0.0, umax, bins + 1)
    idx = np.clip(np.digitize(np.log2(1.0 + h), edges) - 1, 0, bins - 1)
    cnt = np.bincount(idx, minlength=bins).astype(np.float64)
    sx = np.bincount(idx, weights=h, minlength=bins)
    sy = np.bincount(idx, weights=o, minlength=bins)
    ok = cnt > 0
    xm, ym = sx[ok] / cnt[ok], sy[ok] / cnt[ok]
    order = np.argsort(xm)
    xm, ym = xm[order], np.maximum.accumulate(ym[order])
    return Curve(xm, ym)


def fit_tonemap(draw):
    """Per-channel monotone T from one EDVRTON1 draw's HDR/output crop pair."""
    blobs = draw['blobs']
    hdr_m, out_m = blobs[2]['meta'], blobs[3]['meta']
    if not blobs[2]['data'] or not blobs[3]['data']:
        fail('tonemap draw lacks the HDR or output crop payload')
    fmt_h = hdr_m.get('view_format', hdr_m.get('format'))
    if fmt_h not in (R11G11B10_FLOAT, R16G16B16A16_FLOAT, R32G32B32A32_FLOAT):
        fail(f'tonemap HDR crop format {fmt_h} not decodable')
    if out_m.get('format') != RGBA8_TYPELESS and out_m.get('view_format') not in (RGBA8_UNORM, RGBA8_SRGB, RGBA8_TYPELESS):
        fail(f'tonemap output crop view format {out_m.get("view_format")} unexpected')
    hs, os_ = hdr_m['size'], out_m['size']
    # The two crops are the same screen region of equal-size textures; align
    # them in absolute texture coordinates and intersect on partial overlap.
    x0 = max(hdr_m['origin'][0], out_m['origin'][0])
    y0 = max(hdr_m['origin'][1], out_m['origin'][1])
    x1 = min(hdr_m['origin'][0] + hs[0], out_m['origin'][0] + os_[0])
    y1 = min(hdr_m['origin'][1] + hs[1], out_m['origin'][1] + os_[1])
    if x1 <= x0 or y1 <= y0:
        fail(f'tonemap crops do not overlap: hdr origin {hdr_m["origin"]} size {hs}, '
             f'out origin {out_m["origin"]} size {os_}')
    aligned = (x0, y0, x1, y1) == (hdr_m['origin'][0], hdr_m['origin'][1],
                                   hdr_m['origin'][0] + hs[0], hdr_m['origin'][1] + hs[1]) \
        and hs[:2] == os_[:2]
    hdr = decode_texture(fmt_h, blobs[2]['data'], hs[0], hs[1])
    out = np.frombuffer(blobs[3]['data'], dtype=np.uint8).reshape(os_[1], os_[0], 4)
    sl_h = np.s_[y0 - hdr_m['origin'][1]:y1 - hdr_m['origin'][1],
                 x0 - hdr_m['origin'][0]:x1 - hdr_m['origin'][0]]
    sl_o = np.s_[y0 - out_m['origin'][1]:y1 - out_m['origin'][1],
                 x0 - out_m['origin'][0]:x1 - out_m['origin'][0]]
    h = hdr[sl_h][..., :3].reshape(-1, 3).astype(np.float64)
    o = out[sl_o][..., :3].reshape(-1, 3).astype(np.float64)
    curves = [fit_channel(h[:, c], o[:, c]) for c in range(3)]
    resid = np.stack([curves[c](h[:, c]) - o[:, c] for c in range(3)], axis=-1)
    exposure = blobs[0]['data'] and struct.unpack('<f', blobs[0]['data'][:4])[0] or float('nan')

    def apply(v):
        return np.stack([curves[c](v[..., c]) for c in range(3)], axis=-1)

    return {'apply': apply, 'curves': curves, 'pooled': int(h.shape[0]),
            'fit_p50': float(np.percentile(np.abs(resid), 50)),
            'fit_p99': float(np.percentile(np.abs(resid), 99)),
            'exposure': exposure, 'aligned': bool(aligned),
            'region': [int(x0), int(y0), int(x1), int(y1)]}


# ------------------------------------------------------------- panel draw

def words_to_floats(words):
    return struct.unpack(f'<{len(words)}f', struct.pack(f'<{len(words)}I', *words))


def gather_floats(payload, stride, binding, elem_off, capture_off, indices, ncomp):
    """Per-vertex float element from a captured VB window, absolute indices."""
    base = binding + np.asarray(indices, dtype=np.int64) * stride + elem_off
    rel = base - capture_off
    if len(rel) and (rel.min() < 0 or rel.max() + 4 * ncomp > len(payload)):
        fail('vertex element outside the retained VB window')
    raw = np.frombuffer(payload, dtype=np.uint8)
    pick = raw[rel[:, None] + np.arange(4 * ncomp)[None, :]]
    return pick.copy().view('<f4').reshape(len(rel), ncomp)


def replay_geometry(draw, blobs, crop_shape):
    """Rasterize the draw into the rtv crop. Returns (mask, uv, reason)."""
    info = draw.info
    h_crop, w_crop = crop_shape
    if info.get('instances', 1) != 1:
        return None, None, f"{info.get('instances')} instances"
    layout = info.get('layout') or []
    pos_el = uv_el = None
    for sem, idx, fmt, slot, off, cls, step in layout:
        if cls != 0:
            return None, None, 'instanced input layout'
        s = sem.upper()
        if s == 'POSITION' and idx == 0 and pos_el is None:
            pos_el = (fmt, slot, off)
        if s == 'TEXCOORD' and idx == 0 and uv_el is None:
            uv_el = (fmt, slot, off)
    if pos_el is None or pos_el[0] not in (2, 6):
        return None, None, 'no float POSITION element'
    if uv_el is None or uv_el[0] not in (2, 6, 16):
        return None, None, 'no float TEXCOORD0 element'
    npos = 4 if pos_el[0] == 2 else 3
    nuv = 2 if uv_el[0] == 16 else (4 if uv_el[0] == 2 else 3)
    kind = chr(info.get('kind', 0))
    count = info.get('count', 0)
    if kind in 'XI':
        ib = blobs.get('ib')
        if ib is None or not ib.meta.get('complete'):
            return None, None, 'index buffer unavailable'
        fmt = ib.meta.get('format')
        code = {R16_UINT: '<u2', R32_UINT: '<u4'}.get(fmt)
        if code is None:
            return None, None, f'index format {fmt}'
        indices = np.frombuffer(ib.payload, dtype=code).astype(np.int64) + info.get('base', 0)
    elif kind in 'NV':
        indices = np.arange(info.get('start', 0), info.get('start', 0) + count, dtype=np.int64)
    else:
        return None, None, f'draw kind {kind!r}'
    topology = info.get('topology')
    if topology == 4:
        tri = indices.reshape(-1, 3)
    elif topology == 5:
        if len(indices) < 3:
            return None, None, 'strip shorter than a triangle'
        tri = np.stack([indices[:-2], indices[1:-1], indices[2:]], axis=-1)
    else:
        return None, None, f'topology {topology}'
    vb = {}
    for el, n in ((pos_el, npos), (uv_el, nuv)):
        name = f'vb{el[1]}'
        b = blobs.get(name)
        if b is None or not b.meta.get('complete'):
            return None, None, f'{name} unavailable'
        if name not in vb:
            vb[name] = b
        el_stride = b.meta.get('stride', 0)
        if not el_stride:
            return None, None, f'{name} has zero stride'
    b0 = blobs.get('vs_b0')
    if b0 is None or not b0.meta.get('complete') or len(b0.payload) < 128:
        return None, None, 'vs_b0 rows 4..7 unavailable'
    rows = np.frombuffer(b0.payload[:128], dtype='<f4').reshape(8, 4)[4:8].astype(np.float64)
    try:
        pos = gather_floats(vb[f'vb{pos_el[1]}'].payload, vb[f'vb{pos_el[1]}'].meta['stride'],
                            vb[f'vb{pos_el[1]}'].meta.get('binding_offset', 0), pos_el[2],
                            vb[f'vb{pos_el[1]}'].meta.get('offset', 0), tri.reshape(-1), npos)
        uv = gather_floats(vb[f'vb{uv_el[1]}'].payload, vb[f'vb{uv_el[1]}'].meta['stride'],
                           vb[f'vb{uv_el[1]}'].meta.get('binding_offset', 0), uv_el[2],
                           vb[f'vb{uv_el[1]}'].meta.get('offset', 0), tri.reshape(-1), nuv)
    except ValueError as e:
        return None, None, str(e)
    pos = pos.reshape(-1, 3, npos)
    uv = uv.reshape(-1, 3, nuv)[..., :2]
    clip = np.concatenate([pos[..., :3], np.ones((*pos.shape[:2], 1))], axis=-1) @ rows.T
    w = clip[..., 3]
    viewports = info.get('viewports') or []
    if not viewports:
        return None, None, 'no viewport'
    tlx, tly, vw, vh, _, _ = words_to_floats(viewports[0])
    origin = blobs['rtv_before'].meta.get('origin', [0, 0])
    scissors = info.get('scissors') or []
    mask = np.zeros((h_crop, w_crop), dtype=bool)
    uvs = np.zeros((h_crop, w_crop, 2))
    for t in range(clip.shape[0]):
        wt = w[t]
        if np.any(wt <= 1e-8) or np.any(~np.isfinite(wt)):
            continue
        ndc = clip[t, :, :2] / wt[:, None]
        px = tlx + (ndc[:, 0] + 1.0) * 0.5 * vw - origin[0]
        py = tly + (1.0 - ndc[:, 1]) * 0.5 * vh - origin[1]
        x0, x1 = max(int(math.floor(px.min())), 0), min(int(math.ceil(px.max())), w_crop)
        y0, y1 = max(int(math.floor(py.min())), 0), min(int(math.ceil(py.max())), h_crop)
        if scissors:
            l, tp, r, btm = scissors[0]
            x0, x1 = max(x0, int(l) - origin[0]), min(x1, int(r) - origin[0])
            y0, y1 = max(y0, int(tp) - origin[1]), min(y1, int(btm) - origin[1])
        if x1 <= x0 or y1 <= y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)

        def cross(a, b, p):
            return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])

        p0, p1, p2 = (px[0], py[0]), (px[1], py[1]), (px[2], py[2])
        area = cross(p0, p1, p2)
        if abs(area) < 1e-12:
            continue
        l0 = cross(p1, p2, (gx, gy)) / area
        l1 = cross(p2, p0, (gx, gy)) / area
        l2 = cross(p0, p1, (gx, gy)) / area
        inside = (l0 >= -1e-9) & (l1 >= -1e-9) & (l2 >= -1e-9)
        if not inside.any():
            continue
        invw = l0 / wt[0] + l1 / wt[1] + l2 / wt[2]
        u = (l0 * uv[t, 0, 0] / wt[0] + l1 * uv[t, 1, 0] / wt[1] + l2 * uv[t, 2, 0] / wt[2]) / invw
        v = (l0 * uv[t, 0, 1] / wt[0] + l1 * uv[t, 1, 1] / wt[1] + l2 * uv[t, 2, 1] / wt[2]) / invw
        sub = mask[y0:y1, x0:x1]
        sub[inside] = True
        uvs[y0:y1, x0:x1, 0][inside] = u[inside]
        uvs[y0:y1, x0:x1, 1][inside] = v[inside]
    if not mask.any():
        return None, None, 'rasterized footprint is empty'
    return mask, uvs, ''


def change_mask(before, after):
    """Pixels the draw visibly changed, tolerant of R11G11B10 rounding."""
    scale = np.maximum(np.abs(before), np.abs(after)).max(axis=-1)
    thresh = 0.002 + 0.035 * scale
    return (np.abs(after - before).max(axis=-1) > thresh)


def luma(rgb):
    return rgb[..., 0] * LUMA[0] + rgb[..., 1] * LUMA[1] + rgb[..., 2] * LUMA[2]


def recover_geometry(draw, blobs, before, after):
    """a and L per pixel via surface sampling through the replayed UV map."""
    surf_blob = blobs.get('ps_t2')
    if surf_blob is None or not surf_blob.meta.get('complete'):
        return None, 'ps_t2 surface unavailable'
    try:
        surf = panel_texture(surf_blob)
    except (ValueError, KeyError) as e:
        return None, f'ps_t2 not decodable: {e}'
    mask, uv, why = replay_geometry(draw, blobs, before.shape[:2])
    if mask is None:
        return None, f'geometry not replayable: {why}'
    sh, sw = surf.shape[:2]
    sx = np.clip((uv[..., 0] * sw).astype(int), 0, sw - 1)
    sy = np.clip((uv[..., 1] * sh).astype(int), 0, sh - 1)
    a = surf[sy, sx, 3]
    L = surf[sy, sx, :3]
    # The rasterized footprint is the coverage evidence. Stock before/after
    # equality must NOT exclude a pixel: a translucent draw can leave the
    # stock HDR exactly unchanged (F*(1-a)+L == F) while the separated
    # composition differs by tens of steps. Depth-rejected pixels now fail
    # the cross-check residual (L != a*F there) instead of being dropped.
    return {'a': a, 'L': L, 'region': mask, 'rasterized': int(mask.sum()),
            'changed': int((mask & change_mask(before, after)).sum()),
            'xcheck': after - (before * (1.0 - a[..., None]) + L)}, ''


def recover_luminance(before, after):
    """a solved from the blend identity in luma against one estimated HUD radiance.

    A = (1-a)*F + L implies luma(A) - luma(L) = (1-a)*luma(F), so with an
    estimated constant radiance Lbar: a = 1 - (luma(A)-luma(Lbar))/luma(F).
    Wrong where the panel is multi-coloured or F ~= Lbar; the limitation is
    printed by the caller whenever this route is used.
    """
    region = change_mask(before, after)
    if not region.any():
        return None, 'footprint changed no pixel'
    lum_f, lum_a = luma(before), luma(after)
    dark_thr = np.percentile(lum_f[region], 10)
    dark = region & (lum_f <= dark_thr)
    if dark.sum() < 8:
        k = min(7, region.sum() - 1)
        dark = region & (lum_f <= np.partition(lum_f[region], k)[k])
    Lbar = np.median(after[dark], axis=0)
    lum_l = float(luma(Lbar))
    bright = lum_f > 0.02
    a = np.zeros(lum_f.shape)
    a[bright] = np.clip(1.0 - (lum_a[bright] - lum_l) / lum_f[bright], 0.0, 1.0)
    # Background too dark to observe coverage: covered if it reads as Lbar.
    a[~bright] = np.where(np.abs(lum_a[~bright] - lum_l) <= np.abs(lum_a[~bright] - lum_f[~bright]),
                          1.0, 0.0)
    L = np.clip(after - before * (1.0 - a[..., None]), 0.0, None)
    return {'a': a, 'L': L, 'region': region, 'changed': int(region.sum()),
            'rasterized': None, 'xcheck': None}, ''


def coverage_bound(tfit, F):
    """Per-pixel bound on the parity error a STOCK-UNCHANGED covered pixel could
    carry. after == before forces L == a*F, so the error at coverage a is
    max_c |T(F_c) - ((1-a)*T(F_c) + T(a*F_c))|; bound = max over a on a grid.
    Computed per channel on the fitted knots, then interpolated to each pixel.
    An estimate of a bound: the gap is the chord-vs-curve distance, exact at
    the knots and tight for the concave game tonemaps this gate measures."""
    F = np.asarray(F, dtype=np.float64).reshape(-1, 3)
    alphas = np.linspace(0.0, 1.0, 49)[:, None]
    bound = np.zeros(F.shape[0])
    for c in range(3):
        xs, ys = tfit['curves'][c].x, tfit['curves'][c].y
        keep = np.concatenate(([True], np.diff(xs) > 0))
        xs, ys = xs[keep], ys[keep]
        taf = np.interp((alphas * xs[None, :]).ravel(), xs, ys).reshape(len(alphas), len(xs))
        gap = np.abs(ys[None, :] - ((1.0 - alphas) * ys[None, :] + taf)).max(axis=0)
        bound = np.maximum(bound, np.interp(np.clip(F[:, c], 0.0, None), xs, gap))
    return bound


def measure_draw(index, draw, tfit, bright):
    blobs = draw.blobs
    for name in ('rtv_before', 'rtv_after'):
        if name not in blobs or not blobs[name].meta.get('complete'):
            return {'index': index, 'route': 'skip', 'reason': f'{name} unavailable'}
    before = panel_texture(blobs['rtv_before'])[..., :3]
    after = panel_texture(blobs['rtv_after'])[..., :3]
    rec, why = recover_geometry(draw, blobs, before, after)
    route = 'geometry'
    if rec is None:
        rec, why2 = recover_luminance(before, after)
        route = 'luminance'
        why = f'{why}; LUMINANCE FALLBACK: a is luma-mixing against one '
        why += 'estimated HUD radiance -- wrong where the panel is multi-coloured'
        if rec is None:
            return {'index': index, 'route': 'skip', 'reason': why2}
    a, L, region = rec['a'], rec['L'], rec['region']
    hud = region & (a > EPS_A)
    if not hud.any():
        return {'index': index, 'route': route, 'reason': 'no HUD pixels (a <= eps)',
                'rasterized': rec['rasterized'], 'changed': rec['changed']}
    F, A = before[hud], after[hud]
    aa, LL = a[hud], L[hud]
    tF, tA, tL = tfit['apply'](F), tfit['apply'](A), tfit['apply'](np.clip(LL, 0.0, None))
    err = np.abs(tA - (tF * (1.0 - aa[:, None]) + tL)).max(axis=-1)
    regime = hud & (a < 1.0 - EPS_A) & (a > EPS_A) & (luma(before) > bright)
    rerr = np.abs(tfit['apply'](after[regime]) -
                  (tfit['apply'](before[regime]) * (1.0 - a[regime, None]) +
                   tfit['apply'](np.clip(L[regime], 0.0, None)))).max(axis=-1) if regime.any() else np.array([])
    oor = max(tfit['curves'][c].oor(np.concatenate([F[:, c], A[:, c], LL[:, c]])) for c in range(3))
    out = {'index': index, 'route': route, 'reason': why if route == 'luminance' else '',
           'ordinal': draw.info.get('ordinal'), 'frame': draw.info.get('frame'),
           'rasterized': rec['rasterized'], 'changed': rec['changed'],
           'hud': int(hud.sum()), 'regime': int(regime.sum()), 'oor': oor,
           'err': err, 'rerr': rerr}
    if rec['xcheck'] is not None:
        xc = np.abs(rec['xcheck'][region]).max(axis=-1) if region.any() else np.array([0.0])
        out['xcheck_p50'] = float(np.percentile(xc, 50))
        out['xcheck_p99'] = float(np.percentile(xc, 99))
        out['xcheck_max'] = float(xc.max())
    if route == 'luminance':
        # Stock-unchanged pixels whose solved a claims coverage: the luma
        # equation is degenerate there (after == before), so they can be
        # covered or not. Count them and bound the possible error instead of
        # dropping them silently.
        amb = (~region) & (a > EPS_A)
        out['ambiguous'] = int(amb.sum())
        if amb.any():
            b = coverage_bound(tfit, before[amb])
            out['amb_p50'] = float(np.percentile(b, 50))
            out['amb_p99'] = float(np.percentile(b, 99))
            out['amb_max'] = float(b.max())
    return out


def pct(x, q):
    return float(np.percentile(x, q)) if len(x) else float('nan')


def summarize_draw(d):
    if 'err' not in d:
        return d
    d.update(err_p50=pct(d['err'], 50), err_p99=pct(d['err'], 99), err_max=float(d['err'].max()),
             rerr_p99=pct(d['rerr'], 99))
    return d


# -------------------------------------------------------------------- CLI

def _stamp(path, prefix):
    """The <stamp> of a panels_<stamp>.bin / tonemap_<stamp>.bin name, else None."""
    name = os.path.basename(path)
    if name.startswith(prefix) and name.endswith('.bin'):
        return name[len(prefix):-len('.bin')]
    return None


def find_pair(path):
    panels = {os.path.basename(p)[len('panels_'):-len('.bin')]: p
              for p in glob.glob(os.path.join(path, 'panels_*.bin'))}
    tonemaps = {os.path.basename(p)[len('tonemap_'):-len('.bin')]: p
                for p in glob.glob(os.path.join(path, 'tonemap_*.bin'))}
    common = sorted(set(panels) & set(tonemaps))
    if len(common) == 1:
        return panels[common[0]], tonemaps[common[0]]
    if not common and panels and tonemaps:
        raise InvalidData(f'no panels_*/tonemap_* pair with a matching stamp in {path}: '
                          f'panels stamps {sorted(panels)}, tonemap stamps {sorted(tonemaps)}')
    fail(f'cannot pair panels_*/tonemap_* in {path}: panels stamps {sorted(panels)}, '
         f'tonemap stamps {sorted(tonemaps)}, common {common}')


def sniff_pair(first, second):
    def magic(p):
        with open(p, 'rb') as f:
            return f.read(8)
    pair = {magic(first): first, magic(second): second}
    if b'EDVRPNL1' not in pair or b'EDVRTON1' not in pair:
        fail(f'expected one EDVRPNL1 and one EDVRTON1 file, got {magic(first)!r} and {magic(second)!r}')
    return pair[b'EDVRPNL1'], pair[b'EDVRTON1']


def analyze(panels_path, tonemap_path, budget=2.0, bright=1.0, verbose=False,
            tonemap_draw=None, allow_mismatched=False):
    snap = pnl.read_snapshot(panels_path)
    tsnap = ton.read(tonemap_path)
    if not tsnap['draws']:
        fail('tonemap bin holds no draw')
    tdraws = tsnap['draws']
    target = snap.header.get('target_identity', 0)
    pframes = {d.info.get('frame') for d in snap.draws if d.info.get('frame') is not None}
    pframe = next(iter(pframes)) if len(pframes) == 1 else snap.header.get('first_frame')

    # Pairing must be PROVEN before T is fit: same capture stamp (when the
    # names carry one), same frame, and the tonemap draw's HDR resource ==
    # the panels target identity. Otherwise the verdict is INVALID; an
    # exploratory --allow-mismatched measures anyway, capped at non-PASS.
    mismatch = []
    ps, ts = _stamp(panels_path, 'panels_'), _stamp(tonemap_path, 'tonemap_')
    if ps is not None and ts is not None and ps != ts:
        mismatch.append(f'capture stamps differ: panels_{ps}.bin vs tonemap_{ts}.bin')
    resources = [d['blobs'][2]['meta'].get('resource') for d in tdraws]
    res_matches = [i for i, r in enumerate(resources) if r == target] if target else []
    pick = 0
    if tonemap_draw is not None:
        if not 0 <= tonemap_draw < len(tdraws):
            fail(f'--tonemap-draw {tonemap_draw} out of range ({len(tdraws)} draws)')
        pick = tonemap_draw
        if target and resources[pick] != target:
            mismatch.append(f'tonemap draw {pick} HDR resource {resources[pick]} '
                            f'!= panels target {target}')
    elif not target:
        mismatch.append('panels bin carries no target identity; the pairing cannot be proven')
    elif not res_matches:
        mismatch.append(f'no tonemap draw reads the panels HDR target {target} '
                        f'(captured HDR resources: {resources})')
    elif len(res_matches) > 1:
        mismatch.append(f'{len(res_matches)} tonemap draws read the panels target {target}; '
                        f'disambiguate with --tonemap-draw')
        pick = res_matches[0]
    else:
        pick = res_matches[0]
    if pframe is not None and tdraws[pick].get('frame') != pframe:
        mismatch.append(f'tonemap draw frame {tdraws[pick].get("frame")} != panels frame {pframe}')
    if mismatch and not allow_mismatched:
        raise InvalidData('; '.join(mismatch) +
                          ' (re-measure with --allow-mismatched; the outcome is capped at non-PASS)')
    tfit = fit_tonemap(tdraws[pick])
    draws = [summarize_draw(measure_draw(i, d, tfit, bright)) for i, d in enumerate(snap.draws)]
    errs = [d['err'] for d in draws if 'err' in d]
    rerrs = [d['rerr'] for d in draws if 'err' in d and len(d['rerr'])]
    pooled = np.concatenate(errs) if errs else np.array([])
    prerr = np.concatenate(rerrs) if rerrs else np.array([])
    hud = int(sum(d.get('hud', 0) for d in draws))
    regime = int(sum(d.get('regime', 0) for d in draws))
    if len(prerr) >= REGIME_MIN:
        basis, value = 'restricted p99', pct(prerr, 99)
    else:
        basis, value = 'overall p99', pct(pooled, 99)
    # A PASS also requires the evidence chain to be healthy: the fitted T
    # must reproduce the captured output bytes, and the blend identity must
    # hold on every geometry-routed draw.
    invalid = []
    if tfit['fit_p99'] > FIT_P99_CEILING:
        invalid.append(f'T fit residual p99 {tfit["fit_p99"]:.2f} steps exceeds the '
                       f'{FIT_P99_CEILING:g}-step ceiling (T untrustworthy: cross-channel '
                       f'LUT terms or a corrupt capture)')
    for d in draws:
        if d.get('xcheck_p99', 0.0) > XCHECK_P99_CEILING:
            invalid.append(f'draw {d["index"]} blend cross-check p99 {d["xcheck_p99"]:.3f} HDR '
                           f'units exceeds the {XCHECK_P99_CEILING:g} ceiling (the a/L chain '
                           f'is broken)')
    passed = bool(len(pooled)) and value <= budget and not invalid and not mismatch
    return {'panels': str(panels_path), 'tonemap': str(tonemap_path), 'tonemap_draw': pick,
            'tonemap_draws': len(tdraws), 'tfit': tfit, 'draws': draws,
            'hud': hud, 'regime': regime, 'pooled': pooled, 'prerr': prerr,
            'basis': basis, 'value': value, 'budget': budget, 'passed': passed,
            'invalid': invalid, 'mismatch': mismatch, 'bright': bright}


def print_report(rep, verbose=False):
    tf = rep['tfit']
    if rep.get('mismatch'):
        print('!! ALLOW-MISMATCHED: captures are NOT a proven pair: ' + '; '.join(rep['mismatch']))
        print('!! the measurement below is exploratory; the outcome is capped at non-PASS')
    print(f"hud parity (G-F): {os.path.basename(rep['panels'])} + {os.path.basename(rep['tonemap'])}")
    note = '' if tf['aligned'] else f" (partial overlap, intersected to {tf['region']})"
    print(f"tonemap T: draw {rep['tonemap_draw']}/{rep['tonemap_draws']}, {tf['pooled']} pooled px{note}, "
          f"fit residual p50 {tf['fit_p50']:.2f} / p99 {tf['fit_p99']:.2f} steps, "
          f"exposure {tf['exposure']:.4g}")
    print("  assumptions: T spatially uniform (full-screen pass); per-channel monotone fit "
          "(an EDHM 3D LUT with cross-channel terms shows up as fit residual)")
    if verbose:
        hdr = (f"{'draw':>4} {'ord':>4} {'route':<10} {'footprint':>9} {'changed':>8} {'hud':>7} "
               f"{'regime':>7} {'xcheck p50/p99':>15} {'err p50':>8} {'err p99':>8} {'err max':>8}")
        print(hdr)
        for d in rep['draws']:
            if 'err' not in d:
                print(f"{d['index']:>4} {str(d.get('ordinal', '-')):>4} {d['route']:<10} "
                      f"{'-':>9} {'-':>8} {'-':>7} {'-':>7} {'-':>15} {'-':>8} {'-':>8} {'-':>8}  "
                      f"{d.get('reason', '')}")
                continue
            xc = (f"{d['xcheck_p50']:.3f}/{d['xcheck_p99']:.3f}" if 'xcheck_p50' in d
                  else 'by-constr.')
            raster = d.get('rasterized')
            print(f"{d['index']:>4} {str(d.get('ordinal', '-')):>4} {d['route']:<10} "
                  f"{str(raster if raster is not None else '-'):>9} {d['changed']:>8} {d['hud']:>7} "
                  f"{d['regime']:>7} {xc:>15} {d['err_p50']:>8.2f} {d['err_p99']:>8.2f} "
                  f"{d['err_max']:>8.2f}  {d.get('reason', '')}")
    for d in rep['draws']:
        if d['route'] == 'luminance':
            print(f"  draw {d['index']}: {d['reason']}")
        if d.get('ambiguous'):
            print(f"  draw {d['index']}: {d['ambiguous']} stock-unchanged pixels carry "
                  f"alpha-evidence of coverage under the luma estimate; if covered, their "
                  f"parity error is bounded at p50 {d['amb_p50']:.2f} / p99 {d['amb_p99']:.2f} / "
                  f"max {d['amb_max']:.2f} steps (counted, not measured: the luma equation is "
                  f"degenerate where after == before)")
        if 'err' in d and d.get('oor', 0) > 0.01:
            print(f"  draw {d['index']}: {100 * d['oor']:.1f}% of T evaluations beyond the fitted "
                  f"HDR range (clamped)")
    if not len(rep['pooled']):
        print('POOLED: no HUD pixels in any draw')
    else:
        share = 100.0 * rep['regime'] / rep['hud'] if rep['hud'] else 0.0
        print(f"POOLED: {rep['hud']} HUD px; err p50 {pct(rep['pooled'], 50):.2f} "
              f"p99 {pct(rep['pooled'], 99):.2f} max {float(rep['pooled'].max()):.2f} steps")
        print(f"  regime that matters (0<a<1 and background luma > {rep['bright']:g}): "
              f"{rep['regime']} px ({share:.1f}% of HUD), restricted p99 {pct(rep['prerr'], 99):.2f} steps")
    if rep['invalid']:
        print(f"VERDICT: INVALID ({'; '.join(rep['invalid'])})")
    elif not len(rep['pooled']):
        print('VERDICT: NO DATA (no HUD pixels)')
    elif rep['passed']:
        print(f"VERDICT: PASS ({rep['basis']} {rep['value']:.2f} steps vs budget "
              f"{rep['budget']:g}; G-F budget)")
    else:
        why = ('; mismatched captures admitted under --allow-mismatched cap the outcome at '
               'non-PASS') if rep['mismatch'] else ''
        print(f"VERDICT: FAIL ({rep['basis']} {rep['value']:.2f} steps vs budget "
              f"{rep['budget']:g}; G-F budget{why})")


# -------------------------------------------------------------- self-test

def _pack_float(v, mbits):
    """Scalar float -> unsigned 5-bit-exponent packed float (fixture encoder)."""
    if v <= 0 or not math.isfinite(v):
        return 0
    e = math.floor(math.log2(v))
    if e < -14:
        return min(int(round(v * (1 << (14 + mbits)))), (1 << mbits) - 1)
    e = min(e, 15)
    mant = int(round((v / 2.0 ** e - 1.0) * (1 << mbits)))
    if mant == (1 << mbits):
        mant, e = 0, e + 1
        if e > 15:
            e, mant = 15, (1 << mbits) - 1
    return ((e + 15) << mbits) | mant


def pack_r11g11b10(rgb):
    """(H, W, 3) float -> (H, W) uint32 R11G11B10_FLOAT."""
    flat = rgb.reshape(-1, 3)
    out = np.zeros(len(flat), dtype=np.uint32)
    for i, (r, g, b) in enumerate(flat):
        out[i] = (_pack_float(float(r), 6) | (_pack_float(float(g), 6) << 11)
                  | (_pack_float(float(b), 5) << 22))
    return out.reshape(rgb.shape[:2])


def _text(s):
    b = s.encode('utf-8')
    return struct.pack('<I', len(b)) + b


def _u32(v):
    return struct.pack('<I', v)


def build_tonemap_bin(path, w=64, h=48, seed=7, draws=1, frame=42, hdr_resource=9003,
                      gain_noise=False):
    """Synthetic EDVRTON1 exactly per eye_tonemap_snapshot.h:226-253.

    draws=2 writes a second same-frame draw reading the SAME HDR resource
    (the pairing-ambiguity fixture); frame/hdr_resource parameterize the
    negative pairing fixtures; gain_noise applies a per-pixel gain to the
    output crop so the per-channel monotone T fit residual exceeds the
    INVALID ceiling while the parity metric itself is unaffected.
    """
    rng = np.random.default_rng(seed)
    hdr = rng.random((h, w, 3)) * 9.0
    t_true = lambda x: x / (1.0 + x)
    if gain_noise:
        gain = 0.85 + 0.30 * rng.random((h, w, 1))
        out = np.round(np.clip(t_true(hdr) * gain, 0.0, 1.0) * 255.0).astype(np.uint8)
    else:
        out = np.round(t_true(hdr) * 255.0).astype(np.uint8)
    out_rgba = np.concatenate([out, np.full((h, w, 1), 255, np.uint8)], axis=-1)
    hdr_packed = pack_r11g11b10(hdr)

    def tex(type_, resource, fmt, view_fmt, view, src, origin, size, data):
        m = {'type': type_, 'resource': resource, 'format': fmt, 'view_format': view_fmt,
             'view': view, 'source': src, 'origin': origin, 'size': size,
             'row': size[0] * 4, 'bytes': len(data)}
        return _text(json.dumps(m)) + _u32(len(data)) + data

    def draw(ordinal, base):
        meta = {'frame': frame, 'ordinal': ordinal, 'vs': ton.VS, 'ps': ton.PS, 'kind': ord('D'),
                'count': 3, 'start': 0, 'instances': 1, 'start_instance': 0, 'topology': 5,
                'vb_stride': 20, 'vb_offset': 0, 'complete_inputs': True,
                'layout': [['POSITION', 0, 6, 0, 0, 0, 0], ['TEXCOORD', 0, 16, 0, 12, 0, 0]],
                'blend': None, 'blend_factor': [0, 0, 0, 0], 'sample_mask': 0xffffffff,
                'depth': None, 'stencil_ref': 0, 'raster': None,
                'viewports': [[struct.unpack('<I', struct.pack('<f', x))[0]
                               for x in (0.0, 0.0, float(w), float(h), 0.0, 1.0)]],
                'scissors': [[0, 0, w, h]], 'samplers': [None, None, None]}
        blobs = [
            tex(2, base + 1, R32_FLOAT, R32_FLOAT, [R32_FLOAT, 4, 0, 1, 0, 0],
                [1, 1, 1], [0, 0, 0], [1, 1, 1], struct.pack('<f', 1.0)),
            tex(3, base + 2, RGBA8_UNORM, RGBA8_UNORM, [RGBA8_UNORM, 8, 0, 1, 0, 0],
                [4, 4, 4], [0, 0, 0], [4, 4, 4], bytes(256)),
            tex(2, hdr_resource, R11G11B10_FLOAT, R11G11B10_FLOAT, [R11G11B10_FLOAT, 4, 0, 1, 0, 0],
                [w, h, 1], [0, 0, 0], [w, h, 1], hdr_packed.astype('<u4').tobytes()),
            # Blob 3's "view" is the RTV descriptor (5 words), not an SRV one.
            tex(2, base + 4, RGBA8_TYPELESS, RGBA8_UNORM, [RGBA8_UNORM, 4, 0, 0, 0],
                [w, h, 1], [0, 0, 0], [w, h, 1], out_rgba.tobytes()),
            _text(json.dumps({'type': 1, 'resource': base + 5, 'whole': 256, 'offset': 0,
                              'bytes': 256})) + _u32(256) + bytes(256),
            _text(json.dumps({'type': 1, 'resource': base + 6, 'whole': 60, 'offset': 0,
                              'bytes': 60})) + _u32(60) + bytes(60),
        ]
        return _text(json.dumps(meta)) + b''.join(blobs)

    body = b''.join(draw(i + 1, 9000 + 10 * i) for i in range(draws))
    per_draw = 4 + hdr_packed.astype('<u4').nbytes + out_rgba.nbytes + 256 + 60 + 256
    head = b'EDVRTON1' + _u32(1) + _u32(draws) + _u32(0) + _u32(draws * per_draw)
    shaders = _u32(2)
    for hsh in (ton.VS, ton.PS):
        blob = b'DXBC' + bytes(60)
        shaders += struct.pack('<Q', hsh) + _u32(len(blob)) + blob
    Path(path).write_bytes(head + body + shaders + _u32(0))
    return hdr, t_true


def _panel_blob(name, meta, payload):
    meta = dict(meta)
    meta.setdefault('bytes', len(payload))
    meta.setdefault('complete', True)
    meta.setdefault('reason', '')
    return _text(name) + _text(json.dumps(meta)) + _u32(len(payload)) + payload


def _tex_meta(resource, w, h, storage, cropped, view=None):
    m = {'type': 'texture', 'resource': resource,
         'resource_desc': [w, h, 1, 1, storage if not cropped else R11G11B10_FLOAT, 1, 0, 0, 8, 0, 0],
         'storage_format': storage, 'origin': [0, 0],
         'mips': [{'level': 0, 'width': w, 'height': h, 'row_bytes': w * 4,
                   'rows': h, 'offset': 0, 'size': w * h * 4}]}
    if view is not None:
        m['view'] = view
        m['view_first_mip'] = view[2]
        m['view_mips'] = view[3]
    return m


def build_panels_bin(path, w=64, h=48, corrupt=False, second_draw=True, equality=False):
    """Synthetic EDVRPNL1 exactly per eye_panel_snapshot.h:452-506.

    One indexed quad over the right half of a 64x48 HDR crop: left half of
    the panel opaque (a=255/255), right half translucent (a=128/255), over a
    dark then bright background; after = before*(1-a)+L encoded through the
    same R11G11B10 quantizer. Draw 2 is instanced to force the fallback.
    equality=True makes the translucent half satisfy F*(1-a)+L == F EXACTLY
    (F = 1.5, a = 128/255, L = 192/255 per channel, all exactly representable
    in R11G11B10): the stock-unchanged covered pixels of the R3 regression.
    """
    a_byte, l_byte = 128, ((192, 192, 192) if equality else (128, 77, 26))
    a_cov = a_byte / 255.0
    L = np.array(l_byte, dtype=np.float64) / 255.0
    before = np.zeros((h, w, 3))
    before[:, :] = (0.1, 0.1, 0.1)
    before[:, 32:48] = (0.05, 0.02, 0.01)          # dark, under the opaque half
    before[:, 48:] = 1.5 if equality else (8.0, 4.0, 2.0)   # under the translucent half
    after = before.copy()
    after[:, 32:48] = L                            # a=1: after == L
    after[:, 48:] = before[:, 48:] * (1.0 - a_cov) + L     # == before exactly when equality
    if corrupt:
        after[:, 48:] += (0.0, 0.0, 1.0)           # break the blend identity
    surf = np.zeros((16, 16, 4), dtype=np.uint8)
    surf[..., :3] = l_byte
    surf[:, :8, 3] = 255
    surf[:, 8:, 3] = a_byte
    verts = np.array([[0.0, 1.0, 0.0, 0.0, 0.0], [1.0, 1.0, 0.0, 1.0, 0.0],
                      [1.0, -1.0, 0.0, 1.0, 1.0], [0.0, -1.0, 0.0, 0.0, 1.0]], dtype='<f4')
    ib = np.array([0, 1, 2, 0, 2, 3], dtype='<u2')
    b0 = np.zeros((12, 4), dtype='<f4')
    b0[4:8] = np.eye(4, dtype='<f4')
    viewport = [struct.unpack('<I', struct.pack('<f', x))[0]
                for x in (0.0, 0.0, float(w), float(h), 0.0, 1.0)]
    blend = [0, 0, 1, 2, 6, 1, 2, 6, 1, 15] + [0] * 56

    def draw(ordinal, instances):
        info = {'frame': 42, 'ordinal': ordinal, 'vs': pnl.VS, 'ps': pnl.PS,
                'kind': ord('X'), 'count': 6, 'instances': instances, 'start_instance': 0,
                'start': 0, 'base': 0, 'rtv_view': [R11G11B10_FLOAT, 4, 0, 0, 0],
                'dsv_view': None, 'blend': blend, 'blend_factor': [0, 0, 0, 0],
                'sample_mask': 0xffffffff, 'depth': None, 'stencil_ref': 0, 'raster': None,
                'viewports': [viewport], 'scissors': [], 'samplers': [None, None],
                'topology': 4,
                'layout': [['POSITION', 0, 6, 0, 0, 0, 0], ['TEXCOORD', 0, 16, 0, 12, 0, 0]],
                'complete': True, 'reasons': []}
        blobs = [
            _panel_blob('rtv_before', _tex_meta(9003, w, h, R11G11B10_FLOAT, True),
                        pack_r11g11b10(before).astype('<u4').tobytes()),
            _panel_blob('rtv_after', _tex_meta(9003, w, h, R11G11B10_FLOAT, True),
                        pack_r11g11b10(after).astype('<u4').tobytes()),
            _panel_blob('ps_t2', _tex_meta(9007, 16, 16, RGBA8_UNORM, False,
                                           view=[RGBA8_UNORM, 4, 0, 1, 0, 0]),
                        surf.tobytes()),
            _panel_blob('ib', {'type': 'buffer', 'resource': 9008,
                               'resource_desc': [12, 0, 1, 0, 0, 0], 'whole': 12, 'offset': 0,
                               'format': R16_UINT, 'binding_offset': 0}, ib.tobytes()),
            _panel_blob('vb0', {'type': 'buffer', 'resource': 9009,
                                'resource_desc': [80, 0, 1, 0, 0, 0], 'whole': 80, 'offset': 0,
                                'stride': 20, 'binding_offset': 0, 'inputclass': 0, 'step': 0},
                        verts.tobytes()),
            _panel_blob('vs_b0', {'type': 'buffer', 'resource': 9010,
                                  'resource_desc': [192, 0, 1, 0, 0, 0], 'whole': 192,
                                  'offset': 0, 'minimum': 192}, b0.tobytes()),
        ]
        return _text(json.dumps(info)) + _u32(len(blobs)) + b''.join(blobs)

    draws = [draw(7, 1)]
    if second_draw:
        draws.append(draw(8, 2))
    retained = 2 * (w * h * 4) + 1024 + 12 + 80 + 192
    reserved = retained * len(draws)
    head = (b'EDVRPNL1' + _u32(1) + _u32(len(draws)) + _u32(42) + _u32(0) + _u32(0)
            + _u32(0) + _u32(0) + struct.pack('<Q', 9003) + struct.pack('<Q', reserved))
    Path(path).write_bytes(head + b''.join(draws) + _u32(0))
    return {'a': a_cov, 'L': L, 'before': before, 'after': after}


def self_test():
    import shutil
    import subprocess
    with tempfile.TemporaryDirectory() as td:
        tonemap_bin = os.path.join(td, 'tonemap_20260927_120000.bin')
        panels_bin = os.path.join(td, 'panels_20260927_120000.bin')
        hdr, t_true = build_tonemap_bin(tonemap_bin)
        fx = build_panels_bin(panels_bin)
        # The synthetic bins must round-trip through the REAL parsers.
        tsnap = ton.read(tonemap_bin)
        snap = pnl.read_snapshot(panels_bin)
        for d in snap.draws:
            for name in ('rtv_before', 'rtv_after', 'ps_t2', 'ib', 'vb0', 'vs_b0'):
                pnl.verify_blob(d.blobs[name])
        assert len(tsnap['draws']) == 1 and len(snap.draws) == 2

        rep = analyze(panels_bin, tonemap_bin)
        tf = rep['tfit']
        assert tf['pooled'] == 64 * 48 and tf['aligned'], 'crop alignment failed'
        assert tf['fit_p99'] < 1.5, f"T fit residual too large: {tf['fit_p99']}"
        # Monotone by construction: evaluate on a ramp.
        ramp = np.linspace(0.0, 9.0, 500)
        for c in tf['curves']:
            assert np.all(np.diff(c(ramp)) >= -1e-9), 'T not monotone'

        d0, d1 = rep['draws'][0], rep['draws'][1]
        # (a) geometry route on draw 0; opaque + uncovered give err ~ 0.
        assert d0['route'] == 'geometry', d0
        assert d0['rasterized'] == 32 * 48 and d0['hud'] == 32 * 48, d0
        assert d0['changed'] == 32 * 48, 'opaque-over-dark region lost'
        a_cov, L = fx['a'], fx['L']
        # Hand check per channel for the translucent-over-bright half:
        F = np.array([8.0, 4.0, 2.0])
        A = F * (1.0 - a_cov) + L
        expected = 255.0 * float(np.max(np.abs(
            t_true(A) - (t_true(F) * (1.0 - a_cov) + t_true(L)))))
        assert expected > 5.0, f'hand value too small to be a test: {expected}'
        assert abs(d0['rerr_p99'] - expected) < 0.25 * expected, \
            f"translucent err {d0['rerr_p99']} vs hand {expected}"
        # (a) Opaque-over-dark half: |T(Aq) - T(L)| with Aq ~= L -> err ~ 0.
        # The err array is row-major over the panel: 16 opaque then 16
        # translucent columns per row.
        err_map = d0['err'].reshape(48, 32)
        assert err_map[:, :16].max() < 1.0, \
            f"opaque/covered err not ~0: {err_map[:, :16].max()} steps"
        assert abs(err_map[:, 16:].max() - expected) < 0.25 * expected
        # (c) cross-check residual small on consistent inputs (R11 rounding only).
        assert d0['xcheck_p99'] < 0.05, f"cross-check residual {d0['xcheck_p99']}"
        assert d0['xcheck_max'] < 0.1, f"cross-check max {d0['xcheck_max']}"
        # (b) regime bookkeeping: half the HUD pixels are translucent-over-bright.
        assert d0['regime'] == 16 * 48, d0['regime']
        assert rep['basis'] == 'restricted p99'

        # Fallback route on the instanced second draw.
        assert d1['route'] == 'luminance', d1
        assert d1['hud'] > 0 and 'xcheck_p50' not in d1
        # Luminance route: stock-unchanged pixels whose solved a claims
        # coverage are counted and bounded, not silently dropped. The whole
        # unchanged background qualifies (the luma equation is degenerate
        # there); on this dark background the bound is small.
        assert d1['ambiguous'] == 64 * 48 - 32 * 48, d1.get('ambiguous')
        assert d1['amb_p99'] < 5.0, d1.get('amb_p99')

        # (e) R3 regression: stock-unchanged translucent pixels stay in the
        # HUD set. The equality fixture satisfies F*(1-a)+L == F exactly on
        # the translucent half; pre-fix the tool excluded that half and
        # manufactured a PASS from the opaque half alone (proved below).
        os.makedirs(os.path.join(td, 'eq'))
        eq_bin = os.path.join(td, 'eq', 'panels_eq.bin')
        eq_ton = os.path.join(td, 'eq', 'tonemap_eq.bin')
        fx_eq = build_panels_bin(eq_bin, second_draw=False, equality=True)
        shutil.copy(tonemap_bin, eq_ton)
        eq = analyze(eq_bin, eq_ton)
        d0 = eq['draws'][0]
        assert d0['route'] == 'geometry' and d0['rasterized'] == 32 * 48
        assert d0['changed'] == 16 * 48, 'the translucent half must be stock-unchanged'
        assert d0['hud'] == 32 * 48, 'stock-unchanged covered pixels dropped from the HUD set'
        assert d0['regime'] == 16 * 48 and eq['basis'] == 'restricted p99', eq['basis']
        assert d0['xcheck_p99'] < XCHECK_P99_CEILING and not eq['invalid'], eq['invalid']
        Feq = np.array([1.5] * 3)
        expected_eq = 255.0 * float(np.max(np.abs(
            t_true(Feq) - (t_true(Feq) * (1.0 - fx_eq['a']) + t_true(fx_eq['L'])))))
        assert expected_eq > 30.0, f'equality hand value too small to be a test: {expected_eq}'
        assert abs(eq['value'] - expected_eq) < 0.25 * expected_eq, \
            f"equality err {eq['value']} vs hand {expected_eq}"
        assert not eq['passed'], 'equality fixture must not PASS'
        # Pre-fix proof: the stock-changed subset alone (the old mask) passes
        # at the default budget -- the old PASS came from opaque pixels only.
        Aq = fx_eq['after'][:, 32:48].reshape(-1, 3)
        old_err = np.abs(eq['tfit']['apply'](Aq)
                         - eq['tfit']['apply'](np.broadcast_to(fx_eq['L'], Aq.shape))).max(axis=-1)
        assert pct(old_err, 99) <= 2.0, 'pre-fix PASS population not reproduced'

        # (f) R4 negative pairing fixtures: each was an ordinary PASS pre-fix
        # (the analyzer silently fell back to draw 0 / accepted the stamps).
        def must_invalid(p, t, needle):
            try:
                analyze(p, t)
            except InvalidData as e:
                assert needle in str(e), str(e)
                return
            raise AssertionError(f'expected InvalidData: {needle}')

        os.makedirs(os.path.join(td, 'wrongres'))
        wr_p = os.path.join(td, 'wrongres', 'panels_m.bin')
        wr_t = os.path.join(td, 'wrongres', 'tonemap_m.bin')
        shutil.copy(panels_bin, wr_p)
        build_tonemap_bin(wr_t, hdr_resource=9993)      # target is 9003
        must_invalid(wr_p, wr_t, 'no tonemap draw reads the panels HDR target 9003')
        cap = analyze(wr_p, wr_t, budget=100.0, allow_mismatched=True)
        assert cap['mismatch'] and not cap['passed'] and cap['value'] <= 100.0, \
            'the --allow-mismatched fallback is the pre-fix path: it must cap at non-PASS'

        os.makedirs(os.path.join(td, 'wrongframe'))
        wf_p = os.path.join(td, 'wrongframe', 'panels_f.bin')
        wf_t = os.path.join(td, 'wrongframe', 'tonemap_f.bin')
        shutil.copy(panels_bin, wf_p)
        build_tonemap_bin(wf_t, frame=43)               # panels frame is 42
        must_invalid(wf_p, wf_t, 'frame 43 != panels frame 42')

        os.makedirs(os.path.join(td, 'amb'))
        am_p = os.path.join(td, 'amb', 'panels_a.bin')
        am_t = os.path.join(td, 'amb', 'tonemap_a.bin')
        shutil.copy(panels_bin, am_p)
        build_tonemap_bin(am_t, draws=2)                # both read target 9003
        must_invalid(am_p, am_t, 'disambiguate with --tonemap-draw')
        ok = analyze(am_p, am_t, budget=100.0, tonemap_draw=0)
        assert ok['passed'] and not ok['mismatch'] and ok['tonemap_draw'] == 0

        mix = os.path.join(td, 'stampmix')
        os.makedirs(mix)
        shutil.copy(panels_bin, os.path.join(mix, 'panels_20260927_120000.bin'))
        shutil.copy(tonemap_bin, os.path.join(mix, 'tonemap_20260927_999999.bin'))
        try:
            find_pair(mix)
        except InvalidData as e:
            assert 'matching stamp' in str(e), str(e)
        else:
            raise AssertionError('mismatched directory stamps accepted')

        # (g) Residual ceilings. Corrupt blend chain: pre-fix PASS at budget
        # 100 with a 1.0-HDR-unit cross-check; now INVALID at any budget.
        os.makedirs(os.path.join(td, 'bad'))
        bad_bin = os.path.join(td, 'bad', 'panels_bad.bin')
        bad_ton = os.path.join(td, 'bad', 'tonemap_bad.bin')
        build_panels_bin(bad_bin, corrupt=True, second_draw=False)
        shutil.copy(tonemap_bin, bad_ton)
        bad = analyze(bad_bin, bad_ton, budget=100.0)
        assert bad['draws'][0]['xcheck_p99'] > 0.5, \
            f"corruption not detected: {bad['draws'][0].get('xcheck_p99')}"
        assert bad['invalid'] and not bad['passed'] and bad['value'] <= 100.0, \
            'corrupt blend chain must be INVALID even within budget'
        # T-fit residual: a per-pixel gain makes out non-functional in hdr.
        os.makedirs(os.path.join(td, 'gain'))
        gn_p = os.path.join(td, 'gain', 'panels_g.bin')
        gn_t = os.path.join(td, 'gain', 'tonemap_g.bin')
        shutil.copy(panels_bin, gn_p)
        build_tonemap_bin(gn_t, gain_noise=True)
        gain = analyze(gn_p, gn_t, budget=100.0)
        assert gain['tfit']['fit_p99'] > FIT_P99_CEILING, gain['tfit']['fit_p99']
        assert gain['invalid'] and not gain['passed'] and gain['value'] <= 100.0, \
            'a large T-fit residual must be INVALID even within budget'

        # (d) verdict flips with budget, through the CLI, and the ledger dir
        # auto-pairing finds the stamped pair.
        rc = subprocess.run([sys.executable, os.path.abspath(__file__), td],
                            capture_output=True, text=True)
        assert rc.returncode == 1, (rc.returncode, rc.stdout, rc.stderr)
        assert 'VERDICT: FAIL' in rc.stdout, rc.stdout
        rc = subprocess.run([sys.executable, os.path.abspath(__file__), td, '--budget', '100'],
                            capture_output=True, text=True)
        assert rc.returncode == 0 and 'VERDICT: PASS' in rc.stdout, (rc.returncode, rc.stdout)
        rc = subprocess.run([sys.executable, os.path.abspath(__file__),
                             panels_bin, tonemap_bin, '--budget', '100'],
                            capture_output=True, text=True)
        assert rc.returncode == 0, (rc.returncode, rc.stderr)
        # (h) CLI wiring of the new outcomes: INVALID exits 2 with the reason,
        # the equality fixture fails the budget (exit 1), --allow-mismatched
        # prints the banner and caps at non-PASS.
        def cli(*args):
            return subprocess.run([sys.executable, os.path.abspath(__file__), *args],
                                  capture_output=True, text=True)
        rc = cli(os.path.join(td, 'eq'))
        assert rc.returncode == 1 and 'VERDICT: FAIL' in rc.stdout, (rc.returncode, rc.stdout)
        rc = cli(mix)
        assert rc.returncode == 2 and 'VERDICT: INVALID' in rc.stdout, (rc.returncode, rc.stdout)
        rc = cli(wr_p, wr_t)
        assert rc.returncode == 2 and 'VERDICT: INVALID' in rc.stdout, (rc.returncode, rc.stdout)
        rc = cli(wr_p, wr_t, '--budget', '100', '--allow-mismatched')
        assert (rc.returncode == 1 and 'ALLOW-MISMATCHED' in rc.stdout
                and 'VERDICT: FAIL' in rc.stdout), (rc.returncode, rc.stdout)
        rc = cli(bad_bin, bad_ton, '--budget', '100')
        assert rc.returncode == 2 and 'VERDICT: INVALID' in rc.stdout, (rc.returncode, rc.stdout)
        rc = cli(gn_p, gn_t, '--budget', '100')
        assert rc.returncode == 2 and 'VERDICT: INVALID' in rc.stdout, (rc.returncode, rc.stdout)
        # Uncovered pixels (a == 0) never enter the metric: err there is
        # identically 0 by T(F) - T(F), and the footprint excludes them.
        assert rep['hud'] == 2 * (32 * 48), rep['hud']
    print('hud_parity self-test: PASS (T recovery, opaque/covered identity, translucent-over-bright '
          'hand check, stock-unchanged translucent inclusion, luminance ambiguous bound, pairing '
          'mismatch INVALIDs, residual-ceiling INVALIDs, verdict exit codes)')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*', help='a ledger dir, or panels.bin + tonemap.bin')
    ap.add_argument('--budget', type=float, default=2.0,
                    help='G-F budget in 8-bit steps at p99 (default 2.0)')
    ap.add_argument('--bright-luma', type=float, default=1.0,
                    help='background luma threshold for the regime that matters (default 1.0 HDR)')
    ap.add_argument('--tonemap-draw', type=int, default=None,
                    help='which EDVRTON1 draw to fit T on (default: the unique draw whose HDR '
                         'resource matches the panels target identity; no unique compatible '
                         'draw is INVALID)')
    ap.add_argument('--allow-mismatched', action='store_true',
                    help='measure even when the captures are not a proven pair (stamp/frame/'
                         'target mismatch); prints a banner and caps the outcome at non-PASS')
    ap.add_argument('--verbose', action='store_true', help='per-draw table')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        self_test()
        return 0
    try:
        if len(a.paths) == 1 and os.path.isdir(a.paths[0]):
            panels_path, tonemap_path = find_pair(a.paths[0])
        elif len(a.paths) == 2:
            panels_path, tonemap_path = sniff_pair(a.paths[0], a.paths[1])
        else:
            ap.error('give a ledger directory or the two bin paths')
        rep = analyze(panels_path, tonemap_path, budget=a.budget, bright=a.bright_luma,
                      verbose=a.verbose, tonemap_draw=a.tonemap_draw,
                      allow_mismatched=a.allow_mismatched)
        print_report(rep, verbose=a.verbose)
        if rep['invalid'] or not len(rep['pooled']):
            return 2
        return 0 if rep['passed'] else 1
    except InvalidData as e:
        print(f'VERDICT: INVALID ({e})')
        return 2
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f'hud_parity: FAIL: {e}')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
