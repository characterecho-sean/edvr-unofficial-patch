#!/usr/bin/env python3
"""Exact Schwarzschild optics: the ground truth for EDVR's black-hole shader.

    python tools/blackhole_optics.py --at 10 1000           # 10 solar masses, seen from 1000 km
    python tools/blackhole_optics.py --render bh.png --mass 10 --distance 600 --disk on
    python tools/blackhole_optics.py --render bh.png --dry-run
    python tools/blackhole_optics.py --self-test

Elite draws a black hole as a warped blob with no shadow. What a static
observer near a non-rotating hole actually sees is fixed by general
relativity, and this file computes it, so the shader that replaces the blob
is checked against physics in the build rather than by eye in a headset.
docs/black-holes.md is the arc this serves.

Units are geometric, G = c = 1, lengths in M = GM/c^2 (1.4766 km per solar
mass). A ray leaves the eye at angle alpha from the direction of the hole.
Its orbit lies in one plane; with u = 1/r and phi the angle it sweeps around
the hole it obeys the Binet equation u'' = 3u^2 - u, starting at u = 1/r_o
with u' = u sqrt(1 - 2u) cot(alpha). It escapes when u returns to 0 having
swept phi_esc, and then shows the sky in the direction
cos(phi_esc) e1 + sin(phi_esc) e2, where e1 points from the hole to the eye
and e2 is the ray's own direction with its e1 part removed. It is captured
when u reaches 1/2, the horizon. That march, a fixed-step RK4 in phi, is the
shader's algorithm. The self-test holds it to an independent quadrature of
the same orbit and holds both to published results:

  * the shadow, sin(alpha_sh) = 3 sqrt(3)/r_o sqrt(1 - 2/r_o) (Synge 1966);
  * weak-field bending seen from infinity, 4/b + 15 pi/(4 b^2) + 128/(3 b^3)
    + 3465 pi/(64 b^4) (Keeton & Petters 2005);
  * strong-field bending, -log(b/b_c - 1) + log(216 (7 - 4 sqrt 3)) - pi
    (Bozza 2002);
  * a thin disk's flux, zero at the innermost stable orbit r = 6 (Page &
    Thorne 1974), and the redshift of its circular orbits seen by a static
    observer, which sets the colour and brightness of each side.

--render draws a preview (the shader's algorithm, in Python, over a
procedural sky); --dry-run with it computes everything and writes nothing
at all. Standard library only. Exit 0 on success, 1 when a self-test check
fails, 2 on a usage error.
"""
import argparse
import bisect
import math
import os
import shutil
import struct
import sys
import tempfile
import zlib
from array import array

KM_PER_SOLAR_MASS = 1.4766250   # GM_sun / c^2, IAU nominal GM_sun
B_CRIT = 3.0 * math.sqrt(3.0)   # critical impact parameter; the photon sphere is r = 3
R_ISCO = 6.0                    # innermost stable circular orbit, the disk's inner edge

# The shader's march: RK4 in phi at a fixed step. A ray still circling after
# PHI_CAP has passed within a hair of the photon sphere and is drawn as
# captured; the self-test bounds what that costs.
STEP = math.pi / 96.0
PHI_CAP = 6.0 * math.pi


# ---------------------------------------------------------------- the orbit

def shadow_radius(r_o):
    """Angular radius (rad) of the shadow seen by a static observer at r_o > 2."""
    s = B_CRIT / r_o * math.sqrt(1.0 - 2.0 / r_o)
    a = math.asin(min(1.0, s))
    return a if r_o >= 3.0 else math.pi - a


def impact_parameter(r_o, alpha):
    """b = L/E of the ray leaving a static observer at r_o at angle alpha from the hole."""
    return r_o * math.sin(alpha) / math.sqrt(1.0 - 2.0 / r_o)


def _acc(u):
    return 3.0 * u * u - u


def _rk4(u, v, h):
    k1u = v
    k1v = _acc(u)
    k2u = v + 0.5 * h * k1v
    k2v = _acc(u + 0.5 * h * k1u)
    k3u = v + 0.5 * h * k2v
    k3v = _acc(u + 0.5 * h * k2u)
    k4u = v + h * k3v
    k4v = _acc(u + h * k3u)
    return (u + h / 6.0 * (k1u + 2.0 * k2u + 2.0 * k3u + k4u),
            v + h / 6.0 * (k1v + 2.0 * k2v + 2.0 * k3v + k4v))


def _hermite(u0, v0, u1, v1, h, t):
    """Cubic Hermite u at fraction t of a step of length h."""
    t2 = t * t
    t3 = t2 * t
    return ((2 * t3 - 3 * t2 + 1) * u0 + (t3 - 2 * t2 + t) * h * v0
            + (-2 * t3 + 3 * t2) * u1 + (t3 - t2) * h * v1)


