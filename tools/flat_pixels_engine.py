"""Helper for flat_pixels.py, also checked by --self-test in the build gate.

Offline replay of the flat mono prep shader's engineBefore decision.

Sampling is deterministic and capped per ROI. Counts on a stepped ROI describe
the sampled pixels only; this is evidence about captured inputs, not ownership
proof for unobserved pixels.
"""

import math

import numpy as np


def marker_hash(r):
    words = [r[1, 0], r[1, 1], r[1, 2], r[0, 2], r[0, 3],
             r[18, 1], r[18, 2], r[18, 3], r[19, 2], r[19, 3]]
    h = 0x811C9DC5
    for word in words:
        h = ((h ^ int(word)) * 0x01000193) & 0xffffffff
        h ^= h >> 13
    return h


def record_kind(r, token):
    # The fresh kind at the compose frame's token: the marker folds the
    # emission frame in (the last FNV word), so it certifies only there.
    # 1 joined, 2 masked, 3 neither (a stale marker or not a rig record;
    # stale_stamp_kind tells those apart).
    h = marker_hash_stamped(r, token)
    marker = int(r[18, 0])
    if marker == (0x7FC0ED01 ^ h):
        return "joined"
    if marker == (0x7FC0ED02 ^ h):
        return "masked"
    return "unmarked"


def stale_stamp_kind(r, token):
    # The decline path's second half: a marker stamped within the last 64
    # frames (the emit table and census horizon) still names its kind. 6:
    # a joined marker from an older frame -- a stale pose pair, the camera
    # term; 2: an older masked marker keeps no history; 3: not a rig record
    # (or older than the window -- still the camera term).
    h = int(marker_hash(r))
    token = int(token)
    for age in range(1, 65):
        hs = ((h ^ ((token - age) & 0xffffffff)) * 0x01000193) & 0xffffffff
        hs ^= hs >> 13
        if int(r[18, 0]) == (0x7FC0ED01 ^ hs):
            return 6
        if int(r[18, 0]) == (0x7FC0ED02 ^ hs):
            return 2
    return 3


def marker_hash_stamped(r, token):
    # marker_hash plus the frame stamp (the emit's present-frame clock, the
    # last FNV word); what a joined marker must match the frame it is read.
    h = marker_hash(r)
    h = ((h ^ int(token)) * 0x01000193) & 0xffffffff
    h ^= h >> 13
    return h


def stamp_fresh(r, token_bits):
    # EN[276].x carries the frame stamp as uint bits; a joined marker whose
    # stamp is not this frame holds an older frame's pose pair (the record
    # was culled, not re-evaluated) and the camera term stands.
    return int(r[18, 0]) == (0x7FC0ED01 ^ marker_hash_stamped(r, token_bits))


def moved(r):
    return not (np.array_equal(r[0, 1:4], r[19, 1:4]) and
                np.array_equal(r[1, :3], r[18, 1:4]))


def quat(words):
    p = [int(words[0]) & 65535, int(words[0]) >> 16,
         int(words[1]) & 65535, int(words[1]) >> 16]
    return np.asarray(p, dtype=np.float32) * np.float32(1 / 32767) - 1


def turn(q, v):
    return ((2 * q[3] * q[3] - 1) * v + 2 * np.dot(q[:3], v) * q[:3] +
            2 * q[3] * np.cross(q[:3], v))


