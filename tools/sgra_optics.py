#!/usr/bin/env python3
"""Kerr optics and a hot accretion flow: the ground truth for EDVR's Sagittarius A*.

    python tools/sgra_optics.py --at 50 --incl 155        # Elite's Sgr A* from 50 ls, Sol's side
    python tools/sgra_optics.py --render sgra.png --distance 20 --incl 155 [--view radio]
    python tools/sgra_optics.py --assets docs/assets/sgra [--dry-run]
    python tools/sgra_optics.py --self-test

docs/design-sagittarius-a-2026-09-30.md is the design this serves. Sgr A*
spins, so the Schwarzschild march of tools/blackhole_optics.py is not enough
for it: this traces rays through the Kerr metric, and through the hot, thick,
optically thin flow round the hole, and it is what the design's images were
rendered with.

Units G = c = M = 1; a is the spin, 0 <= a < 1; Boyer-Lindquist r, theta,
phi, with the spin along theta = 0. A photon has constants lambda = L_z / E
and eta (Carter's constant over E^2). In Mino time sigma (d lambda_affine =
Sigma d sigma) its r and theta motions separate, and with u = 1/r:

    u''  = (2C - K) u + 3K u^2 + 2(C^2 - a^2 K) u^3,
           C = a^2 - a lambda, K = eta + (lambda - a)^2
    mu'' = (a^2 - eta - lambda^2) mu - 2 a^2 mu^3,  mu = cos(theta)
    phi' = a u (2 - a lambda u) / (1 - 2u + a^2 u^2) + lambda / (1 - mu^2)

both polynomial, smooth through turning points and over the poles (theta
itself is not: its equation is stiff within a degree of the axis, which
broke the first version), and u reaches 0 (escape) in finite sigma, as the
Binet form does for Schwarzschild. The camera is the zero angular momentum
observer; a ray is traced backward from it (s = -sigma) with RK4 steps
that move theta and phi by at most STEP, cut to 5% in r inside the flow
and to u/u' on the way in. It ends at u = 0 (the sky direction it came
from is theta, phi there), at the horizon, or after sweeping PATH_CAP on
the sky.

Emission along the way adds g^(3 + alpha) j dl, g the shift from the
fluid to the camera, dl the fluid-frame length; the flow's light is
optically thin and the sky shows through it.

The self-test holds it to: Bardeen's shadow outline (1973) for a distant
eye, the equatorial photon orbits and innermost stable orbits (Bardeen,
Press & Teukolsky 1972), both first integrals along every ray, Carter's
constant against Bardeen's image coordinates, the a = 0 limit against
blackhole_optics.py's march, and the redshift of Keplerian orbits.
Standard library only; --render and --assets use every core. --dry-run
with either computes and writes nothing at all. Exit 0 on success, 1 when a
self-test check fails, 2 on a usage error.
"""
import argparse
import math
import os
import shutil
import sys
import tempfile
from multiprocessing import Pool

import blackhole_optics as bho

# Elite's Sagittarius A* (EDSM, from the journal): 516,608 solar masses. The
# real one is about 8 times heavier; the image depends only on distance in
# units of M, so the game's mass keeps the game's own approach geometry
# (docs/design-sagittarius-a-2026-09-30.md, "Mass").
SGRA_SOLAR_MASSES = 516608.0
# EHT 2022 Paper V: the two models that pass all but the variability
# constraints are magnetically arrested, prograde, a = 0.5 and a = 0.94,
# seen 30 degrees off the axis. GRAVITY 2023: flares at 8.9 GM/c^2 go round
# clockwise with inclination 154.9 +- 4.6 deg.
SGRA_SPIN = 0.94
SUN_SIDE = 155.0     # degrees from the spin axis of the view from Sol
LS_KM = 299792.458
M_LS = SGRA_SOLAR_MASSES * bho.KM_PER_SOLAR_MASS / LS_KM   # M in light-seconds
M_SECONDS = M_LS                                           # GM/c^3 in seconds

STEP = math.pi / 96.0
PATH_CAP = 6.0 * math.pi
FLOW_FRAC = 0.05
R_FLOW = 60.0
MAX_STEPS = 20000

# The flow: a hot, geometrically thick, optically thin torus with an empty
# funnel along the spin, rotating at the Keplerian angular momentum of its
# cylindrical radius (the innermost stable orbit's inside it).
FLOW_THICKNESS = 0.45        # H / cylindrical radius
EYE_PROFILE = 4.0            # visible emissivity ~ r^-4 ...
RADIO_PROFILE = 3.0          # 1.3 mm emissivity ~ r^-3 ...
FLOW_OUTER = 25.0            # ... both tapering off past 25 M
TURBULENCE = 0.6             # log-normal contrast of the sheared structure
# Spiral modes (m, k, phase): cos(m phi + k ln r + phase); k ~ 2m is a
# pitch of about 27 degrees, what differential rotation winds structure to.
MODES = ((1, 2.3, 0.4), (2, 4.1, 2.1), (3, 5.9, 4.6), (4, 8.3, 1.3), (6, 12.2, 5.5))
EYE_ALPHA = 0.5              # F_nu ~ nu^-0.5: the measured 1.6-2.2 um slope, carried into the visible
RADIO_ALPHA = 0.0
SPOT_RADIUS = 9.0            # hot spot orbit, units of M
SPOT_WIDTH = 0.8
SPOT_GAIN = 1.0e4            # a bright flare: about 10x the flow's own light
SPOT_ALPHA = 0.5             # the same slope: measured flux-independent from 1 to 40 mJy


# ---------------------------------------------------------------- Kerr