def _hermite_root(u0, v0, u1, v1, h, target):
    """Fraction t in [0, 1] where the Hermite cubic reaches target; the
    endpoints straddle it. Bisection, then it is exact to double precision."""
    lo, hi = 0.0, 1.0
    below = u0 < target
    for _ in range(60):
        mid = 0.5 * (lo + hi)
        if (_hermite(u0, v0, u1, v1, h, mid) < target) == below:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def march(r_o, alpha, step=STEP, phi_cap=PHI_CAP, crossings=None, record=None):
    """The shader's algorithm. Returns (kind, phi_end, hits):

    kind 'escape'  -- u reached 0; phi_end is the sweep phi_esc.
    kind 'capture' -- u reached 1/2 (the horizon) at phi_end.
    kind 'orbit'   -- still circling at phi_cap; drawn as captured.

    The step is STEP, cut to u/u' on the way in so a ray aimed near the hole
    moves u by at most its own size per step. On the way out, once u < STEP
    |u'| the rest of the sweep is less than a step, and it is finished in
    closed form: asin(e) - e^3 u / 4, e = u b, b = 1/sqrt(u'^2 + u^2 - 2u^3),
    the integral of du / sqrt(b^-2 - u^2 + 2u^3) to first order in 2u^3,
    good to about e^5 u. Without that a ray looking almost straight away
    from the hole, u' ~ cot(alpha), overflows the first step.

    crossings, when given, is (phi_first, r_in, r_out): the ray's plane meets
    the disk plane at phi_first + k pi, and hits lists (k, phi, r) for every
    crossing reached before the end, with r the radius there, inside the disk
    or not. record, when given, is three array('d') that receive phi, u and
    u' at every step and at the end."""
    u = 1.0 / r_o
    sa = math.sin(alpha)
    ca = math.cos(alpha)
    if sa <= 0.0:
        return ("capture" if ca > 0.0 else "escape"), 0.0, []
    v = u * math.sqrt(1.0 - 2.0 * u) * ca / sa
    hits = []
    next_cross = crossings[0] if crossings else None
    k = 0
    phi = 0.0

    def keep(p, uu, vv):
        if record is not None:
            record[0].append(p)
            record[1].append(uu)
            record[2].append(vv)
    keep(phi, u, v)
    while phi < phi_cap:
        if v < 0.0 and u < -v * step:
            b = 1.0 / math.sqrt(v * v + u * u - 2.0 * u ** 3)
            e = min(1.0, u * b)
            rest = math.asin(e) - 0.25 * e ** 3 * u
            while next_cross is not None and next_cross <= phi + rest:
                uc = math.sin(phi + rest - next_cross) / b
                hits.append((k, next_cross, 1.0 / uc if uc > 0 else float("inf")))
                k += 1
                next_cross += math.pi
            keep(phi + rest, 0.0, -1.0 / b)
            return "escape", phi + rest, hits
        h = step if v <= 0.0 else min(step, u / v)
        u1, v1 = _rk4(u, v, h)
        end_kind = None
        t_end = 1.0
        if u1 <= 0.0:
            end_kind = "escape"
            t_end = _hermite_root(u, v, u1, v1, h, 0.0)
        elif u1 >= 0.5:
            end_kind = "capture"
            t_end = _hermite_root(u, v, u1, v1, h, 0.5)
        while next_cross is not None and next_cross <= phi + t_end * h:
            uc = _hermite(u, v, u1, v1, h, (next_cross - phi) / h)
            hits.append((k, next_cross, 1.0 / uc if uc > 0 else float("inf")))
            k += 1
            next_cross += math.pi
        if end_kind is not None:
            ue = 0.0 if end_kind == "escape" else 0.5
            keep(phi + t_end * h, ue, v + (v1 - v) * t_end)
            return end_kind, phi + t_end * h, hits
        u, v = u1, v1
        phi += h
        keep(phi, u, v)
    return "orbit", phi, hits


def _roots(b):
    """(u1, u2, delta) for 2u^3 - u^2 + 1/b^2 = 0 with b > b_c: u1 < 0 < u2
    (the periapsis) < u3 = u2 + delta. delta comes from its own formula, so it
    keeps its precision as b approaches b_c and u2, u3 merge at 1/3."""
    k = 1.0 / (b * b)
    theta = math.acos(max(-1.0, min(1.0, 1.0 - 54.0 * k)))
    u1 = math.cos(theta / 3.0 - 4.0 * math.pi / 3.0) / 3.0 + 1.0 / 6.0
    u2 = math.cos(theta / 3.0 - 2.0 * math.pi / 3.0) / 3.0 + 1.0 / 6.0
    delta = math.sin((math.pi - theta) / 3.0) / math.sqrt(3.0)
    return u1, u2, delta