def engine_before(r, now, old, raw_uv, depth):
    n0, n1, n2, n3, nc = now[270], now[271], now[272], now[273], now[275, :3]
    b0, b1, b2, b3, bc = old[270], old[271], old[272], old[273], old[275, :3]
    if (n0[2] != 0 or n1[2] != 0 or n2[2] != 0 or n3[3] != 0 or n3[2] <= 0 or
            b0[2] != 0 or b1[2] != 0 or b2[2] != 0 or b3[3] != 0 or depth <= 0):
        return None
    z = np.float32(n3[2] / depth)
    a = np.asarray([n0[0], n1[0], n2[0]], dtype=np.float32)
    b = np.asarray([n0[1], n1[1], n2[1]], dtype=np.float32)
    c = np.asarray([n0[3], n1[3], n2[3]], dtype=np.float32)
    ca, cb, cc = np.cross(b, c), np.cross(c, a), np.cross(a, b)
    det = np.dot(a, ca)
    if not np.isfinite(det) or abs(det) <= 1e-12:
        return None
    ndc = raw_uv * np.asarray([2, -2], dtype=np.float32) + np.asarray([-1, 1], dtype=np.float32)
    rhs = np.asarray([ndc[0] * z - n3[0], ndc[1] * z - n3[1], z], dtype=np.float32)
    world = (ca * rhs[0] + cb * rhs[1] + cc * rhs[2]) / det
    if not moved(r):
        prev_world = world + (nc - bc)
    else:
        f = r.view(np.float32)
        scale_now, scale_old = f[0, 1], f[19, 1]
        q_now, q_old = quat(r[0, 2:4]), quat(r[19, 2:4])
        axes = [turn(q_now, np.eye(3, dtype=np.float32)[i] * scale_now) for i in range(3)]
        ix, iy, iz = np.cross(axes[1], axes[2]), np.cross(axes[2], axes[0]), np.cross(axes[0], axes[1])
        det = np.dot(axes[0], ix)
        if not np.isfinite(det) or abs(det) <= 1e-30:
            return None
        local = world - (f[1, :3] - nc)
        v = np.asarray([np.dot(ix, local), np.dot(iy, local), np.dot(iz, local)],
                       dtype=np.float32) / det
        prev_world = (f[18, 1:4] - bc) + scale_old * turn(q_old, v)
    before = prev_world[0] * b0 + prev_world[1] * b1 + prev_world[2] * b2 + b3
    return before if before[3] > 0 and np.all(np.isfinite(before)) else None


def camera_before(now, old, raw_uv, depth):
    a, b, c = now[:3, 0], now[:3, 1], now[:3, 3]
    ca, cb, cc = np.cross(b, c), np.cross(c, a), np.cross(a, b)
    det = np.dot(a, ca)
    if not np.isfinite(det) or det == 0 or now[3, 2] == 0:
        return None
    iz = np.float32(depth / now[3, 2])
    rhs = np.asarray([raw_uv[0] * 2 - 1, 1 - raw_uv[1] * 2, 1], dtype=np.float32)
    rhs -= now[3, [0, 1, 3]] * iz
    pos = (ca * rhs[0] + cb * rhs[1] + cc * rhs[2]) / det
    pos += (now[5, :3] - old[5, :3]) * iz
    before = pos[0] * old[0] + pos[1] * old[1] + pos[2] * old[2] + iz * old[3]
    return before if before[3] > 0 and np.all(np.isfinite(before)) else None


def motion_from_before(before, raw_uv, width, height):
    if before is None:
        return None
    prev = before[:2] / before[3] * np.asarray([.5, -.5], dtype=np.float32) + .5
    motion = (prev - raw_uv) * np.asarray([width, height], dtype=np.float32)
    expected = before[2] / before[3]
    if (not np.all(np.isfinite(motion)) or np.any(np.abs(motion) > 65504) or
            np.any(prev < 0) or np.any(prev > 1) or
            not np.isfinite(expected) or expected < 0 or expected > 1):
        return None
    return motion


ROWS_NDC_TOLERANCE = 2e-6   # NDC; 0.003 px at 2880 wide, far above float error, far below any phase


def rows_ndc(jitter_px, width, height):
    """The NDC shift a raster phase (render pixels, positive right/down) puts into the
    game's rows when the camera injector jitters the frustum: x += 2*px/W * w,
    y += -2*py/H * w (flat_camera_phase.h; the shader's unjitterRow removes it)."""
    return np.asarray([2.0 * jitter_px[0] / width, -2.0 * jitter_px[1] / height], dtype=np.float32)


def measure_rows_ndc(rows):
    """The NDC shift the rows carry, read from the rows themselves. Rows 0..2 hold, per
    world axis, the clip contribution (x, y, z, w); for a rigid view and a symmetric
    perspective the x and y columns are orthogonal to the w column, so any dot product
    is the injected shift. None when the w column vanishes."""
    a = np.asarray([rows[0][0], rows[1][0], rows[2][0]], dtype=np.float64)
    b = np.asarray([rows[0][1], rows[1][1], rows[2][1]], dtype=np.float64)
    f = np.asarray([rows[0][3], rows[1][3], rows[2][3]], dtype=np.float64)
    ff = float(np.dot(f, f))
    if ff < 1e-12:
        return None
    return np.asarray([np.dot(a, f) / ff, np.dot(b, f) / ff], dtype=np.float64)