def horizon(a):
    return 1.0 + math.sqrt(1.0 - a * a)


def photon_orbit_radius(a, prograde=True):
    """Radius of the circular equatorial photon orbit."""
    return 2.0 * (1.0 + math.cos(2.0 / 3.0 * math.acos(-a if prograde else a)))


def isco_radius(a, prograde=True):
    """Innermost stable circular orbit (Bardeen, Press & Teukolsky 1972)."""
    z1 = 1.0 + (1.0 - a * a) ** (1.0 / 3.0) * ((1.0 + a) ** (1.0 / 3.0) + (1.0 - a) ** (1.0 / 3.0))
    z2 = math.sqrt(3.0 * a * a + z1 * z1)
    s = math.sqrt((3.0 - z1) * (3.0 + z1 + 2.0 * z2))
    return 3.0 + z2 - s if prograde else 3.0 + z2 + s


def spherical_photon_constants(a, r):
    """(lambda, eta) of the spherical photon orbit at r; a > 0."""
    lam = -(r ** 3 - 3.0 * r * r + a * a * r + a * a) / (a * (r - 1.0))
    eta = -r ** 3 * (r ** 3 - 6.0 * r * r + 9.0 * r - 4.0 * a * a) / (a * a * (r - 1.0) ** 2)
    return lam, eta


def keplerian_ell(a, r):
    """Specific angular momentum -u_phi/u_t of a prograde circular orbit."""
    sr = math.sqrt(r)
    return (r * r - 2.0 * a * sr + a * a) / (r * sr - 2.0 * sr + a)


def metric(a, r, th):
    """(Sigma, Delta, A, g_tt, g_tphi, g_phiphi) at (r, theta)."""
    s = math.sin(th)
    c = math.cos(th)
    s2 = s * s
    sig = r * r + a * a * c * c
    dl = r * r - 2.0 * r + a * a
    big_a = (r * r + a * a) ** 2 - a * a * dl * s2
    return sig, dl, big_a, -(1.0 - 2.0 * r / sig), -2.0 * a * r * s2 / sig, big_a * s2 / sig


def camera_ray(a, r_o, th_o, n):
    """A photon arriving at the zero-angular-momentum camera at (r_o, th_o)
    moving along n = (n_r, n_theta, n_phi), a unit vector in the camera's
    frame. Returns (lam, eta, u, u_s, th_s, e_cam) for the backward trace:
    u_s and th_s are du/ds and dtheta/ds with s = -sigma, and e_cam is the
    photon's energy seen by the camera per unit energy at infinity."""
    sig, dl, big_a, _, _, _ = metric(a, r_o, th_o)
    s = math.sin(th_o)
    c = math.cos(th_o)
    lapse = math.sqrt(sig * dl / big_a)
    omega = 2.0 * a * r_o / big_a
    ang = n[2] * s * math.sqrt(big_a / sig)          # L for unit local energy
    e_inf = lapse + omega * ang
    lam = ang / e_inf
    p_th = math.sqrt(sig) * n[1] / e_inf
    p_r = math.sqrt(sig / dl) * n[0] / e_inf
    eta = p_th * p_th - a * a * c * c + lam * lam * c * c / (s * s)
    u = 1.0 / r_o
    return lam, eta, u, u * u * dl * p_r, -p_th, 1.0 / e_inf


def radial_potential(a, lam, eta, u):
    """u^4 R(1/u); equals (du/dsigma)^2 on the ray."""
    cc = a * a - a * lam
    kk = eta + (lam - a) ** 2
    return 1.0 + (2.0 * cc - kk) * u * u + 2.0 * kk * u ** 3 + (cc * cc - a * a * kk) * u ** 4


def polar_potential(a, lam, eta, th):
    """Theta(theta); equals (dtheta/dsigma)^2 on the ray."""
    c = math.cos(th)
    s = math.sin(th)
    return eta + a * a * c * c - lam * lam * c * c / (s * s)


def mu_potential(a, lam, eta, mu):
    """(1 - mu^2) Theta; equals (dmu/dsigma)^2 on the ray, mu = cos(theta)."""
    return (1.0 - mu * mu) * (eta + a * a * mu * mu) - lam * lam * mu * mu


