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


def record_kind(r):
    h = marker_hash(r)
    marker = int(r[18, 0])
    if marker == (0x7FC0ED01 ^ h):
        return "joined"
    if marker == (0x7FC0ED02 ^ h):
        return "masked"
    return "unmarked"


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


def analyze(meta, rois):
    rw, rh = meta["render_width"], meta["render_height"]
    now = np.asarray(meta["camera"], dtype=np.float32)
    old = np.asarray(meta["previous_camera"] or meta["camera"], dtype=np.float32)
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
        scene_now = np.memmap(meta["buffers"]["scene_now"][0], mode="r", dtype="<f4").reshape((-1, 4))
        scene_old = np.memmap(meta["buffers"]["scene_previous"][0], mode="r", dtype="<f4").reshape((-1, 4))
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
                    if not (code_f >= 1):
                        branch = "camera_no_slot"
                    elif z <= 0 or np.asarray(z).view(np.uint32) != np.asarray(slot_z).view(np.uint32):
                        branch = "rejected_stale_or_depth"
                    elif code_f >= 4294967296 or not float(code_f).is_integer() or int(code_f) % 2 == 0:
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
                            kind = record_kind(record)
                            if kind == "masked":
                                branch = "rejected_masked_record"
                            elif kind == "unmarked":
                                branch = "camera_unmarked_record"
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
            "sampling": "exact when step=1; otherwise regular grid and sampled counts",
            "comparison_note": "Offline float32 reprojection versus emitted float16 motion; tolerance is 1 px + 0.005 times emitted-vector magnitude, a diagnostic allowance rather than a float16 ULP. Disagreement does not establish cause. Captured depth is prepared OutDepth, so original nonfinite/out-of-range depth cannot be reconstructed",
            "rois": result}


def self_test():
    fbits = lambda x: np.asarray(x, dtype=np.float32).view(np.uint32)
    rows = np.zeros((276, 4), dtype=np.float32)
    rows[270, 0] = 1
    rows[271, 1] = -1
    rows[272, 3] = 1
    rows[273, 2] = 1
    old = rows.copy()
    r = np.zeros((21, 4), dtype=np.uint32)
    r[0, 1] = r[19, 1] = fbits(1)
    identity = (65534 << 16) | 32767
    r[0, 2:4] = r[19, 2:4] = [32767 | (32767 << 16), identity]
    r[1, :3] = r[18, 1:4] = fbits([0, 0, 2])
    r[18, 0] = 0x7FC0ED01 ^ marker_hash(r)
    assert record_kind(r) == "joined" and not moved(r)
    uv = np.asarray([.5, .5], dtype=np.float32)
    before = engine_before(r, rows, old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) < .01
    # The object and camera shift together: the object stays put on screen.
    rows[275, 0] = 1
    r[1, 0] = fbits(1)
    r[18, 0] = 0x7FC0ED01 ^ marker_hash(r)
    assert moved(r) and record_kind(r) == "joined"
    before = engine_before(r, rows, old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) < .01
    # A camera shift relative to a fixed object must retain its vector.
    r[1, 0] = fbits(0)
    r[18, 0] = 0x7FC0ED01 ^ marker_hash(r)
    before = engine_before(r, rows, old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) > 1


    r[18, 0] = 0x7FC0ED02 ^ marker_hash(r)
    assert record_kind(r) == "masked"
    r[18, 0] ^= 1
    assert record_kind(r) == "unmarked"
    # A menu-like rotation carries the surface to a different old pixel.
    rows[275, 0] = 0
    turn_half = round((math.sqrt(.5) + 1) * 32767)
    r[0, 3] = turn_half | (turn_half << 16)
    r[18, 0] = 0x7FC0ED01 ^ marker_hash(r)
    before = engine_before(r, rows, old, np.asarray([.6, .5], dtype=np.float32), np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, np.asarray([.6, .5], dtype=np.float32), 100, 100)) > 1
    camera_now = np.asarray([rows[270], rows[271], rows[272], rows[273],
                             np.zeros(4, dtype=np.float32), np.zeros(4, dtype=np.float32)])
    camera_old = camera_now.copy()
    camera_now[5, 0] = 1
    before = camera_before(camera_now, camera_old, uv, np.float32(.5))
    assert np.linalg.norm(motion_from_before(before, uv, 100, 100)) > 1


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    if not parser.parse_args().self_test:
        parser.error("only --self-test is supported; use flat_pixels.py for captures")
    self_test()
    print("flat_pixels_engine self-test passed")