def infer_rows_ndc(rows, jitter_px, width, height):
    """(ndc, source) for a capture that did not declare rows_jitter (every capture made
    before the field). The rows carry the raster phase when their measured shift equals
    it (the upstream camera injector: the game derived them from a jittered frustum), and
    carry nothing when it is zero (the legacy adapter patches a private copy). Anything
    else is left alone: removing a phase the runtime never put there would corrupt the
    rows the way the missing removal corrupts them."""
    measured = measure_rows_ndc(rows)
    if measured is None:
        return np.zeros(2, dtype=np.float32), "unjittered"
    expected = rows_ndc(jitter_px, width, height)
    if float(np.max(np.abs(expected))) > 0 and float(np.max(np.abs(measured - expected))) <= ROWS_NDC_TOLERANCE:
        return expected, "inferred-carried"
    if float(np.max(np.abs(measured))) <= ROWS_NDC_TOLERANCE:
        return np.zeros(2, dtype=np.float32), "unjittered"
    return np.zeros(2, dtype=np.float32), "unrecognized"


def unjitter_rows(rows, ndc, first=0, count=4):
    """A copy of `rows` with the phase removed from rows first..first+count-1, exactly
    the shader's unjitterRow: r.xy -= ndc * r.w. A zero shift returns the rows as they were."""
    out = np.array(rows, dtype=np.float32, copy=True)
    if float(ndc[0]) == 0 and float(ndc[1]) == 0:
        return out
    for i in range(first, first + count):
        out[i, 0] -= np.float32(ndc[0]) * out[i, 3]
        out[i, 1] -= np.float32(ndc[1]) * out[i, 3]
    return out