class Flow(object):
    """What the flow and the hot spot emit, and how they move."""

    def __init__(self, a, frames=(), spot_phase=0.0):
        self.a = a
        self.r_isco = isco_radius(a)
        self.frames = tuple(frames)       # observer times of the hot-spot frames
        self.spot_omega = 1.0 / (SPOT_RADIUS ** 1.5 + a)
        self.spot_phase = spot_phase
        rs = math.sqrt(SPOT_RADIUS * SPOT_RADIUS + a * a)
        self.spot_rho = rs

    def fluid_energy(self, r, th, lam, omega_override=None):
        """(E_fluid = -p.u, Sigma) for the flow, or for a body rotating at
        omega_override; E_fluid is None where no such motion is timelike
        (at or inside the horizon)."""
        a = self.a
        sig, dl, big_a, gtt, gtp, gpp = metric(a, r, th)
        if dl <= 0.0:
            return None, sig
        if omega_override is None:
            rho = max(r * abs(math.sin(th)), self.r_isco)
            ell = keplerian_ell(a, rho)
            om = -(gtp + ell * gtt) / (gpp + ell * gtp)
        else:
            om = omega_override
        norm = -(gtt + 2.0 * om * gtp + om * om * gpp)
        if norm <= 1e-12:
            om = 2.0 * a * r / big_a          # fall back to the local ZAMO
            norm = -(gtt + 2.0 * om * gtp + om * om * gpp)
            if norm <= 0.0:
                return None, sig
        return (1.0 - om * lam) / math.sqrt(norm), sig

    def profile(self, r, th, power):
        s = math.sin(th)
        rho = r * abs(s)
        z = r * math.cos(th)
        h = FLOW_THICKNESS * rho
        if h <= 1e-9:
            return 0.0
        return r ** -power * math.exp(-0.5 * (z / h) ** 2) / (1.0 + (r / FLOW_OUTER) ** 4)

    @staticmethod
    def structure(r, ph):
        """Mean-one log-normal modulation: turbulence sheared into spirals.
        A model of the look of the flows in GRMHD simulations, not a
        measurement; see the design doc."""
        lr = math.log(r)
        acc = 0.0
        for m, k, ps in MODES:
            acc += math.cos(m * ph + k * lr + ps)
        acc /= math.sqrt(0.5 * len(MODES))
        return math.exp(TURBULENCE * acc - 0.5 * TURBULENCE * TURBULENCE)

    def emission(self, u, th, ph, t, lam, e_cam, out):
        """Add emissivity * dl/ds at this point to out: [eye, radio, spot...]."""
        r = 1.0 / u
        if r > R_FLOW:
            return
        j_eye = self.profile(r, th, EYE_PROFILE)
        spot_near = self.frames and abs(r - SPOT_RADIUS) <= 5.0 * SPOT_WIDTH
        if j_eye < 1e-12 and not spot_near:
            return
        e_fl, sig = self.fluid_energy(r, th, lam)
        if e_fl is None or e_fl <= 0.0:
            return
        g = e_cam / e_fl
        dlds = sig * e_fl * self.structure(r, ph)
        out[0] += g ** (3.0 + EYE_ALPHA) * j_eye * dlds
        out[1] += g ** (3.0 + RADIO_ALPHA) * self.profile(r, th, RADIO_PROFILE) * dlds
        if not spot_near:
            return
        a = self.a
        rs = math.sqrt(r * r + a * a)
        x = rs * math.sin(th) * math.cos(ph)
        y = rs * math.sin(th) * math.sin(ph)
        z = r * math.cos(th)
        e_sp = None
        for i, t_obs in enumerate(self.frames):
            ph_s = self.spot_phase + self.spot_omega * (t_obs + t)
            dx = x - self.spot_rho * math.cos(ph_s)
            dy = y - self.spot_rho * math.sin(ph_s)
            d2 = dx * dx + dy * dy + z * z
            if d2 > 25.0 * SPOT_WIDTH * SPOT_WIDTH:
                continue
            if e_sp is None:
                e_sp, _ = self.fluid_energy(r, th, lam, self.spot_omega)
                if e_sp is None or e_sp <= 0.0:
                    return
            j = SPOT_GAIN * SPOT_RADIUS ** -EYE_PROFILE * math.exp(-0.5 * d2 / (SPOT_WIDTH * SPOT_WIDTH))
            out[2 + i] += (e_cam / e_sp) ** (3.0 + SPOT_ALPHA) * j * sig * e_sp