def _simpson(f, a, b, n):
    if n % 2:
        n += 1
    h = (b - a) / n
    s = f(a) + f(b)
    for i in range(1, n):
        s += (4.0 if i % 2 else 2.0) * f(a + i * h)
    return s * h / 3.0


def _to_periapsis(b, a, n=4000):
    """Sweep from u = a to the periapsis u2, integral of du / sqrt(f(u)).
    With u = u2 - delta sinh^2(t) the integrand is 2 / sqrt(2 (u2 - u1 -
    delta sinh^2 t)), smooth on the whole range even as delta goes to 0."""
    u1, u2, delta = _roots(b)
    if a >= u2:
        return 0.0
    top = math.asinh(math.sqrt((u2 - a) / delta))
    return _simpson(lambda t: 2.0 / math.sqrt(2.0 * (u2 - u1 - delta * math.sinh(t) ** 2)),
                    0.0, top, n)


def sweep_quadrature(r_o, alpha, n=4000):
    """Independent reference for march's escape sweep. None for a captured ray."""
    b = impact_parameter(r_o, alpha)
    uo = 1.0 / r_o
    inward = alpha < 0.5 * math.pi
    if b <= B_CRIT:
        if inward or r_o <= 3.0:
            return None
        k = 1.0 / (b * b)
        return _simpson(lambda u: 1.0 / math.sqrt(k - u * u + 2.0 * u ** 3), 0.0, uo, n)
    full = _to_periapsis(b, 0.0, n)
    part = _to_periapsis(b, uo, n)
    return full + part if inward else full - part


def deflection_at_infinity(b, n=4000):
    """Total bending of a ray from infinity to infinity, impact parameter b > b_c."""
    return 2.0 * _to_periapsis(b, 0.0, n) - math.pi


def _bisect(f, lo, hi, iters=80):
    flo = f(lo)
    for _ in range(iters):
        mid = 0.5 * (lo + hi)
        fm = f(mid)
        if (fm > 0) == (flo > 0):
            lo, flo = mid, fm
        else:
            hi = mid
    return 0.5 * (lo + hi)


def einstein_ring(r_o):
    """Angular radius (rad) of the ring a point directly behind the hole
    becomes: the ray that sweeps exactly pi."""
    a_sh = shadow_radius(r_o)
    return _bisect(lambda a: sweep_quadrature(r_o, a) - math.pi,
                   a_sh * (1.0 + 1e-9) + 1e-15, 0.5 * math.pi)


def lensing_reach(r_o, threshold):
    """Angle (rad) from the hole inside which light is bent more than threshold."""
    a_sh = shadow_radius(r_o)

    def bend(a):
        return sweep_quadrature(r_o, a) - (math.pi - a) - threshold
    if bend(math.pi * (1.0 - 1e-9)) > 0:
        return math.pi
    return _bisect(bend, a_sh * (1.0 + 1e-9) + 1e-15, math.pi * (1.0 - 1e-9))


# ---------------------------------------------------------------- the disk

def disk_flux(r):
    """Page-Thorne flux of a thin disk, units of Mdot / M^2; 0 inside r = 6."""
    if r <= R_ISCO:
        return 0.0
    x = math.sqrt(r)
    s3 = math.sqrt(3.0)
    s6 = math.sqrt(6.0)
    bracket = x - s6 + 0.5 * s3 * math.log((x + s3) * (s6 - s3) / ((x - s3) * (s6 + s3)))
    return 3.0 / (8.0 * math.pi) * bracket / ((r - 3.0) * r ** 2.5)


def disk_flux_peak():
    """(r, flux) at the flux maximum; golden-section search."""
    lo, hi = R_ISCO, 40.0
    g = (math.sqrt(5.0) - 1.0) / 2.0
    for _ in range(200):
        a = hi - g * (hi - lo)
        b = lo + g * (hi - lo)
        if disk_flux(a) < disk_flux(b):
            lo = a
        else:
            hi = b
    r = 0.5 * (lo + hi)
    return r, disk_flux(r)


def disk_redshift(r, lz, r_o):
    """g = E_seen / E_emitted for light from a circular orbit at r (> 3)
    reaching a static observer at r_o, lz its angular momentum per unit
    energy about the disk axis (positive along the orbit's own sense)."""
    return math.sqrt(1.0 - 3.0 / r) / (math.sqrt(1.0 - 2.0 / r_o) * (1.0 - lz * r ** -1.5))


# ---------------------------------------------------------------- colour

def _lobe(lam, mu, s1, s2):
    t = (lam - mu) / (s1 if lam < mu else s2)
    return math.exp(-0.5 * t * t)