def analyze(meta, rois, unjitter=True, static_scene=None):
    """`unjitter=False` is the replay this tool made before the camera injector: the rows
    are used as captured. Under the injector they carry the phase, and that replay is off
    by exactly |current phase - previous phase| on every pixel (the self-test pins it).

    `static_scene` is flags.w of the prep shader, the 3D main menu's stale-slot policy: a
    slot that something else has drawn over takes the camera term instead of refusing
    history. None replays the frame as the capture declares it (False for a capture made
    before the flag existed); True or False forces it, so an old capture can be asked what
    the policy would have done, and a new one what it would have done without."""
    declared_static = bool(meta.get("static_scene", False))
    static = declared_static if static_scene is None else bool(static_scene)
    rw, rh = meta["render_width"], meta["render_height"]
    now = np.asarray(meta["camera"], dtype=np.float32)
    old = np.asarray(meta["previous_camera"] or meta["camera"], dtype=np.float32)
    ndc_now = np.zeros(2, dtype=np.float32)
    ndc_old = np.zeros(2, dtype=np.float32)
    rows_source = "not-removed"
    if unjitter:
        declared, declared_previous = meta.get("rows_jitter"), meta.get("previous_rows_jitter")
        if declared is not None and declared_previous is not None:
            ndc_now, ndc_old = rows_ndc(declared, rw, rh), rows_ndc(declared_previous, rw, rh)
            rows_source = "declared"
        else:
            ndc_now, source_now = infer_rows_ndc(now, meta["jitter"], rw, rh)
            ndc_old, source_old = infer_rows_ndc(old, meta["previous_jitter"], rw, rh)
            rows_source = source_now if source_now == source_old else f"{source_now}/{source_old}"
        now = unjitter_rows(now, ndc_now)
        old = unjitter_rows(old, ndc_old)
    depth = np.memmap(meta["textures"]["depth"], mode="r", dtype="<f4", shape=(rh, rw))
    emitted = np.memmap(meta["textures"]["motion"], mode="r", dtype="<f2", shape=(rh, rw, 2))
    rejection = np.memmap(meta["textures"]["rejection"], mode="r", dtype=np.uint8, shape=(rh, rw))
    complete = meta["engine"]["complete"]
    slots = pool = scene_now = scene_old = None
    first = count = pool_stride = 0
    if complete:
        slots = np.memmap(meta["textures"]["slots"], mode="r", dtype="<f4", shape=(rh, rw, 2))
        path, record = meta["buffers"]["pool"]
        pool_stride = record["stride"]
        if pool_stride == 336:
            pool = np.memmap(path, mode="r", dtype="<u4").reshape((-1, 21, 4))
        first, count = record["first_element"], record["num_elements"]
        # The engine's scene snapshots are the game's b1 upload too: the same phase as the
        # camera rows, the before-snapshot's being the previous frame's (the shader unjitters
        # EN[270..273] and EB[270..273] the same way).
        scene_now = unjitter_rows(np.memmap(meta["buffers"]["scene_now"][0], mode="r", dtype="<f4").reshape((-1, 4)),
                                  ndc_now, first=270)
        scene_old = unjitter_rows(np.memmap(meta["buffers"]["scene_previous"][0], mode="r", dtype="<f4").reshape((-1, 4)),
                                  ndc_old, first=270)
    result = []
    for label, (x, y, width, height) in rois:
        if x + width > rw or y + height > rh:
            raise ValueError(f"ROI {label} exceeds {rw}x{rh} render pixels")
        step = max(1, math.ceil(math.sqrt(width * height / 8192)))
        counts, slots_used, errors = {}, set(), {"engine": [], "camera": [], "rejected": []}
        reject_mismatch = 0
        for py in range(y, y + height, step):
            for px in range(x, x + width, step):
                z = np.float32(depth[py, px])
                size = np.asarray([rw, rh], dtype=np.float32)
                raw_uv = ((np.asarray([px, py], dtype=np.float32) + .5) / size -
                          np.asarray(meta["jitter"], dtype=np.float32) / size)
                branch, before = "camera", None
                if meta["reset"] or not np.isfinite(z) or z < 0 or z > 1:
                    branch = "rejected_input"
                elif not complete:
                    branch = "camera_engine_incomplete"
                else:
                    code_f, slot_z = slots[py, px]
                    depth_stale = bool(np.asarray(z).view(np.uint32) != np.asarray(slot_z).view(np.uint32))
                    # The shader's order (engineBefore): sky and the out-of-range sentinel refuse
                    # first; then a slot the pixel's own depth disagrees with -- refused, or the
                    # camera term in the 3D main menu; then a malformed code.
                    if not (code_f >= 1):
                        branch = "camera_no_slot"
                    elif z <= 0:
                        branch = "rejected_stale_or_depth"
                    elif code_f >= 4294967296:
                        branch = "rejected_stale_or_depth" if depth_stale else "rejected_corrupt_code"
                    elif depth_stale:
                        branch = "camera_stale_static" if static else "rejected_stale_or_depth"
                    elif not float(code_f).is_integer() or int(code_f) % 2 == 0:
                        branch = "rejected_corrupt_code"
                    elif pool_stride != 336:
                        branch = "rejected_pool_stride"
                    else:
                        slot = int(code_f) >> 1
                        if slot >= count:
                            branch = "rejected_out_of_pool"
                        else:
                            slots_used.add(slot)
                            record = pool[first + slot]
                            if scene_now.shape[0] <= 276:
                                raise ValueError(f"scene constants hold {scene_now.shape[0]} rows; "
                                                 "the freshness stamp needs row 276")
                            token = np.float32(scene_now[276, 0]).view(np.uint32)
                            kind = record_kind(record, token)
                            if kind == "masked":
                                branch = "rejected_masked_record"
                            elif kind == "unmarked":
                                stale = stale_stamp_kind(record, token)
                                if stale == 6:
                                    branch = "camera_stale_stamp"
                                elif stale == 2:
                                    branch = "rejected_masked_record"
                                else:
                                    branch = "camera_unmarked_record"
                            elif not stamp_fresh(record, token):
                                branch = "camera_stale_stamp"
                            else:
                                before = engine_before(record, scene_now, scene_old, raw_uv, z)
                                if before is None:
                                    branch = ("rejected_moved_reprojection" if moved(record)
                                              else "camera_static_reprojection_fallback")
                                else:
                                    branch = "engine_joined"
                if branch.startswith("camera"):
                    before = camera_before(now, old, raw_uv, z)
                motion = motion_from_before(before, raw_uv, rw, rh) if before is not None else None
                if motion is None:
                    if not branch.startswith("rejected"):
                        branch = "rejected_output"
                    motion = np.zeros(2, dtype=np.float32)
                counts[branch] = counts.get(branch, 0) + 1
                reject_mismatch += bool(branch.startswith("rejected")) != bool(rejection[py, px])
                bucket = "engine" if branch == "engine_joined" else "rejected" if branch.startswith("rejected") else "camera"
                observed = emitted[py, px].astype(np.float32)
                errors[bucket].append((float(np.linalg.norm(motion - observed)), float(np.linalg.norm(observed))))
        comparisons = {}
        for bucket, pairs in errors.items():
            if pairs:
                differences = np.asarray([p[0] for p in pairs])
                tolerances = np.asarray([1 + .005 * p[1] for p in pairs])
                comparisons[bucket] = {"samples": len(pairs), "median_error_px": float(np.median(differences)),
                                       "p95_error_px": float(np.percentile(differences, 95)),
                                       "max_error_px": float(differences.max()),
                                       "within_replay_tolerance": int(np.count_nonzero(differences <= tolerances))}
        result.append({"name": label, "xywh": [x, y, width, height], "sample_step": step,
                       "sampled_pixels": sum(counts.values()), "branch_counts": counts,
                       "records_used": sorted(slots_used), "rejection_mismatch_pixels": reject_mismatch,
                       "motion_comparison": comparisons})
    return {"engine_complete": complete, "input_grid": "render pixels",
            "static_scene": {"declared": declared_static, "replayed_as": static},
            "sampling": "exact when step=1; otherwise regular grid and sampled counts",
            "rows_jitter": {"source": rows_source, "ndc_now": [float(v) for v in ndc_now],
                            "ndc_previous": [float(v) for v in ndc_old]},
            "comparison_note": "Offline float32 reprojection versus emitted float16 motion, with the phase the camera rows carry removed first as the shader does (rows_jitter: declared by the capture, or inferred from the rows); tolerance is 1 px + 0.005 times emitted-vector magnitude, a diagnostic allowance rather than a float16 ULP. Disagreement does not establish cause. Captured depth is prepared OutDepth, so original nonfinite/out-of-range depth cannot be reconstructed",
            "rois": result}