def trace(a, r_o, th_o, n, flow=None, step=STEP, record=None):
    """Backward-trace the photon that reaches the camera moving along n.
    Returns (kind, th, ph, light): kind 'escape' (th, ph: the sky direction
    it came from), 'capture' (reached the horizon) or 'orbit' (still
    circling after PATH_CAP, drawn as captured); light is the emission
    gathered on the way, [eye, radio, spot frames...], zeros without a
    flow. record, when a list, receives (s, u, u_s, mu, mu_s, ph, t)."""
    lam, eta, u, us, ths, e_cam = camera_ray(a, r_o, th_o, n)
    if abs(lam) < 1e-4:
        # Exactly through the pole the azimuth jumps by pi, which the mu
        # form cannot see; a ray 1e-4 off it swings through pi smoothly.
        # eta moves with lambda so mu's first integral still holds here.
        grow = 1e-8 - lam * lam
        lam = 1e-4 if lam >= 0.0 else -1e-4
        eta += grow * math.cos(th_o) ** 2 / math.sin(th_o) ** 2
    mu = math.cos(th_o)
    mus = -math.sin(th_o) * ths
    cc = a * a - a * lam
    kk = eta + (lam - a) ** 2
    k1c = 2.0 * cc - kk
    k2c = 3.0 * kk
    k3c = 2.0 * (cc * cc - a * a * kk)
    a2 = a * a
    m1c = a2 - eta - lam * lam
    m3c = -2.0 * a2
    u_cap = 1.0 / (horizon(a) * (1.0 + 1e-3))
    ph = 0.0
    t = 0.0
    s_total = 0.0
    swept = 0.0
    light = [0.0] * (2 + (len(flow.frames) if flow else 0))

    def acc_u(x):
        return x * (k1c + x * (k2c + x * k3c))

    def acc_mu(y):
        return y * (m1c + m3c * y * y)

    def vel(x, y):
        s2 = max(1.0 - y * y, 1e-300)
        d = 1.0 - 2.0 * x + a2 * x * x
        vp = -(a * x * (2.0 - a * lam * x) / d + lam / s2)
        xxd = x * x * d
        vt = -((1.0 + a2 * x * x) * (1.0 + cc * x * x) / xxd + a * (lam - a * s2)) if xxd > 0.0 else 0.0
        return vp, vt

    def emit(x, y, p, tt, sink):
        if flow is not None and x > 1.0 / R_FLOW:
            flow.emission(x, math.acos(max(-1.0, min(1.0, y))), p, tt, lam, e_cam, sink)

    here = [0.0] * len(light)
    emit(u, mu, ph, t, here)
    for _ in range(MAX_STEPS):
        vp, vt = vel(u, mu)
        s2 = max(1.0 - mu * mu, 1e-300)
        th_rate2 = mus * mus / s2
        h = step / max(math.sqrt(th_rate2 + vp * vp), 1e-300)
        if us > 0.0:
            h = min(h, u / us)
        if flow is not None and u > 1.0 / R_FLOW and us != 0.0:
            h = min(h, FLOW_FRAC * u / abs(us))
        if us < 0.0 and u < -us * h:
            h = min(h, 1.05 * u / -us)
        # RK4 on (u, u_s, mu, mu_s, ph, t)
        a1u, a1m = acc_u(u), acc_mu(mu)
        p1, t1 = vp, vt
        u2, us2 = u + 0.5 * h * us, us + 0.5 * h * a1u
        m2, ms2 = mu + 0.5 * h * mus, mus + 0.5 * h * a1m
        a2u, a2m = acc_u(u2), acc_mu(m2)
        p2, t2 = vel(u2, m2)
        u3, us3 = u + 0.5 * h * us2, us + 0.5 * h * a2u
        m3, ms3 = mu + 0.5 * h * ms2, mus + 0.5 * h * a2m
        a3u, a3m = acc_u(u3), acc_mu(m3)
        p3, t3 = vel(u3, m3)
        u4, us4 = u + h * us3, us + h * a3u
        m4, ms4 = mu + h * ms3, mus + h * a3m
        a4u, a4m = acc_u(u4), acc_mu(m4)
        p4, t4 = vel(u4, m4)
        nu = u + h / 6.0 * (us + 2.0 * us2 + 2.0 * us3 + us4)
        nus = us + h / 6.0 * (a1u + 2.0 * a2u + 2.0 * a3u + a4u)
        nmu = mu + h / 6.0 * (mus + 2.0 * ms2 + 2.0 * ms3 + ms4)
        nmus = mus + h / 6.0 * (a1m + 2.0 * a2m + 2.0 * a3m + a4m)
        nph = ph + h / 6.0 * (p1 + 2.0 * p2 + 2.0 * p3 + p4)
        nt = t + h / 6.0 * (t1 + 2.0 * t2 + 2.0 * t3 + t4) if min(u2, u3, u4) > 0.0 else t
        if nu <= 0.0:
            f = bho._hermite_root(u, us, nu, nus, h, 0.0)
            mu_end = max(-1.0, min(1.0, bho._hermite(mu, mus, nmu, nmus, h, f)))
            ph_end = ph + (nph - ph) * f
            if record is not None:
                record.append((s_total + f * h, 0.0, us + (nus - us) * f, mu_end, mus, ph_end, t))
            return "escape", math.acos(mu_end), ph_end, light
        there = [0.0] * len(light)
        emit(nu, nmu, nph, nt, there)
        for i in range(len(light)):
            light[i] += 0.5 * h * (here[i] + there[i])
        here = there
        swept += math.sqrt(th_rate2 + s2 * vp * vp) * h
        u, us, mu, mus, ph, t = nu, nus, nmu, nmus, nph, nt
        s_total += h
        if record is not None:
            record.append((s_total, u, us, mu, mus, ph, t))
        if u >= u_cap:
            return "capture", math.acos(max(-1.0, min(1.0, mu))), ph, light
        if swept > PATH_CAP:
            return "orbit", math.acos(max(-1.0, min(1.0, mu))), ph, light
    return "orbit", math.acos(max(-1.0, min(1.0, mu))), ph, light


def camera_direction(px, py):
    """The arriving photon's direction n = -d for the view ray through image
    point (px, py) (tangent-plane units; right = +phi, up = -theta, forward
    = toward the hole)."""
    inv = 1.0 / math.sqrt(1.0 + px * px + py * py)
    return (inv, py * inv, -px * inv)


def sky_direction(th, ph):
    return (math.sin(th) * math.cos(ph), math.sin(th) * math.sin(ph), math.cos(th))


def shadow_edge(a, r_o, th_o, chi, iters=40):
    """Angular radius (rad) of the shadow at image position angle chi
    (0 = right, pi/2 = up), by bisection on capture."""
    lo, hi = 0.0, math.pi * 0.5
    for _ in range(iters):
        mid = 0.5 * (lo + hi)
        t = math.tan(mid)
        kind = trace(a, r_o, th_o, camera_direction(t * math.cos(chi), t * math.sin(chi)))[0]
        if kind == "escape":
            hi = mid
        else:
            lo = mid
    return 0.5 * (lo + hi)


# ---------------------------------------------------------------- pictures

GALAXY_POLE = (0.0, 1.0, 0.0)   # the Galactic plane holds the spin axis (see the design)