def cmf(lam):
    """CIE 1931 2-degree colour matching functions at lam nm, the
    multi-lobe fit of Wyman, Sloan & Shirley (JCGT 2013)."""
    x = (1.056 * _lobe(lam, 599.8, 37.9, 31.0) + 0.362 * _lobe(lam, 442.0, 16.0, 26.7)
         - 0.065 * _lobe(lam, 501.1, 20.4, 26.2))
    y = 0.821 * _lobe(lam, 568.8, 46.9, 40.5) + 0.286 * _lobe(lam, 530.9, 16.3, 31.1)
    z = 1.217 * _lobe(lam, 437.0, 11.8, 36.0) + 0.681 * _lobe(lam, 459.0, 26.0, 13.8)
    return x, y, z


C2 = 1.438776877e-2   # second radiation constant hc/k, m K


def _planck_xyz(t_kelvin):
    X = Y = Z = 0.0
    for i in range(95):
        lam = 360.0 + 5.0 * i
        e = C2 / (lam * 1e-9 * t_kelvin)
        if e > 600.0:
            continue
        p = 1.0 / ((lam * 1e-3) ** 5 * math.expm1(e))
        x, y, z = cmf(lam)
        X += p * x
        Y += p * y
        Z += p * z
    return X, Y, Z


_Y5800 = _planck_xyz(5800.0)[1]


def blackbody_xyz(t_kelvin):
    """CIE XYZ of a blackbody, Planck's law over 360-830 nm at 5 nm.
    Relative units: 1.0 is Y of a 5800 K body."""
    X, Y, Z = _planck_xyz(t_kelvin)
    return X / _Y5800, Y / _Y5800, Z / _Y5800


def xyz_to_linear_srgb(X, Y, Z):
    return (3.2406 * X - 1.5372 * Y - 0.4986 * Z,
            -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
            0.0557 * X - 0.2040 * Y + 1.0570 * Z)


# ---------------------------------------------------------------- preview

def _hash(*ints):
    h = 0x811C9DC5
    for i in ints:
        h ^= i & 0xFFFFFFFF
        h = (h * 0x01000193) & 0xFFFFFFFF
        h ^= h >> 15
        h = (h * 0x2C1B3C6D) & 0xFFFFFFFF
        h ^= h >> 12
    return h


_STAR_COLOURS = []


def _star_colours():
    if not _STAR_COLOURS:
        for i in range(16):
            t = 3000.0 * (4.0 ** (i / 15.0))   # 3000 K .. 12000 K
            rgb = xyz_to_linear_srgb(*blackbody_xyz(t))
            m = max(rgb)
            _STAR_COLOURS.append(tuple(max(0.0, c / m) for c in rgb))
    return _STAR_COLOURS


GALAXY_POLE = (0.30, 0.94, 0.16)


def _value_noise(x, y):
    """Smooth value noise on a unit lattice, 0..1."""
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)

    def at(i, j):
        return (_hash(7, int(i) & 0xFFFF, int(j) & 0xFFFF) & 0xFFFF) / 65535.0
    a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * fx
    b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * fx
    return a + (b - a) * fy


def sky(s):
    """A procedural sky: a galactic band with a dust lane and a field of
    point stars on a cube grid, so one direction reads one cell. Linear RGB.
    A stand-in for Elite's own backdrop, which the shader samples instead."""
    ax, ay, az = abs(s[0]), abs(s[1]), abs(s[2])
    if ax >= ay and ax >= az:
        face, a, b, m = (0 if s[0] > 0 else 1), s[1], s[2], ax
    elif ay >= az:
        face, a, b, m = (2 if s[1] > 0 else 3), s[0], s[2], ay
    else:
        face, a, b, m = (4 if s[2] > 0 else 5), s[0], s[1], az
    n = 120
    fa = (a / m * 0.5 + 0.5) * n
    fb = (b / m * 0.5 + 0.5) * n
    ia, ib = int(fa), int(fb)
    h = _hash(face, ia, ib)
    col = [0.0, 0.0, 0.0]
    if h % 5 == 0:
        ca = ia + 0.25 + 0.5 * ((h >> 8) & 255) / 255.0
        cb = ib + 0.25 + 0.5 * ((h >> 16) & 255) / 255.0
        d2 = ((fa - ca) ** 2 + (fb - cb) ** 2) * (m * m)
        w = math.exp(-0.5 * d2 / (0.12 * 0.12))
        if w > 1e-4:
            mag = ((h >> 24) & 255) / 255.0
            bright = 9.0 * math.exp(-7.0 * mag)
            c = _star_colours()[(h >> 4) & 15]
            for i in range(3):
                col[i] += bright * w * c[i]
    z = s[0] * GALAXY_POLE[0] + s[1] * GALAXY_POLE[1] + s[2] * GALAXY_POLE[2]
    lon = math.atan2(s[2], s[0])
    clump = 0.55 + 0.9 * _value_noise(lon * 5.0, z * 12.0)
    dust = 1.0 - 0.75 * math.exp(-((z - 0.02 + 0.03 * math.sin(lon * 3.0)) / 0.045) ** 2)
    band = (0.05 * math.exp(-(z / 0.18) ** 2) * clump * dust
            + 0.008 * math.exp(-(z / 0.5) ** 2))
    col[0] += band * 1.0
    col[1] += band * 0.84
    col[2] += band * 0.70
    return col