def self_test():
    fbits = lambda x: np.asarray(x, dtype=np.float32).view(np.uint32)
    # The freshness stamp the compose's EN[276].x carries (the emit's
    # present-frame clock in production).
    stamp = 4242
    rows = np.zeros((277, 4), dtype=np.float32)
    rows[270, 0] = 1
    rows[271, 1] = -1
    rows[272, 3] = 1
    rows[273, 2] = 1
    rows[276, 0] = np.uint32(stamp).view(np.float32)
    old = rows.copy()
    r = np.zeros((21, 4), dtype=np.uint32)
    r[0, 1] = r[19, 1] = fbits(1)
    identity = (65534 << 16) | 32767
    r[0, 2:4] = r[19, 2:4] = [32767 | (32767 << 16), identity]
    r[1, :3] = r[18, 1:4] = fbits([0, 0, 2])
    r[18, 0] = 0x7FC0ED01 ^ marker_hash_stamped(r, stamp)
    assert record_kind(r, stamp) == "joined" and not moved(r)
    uv = np.asarray([.5, .5], dtype=np.float32)
    before = engine_before(r, rows, old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) < .01
    # The object and camera shift together: the object stays put on screen.
    rows[275, 0] = 1
    r[1, 0] = fbits(1)
    r[18, 0] = 0x7FC0ED01 ^ marker_hash_stamped(r, stamp)
    assert moved(r) and record_kind(r, stamp) == "joined"
    before = engine_before(r, rows, old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) < .01
    # A camera shift relative to a fixed object must retain its vector.
    r[1, 0] = fbits(0)
    r[18, 0] = 0x7FC0ED01 ^ marker_hash_stamped(r, stamp)
    before = engine_before(r, rows, old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) > 1

    r[18, 0] = 0x7FC0ED02 ^ marker_hash_stamped(r, stamp)
    assert record_kind(r, stamp) == "masked"
    r[18, 0] ^= 1
    assert record_kind(r, stamp) == "unmarked"
    # The freshness stamp (2026-09-25): the marker folds the emission frame
    # in; it certifies at that frame and declines at any other, and the
    # stale-stamp window finds an older joined marker as kind 6 while an
    # older masked marker keeps no history.
    r[18, 0] = 0x7FC0ED01 ^ marker_hash_stamped(r, stamp)
    assert record_kind(r, stamp) == "joined" and stamp_fresh(r, stamp)
    assert record_kind(r, stamp + 1) == "unmarked"
    assert stale_stamp_kind(r, stamp + 1) == 6
    assert stale_stamp_kind(r, stamp + 64) == 6
    assert stale_stamp_kind(r, stamp + 65) == 3
    assert marker_hash_stamped(r, stamp) != marker_hash_stamped(r, stamp + 1)
    r[18, 0] = 0x7FC0ED02 ^ marker_hash_stamped(r, stamp)
    assert record_kind(r, stamp) == "masked" and not stamp_fresh(r, stamp)
    assert stale_stamp_kind(r, stamp + 1) == 2
    # A menu-like rotation carries the surface to a different old pixel.
    rows[275, 0] = 0
    turn_half = round((math.sqrt(.5) + 1) * 32767)
    r[0, 3] = turn_half | (turn_half << 16)
    r[18, 0] = 0x7FC0ED01 ^ marker_hash_stamped(r, stamp)
    before = engine_before(r, rows, old, np.asarray([.6, .5], dtype=np.float32), np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, np.asarray([.6, .5], dtype=np.float32), 100, 100)) > 1
    camera_now = np.asarray([rows[270], rows[271], rows[272], rows[273],
                             np.zeros(4, dtype=np.float32), np.zeros(4, dtype=np.float32)])
    camera_old = camera_now.copy()
    camera_now[5, 0] = 1
    before = camera_before(camera_now, camera_old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) > 1
    jitter_self_test()