def core_sky(s):
    """A stand-in for the sky at the Galactic Centre, linear RGB: a dense
    field of mostly red-giant stars, the Galactic disk as a bright band
    split by dust, and the bulge's glow all round. Elite's own backdrop
    replaces it in the game."""
    ax, ay, az = abs(s[0]), abs(s[1]), abs(s[2])
    if ax >= ay and ax >= az:
        face, p, q, m = (0 if s[0] > 0 else 1), s[1], s[2], ax
    elif ay >= az:
        face, p, q, m = (2 if s[1] > 0 else 3), s[0], s[2], ay
    else:
        face, p, q, m = (4 if s[2] > 0 else 5), s[0], s[1], az
    n = 150
    fp = (p / m * 0.5 + 0.5) * n
    fq = (q / m * 0.5 + 0.5) * n
    ip, iq = int(fp), int(fq)
    h = bho._hash(face + 11, ip, iq)
    col = [0.0, 0.0, 0.0]
    if h % 3 == 0:
        cp = ip + 0.25 + 0.5 * ((h >> 8) & 255) / 255.0
        cq = iq + 0.25 + 0.5 * ((h >> 16) & 255) / 255.0
        d2 = ((fp - cp) ** 2 + (fq - cq) ** 2) * (m * m)
        w = math.exp(-0.5 * d2 / (0.12 * 0.12))
        if w > 1e-4:
            bright = 7.0 * math.exp(-6.0 * ((h >> 24) & 255) / 255.0)
            k = (h >> 4) & 15
            c = bho._star_colours()[k // 3 if k < 13 else k]    # mostly cool giants
            for i in range(3):
                col[i] += bright * w * c[i]
    z = s[0] * GALAXY_POLE[0] + s[1] * GALAXY_POLE[1] + s[2] * GALAXY_POLE[2]
    lon = math.atan2(s[2], s[0])
    clump = 0.6 + 0.8 * bho._value_noise(lon * 6.0, z * 14.0)
    dust = 1.0 - 0.8 * math.exp(-((z - 0.015 * math.sin(lon * 3.0)) / 0.035) ** 2)
    band = 0.08 * math.exp(-(z / 0.14) ** 2) * clump * dust
    glow = 0.006 + 0.02 * math.exp(-(z / 0.45) ** 2)
    col[0] += band * 1.0 + glow * 1.0
    col[1] += band * 0.82 + glow * 0.85
    col[2] += band * 0.66 + glow * 0.7
    return col


def _power_law_rgb(alpha):
    """Linear sRGB of F_nu ~ nu^-alpha across the visible, max channel 1."""
    X = Y = Z = 0.0
    for i in range(95):
        lam = 360.0 + 5.0 * i
        f = lam ** (alpha - 2.0)
        x, y, z = bho.cmf(lam)
        X += f * x
        Y += f * y
        Z += f * z
    rgb = bho.xyz_to_linear_srgb(X, Y, Z)
    m = max(rgb)
    return tuple(max(0.0, c / m) for c in rgb)


def afmhot(x):
    x = max(0.0, min(1.0, x))
    return (min(1.0, 2.0 * x), max(0.0, min(1.0, 2.0 * x - 0.5)), max(0.0, min(1.0, 2.0 * x - 1.0)))


def _render_row(job):
    a, r_o, th_o, fov, width, height, y, frames, phase, with_flow = job
    flow = Flow(a, frames, phase) if with_flow else None
    tan_half = math.tan(math.radians(fov) * 0.5)
    py = (1.0 - 2.0 * (y + 0.5) / height) * tan_half * height / float(width)
    row = []
    for x in range(width):
        px = (2.0 * (x + 0.5) / width - 1.0) * tan_half
        kind, th, ph, light = trace(a, r_o, th_o, camera_direction(px, py), flow)
        sky = core_sky(sky_direction(th, ph)) if kind == "escape" else None
        if flow is None:
            light = [0.0] * (2 + len(frames))
        row.append((sky, light))
    return y, row


def render(a, r_o, th_o, fov, width, height, frames=(), phase=0.0, procs=None, with_flow=True):
    """Rows of (sky rgb or None, [eye, radio, spot frames...])."""
    jobs = [(a, r_o, th_o, fov, width, height, y, tuple(frames), phase, with_flow) for y in range(height)]
    rows = [None] * height
    if procs == 1:
        for job in jobs:
            y, row = _render_row(job)
            rows[y] = row
        return rows
    with Pool(procs) as pool:
        for y, row in pool.imap_unordered(_render_row, jobs, chunksize=2):
            rows[y] = row
    return rows


def _tone(v):
    """A filmic curve (Narkowicz's ACES fit: a toe for real blacks and a
    soft shoulder), then sRGB."""
    v = max(0.0, v)
    v = min(1.0, v * (2.51 * v + 0.03) / (v * (2.43 * v + 0.59) + 0.14))
    return 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def blur_radio(rows, sigma_px):
    """Gaussian-blur the 1.3 mm channel, as a telescope's beam does."""
    height, width = len(rows), len(rows[0])
    rad = int(3.0 * sigma_px + 1)
    ker = [math.exp(-0.5 * (i / sigma_px) ** 2) for i in range(-rad, rad + 1)]
    tot = sum(ker)
    ker = [k / tot for k in ker]
    src = [[light[1] for _, light in row] for row in rows]
    tmp = [[sum(ker[k + rad] * line[min(width - 1, max(0, x + k))] for k in range(-rad, rad + 1))
            for x in range(width)] for line in src]
    out = [[sum(ker[k + rad] * tmp[min(height - 1, max(0, y + k))][x] for k in range(-rad, rad + 1))
            for x in range(width)] for y in range(height)]
    return [[(sky, [light[0], out[y][x]] + list(light[2:])) for x, (sky, light) in enumerate(row)]
            for y, row in enumerate(rows)]


def auto_gain(rows, view, frame=None, exposure=1.0):
    """The gain that puts the brightest 0.5% of the flow at the top of the
    display range, as an auto-exposure would."""
    vals = []
    for row in rows:
        for _, light in row:
            v = light[1] if view == "radio" else light[0] + (light[2 + frame] if frame is not None else 0.0)
            vals.append(v)
    vals.sort()
    top = vals[min(len(vals) - 1, int(0.995 * len(vals)))] or 1.0
    return exposure * (0.95 if view == "radio" else 2.5) / top


def compose(rows, view, gain, frame=None):
    """8-bit rows. view 'eye': the lensed sky plus the flow's glow (and the
    hot spot of one frame), brightness compressed as a tonemapper does;
    view 'radio': the flow at 1.3 mm in the EHT's colour map, no stars."""
    eye_rgb = _power_law_rgb(EYE_ALPHA)
    spot_rgb = _power_law_rgb(SPOT_ALPHA)
    out = []
    for row in rows:
        line = bytearray()
        for sky, light in row:
            if view == "radio":
                c = afmhot(gain * light[1])
                line.extend(int(255.0 * v + 0.5) for v in c)
                continue
            glow = gain * light[0]
            spot = gain * light[2 + frame] if frame is not None else 0.0
            base = sky if sky is not None else (0.0, 0.0, 0.0)
            for i in range(3):
                v = _tone(base[i] + glow * eye_rgb[i] + spot * spot_rgb[i])
                line.append(max(0, min(255, int(v * 255.0 + 0.5))))
        out.append(bytes(line))
    return out


def side_by_side(panels, gap=4):
    """Join equal-height 8-bit RGB panels left to right with a dark gap."""
    height = len(panels[0])
    rows = []
    for y in range(height):
        parts = []
        for i, p in enumerate(panels):
            if i:
                parts.append(bytes(3 * gap))
            parts.append(p[y])
        rows.append(b"".join(parts))
    width = sum(len(p[0]) // 3 for p in panels) + gap * (len(panels) - 1)
    return rows, width


# ---------------------------------------------------------------- reports

def describe(distance_ls, incl_deg, a=SGRA_SPIN):
    r_o = distance_ls / M_LS
    th_o = math.radians(incl_deg)
    deg = 180.0 / math.pi
    lines = ["Elite's Sgr A*: %g solar masses, M = %.3f ls (%.0f km), GM/c^3 = %.2f s, spin a = %g"
             % (SGRA_SOLAR_MASSES, M_LS, M_LS * LS_KM, M_SECONDS, a),
             "  horizon %.3f M, prograde photon orbit %.3f M, innermost stable orbit %.3f M"
             % (horizon(a), photon_orbit_radius(a), isco_radius(a)),
             "  a hot spot at %g M orbits in %.1f minutes" % (
                 SPOT_RADIUS, 2.0 * math.pi * (SPOT_RADIUS ** 1.5 + a) * M_SECONDS / 60.0),
             "Seen from %g ls (r = %.2f M), %g deg from the spin axis:" % (distance_ls, r_o, incl_deg)]
    if r_o < 4.0:
        lines.append("  closer than 4 M; this report stops there.")
        return lines
    w = (shadow_edge(a, r_o, th_o, 0.0) + shadow_edge(a, r_o, th_o, math.pi)) * deg
    hgt = 2.0 * shadow_edge(a, r_o, th_o, 0.5 * math.pi) * deg
    lines.append("  shadow %.3f deg wide, %.3f deg tall" % (w, hgt))
    return lines


# ---------------------------------------------------------------- self-test

def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
            print("self-test FAILED: " + what)

    # Closed forms.
    check(abs(photon_orbit_radius(0.0) - 3.0) < 1e-12, "photon orbit is 3 M at a = 0")
    check(abs(photon_orbit_radius(1.0) - 1.0) < 1e-9 and abs(photon_orbit_radius(1.0, False) - 4.0) < 1e-9,
          "photon orbits 1 and 4 M at a = 1")
    check(abs(isco_radius(0.0) - 6.0) < 1e-12, "ISCO 6 M at a = 0")
    check(abs(isco_radius(1.0) - 1.0) < 1e-9 and abs(isco_radius(1.0, False) - 9.0) < 1e-9, "ISCO 1 and 9 M at a = 1")
    check(abs(isco_radius(0.9) - 2.3209) < 1e-4, "ISCO 2.3209 M at a = 0.9")
    check(abs(isco_radius(0.94) - 2.0236) < 1e-4, "ISCO 2.0236 M at a = 0.94")
    for a in (0.5, 0.9):
        for pro in (True, False):
            lam, eta = spherical_photon_constants(a, photon_orbit_radius(a, pro))
            check(abs(eta) < 1e-9, "a=%g: the equatorial photon orbit has eta = 0" % a)
    check(abs(keplerian_ell(0.0, 10.0) - 10.0 ** 1.5 / (10.0 - 2.0)) < 1e-12,
          "Schwarzschild Keplerian ell = r^(3/2) / (r - 2)")

    # Camera constants: both first integrals hold at the start, and Carter's
    # constant matches Bardeen's image coordinates for a distant eye.
    a, r_o, th_o = 0.9, 1e4, math.radians(60.0)
    for px, py in ((3e-4, 2e-4), (-6e-4, 1e-4), (1e-4, -7e-4)):
        lam, eta, u, us, ths, _ = camera_ray(a, r_o, th_o, camera_direction(px, py))
        check(abs(radial_potential(a, lam, eta, u) - (us / (u * u)) ** 2 * u ** 4) < 1e-9,
              "the radial first integral holds at the camera")
        check(abs(polar_potential(a, lam, eta, th_o) - ths * ths) < 1e-9 * max(1.0, eta),
              "the polar first integral holds at the camera")
        al, be = px * r_o, py * r_o
        lam_b = -al * math.sin(th_o)
        eta_b = be * be + (al * al - a * a) * math.cos(th_o) ** 2
        check(abs(lam - lam_b) < 2e-3 * max(1.0, abs(lam_b)) and abs(eta - eta_b) < 2e-3 * max(1.0, eta_b),
              "Bardeen coordinates: lam %.5g/%.5g eta %.5g/%.5g" % (lam, lam_b, eta, eta_b))

    # Both first integrals hold along a whole ray.
    rec = []
    trace(0.9, 30.0, math.radians(70.0), camera_direction(0.21, 0.13), record=rec)
    lam, eta, _, _, _, _ = camera_ray(0.9, 30.0, math.radians(70.0), camera_direction(0.21, 0.13))
    worst = 0.0
    for (_, u, us, mu, mus, _, _) in rec[:-1]:
        worst = max(worst, abs(radial_potential(0.9, lam, eta, u) - us * us),
                    abs(mu_potential(0.9, lam, eta, mu) - mus * mus) / max(1.0, eta))
    check(len(rec) > 20 and worst < 1e-7, "first integrals drift %.3g along a ray" % worst)
    # ... and along one that passes within a degree of the pole, which broke
    # the theta form of the equations.
    rec = []
    n_pole = camera_direction(-0.0795 * 1.03 / math.sin(math.radians(60.0)) / 1e4, 4.82 * 1.03 / 1e4)
    kind = trace(0.9, 1e4, math.radians(60.0), n_pole, record=rec)[0]
    lam, eta, _, _, _, _ = camera_ray(0.9, 1e4, math.radians(60.0), n_pole)
    worst = max(abs(mu_potential(0.9, lam, eta, r[3]) - r[4] * r[4]) / max(1.0, eta) for r in rec[:-1])
    check(kind == "escape" and worst < 1e-7, "near-polar ray: %s, drift %.3g" % (kind, worst))

    # Bardeen's shadow outline, a = 0.9, eye 60 deg from the axis at 1e4 M:
    # 1% inside is captured, 1% outside escapes.
    a, r_o, th_o = 0.9, 1e4, math.radians(60.0)
    rp, rm = photon_orbit_radius(a), photon_orbit_radius(a, False)
    tested = 0
    for i in range(1, 12):
        r = rp + (rm - rp) * i / 12.0
        lam, eta = spherical_photon_constants(a, r)
        beta2 = eta + a * a * math.cos(th_o) ** 2 - lam * lam / math.tan(th_o) ** 2
        if beta2 <= 0.0:
            continue
        al, be = -lam / math.sin(th_o), math.sqrt(beta2)
        for sgn in (1.0, -1.0):
            inner = trace(a, r_o, th_o, camera_direction(0.99 * al / r_o, sgn * 0.99 * be / r_o))[0]
            outer = trace(a, r_o, th_o, camera_direction(1.01 * al / r_o, sgn * 1.01 * be / r_o))[0]
            check(inner != "escape" and outer == "escape",
                  "Bardeen outline at r=%.3f: inside %s, outside %s" % (r, inner, outer))
            tested += 1
    check(tested >= 12, "enough of the outline visible at 60 deg (%d)" % tested)

    # a = 0: the shadow is Synge's, and escaping rays land where the
    # Schwarzschild march says.
    r_o = 40.0
    edge = shadow_edge(0.0, r_o, math.radians(90.0), 0.3)
    check(abs(edge - bho.shadow_radius(r_o)) < 1e-6, "a=0 shadow %.8f vs Synge %.8f" % (edge, bho.shadow_radius(r_o)))
    for ang in (0.2, 0.5, 1.2, 2.2, 3.0):
        n = (math.cos(ang), 0.0, -math.sin(ang))    # the view ray is ang from the hole, to the right
        kind, th, ph, _ = trace(0.0, r_o, 0.5 * math.pi, n)
        ref = bho.sweep_quadrature(r_o, ang)
        check(kind == "escape" and abs(th - 0.5 * math.pi) < 1e-9 and abs(ph - ref) < 1e-5,
              "a=0 alpha=%g: phi %.8f vs quadrature %.8f" % (ang, ph, ref))

    # The flow's redshift, Schwarzschild limit, against blackhole_optics.
    flow = Flow(0.0)
    for r in (7.0, 12.0):
        for lz in (-4.0, 0.0, 4.0):
            e_fl, _ = flow.fluid_energy(r, 0.5 * math.pi, lz)
            g = 1.0 / e_fl       # an eye at infinity
            check(abs(g - bho.disk_redshift(r, lz, 1e30)) < 1e-9, "r=%g lz=%g: Keplerian g" % (r, lz))
    # Kerr: a = 0.9 circular orbit, u^t and Omega in closed form.
    a, r = 0.9, 5.0
    om = 1.0 / (r ** 1.5 + a)
    ut = (r ** 1.5 + a) / (r ** 0.75 * math.sqrt(r ** 1.5 - 3.0 * math.sqrt(r) + 2.0 * a))
    e_fl, _ = Flow(a).fluid_energy(r, 0.5 * math.pi, 2.0)
    check(abs(e_fl - ut * (1.0 - om * 2.0)) < 1e-9, "Kerr Keplerian E_fluid %.9f vs %.9f" % (e_fl, ut * (1 - 2 * om)))

    # Emission: the approaching side (lambda > 0) outshines the receding one.
    flow = Flow(0.9)
    left = trace(0.9, 40.0, math.radians(80.0), camera_direction(-0.2, -0.02), flow)[3][0]
    right = trace(0.9, 40.0, math.radians(80.0), camera_direction(0.2, -0.02), flow)[3][0]
    check(left > 2.0 * right > 0.0, "Doppler: approaching %.4g, receding %.4g" % (left, right))

    # --dry-run writes nothing at all.
    scratch = tempfile.mkdtemp(prefix="sgra_optics_")
    try:
        target = os.path.join(scratch, "sub", "x.png")
        rc = main(["--render", target, "--size", "6", "--procs", "1", "--dry-run", "--quiet"])
        check(rc == 0 and os.listdir(scratch) == [], "--dry-run wrote %s" % os.listdir(scratch))
        rc = main(["--render", target, "--size", "6", "--procs", "1", "--quiet"])
        check(rc == 0 and os.path.isfile(target), "a render writes its PNG")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print("sgra_optics self-test: %d check(s) failed" % len(failures))
        return 1
    print("sgra_optics self-test OK")
    return 0


# ---------------------------------------------------------------- main

def _write(path, rows, width, dry_run, quiet):
    data = bho.png_bytes(rows, width, len(rows))
    if dry_run:
        if not quiet:
            print("dry run: would write %d bytes to %s" % (len(data), path))
        return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    if not quiet:
        print("wrote %s (%d bytes)" % (path, len(data)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--at", type=float, metavar="LS", help="report the view from LS light-seconds")
    ap.add_argument("--render", metavar="PNG")
    ap.add_argument("--assets", metavar="DIR", help="render the design doc's images into DIR")
    ap.add_argument("--view", choices=("eye", "radio"), default="eye")
    ap.add_argument("--spin", type=float, default=SGRA_SPIN)
    ap.add_argument("--distance", type=float, default=20.0, help="units of M (default 20)")
    ap.add_argument("--incl", type=float, default=SUN_SIDE, help="degrees from the spin axis (default: Sol's side)")
    ap.add_argument("--fov", type=float, default=60.0)
    ap.add_argument("--size", type=int, default=320)
    ap.add_argument("--exposure", type=float, default=1.0, help="scales the auto-exposure")
    ap.add_argument("--procs", type=int, default=None)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.at is not None:
        for line in describe(args.at, args.incl, args.spin):
            print(line)
        return 0
    if args.render:
        rows = render(args.spin, args.distance, math.radians(args.incl), args.fov,
                      args.size, args.size, procs=args.procs)
        gain = auto_gain(rows, args.view, exposure=args.exposure)
        _write(args.render, compose(rows, args.view, gain), args.size, args.dry_run, args.quiet)
        return 0
    if args.assets:
        return assets(args.assets, args.dry_run, args.procs, args.quiet)
    ap.print_usage()
    return 2


def assets(out_dir, dry_run, procs, quiet):
    """The design doc's images (docs/design-sagittarius-a-2026-09-30.md
    says what each shows). Every one is the eye's view unless named radio;
    SUN_SIDE is the view from Sol's direction."""
    a = SGRA_SPIN

    def eye(r_o, incl, fov, w, h, frames=(), phase=0.0, with_flow=True, spin=a):
        return render(spin, r_o, math.radians(incl), fov, w, h, frames, phase, procs, with_flow)

    def out(name, rows, width):
        _write(os.path.join(out_dir, name), rows, width, dry_run, quiet)

    rows = eye(20.0, SUN_SIDE, 76.0, 480, 300)
    out("sgra-hero.png", compose(rows, "eye", auto_gain(rows, "eye")), 480)

    panels = []
    for incl in (SUN_SIDE, 120.0, 92.0):
        rows = eye(20.0, incl, 62.0, 256, 256)
        panels.append(compose(rows, "eye", auto_gain(rows, "eye")))
    out("sgra-views.png", *side_by_side(panels))

    panels = []
    for r_o in (400.0, 100.0, 30.0):
        rows = eye(r_o, SUN_SIDE, 60.0, 256, 256)
        panels.append(compose(rows, "eye", auto_gain(rows, "eye")))
    out("sgra-approach.png", *side_by_side(panels))

    # Far off, as the EHT sees it (at infinite resolution), and blurred to
    # the EHT's 20 microarcsecond beam: 20 / 5.0 = 4 M FWHM at the real mass.
    r_far, half_width_m = 1000.0, 12.0
    fov = 2.0 * math.degrees(math.atan(half_width_m / r_far))
    rows = render(a, r_far, math.radians(SUN_SIDE), fov, 256, 256, procs=procs)
    sharp = compose(rows, "radio", auto_gain(rows, "radio"))
    m_per_px = 2.0 * half_width_m / 256.0
    blurred_rows = blur_radio(rows, 4.0 / 2.3548 / m_per_px)
    blurred = compose(blurred_rows, "radio", auto_gain(blurred_rows, "radio"))
    out("sgra-radio.png", *side_by_side([sharp, blurred]))

    # A flare: a hot spot at SPOT_RADIUS, four frames a quarter-orbit apart.
    period = 2.0 * math.pi * (SPOT_RADIUS ** 1.5 + a)
    frames = [k * period / 4.0 for k in range(4)]
    # Exposed for the quiet flow, as the moment before the flare; the spot
    # saturates, as a flare ten times the flow's light does.
    rows = eye(40.0, SUN_SIDE, 40.0, 224, 224, frames, phase=0.0)
    gain = auto_gain(rows, "eye")
    out("sgra-flare.png", *side_by_side([compose(rows, "eye", gain, frame=k) for k in range(4)]))

    # The spin's signature: the shadow alone, edge-on, a = 0 and a = SGRA_SPIN.
    panels = []
    for spin in (0.0, a):
        rows = eye(30.0, 90.0, 44.0, 256, 256, with_flow=False, spin=spin)
        panels.append(compose(rows, "eye", 1.0))
    out("sgra-spin.png", *side_by_side(panels))
    return 0


if __name__ == "__main__":
    sys.exit(main())