class _Rows:
    """Orbits marched once per alpha row and interpolated per pixel. Rows are
    uniform in w = sign(a - a_sh) log(1 + |a - a_sh| / eps), dense at the
    shadow's edge where the sweep diverges."""

    def __init__(self, r_o, alpha_max, count):
        self.r_o = r_o
        self.a_sh = shadow_radius(r_o)
        self.eps = 1e-7 * max(self.a_sh, 1e-12)
        self.w_lo = self._w(0.0)
        self.w_hi = self._w(alpha_max)
        self.count = count
        self.rows = []
        for i in range(count):
            a = self._a(self.w_lo + (self.w_hi - self.w_lo) * i / (count - 1))
            rec = (array("d"), array("d"), array("d"))
            kind, phi_end, _ = march(r_o, a, record=rec)
            self.rows.append((a, kind, phi_end, rec))

    def _w(self, a):
        d = a - self.a_sh
        return math.copysign(math.log1p(abs(d) / self.eps), d)

    def _a(self, w):
        return self.a_sh + math.copysign(self.eps * math.expm1(abs(w)), w)

    def pair(self, alpha):
        f = (self._w(alpha) - self.w_lo) / (self.w_hi - self.w_lo) * (self.count - 1)
        i = max(0, min(self.count - 2, int(f)))
        return self.rows[i], self.rows[i + 1], min(1.0, max(0.0, f - i))

    @staticmethod
    def u_at(row, phi):
        _, kind, phi_end, (ps, us, vs) = row
        if phi > phi_end or len(ps) < 2:
            return None
        j = min(bisect.bisect_right(ps, phi) - 1, len(ps) - 2)
        if j < 0:
            return None
        h = ps[j + 1] - ps[j]
        if h <= 0.0:
            return us[j]
        return _hermite(us[j], vs[j], us[j + 1], vs[j + 1], h, (phi - ps[j]) / h)


def render(width, height, mass_solar, distance_km, fov_deg, disk, incl_deg,
           r_out, t_peak, rows=3000, disk_gain=1.0):
    """Linear-light preview, hole at the centre of the view, eye looking at
    it. Returns a list of rows of (r, g, b) floats."""
    m_km = mass_solar * KM_PER_SOLAR_MASS
    r_o = distance_km / m_km
    if r_o <= 3.0:
        raise ValueError("the eye must be outside the photon sphere, r > 3 M (%.1f km)" % (3 * m_km))
    tan_half = math.tan(math.radians(fov_deg) * 0.5)
    corner = math.atan(tan_half * math.hypot(1.0, height / float(width)))
    table = _Rows(r_o, min(math.pi, corner * 1.02), rows)
    inc = math.radians(incl_deg)
    wn = (0.0, math.sin(inc), math.cos(inc))     # disk normal; e1 = +z is the eye
    f_peak = disk_flux_peak()[1]
    y_peak = blackbody_xyz(t_peak)[1]
    lz_scale = r_o / math.sqrt(1.0 - 2.0 / r_o)
    out = []
    for y in range(height):
        line = []
        py = (1.0 - 2.0 * (y + 0.5) / height) * tan_half * height / float(width)
        for x in range(width):
            px = (2.0 * (x + 0.5) / width - 1.0) * tan_half
            inv = 1.0 / math.sqrt(px * px + py * py + 1.0)
            d = (px * inv, py * inv, -inv)
            st = math.hypot(d[0], d[1])
            alpha = math.atan2(st, -d[2])
            e2 = (d[0] / st, d[1] / st, 0.0) if st > 0 else (1.0, 0.0, 0.0)
            ra, rb, t = table.pair(alpha)
            near = ra if t < 0.5 else rb
            colour = None
            if disk:
                A = wn[2]
                B = e2[0] * wn[0] + e2[1] * wn[1]
                phi = math.atan2(-A, B) % math.pi
                if phi < 1e-12:
                    phi = math.pi
                while colour is None:
                    ua = table.u_at(ra, phi)
                    ub = table.u_at(rb, phi)
                    if ua is None and ub is None:
                        break
                    if ua is not None and ub is not None:
                        uc = ua + (ub - ua) * t
                    else:
                        # one row ended first: the pixel is on the shadow's edge
                        uc = ua if t < 0.5 else ub
                    if uc is not None and uc > 0.0:
                        rc = 1.0 / uc
                        if R_ISCO <= rc <= r_out:
                            # the arriving photon moves along -d, so its lz
                            # about the disk axis is -(e1 x d).w, e1 = +z
                            lz = -lz_scale * (-d[1] * wn[0] + d[0] * wn[1])
                            g = disk_redshift(rc, lz, r_o)
                            temp = t_peak * (disk_flux(rc) / f_peak) ** 0.25 * g
                            colour = list(xyz_to_linear_srgb(*blackbody_xyz(temp)))
                            fade = disk_gain / y_peak * min(1.0, (r_out - rc) / (0.25 * r_out))
                            colour = [c * fade for c in colour]
                            break
                    phi += math.pi
            if colour is None:
                if near[1] != "escape":
                    colour = [0.0, 0.0, 0.0]
                else:
                    if ra[1] == "escape" and rb[1] == "escape":
                        sweep = ra[2] + (rb[2] - ra[2]) * t
                    else:
                        sweep = near[2]
                    cs, sn = math.cos(sweep), math.sin(sweep)
                    colour = sky((sn * e2[0], sn * e2[1], cs))
            line.append(colour)
        out.append(line)
    return out