def synthetic_camera(rw=2880, rh=1620):
    """b1[270..275] of an exact still camera: a rotated view, a symmetric perspective (so
    the x and y clip columns are orthogonal to the w column, as any unjittered game camera
    is), camera-relative (rows 3 = near plane, row 5 = the eye)."""
    yaw, pitch = math.radians(-20.0), math.radians(12.0)
    ry = np.asarray([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
    rx = np.asarray([[1, 0, 0], [0, math.cos(pitch), -math.sin(pitch)], [0, math.sin(pitch), math.cos(pitch)]])
    view = rx @ ry
    a, b, f = 1.0429 * view[0], 1.83716 * view[1], -view[2]
    rows = np.zeros((6, 4), dtype=np.float32)
    for i in range(3):
        rows[i] = [a[i], b[i], 0, f[i]]
    rows[3] = [0, 0, 0.025, 0]
    rows[4] = [f[0], f[1], f[2], 0]
    rows[5] = [19.7411, -37.8578, -114.51805, 0]
    return rows


def jitter_rows(rows, ndc, count=4):
    """What the camera injector's frustum does to the game's rows: x += ndc.x*w, y += ndc.y*w."""
    out = np.array(rows, dtype=np.float32, copy=True)
    for i in range(count):
        out[i, 0] += np.float32(ndc[0]) * out[i, 3]
        out[i, 1] += np.float32(ndc[1]) * out[i, 3]
    return out


def jitter_self_test():
    # Under the upstream camera injector (2026-09-29) the rows the game builds carry the raster
    # phase. This tool's replay used them as captured, so on a STILL scene it reported a motion
    # error of exactly |current phase - previous phase| on every pixel (0.609 px for the Krait
    # capture's (-0.125, -0.278) against (0.125, 0.278)) -- a tool defect, not a shader one.
    rw, rh = 2880, 1620
    base = synthetic_camera(rw, rh)
    assert np.max(np.abs(measure_rows_ndc(base))) < 1e-6, "the synthetic camera is unjittered"
    jn = np.asarray([-0.125, -0.27777779], dtype=np.float32)
    jo = np.asarray([0.125, 0.27777779], dtype=np.float32)
    ndc_now, ndc_old = rows_ndc(jn, rw, rh), rows_ndc(jo, rw, rh)
    now_j, old_j = jitter_rows(base, ndc_now), jitter_rows(base, ndc_old)
    assert np.max(np.abs(measure_rows_ndc(now_j) - ndc_now)) < 1e-6, "the rows measure the phase they carry"
    delta = float(np.linalg.norm(jn - jo))
    assert abs(delta - 0.6092) < 1e-3
    size = np.asarray([rw, rh], dtype=np.float32)
    for px, py, depth in ((100, 200, .0004), (1440, 810, .0008), (2500, 1400, .002), (7, 1600, .0015)):
        raw_uv = (np.asarray([px, py], dtype=np.float32) + .5) / size - jn / size
        z = np.float32(depth)
        truth = motion_from_before(camera_before(base, base, raw_uv, z), raw_uv, rw, rh)
        stock = motion_from_before(camera_before(now_j, old_j, raw_uv, z), raw_uv, rw, rh)
        fixed = motion_from_before(camera_before(unjitter_rows(now_j, ndc_now), unjitter_rows(old_j, ndc_old),
                                                 raw_uv, z), raw_uv, rw, rh)
        assert truth is not None and stock is not None and fixed is not None
        assert np.linalg.norm(truth) < 1e-3, "a still scene has no motion"
        # The stock replay's error IS the phase difference (the assertion that pins the tool defect) ...
        assert abs(float(np.linalg.norm(stock - truth)) - delta) < 2e-3, float(np.linalg.norm(stock - truth))
        # ... and with the rows unjittered as the shader does it is float error.
        assert float(np.linalg.norm(fixed - truth)) < 1e-3, float(np.linalg.norm(fixed - truth))
    # Recognition when the capture does not declare the rows' phase (every capture before the field).
    ndc, source = infer_rows_ndc(now_j, jn, rw, rh)
    assert source == "inferred-carried" and np.max(np.abs(ndc - ndc_now)) < 1e-7
    ndc, source = infer_rows_ndc(base, jn, rw, rh)
    assert source == "unjittered" and not ndc.any(), "rows that carry nothing stay as they are"
    ndc, source = infer_rows_ndc(jitter_rows(base, ndc_now + np.float32(1e-4)), jn, rw, rh)
    assert source == "unrecognized" and not ndc.any(), "a shift that is not the raster phase is not removed"
    ndc, source = infer_rows_ndc(base, np.asarray([0, 0], dtype=np.float32), rw, rh)
    assert source == "unjittered" and not ndc.any()
    # A zero shift returns the rows untouched, whatever they hold.
    junk = np.asarray([[1, 2, 3, 4]] * 6, dtype=np.float32)
    assert np.array_equal(unjitter_rows(junk, np.zeros(2, dtype=np.float32)), junk)
    # The convention, pinned to the GAME and not to this file's own arithmetic (a test that
    # jitters and unjitters with the same rows_ndc cannot see a wrong sign): rows 0..3 of the
    # Krait main-menu capture (Epic 2026-09-29, frame 67611, DLSS, render 2880x1620), made
    # while the camera injector carried the phase (-0.125, -0.2777778) and, in the previous
    # frame's rows, (+0.125, +0.2777778). Positive py must give a POSITIVE y shift.
    live_now = np.asarray([[-1.04289675, -0.133361578, 0, -0.240010694],
                           [0.00107645115, 1.83692801, 0, -0.28215909],
                           [0.26924336, -0.523911774, 0, -0.928860128], [0, 0, 0.0250000004, 0]], dtype=np.float32)
    live_old = np.asarray([[-1.04293942, -0.133204669, 0, -0.240005717],
                           [0.00102006283, 1.83712447, 0, -0.282153785],
                           [0.269078195, -0.523262024, 0, -0.928863049], [0, 0, 0.0250000004, 0]], dtype=np.float32)
    for rows_live, phase in ((live_now, (-0.125, -0.277777791)), (live_old, (0.125, 0.277777791))):
        measured = measure_rows_ndc(rows_live)
        assert np.max(np.abs(measured - rows_ndc(phase, 2880, 1620))) < ROWS_NDC_TOLERANCE, (measured, phase)
        assert infer_rows_ndc(rows_live, phase, 2880, 1620)[1] == "inferred-carried"
    assert measure_rows_ndc(live_now)[1] > 0 > measure_rows_ndc(live_old)[1]


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    if not parser.parse_args().self_test:
        parser.error("only --self-test is supported; use flat_pixels.py for captures")
    self_test()
    print("flat_pixels_engine self-test passed")
