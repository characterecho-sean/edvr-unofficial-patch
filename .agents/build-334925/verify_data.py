#!/usr/bin/env python3
"""Offline check of EDVR's data-side dependencies on build 334925 (against 334473).

Agent material for the 2026-10-23 retarget; run on the retargeted source with the repo's Python:

    python -I .agents/build-334925/verify_data.py [--cache MAP.json]

1. Every vtable in map_all.json: slot by slot, the old table's code pointers mapped through
   build_diff's unit map must equal the new table's slots. Mismatched slots are listed.
2. The slots EDVR reads or compares, taken from the source's own constants:
   Explorer Cam follow (Rr/Ao vtables, four slots each), the UI Supersampling setter and getter
   slots, the VR Supersampling loader slot, and the flat stationary camera's provider pairs.
3. Fixed data bytes the code compares or reads (32 bytes at each non-vtable data RVA of
   map_all.json, old against new), and the .text raw size the Oculus profile floors.

Read-only; exits 1 on any failure.
"""
import argparse
import importlib.util
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tools"))
import build_diff  # noqa: E402

spec = importlib.util.spec_from_file_location("pins", HERE / "pins.py")
pins = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pins)

IMAGE_BASE = 0x140000000
failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL", msg)


def const(path, name):
    """The integer literal assigned to `name` in a source file (first match)."""
    text = (REPO / path).read_text(encoding="utf-8")
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*(0x[0-9A-Fa-f]+)", text)
    if not m:
        raise SystemExit(f"{name} not found in {path}")
    return int(m.group(1), 16)