def _encode(img, exposure):
    rows = []
    for line in img:
        b = bytearray()
        for c in line:
            for v in c:
                v = 1.0 - math.exp(-max(0.0, v) * exposure)
                v = 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
                b.append(max(0, min(255, int(v * 255.0 + 0.5))))
        rows.append(bytes(b))
    return rows


def png_bytes(rows, width, height):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- reports

def describe(mass_solar, distance_km):
    m_km = mass_solar * KM_PER_SOLAR_MASS
    r_o = distance_km / m_km
    lines = ["%g solar masses: M = %.3f km, horizon radius %.2f km, photon sphere %.2f km,"
             % (mass_solar, m_km, 2 * m_km, 3 * m_km),
             "  innermost stable orbit (a feeding disk's inner edge) %.2f km." % (R_ISCO * m_km),
             "Seen from %g km (r = %.2f M):" % (distance_km, r_o)]
    if r_o <= 3.0:
        lines.append("  inside the photon sphere; this tool stops at r = 3 M.")
        return lines
    deg = 180.0 / math.pi
    a_sh = shadow_radius(r_o)
    lines.append("  shadow          %9.4f deg across" % (2 * a_sh * deg))
    lines.append("  Einstein ring   %9.4f deg across (a star exactly behind the hole)"
                 % (2 * einstein_ring(r_o) * deg))
    reach = lensing_reach(r_o, math.radians(1.0 / 60.0))
    if reach >= math.pi:
        lines.append("  lensing         bends the whole sky by more than 1 arcmin")
    else:
        lines.append("  lensing         bends light by more than 1 arcmin within %.4f deg of the hole"
                     % (reach * deg))
    if r_o > R_ISCO:
        lines.append("  disk inner edge %9.4f deg across, face-on and unlensed"
                     % (2 * math.degrees(math.atan(R_ISCO / r_o))))
    return lines


# ---------------------------------------------------------------- self-test

def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
            print("self-test FAILED: " + what)

    # Synge's shadow, against the march on both sides of the edge.
    for r_o in (3.5, 6.0, 20.0, 100.0, 1e4, 1e7):
        a = shadow_radius(r_o)
        check(march(r_o, a * (1 - 1e-6))[0] != "escape", "r_o=%g: inside the shadow is dark" % r_o)
        check(march(r_o, a * (1 + 1e-6))[0] == "escape", "r_o=%g: outside the shadow escapes" % r_o)
    check(abs(shadow_radius(3.0) - 0.5 * math.pi) < 1e-12, "the shadow is a hemisphere at r = 3")
    check(abs(shadow_radius(1e9) * 1e9 - B_CRIT) < 1e-6, "the shadow tends to b_c / r")

    # Weak field, from infinity (Keeton & Petters 2005), through sixth
    # order; what is left is the seventh-order term, about 1.4e4 / b^7.
    for b in (200.0, 1000.0, 10000.0):
        series = (4 / b + 15 * math.pi / (4 * b * b) + 128 / (3 * b ** 3)
                  + 3465 * math.pi / (64 * b ** 4) + 3584 / (5 * b ** 5)
                  + 255255 * math.pi / (256 * b ** 6))
        got = deflection_at_infinity(b)
        check(abs(got - series) < 1e-11 + 2e4 / b ** 7,
              "b=%g: weak-field bending %.12g, series %.12g" % (b, got, series))

    # Strong field (Bozza 2002); the residual must shrink with b - b_c.
    bbar = math.log(216.0 * (7.0 - 4.0 * math.sqrt(3.0))) - math.pi
    res = []
    for eps in (1e-3, 1e-4, 1e-5, 1e-6):
        got = deflection_at_infinity(B_CRIT * (1 + eps), 8000)
        res.append(abs(got - (-math.log(eps) + bbar)))
    check(res[3] < 1e-4 and res[3] < res[2] < res[1] < res[0],
          "strong-field residuals do not converge: %s" % res)

    # The shader's march against the quadrature. Across the view, to 1e-5
    # rad, a hundredth of a VR pixel (~1e-3 rad); within 1e-3 of the photon
    # ring, where the orbit is unstable, to 1e-4.
    for r_o in (8.0, 40.0, 400.0, 1e5):
        a_sh = shadow_radius(r_o)
        worst = 0.0
        for i in range(1, 60):
            a = a_sh + (math.pi - a_sh) * (i / 60.0) ** 2
            kind, phi, _ = march(r_o, a)
            ref = sweep_quadrature(r_o, a)
            check(kind == "escape", "r_o=%g alpha=%g: march escapes" % (r_o, a))
            worst = max(worst, abs(phi - ref))
        check(worst < 1e-5, "r_o=%g: march vs quadrature %.3g rad" % (r_o, worst))
        near = []
        for eps in (1e-3, 1e-4):
            a = math.asin(min(1.0, B_CRIT * (1 + eps) * math.sqrt(1 - 2 / r_o) / r_o))
            near.append(abs(march(r_o, a)[1] - sweep_quadrature(r_o, a, 8000)))
        check(max(near) < 1e-4, "r_o=%g: march near the photon ring %s" % (r_o, near))
    # The cap: every ray still circling at PHI_CAP is within 1e-5 of b_c.
    r_o = 40.0
    lo = math.asin(B_CRIT * math.sqrt(1 - 2 / r_o) / r_o)
    capped = _bisect(lambda a: 1.0 if march(r_o, a)[0] == "escape" else -1.0, lo, lo * 1.01, 60)
    check(impact_parameter(r_o, capped) / B_CRIT - 1.0 < 1e-5,
          "PHI_CAP darkens rays out to b/b_c - 1 = %.3g" % (impact_parameter(r_o, capped) / B_CRIT - 1))

    # A distant eye sees the weak-field Einstein ring, sqrt(4 M / D).
    for r_o in (1e5, 1e7):
        got = einstein_ring(r_o)
        check(abs(got / math.sqrt(4.0 / r_o) - 1.0) < 5.0 / math.sqrt(r_o),
              "r_o=%g: Einstein ring %.6g vs sqrt(4/r) %.6g" % (r_o, got, math.sqrt(4 / r_o)))

    # A straight-out ray is not bent; a flat-space eye far away sees a straight line.
    kind, phi, _ = march(50.0, math.pi)
    check(kind == "escape" and phi < 1e-12, "a radial outward ray escapes unswept")
    # Nearly radial rays, where u' ~ cot(alpha) is huge: out, against the
    # quadrature; in, captured.
    for r_o in (8.0, 50.0, 1e6):
        for off in (1e-2, 1e-5, 1e-9):
            kind, phi, _ = march(r_o, math.pi - off)
            ref = sweep_quadrature(r_o, math.pi - off)
            check(kind == "escape" and abs(phi - ref) < 1e-9,
                  "r_o=%g alpha=pi-%g: sweep %.3g vs %.3g" % (r_o, off, phi, ref))
            check(march(r_o, off * shadow_radius(r_o))[0] == "capture",
                  "r_o=%g: a ray aimed at the hole is captured" % r_o)
    kind, phi, _ = march(1e12, 0.5)
    check(kind == "escape" and abs(phi - (math.pi - 0.5)) < 1e-6, "far from the hole rays are straight")

    # Disk crossings land where the plane says, at the radius the orbit has there.
    r_o, a = 30.0, 0.6
    rec = (array("d"), array("d"), array("d"))
    kind, end, hits = march(r_o, a, crossings=(0.9, R_ISCO, 50.0), record=rec)
    check([h[0] for h in hits] == list(range(len(hits))) and hits and hits[0][1] == 0.9,
          "crossings are phi_first + k pi, in order")
    if hits:
        k2 = _Rows.u_at((a, kind, end, rec), 0.9)
        check(abs(1.0 / k2 - hits[0][2]) < 1e-9, "the recorded orbit and the crossing agree")

    # Page-Thorne flux: zero at the ISCO, positive beyond, Newtonian far out.
    check(disk_flux(R_ISCO) == 0.0 and disk_flux(6.0001) > 0.0, "flux is zero at r = 6 and positive past it")
    far = disk_flux(1e8) * (1e8) ** 3 * 8 * math.pi / 3
    check(abs(far - 1.0) < 1e-3, "flux tends to 3/(8 pi r^3): %.6g" % far)
    rp, _ = disk_flux_peak()
    check(9.0 < rp < 10.5, "flux peaks near r = 9.6 M: %.4g" % rp)

    # Redshift: face-on is gravitational plus transverse Doppler; the side
    # moving toward the eye is bluer than the side moving away.
    g0 = disk_redshift(10.0, 0.0, 1e9)
    check(abs(g0 - math.sqrt(0.7)) < 1e-6, "face-on g = sqrt(1 - 3/r)")
    check(disk_redshift(10.0, 5.0, 1e9) > g0 > disk_redshift(10.0, -5.0, 1e9), "approaching side is bluer")

    # Colour: the Planckian locus at 6504 K is (0.3135, 0.3237).
    X, Y, Z = blackbody_xyz(6504.0)
    x, y = X / (X + Y + Z), Y / (X + Y + Z)
    check(abs(x - 0.3135) < 0.004 and abs(y - 0.3237) < 0.004, "6504 K chromaticity (%.4f, %.4f)" % (x, y))
    r, g, b = xyz_to_linear_srgb(*blackbody_xyz(3000.0))
    check(r > g > b > 0.0, "3000 K is orange")
    r, g, b = xyz_to_linear_srgb(*blackbody_xyz(20000.0))
    check(b > g > r > 0.0, "20000 K is blue")

    # --dry-run writes nothing at all.
    scratch = tempfile.mkdtemp(prefix="bh_optics_")
    try:
        target = os.path.join(scratch, "sub", "preview.png")
        rc = main(["--render", target, "--size", "12", "--rows", "300", "--dry-run", "--quiet"])
        check(rc == 0, "a dry-run render succeeds")
        check(os.listdir(scratch) == [], "--dry-run wrote %s" % os.listdir(scratch))
        rc = main(["--render", target, "--size", "12", "--rows", "300", "--quiet"])
        check(rc == 0 and os.path.isfile(target), "a render writes its PNG")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print("blackhole_optics self-test: %d check(s) failed" % len(failures))
        return 1
    print("blackhole_optics self-test OK")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--at", nargs=2, type=float, metavar=("SOLAR_MASSES", "KM"),
                    help="what an eye at KM from a hole of SOLAR_MASSES sees")
    ap.add_argument("--render", metavar="PNG", help="write a preview image")
    ap.add_argument("--mass", type=float, default=10.0, help="solar masses (default 10)")
    ap.add_argument("--distance", type=float, default=600.0, help="km from the hole (default 600)")
    ap.add_argument("--fov", type=float, default=90.0, help="horizontal field of view, degrees")
    ap.add_argument("--size", type=int, default=480, help="image width and height, pixels")
    ap.add_argument("--disk", choices=("on", "off"), default="off")
    ap.add_argument("--incl", type=float, default=80.0, help="disk inclination, 0 face-on, 90 edge-on")
    ap.add_argument("--disk-out", type=float, default=24.0, help="disk outer edge, units of M")
    ap.add_argument("--disk-gain", type=float, default=1.0,
                    help="disk brightness; 1 puts a face-on peak at Y = 1")
    ap.add_argument("--disk-temp", type=float, default=9000.0, help="peak disk temperature, K")
    ap.add_argument("--exposure", type=float, default=1.6)
    ap.add_argument("--rows", type=int, default=3000, help="alpha rows marched for the preview")
    ap.add_argument("--dry-run", action="store_true", help="compute, write nothing")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.at:
        for line in describe(args.at[0], args.at[1]):
            print(line)
        return 0
    if args.render:
        try:
            img = render(args.size, args.size, args.mass, args.distance, args.fov,
                         args.disk == "on", args.incl, args.disk_out, args.disk_temp, args.rows,
                         args.disk_gain)
        except ValueError as e:
            print("blackhole_optics: %s" % e)
            return 2
        data = png_bytes(_encode(img, args.exposure), args.size, args.size)
        if args.dry_run:
            if not args.quiet:
                print("dry run: would write %d bytes to %s" % (len(data), args.render))
            return 0
        parent = os.path.dirname(os.path.abspath(args.render))
        os.makedirs(parent, exist_ok=True)
        with open(args.render, "wb") as f:
            f.write(data)
        if not args.quiet:
            print("wrote %s (%d bytes)" % (args.render, len(data)))
        return 0
    ap.print_usage()
    return 2


if __name__ == "__main__":
    sys.exit(main())