def array_const(path, name):
    text = (REPO / path).read_text(encoding="utf-8")
    m = re.search(r"\b" + re.escape(name) + r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        raise SystemExit(f"array {name} not found in {path}")
    return [int(x, 16) if x.lower().startswith("0x") else int(x) for x in re.findall(r"0[xX][0-9A-Fa-f]+|\d+", m.group(1))]


def u64(pe, rva):
    return struct.unpack("<Q", pe.read(rva, 8))[0]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=None)
    args = ap.parse_args(argv)
    old_pe, new_pe = pins.PE(pins.OLD_EXE), pins.PE(pins.NEW_EXE)
    bm = build_diff.build_map(str(pins.OLD_EXE), str(pins.NEW_EXE), args.cache)
    entries = json.loads((HERE / "map_all.json").read_text())

    print("== 1. vtables of map_all.json, slot by slot")
    tables = 0
    for e in entries:
        if e["kind"] != "data" or not e.get("vtable"):
            continue
        o, n = int(e["rva"], 16), int(e["new_rva"], 16)
        count = e["vtable"]["slots"]
        bad = []
        for i in range(count):
            try:
                op = u64(old_pe, o + 8 * i)
                np_ = u64(new_pe, n + 8 * i)
            except ValueError:
                break
            want = bm._map_pointer(op)
            if want is None:
                bad.append((i, "unmapped", op - IMAGE_BASE, np_ - IMAGE_BASE))
            elif want != np_:
                bad.append((i, "differs", op - IMAGE_BASE, np_ - IMAGE_BASE))
        tables += 1
        note = ", ".join(f"[{i}] {why} old fn {a:#x} new slot {b:#x}" for i, why, a, b in bad[:6])
        print(f"  {o:#x} -> {n:#x}: {count} slots, {len(bad)} not confirmed" + (f" ({note})" if bad else ""))

    print("== 2. the slots EDVR reads")
    follow = "src/d3d11/explorer_cam_follow_core.h"
    rr, ao = const(follow, "kRrVtableRva"), const(follow, "kAoVtableRva")
    idx = array_const(follow, "kHeadSlotIndex")
    rr_fn, ao_fn = array_const(follow, "kRrFunctions"), array_const(follow, "kAoFunctions")
    for label, vt, fns in (("Rr", rr, rr_fn), ("Ao", ao, ao_fn)):
        for i, fn in zip(idx, fns):
            got = u64(new_pe, vt + 8 * i)
            ok = got == IMAGE_BASE + fn
            print(f"  {label} vtable {vt:#x} slot {i}: holds {got - IMAGE_BASE:#x}, source expects {fn:#x} {'ok' if ok else 'FAIL'}")
            if not ok:
                fail(f"{label} vtable slot {i}")
    ui = "src/d3d11/ui_sizing_math.h"
    for slot_name, fn_name in (("kUiSsSetterSlotRva", "kUiSsSetterRva"), ("kUiSsGetterSlotRva", "kUiSsGetterRva")):
        slot, fn = const(ui, slot_name), const(ui, fn_name)
        got = u64(new_pe, slot)
        ok = got == IMAGE_BASE + fn
        print(f"  {slot_name} {slot:#x}: holds {got - IMAGE_BASE:#x}, {fn_name} {fn:#x} {'ok' if ok else 'FAIL'}")
        if not ok:
            fail(slot_name)
    ss = "src/d3d11/vr_ssaa_hold.cpp"
    slot, fn = const(ss, "kLoaderSlotRva"), const(ss, "kWrapperRva")
    got = u64(new_pe, slot)
    ok = got == IMAGE_BASE + fn
    print(f"  kLoaderSlotRva {slot:#x}: holds {got - IMAGE_BASE:#x}, kWrapperRva {fn:#x} {'ok' if ok else 'FAIL'}")
    if not ok:
        fail("kLoaderSlotRva")
    fsc = (REPO / "src/d3d11/flat_stationary_camera_core.h").read_text(encoding="utf-8")
    m = re.search(r"constexpr uintptr_t known\[\]\[2\] = \{([^;]*)\};", fsc)
    pairs = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\}", m.group(1))]
    for vt, fn in pairs:
        slots = [i for i in range(64) if u64(new_pe, vt + 8 * i) == IMAGE_BASE + fn]
        print(f"  provider pair vtable {vt:#x} function {fn:#x}: function is slot {slots[:3]} of the table {'ok' if slots else 'FAIL'}")
        if not slots:
            fail(f"provider pair {vt:#x}/{fn:#x}")

    print("== 2b. the name literals Explorer Cam compares")

    def cstring(pe, rva, n=48):
        raw = pe.read(rva, n)
        return raw.split(b"\0", 1)[0].decode("ascii", "replace")

    core = (REPO / "src/d3d11/explorer_cam_core.h").read_text(encoding="utf-8")
    block = re.search(r"kPartNames\[kPartCount\] = \{([^}]*)\}", core).group(1)
    names = re.findall(r'"([^"]+)"', block)
    table = const("src/d3d11/explorer_cam_core.h", "kPartNameTableRva")
    wrong = [(i, cstring(new_pe, u64(new_pe, table + 8 * i) - IMAGE_BASE)) for i, nm in enumerate(names)
             if cstring(new_pe, u64(new_pe, table + 8 * i) - IMAGE_BASE) != nm]
    print(f"  part name table {table:#x}: {len(names)} entries, {len(wrong)} wrong {wrong[:3]}")
    count = int(re.search(r"\bkPartCount = (\d+);", core).group(1))
    if wrong or len(names) != count:
        fail("part name table")
    lits = {"kHeadNameRva": ("kHeadName", follow), "kPovNameRva": ("kPovName", "src/d3d11/explorer_cam_core.h"),
            "kFootLNameRva": ("kFootLName", follow), "kFootRNameRva": ("kFootRName", follow)}
    ftext = (REPO / follow).read_text(encoding="utf-8")
    for rva_name, (lit_name, where) in lits.items():
        rva_val = const(where if rva_name == "kPovNameRva" else follow, rva_name)
        want = re.search(r"\b" + lit_name + r'\[\] = "([^"]+)"', ftext).group(1)
        got = cstring(new_pe, rva_val)
        print(f"  {rva_name} {rva_val:#x}: {got!r} {'ok' if got == want else 'FAIL, wants ' + repr(want)}")
        if got != want:
            fail(rva_name)

    print("== 3. fixed data bytes, old against new")
    for e in entries:
        if e["kind"] != "data" or e.get("vtable"):
            continue
        o, n = int(e["rva"], 16), int(e["new_rva"], 16)
        try:
            ob, nb = old_pe.read(o, 32), new_pe.read(n, 32)
        except ValueError:
            continue
        same = sum(1 for a, b in zip(ob, nb) if a == b)
        print(f"  {o:#x} -> {n:#x}: {same}/32 equal ({e['status']})")

    print("== 4. section facts the Oculus profile floors")
    text = next(s for s in new_pe.sections if s["name"] == ".text")
    span = max(text["vsize"], text["rawsize"])
    src =(REPO / "src/common/elite_oculus_profile.h").read_text(encoding="utf-8")
    m = re.search(r"text\.span < (0x[0-9A-Fa-f]+)u", src)
    floor = int(m.group(1), 16)
    ok = span >= floor
    print(f"  .text span {span:#x} (va {text['va']:#x}) against the profile's floor {floor:#x} {'ok' if ok else 'FAIL'}")
    if not ok or text["va"] != 0x1000:
        fail(".text span or va")

    print("verify_data:", "FAILED" if failures else "ok", f"({tables} vtables)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
