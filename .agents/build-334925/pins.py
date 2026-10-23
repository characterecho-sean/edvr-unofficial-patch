#!/usr/bin/env python3
"""Byte and FNV pin checker for the Elite 334473 -> 334925 retarget.

Ported from .agents/build-334473/pins.py. Every pin's value is read from the
source at run time (C++ arrays, hash literals, {rva, n, bytes} tables) or from
tools/elite_oculus.py via ast. The RVA literal the source uses must appear on
the stated line. The pin table's RVAs are build 334473's; the source holds
them as written (--mode pre, the default, before the edit pass) or retargeted
to 334925 (--mode post, after it). For each pin:

  1. the pinned value must reproduce on the 334473 exe at its RVA
     (otherwise UNREPRODUCED and nothing is changed);
  2. the same is computed on the 334925 exe at the mapped RVA:
       KEEP        equal to the pin,
       NEW         differs, and every differing byte lies in a rel32 or
                   disp32 field of the new instruction stream. The new value
                   is printed and can be written with --apply-new,
       SUSPICIOUS  a differing byte is outside any displacement field.
                   Nothing is changed for these.

Read-only except for --write-baseline, which records each pin's value as it
stands in the source (pins_baseline.json) before an edit pass (later --mode
post runs reproduce the 334473 exe against that baseline), --save-pre FILE,
and --apply-new FILE, which rewrites the NEW pins of a saved pre run into the
source. The report goes to stdout. Disassembly is capstone (no objdump here).
"""
from __future__ import annotations

import ast
import bisect
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

try:
    import capstone
except ImportError:  # pragma: no cover
    capstone = None

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def _analysis_dir():
    """analysis/ is untracked: look in this checkout, then in the main checkout."""
    cands = [REPO / "analysis"]
    try:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=REPO,
                                capture_output=True, text=True, check=True).stdout.strip()
        cands.append((REPO / common).resolve().parent / "analysis")
    except Exception:
        pass
    for c in cands:
        if (c / "EliteDangerous64-334925.exe").exists():
            return c
    raise SystemExit("analysis/EliteDangerous64-334925.exe not found")


ANALYSIS = _analysis_dir()
OLD_EXE = ANALYSIS / "EliteDangerous64-334473.exe"
NEW_EXE = ANALYSIS / "EliteDangerous64-334925.exe"
MAP_JSON = HERE / "map_all.json"
MAP_EXTRA_JSON = HERE / "map_extra.json"
OCULUS_PY = REPO / "tools" / "elite_oculus.py"
IMAGE_BASE = 0x140000000
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
WILD = None  # a wildcard byte (a displacement left unchecked)

# RVAs that neither map file carries (none so far). Each would be mapped with
#   tools/build_diff.py map --old ... --new ... --cache ... --rva <old>
EXTRA_MAP = {}

# RVAs the Python twin tables still hold as OLD-build values.
PY_RVA_IS = {}

# "pre": the source still holds build 334473's RVAs and pins; "post": it holds
# 334925's. Set from --mode in main().
MODE = "pre"

# Pinned values as they stood before the edit pass (written with
# --write-baseline). The old-exe reproduction is checked against these, so a
# re-run after editing still shows the old build reproducing each pin.
BASELINE_JSON = HERE / "pins_baseline.json"
BASELINE = {}


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


class PE:
    """Minimal PE32+ reader: sections, RVA -> bytes, base-relocation sites."""

    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        d = self.data
        nt = struct.unpack_from("<I", d, 0x3C)[0]
        assert d[nt:nt + 4] == b"PE\0\0", "bad PE signature"
        nsec = struct.unpack_from("<H", d, nt + 6)[0]
        optsz = struct.unpack_from("<H", d, nt + 20)[0]
        self.timestamp = struct.unpack_from("<I", d, nt + 8)[0]
        opt = nt + 24
        assert struct.unpack_from("<H", d, opt)[0] == 0x20B, "not PE32+"
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        ndd = struct.unpack_from("<I", d, opt + 108)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(ndd)]
        self.sections = []
        sec = opt + optsz
        for i in range(nsec):
            o = sec + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append(dict(name=name, va=va, vsize=vsize,
                                      rawsize=rawsize, rawptr=rawptr, chars=chars))
        self.reloc_sites = self._parse_relocs()
        self._reloc_keys = [s for s, _t in self.reloc_sites]

    def section_of(self, rva, n=1):
        for s in self.sections:
            span = max(s["vsize"], s["rawsize"])
            if s["va"] <= rva and rva + n <= s["va"] + span:
                return s
        return None

    def is_code(self, rva, n=1):
        s = self.section_of(rva, n)
        return s is not None and bool(s["chars"] & 0x20000000)

    def read(self, rva, n):
        s = self.section_of(rva, n)
        if s is None:
            raise ValueError(f"RVA {rva:#x}+{n} is not mapped")
        rel = rva - s["va"]
        if rel + n > s["rawsize"]:
            raise ValueError(f"RVA {rva:#x}+{n} runs past raw data of {s['name']}")
        off = s["rawptr"] + rel
        return self.data[off:off + n]

    def _parse_relocs(self):
        rva, size = self.dirs[5]
        out = []
        if not rva:
            return out
        blob = self.read(rva, size)
        p = 0
        while p + 8 <= len(blob):
            page, bsz = struct.unpack_from("<II", blob, p)
            if bsz == 0:
                break
            for k in range(p + 8, p + bsz, 2):
                w = struct.unpack_from("<H", blob, k)[0]
                t = w >> 12
                if t:
                    out.append((page + (w & 0xFFF), t))
            p += bsz
        out.sort()
        return out

    def reloc_hits(self, lo, hi):
        """Base-relocation sites whose field overlaps [lo, hi) (RVAs)."""
        hits = []
        i = bisect.bisect_left(self._reloc_keys, lo - 8)
        while i < len(self.reloc_sites) and self.reloc_sites[i][0] < hi:
            site, t = self.reloc_sites[i]
            width = 8 if t == 10 else 4
            if site < hi and site + width > lo:
                hits.append((site, t))
            i += 1
        return hits


_PE_CACHE = {}


def _pe_of(exe: Path):
    key = str(exe)
    if key not in _PE_CACHE:
        _PE_CACHE[key] = PE(exe)
    return _PE_CACHE[key]


def disasm(exe: Path, va_lo: int, va_hi: int):
    """capstone decode from va_lo (an instruction boundary). Returns CsInsn list."""
    if capstone is None:
        raise SystemExit("capstone is required (pip install capstone)")
    pe = _pe_of(exe)
    rva = va_lo - IMAGE_BASE
    n = va_hi - va_lo
    while n > 0:
        try:
            code = pe.read(rva, n)
            break
        except ValueError:
            n -= 1
    else:
        return []
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return list(md.disasm(code, va_lo))


def disp_fields(insns):
    """VA -> label for every byte that is a rel32 or disp32 field.

    Returns (fields, problems). Problems are decodes that stopped early or
    displacements capstone did not report; their bytes are NOT marked.
    """
    fields = {}
    problems = []
    for insn in insns:
        va, L, mnem = insn.address, insn.size, insn.mnemonic
        is_branch = mnem in ("call", "jmp") or re.fullmatch(r"j[a-z]+", mnem) is not None
        ops = insn.operands
        if is_branch and ops and ops[0].type == capstone.x86.X86_OP_IMM and insn.imm_size == 4:
            tgt = ops[0].imm
            for i in range(insn.imm_offset, insn.imm_offset + 4):
                fields[va + i] = f"rel32->{tgt:#x}"
            continue
        for op in ops:
            if op.type != capstone.x86.X86_OP_MEM:
                continue
            if op.mem.base == capstone.x86.X86_REG_RIP:
                label = f"rip->{va + L + op.mem.disp:#x}"
            elif op.mem.base == 0 and op.mem.index == 0:
                label = f"abs->{op.mem.disp & 0xFFFFFFFF:#x}"
            else:
                continue
            # capstone 5.0.7 reports disp_size 2 for some prefixed SSE forms;
            # a RIP or absolute displacement is always four bytes, so check the
            # bytes at disp_offset spell the displacement it decoded.
            off = insn.disp_offset
            if off + 4 <= L and struct.unpack_from("<i", insn.bytes, off)[0] == op.mem.disp:
                for i in range(off, off + 4):
                    fields[va + i] = label
            else:
                problems.append((va, f"disp32 not found for {insn.mnemonic} {insn.op_str}"))
            break
    return fields, problems


# ---------------------------------------------------------------------------
# Source readers
# ---------------------------------------------------------------------------

def read_lines(path: Path):
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def hex_literals(line: str):
    """Integer values of every hex literal on a source line."""
    return {int(x, 16) for x in re.findall(r"0[xX]([0-9A-Fa-f]+)", line)}


def cpp_array(path: Path, decl_line: int, name: str):
    """Integer values of the C++ array `name` declared at decl_line.

    Searches from decl_line-1 for `name[`, then reads the brace initializer.
    Comments are stripped. WILD for -1. Multi-dimensional arrays come back
    flattened. Returns (values, first_line_no).
    """
    lines = read_lines(path)
    start = None
    for i in range(decl_line - 1, min(len(lines), decl_line + 40)):
        if re.search(r"\b" + re.escape(name) + r"\s*\[", lines[i]):
            start = i
            break
    if start is None:
        raise ValueError(f"{path.name}: array {name} not found near line {decl_line}")
    text = "\n".join(lines[start:start + 80])
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    eq = text.index("=")
    brace = text.index("{", eq)
    depth = 0
    end = None
    for k in range(brace, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                end = k
                break
    body = text[brace + 1:end]
    vals = []
    for tok in re.findall(r"-?0[xX][0-9A-Fa-f]+|-?\d+", body):
        if tok == "-1":
            vals.append(WILD)
        elif "x" in tok.lower():
            vals.append(int(tok, 16))
        else:
            vals.append(int(tok))
    return vals, start + 1


def masked_text(text):
    """text with // and /* */ comments blanked to spaces (offsets and newlines kept)."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


TABLE_RE = {
    "partframe": re.compile(r"\{0x([0-9A-Fa-f]+)u,(\d+),\{([^}]*)\}"),
    "calls": re.compile(r"\{0x([0-9A-Fa-f]+)u,()\{([^}]*)\}\}"),
    "mem": re.compile(r"\{0x([0-9A-Fa-f]+),\s*(\d+),\s*\{([^}]*)\}\}", re.S),
}


def table_entries(path: Path, lo: int, hi: int, kind: str):
    """Entries of a {0xRVA, [n,] {bytes}} table between source lines lo and hi.

    kind 'partframe': {0xRVAu,n,{bytes}}  -> bytes length must equal n
    kind 'calls':     {0xRVAu,{bytes}}
    kind 'mem':       {0xRVA, n, {bytes}}  (mem_tick.cpp; an entry may span lines)
    Returns [(line, rva, bytes, [(start, end) of each byte token in the file text])].
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.split("\n")
    start = sum(len(l) + 1 for l in lines[:lo - 1])
    stop = sum(len(l) + 1 for l in lines[:hi])
    seg = masked_text(text)[start:stop]
    out = []
    for m in TABLE_RE[kind].finditer(seg):
        rva = int(m.group(1), 16)
        spans, vals = [], []
        for b in re.finditer(r"0[xX]([0-9A-Fa-f]+)", m.group(3)):
            vals.append(int(b.group(1), 16))
            spans.append((start + m.start(3) + b.start(), start + m.start(3) + b.end()))
        if m.group(2) and len(vals) != int(m.group(2)):
            raise ValueError(f"{path.name}: n={m.group(2)} but {len(vals)} bytes")
        out.append((lo + seg.count("\n", 0, m.start()), rva, vals, spans))
    return out


def parsed_table(path: Path, lo: int, hi: int, kind: str):
    """[(line, rva, bytes)] of table_entries."""
    return [(ln, rva, vals) for ln, rva, vals, _spans in table_entries(path, lo, hi, kind)]


def fp_values(line):
    """(rva, size, hash) of a fingerprint(v, rva, size, hash) call on a line."""
    m = re.search(r"fingerprint\(v, (0x[0-9A-Fa-f]+)u?, (0x[0-9A-Fa-f]+)u?, (0x[0-9A-Fa-f]{16})ull?", line)
    if not m:
        raise ValueError("no fingerprint call: " + line.strip())
    return int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)


def hash_literal(line):
    """The single 64-bit hash literal on a source line."""
    m = re.findall(r"0x([0-9A-Fa-f]{16})ull", line)
    if len(m) != 1:
        raise ValueError(f"expected one hash literal, found {len(m)}: {line.strip()}")
    return int(m[0], 16)


def oculus_python_pins():
    """(byte-checks {rva: (bytes, line)}, fingerprints [(rva, size, hash, line)])."""
    tree = ast.parse(OCULUS_PY.read_text(encoding="utf-8"))
    byte_checks, fps = {}, []
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
            if name == "_BYTE_CHECKS":
                for k, v in zip(node.value.keys, node.value.values):
                    val = bytes.fromhex(v.args[0].value)
                    byte_checks[k.value] = (list(val), k.lineno)
            if name == "_FINGERPRINTS":
                for elt in node.value.elts:
                    rva, size, h = (e.value for e in elt.elts)
                    fps.append((rva, size, h, elt.lineno))
    return byte_checks, fps


def load_map():
    old2new, new2old = {}, {}
    for path in (MAP_JSON, MAP_EXTRA_JSON):
        for e in json.loads(path.read_text()):
            old = int(e["rva"], 16)
            new = int(e["new_rva"], 16) if e.get("new_rva") not in (None, "None", "") else None
            old2new.setdefault(old, new)
            if new is not None:
                new2old.setdefault(new, old)
    for old, new in EXTRA_MAP.items():
        old2new.setdefault(old, new)
        new2old.setdefault(new, old)
    return old2new, new2old


_MAPS = None


def src_rva(rva):
    """The literal the source holds for a pin whose 334473 RVA is `rva`.

    pre: the 334473 value itself. post: its 334925 placement (the map entry, else
    the nearest mapped RVA below it with the same delta)."""
    global _MAPS
    if MODE == "pre":
        return rva
    if _MAPS is None:
        _MAPS = load_map()
    fwd = _MAPS[0]
    if fwd.get(rva) is not None:
        return fwd[rva]
    for d in range(1, 0x1000):
        if fwd.get(rva - d) is not None:
            return fwd[rva - d] + d
    raise SystemExit(f"no map for {rva:#x}")


def rva_kind():
    """'old' when the source literal is 334473's, 'new' when 334925's."""
    return "old" if MODE == "pre" else "new"


def id_rva(literal):
    """The 334473 RVA a source literal stands for, so a pin's id and its baseline
    entry are the same before and after the edit pass."""
    global _MAPS
    if MODE == "pre":
        return literal
    if _MAPS is None:
        _MAPS = load_map()
    return _MAPS[1].get(literal, literal)


# ---------------------------------------------------------------------------
# Pin table. A pin's RVA literal is named by (rva_file, rva_line). The value
# is read from the source file, never typed in here.
# ---------------------------------------------------------------------------
OCULUS_H = "src/common/elite_oculus_profile.h"
EXPLORER_H = "src/d3d11/explorer_cam_core.h"
UI_H = "src/d3d11/ui_sizing_math.h"
KIN_CPP = "src/d3d11/kinematic_eval_hook.cpp"
KIN_PROBE_H = "src/d3d11/kinematic_eval_probe.h"
ORWP_CPP = "src/d3d11/object_record_writer_probe.cpp"
ORWP_H = "src/d3d11/object_record_writer_probe.h"
CGP_H = "src/d3d11/cull_gate_probe.h"
OBJ_CPP = "src/d3d11/object_probe.cpp"
PAD_H = "src/d3d11/pad_state_core.h"
PAD_CPP = "src/d3d11/pad_state_probe.cpp"
STORE_CPP = "src/d3d11/store_hook.cpp"
OSC_H = "src/d3d11/optimized_station_core.h"
SCHED_H = "src/d3d11/scheduler_stack_probe.h"
SCHED_CPP = "src/d3d11/scheduler_stack_hook.cpp"
SKIN_CPP = "src/d3d11/skin_entity_hook.cpp"
LG_H = "src/d3d11/light_grid_cache_core.h"
VR_H = "src/d3d11/vertex_resync_core.h"
SSAA_CPP = "src/d3d11/vr_ssaa_hold.cpp"
VRT_CPP = "src/d3d11/vr_window_trampolines.cpp"
TFEB_CPP = "src/d3d11/transition_flash_eye_base.cpp"
ENGV_CPP = "src/d3d11/engine_velocity.cpp"
FCI_CPP = "src/d3d11/flat_camera_inject.cpp"
FCPP_CPP = "src/d3d11/flat_camera_producer_probe.cpp"
FSC_CPP = "src/d3d11/flat_stationary_camera_capture.cpp"
TCC_H = "src/d3d11/terrain_cull_cover_core.h"

SPECS = []


def B(pid, file, decl_line, decl, rva, rva_line, sym, rva_file=None, off=0, row=None, width=None):
    """Byte array `decl` (declared at file:decl_line) checked at rva (+off).
    rva is the literal as the source holds it (new build); rva_line carries it."""
    SPECS.append(dict(kind="bytes", id=pid, file=file, decl_line=decl_line, decl=decl,
                      rva=rva, rva_line=rva_line, rva_file=rva_file or file, off=off,
                      row=row, width=width, sym=sym))


def T(pid, file, lo, hi, table, sym):
    SPECS.append(dict(kind="bytes_parsed", id=pid, file=file, lo=lo, hi=hi, table=table, sym=sym))


def F(pid, file, line, base, base_line, lo, hi, sym, base_file=None):
    """FNV-1a64 over [base+lo, base+hi), literal on file:line, base literal on base_line."""
    SPECS.append(dict(kind="fnv", id=pid, file=file, line=line, base=base, base_line=base_line,
                      base_file=base_file or file, lo=lo, hi=hi, sym=sym))


# --- Oculus profile, C++ (byte checks) ------------------------------------
B("oc-selKinds", OCULUS_H, 413, "selectorKinds", 0x8D546B, 434, "selector kind 1")
B("oc-selKind2", OCULUS_H, 416, "selectorKind2", 0x8D5478, 435, "selector kind 2")
B("oc-selKind3", OCULUS_H, 419, "selectorKind3", 0x8D5480, 436, "selector kind 3")
B("oc-exitsOnSuccess", OCULUS_H, 422, "exitsOnSuccess", 0x8D5488, 437, "selector exit")
B("oc-advances", OCULUS_H, 423, "advances", 0x8D549C, 438, "selector loop advance")
B("oc-virtualInit", OCULUS_H, 426, "virtualInit", 0x4E4E50, 439, "factory virtual init")
B("oc-initFailure", OCULUS_H, 427, "initFailure", 0x4E477C, 440, "LibOVR init failure")
B("oc-falseReturn", OCULUS_H, 428, "falseReturn", 0x4E47B1, 441, "LibOVR false return")
B("oc-nullModule", OCULUS_H, 429, "nullModule", 0x4E720E, 442, "null module path")
B("oc-continuation", OCULUS_H, 452, "continuation", 0x4E71BC, 48, "SDK loader continuation")

# --- explorer cam prologues -------------------------------------------------
B("ec-freeCamera", EXPLORER_H, 53, "kFreeCameraPrologue", 0x1072A80, 38, "FreeCameraActivity update")
B("ec-collision", EXPLORER_H, 61, "kCollisionPrologue", 0x1092240, 39, "collision step")
B("ec-boxPush", EXPLORER_H, 69, "kBoxPushPrologue", 0x10902B0, 40, "commander box push")
B("ec-cameraUi", EXPLORER_H, 76, "kCameraUiPrologue", 0x47CA180, 41, "VanityCameraUIActivity update")
B("ec-controller", EXPLORER_H, 81, "kControllerPrologue", 0x2DF3F60, 42, "VesselCameraMountControl update")
B("ec-avatarFade", EXPLORER_H, 87, "kAvatarFadePrologue", 0x3DD8D00, 43, "AvatarModelComponent fade")
B("ec-findJoint", EXPLORER_H, 95, "kFindJointPrologue", 0xFDEC10, 93, "skeleton FindJoint")
B("ec-zoomDof", EXPLORER_H, 103, "kZoomDofPrologue", 0x1079A90, 101, "zoom/DOF controls update")

# --- UI panel scale (ui_sizing_math.h) --------------------------------------
B("ui-shape-site0", UI_H, 174, "kUiPanelShape", 0x457323A, 185, "panel shape @ site 0")
B("ui-shape-site1", UI_H, 174, "kUiPanelShape", 0x4573D45, 185, "panel shape @ site 1")
B("ui-siteBytes0", UI_H, 186, "kUiPanelSiteBytes", 0x457323A, 185, "panel site bytes row 0", row=0, width=30)
B("ui-siteBytes1", UI_H, 186, "kUiPanelSiteBytes", 0x4573D45, 185, "panel site bytes row 1", row=1, width=30)
B("ui-prolog0", UI_H, 194, "kUiPanelProlog0", 0x4573040, 193, "panel function 0 prologue")
B("ui-prolog1", UI_H, 195, "kUiPanelProlog1", 0x4573CC0, 193, "panel function 1 prologue")
B("ui-ssSetter", UI_H, 657, "kUiSsSetterBytes", 0x2878E00, 654, "scale setter")
B("ui-ssGetter", UI_H, 660, "kUiSsGetterBytes", 0x2843B00, 654, "scale getter")

# --- kinematic evaluator and set (kinematic_eval_hook.cpp) ------------------
B("kin-eval-prologue", KIN_CPP, 896, "kPrologue", 0x4311CA0, 27, "targetValid evaluator prologue",
  rva_file=KIN_PROBE_H)
B("kin-eval-tail", KIN_CPP, 928, "tail", 0x4311CA0, 27, "evaluator bytes after our patch",
  rva_file=KIN_PROBE_H, off=5)
B("kin-part-prologue", KIN_CPP, 314, "kPartPrologue", 0x42B6C80, 311, "part-test prologue")
T("kin-partframe", KIN_CPP, 318, 346, "partframe", "part-test frame (kPartFrame)")
B("kin-primary-prologue", KIN_CPP, 982, "prologue", 0x42B6DF0, 990, "primary prologue")
B("kin-primary-call", KIN_CPP, 984, "call", 0x42B7503, 999, "primary caller call")
B("kin-primary-append", KIN_CPP, 985, "append", 0x42B7098, 1000, "primary append sequence")
B("kin-primary-key", KIN_CPP, 986, "key", 0x42B6E1B, 1001, "primary key load")
B("kin-primary-owner", KIN_CPP, 987, "owner", 0x42B6F9C, 1002, "primary owner load")
B("kin-pool-prologue", KIN_CPP, 1029, "prologue", 0x4C847C0, 1036, "pool copier prologue")
B("kin-pool-layout", KIN_CPP, 1031, "layout", 0x4C847DC, 1043, "pool copier layout")
B("kin-merge-prologue", KIN_CPP, 1062, "prologue", 0x4351440, 1063, "list merge prologue")
B("kin-clear-prologue", KIN_CPP, 1088, "prologue", 0x36846D0, 1089, "typed clear prologue")
F("kin-primary-hash", KIN_CPP, 1006, 0x42B6DF0, 1004, 5, 0x2DA, "primary body FNV")
F("kin-outer-hash", KIN_CPP, 1007, 0x42B70E0, 1005, 5, 0xC96, "outer builder body FNV")
F("kin-pool-hash", KIN_CPP, 1046, 0x4C847C0, 1045, 5, 0x283, "pool copier body FNV")
F("kin-merge-hash", KIN_CPP, 1074, 0x4351440, 1073, 8, 0x2A0, "list merge body FNV")
F("kin-clear-hash", KIN_CPP, 1097, 0x36846D0, 1096, 5, 0x152, "typed clear body FNV")

# --- object record writer ---------------------------------------------------
T("orwp-calls", ORWP_CPP, 20, 27, "calls", "object record writer call sites (kCalls)")
B("orwp-lookup", ORWP_CPP, 29, "kLookupPrologue", 0x3699CA0, 22, "object lookup prologue", rva_file=ORWP_H)
B("orwp-ownership-virtual", ORWP_CPP, 30, "kOwnershipVirtualCall", 0x431DF1F, 14,
  "ownership virtual call (base = return RVA)", off=-3)

# --- other hook prologues ---------------------------------------------------
B("cgp-frustum", OBJ_CPP, 221, "kFrustumPrologue", 0x4F4F70, 118, "cull frustum prologue", rva_file=CGP_H)
B("store-prologue", OSC_H, 1361, "kStorePrologue", 0x4314D00, 26, "store prologue", rva_file=STORE_CPP)
B("pad-reader0", PAD_H, 22, "kReaderPin", 0x12C4D60, 20, "pad reader 0 pin")
B("pad-reader1", PAD_H, 22, "kReaderPin", 0x12CC0C0, 20, "pad reader 1 pin")
B("pad-reader2", PAD_H, 22, "kReaderPin", 0x12CBF20, 20, "pad reader 2 pin")
B("pad-reader3", PAD_H, 22, "kReaderPin", 0x12CC460, 20, "pad reader 3 pin")
B("sched-drain", SCHED_CPP, 30, "kDrainPrologue", 0x42E2600, 54, "scheduler drain prologue", rva_file=SCHED_H)
B("sched-reset", SCHED_CPP, 32, "kResetPrologue", 0x36A3C50, 54, "scheduler reset prologue", rva_file=SCHED_H)
B("skin-prologue", SKIN_CPP, 27, "kPrologue", 0x4C56CC0, 22, "skin job assembly prologue")
B("lg-builder", LG_H, 54, "kPrologue", 0x58B2B0, 51, "light grid builder prologue")
B("lg-refresh", LG_H, 76, "kRefreshPrologue", 0x4F3150, 75, "light grid refresh prologue")
B("lg-copy", LG_H, 85, "kCopyPrologue", 0x57A5C0, 84, "light grid copy prologue")
B("vr-flushIa", VR_H, 48, "kFlushIaPrologue", 0x522BB0, 44, "FlushIA prologue")
B("ssaa-wrapper", SSAA_CPP, 37, "kWrapperBytes", 0x2864DA0, 31, "SSAA wrapper bytes")
B("vrt-request", VRT_CPP, 29, "kRequestBytes", 0x7E9EB0, 28, "window request bytes")
B("vrt-copy", VRT_CPP, 36, "kCopyBytes", 0x281E710, 35, "window copy bytes")
B("tfeb-consumer", TFEB_CPP, 43, "kConsumerBytes", 0x28457F0, 42, "transition flash consumer")
B("eng-lookup", ENGV_CPP, 589, "kLookupPrologue", 0x3699CA0, 588, "engine lookup prologue (32)")
B("eng-append", ENGV_CPP, 593, "kAppendBytes", 0x4315E45, 592, "engine append bytes")
B("fci-refresh", FCI_CPP, 107, "kRefreshPrologue", 0x592360, 99, "flat camera refresh prologue")
B("fcpp-upload", FCPP_CPP, 50, "kUploadPrologue", 0x51B7A0, 49, "flat camera upload prologue")
B("fsc-renderer", FSC_CPP, 25, "kPrologue", 0x2874350, 24, "flat stationary renderer prologue")

# --- Elite memory accounting (mem_tick.cpp; added after the 334473 retarget) -------
MEM_CPP = "src/d3d11/mem_tick.cpp"
T("mem-gpu", MEM_CPP, 112, 123, "mem", "GPU resource accounting pins (kGpuPins)")
T("mem-cpu", MEM_CPP, 129, 139, "mem", "CPU heap manager pins (kCpuPins)")

# --- terrain cull cover -----------------------------------------------------
B("tc-head", TCC_H, 50, "kHead", 0x4441580, 42, "cull-plane builder head")
B("tc-fragA", TCC_H, 57, "kFragA", 0x4458D5D, 44, "call-site fragment A")

# --- Oculus fingerprints (C++, one per line 401-411) -------------------------
for _ln in range(401, 412):
    SPECS.append(dict(kind="fp", id=f"oc-fp-{_ln}", file=OCULUS_H, line=_ln, sym="Oculus fingerprint"))


# ---------------------------------------------------------------------------
# Checker
# ---------------------------------------------------------------------------

class Ctx:
    def __init__(self):
        self.old = PE(OLD_EXE)
        self.new = PE(NEW_EXE)
        self.old2new, self.new2old = load_map()
        self.rows = []

    def new_of(self, rva):
        return self.old2new.get(rva)

    def pair(self, base, off, rva_is=None):
        """(old_eff, new_eff, how) for a pin whose source literal is `base`.

        rva_is 'new': the literal is the new-build RVA and the old one is its
        map entry. rva_is 'old': the literal is the old-build RVA (the one
        the source holds before the edit pass) and the new one is its map
        entry. A sub-function RVA with no map entry is anchored on the nearest
        mapped RVA below it, with the same delta; the old-exe reproduction
        still has to pass. Default: the run's --mode.
        """
        rva_is = rva_is or rva_kind()
        if rva_is == "old":
            fwd = self.old2new          # old -> new
            back = self.new2old         # new -> old (unused here)
            if base in fwd and fwd[base] is not None:
                old_base, new_base, how = base, fwd[base], "map"
            else:
                new_base, how = None, None
                for d in range(1, 0x1000):
                    if base - d in fwd and fwd[base - d] is not None:
                        new_base, how = fwd[base - d] + d, f"anchored(-{d:#x})"
                        break
                if new_base is None:
                    return None, None, "no-map"
                old_base = base
            return old_base + off, new_base + off, how
        new_eff_base = base
        if base in self.new2old:
            old_base, how = self.new2old[base], "map"
        else:
            old_base, how = None, None
            for d in range(1, 0x1000):
                cand = base - d
                if cand in self.new2old:
                    old_base, how = self.new2old[cand] + d, f"anchored(-{d:#x})"
                    break
        if old_base is None:
            return None, new_eff_base + off, "no-map"
        return old_base + off, new_eff_base + off, how


def _diff_analysis(old_va, new_va, old_b, new_b, mask):
    """Return (explained_positions, bad_positions, detail) for differing bytes."""
    n = len(old_b)
    old_fields, old_prob = disp_fields(disasm(OLD_EXE, old_va, old_va + n + 16))
    new_fields, new_prob = disp_fields(disasm(NEW_EXE, new_va, new_va + n + 16))
    diffs = [i for i in range(n) if mask[i] is not WILD and old_b[i] != new_b[i]]
    good, bad = [], []
    for i in diffs:
        (good if (new_va + i) in new_fields else bad).append(i)
    detail = []
    for i in diffs[:16]:
        label = new_fields.get(new_va + i, "NOT-IN-FIELD")
        detail.append(f"+{i}:{old_b[i]:02X}->{new_b[i]:02X}[{label}]")
    if new_prob or old_prob:
        detail.append(f"decode-problems new={new_prob[:2]} old={old_prob[:2]}")
    return good, bad, " ".join(detail)


def _masked_equal(pinned, actual):
    return len(pinned) == len(actual) and all(p is WILD or p == a for p, a in zip(pinned, actual))


def _hexstr(vals):
    return " ".join("??" if v is WILD else f"{v:02X}" for v in vals)


def check_bytes(ctx, pid, sym, src_label, pinned, base_new, off, base_file, base_line, rva_is=None):
    """Check one byte pin. pinned: ints (WILD allowed)."""
    rva_is = rva_is or rva_kind()
    n = len(pinned)
    row = dict(id=pid, src=src_label, sym=sym, size=n)
    lines = read_lines(base_file)
    if not any(base_new in hex_literals(lines[i]) for i in range(max(0, base_line - 4), min(len(lines), base_line + 3))):
        row["status"] = "SRC-MISMATCH"
        row["detail"] = f"RVA {base_new:#x} not on {base_file.name}:{base_line}"
        ctx.rows.append(row)
        return
    old_eff, new_eff, how = ctx.pair(base_new, off, rva_is)
    row.update(old=old_eff, new=new_eff, how=how)
    if old_eff is None:
        row["status"] = "NO-MAP"
        ctx.rows.append(row)
        return
    if not ctx.new.is_code(new_eff, n) or not ctx.old.is_code(old_eff, n):
        row["status"] = "NOT-CODE"
        ctx.rows.append(row)
        return
    row["pinned"] = pinned
    old_pin = BASELINE.get(pid, pinned)
    old_pin = [WILD if v is None else v for v in old_pin] if isinstance(old_pin, list) else old_pin
    old_b = list(ctx.old.read(old_eff, n))
    if not _masked_equal(old_pin, old_b):
        row["status"] = "UNREPRODUCED"
        row["detail"] = f"old exe at {old_eff:#x} reads {_hexstr(old_b)}, pin {_hexstr(old_pin)}"
        ctx.rows.append(row)
        return
    row["old_ok"] = True
    new_b = list(ctx.new.read(new_eff, n))
    if _masked_equal(pinned, new_b):
        row["status"] = "KEEP"
        ctx.rows.append(row)
        return
    good, bad, detail = _diff_analysis(IMAGE_BASE + old_eff, IMAGE_BASE + new_eff, old_b, new_b, pinned)
    row["detail"] = detail
    row["ndiff"] = len(good) + len(bad)
    if bad:
        row["status"] = "SUSPICIOUS"
    else:
        row["status"] = "NEW"
        row["newvalue"] = [WILD if pinned[i] is WILD else new_b[i] for i in range(n)]
    ctx.rows.append(row)


def run_bytes_spec(ctx, s):
    src = REPO / s["file"]
    vals, _first = cpp_array(src, s["decl_line"], s["decl"])
    if s.get("row") is not None:
        vals = vals[s["row"] * s["width"]:(s["row"] + 1) * s["width"]]
    label = f"{s['file']}:{s['decl_line']} {s['decl']}"
    check_bytes(ctx, s["id"], s["sym"], label, vals, src_rva(s["rva"]), s["off"],
                REPO / s["rva_file"], s["rva_line"])


def run_fnv(ctx, pid, sym, label, hval, base_new, lo, hi, base_file, base_line, size=None, rva_is=None):
    """FNV-1a64 over [base+lo, base+hi). hval is the source's pinned value."""
    rva_is = rva_is or rva_kind()
    if size is None:
        size = hi - lo
    lines = read_lines(base_file)
    if not any(base_new in hex_literals(lines[i]) for i in range(max(0, base_line - 4), min(len(lines), base_line + 3))):
        ctx.rows.append(dict(id=pid, src=label, sym=sym, status="SRC-MISMATCH",
                             detail=f"base {base_new:#x} not near {base_file.name}:{base_line}"))
        return
    old_start, new_start, how = ctx.pair(base_new, lo, rva_is)
    row = dict(id=pid, src=label, sym=sym, size=size, old=old_start, new=new_start, how=how,
               pinned=hval)
    if old_start is None:
        row["status"] = "NO-MAP"
        ctx.rows.append(row)
        return
    old_b = ctx.old.read(old_start, size)
    old_h = fnv1a64(old_b)
    row["old_hash"] = old_h
    old_pin = BASELINE.get(pid, hval)
    row["old_pin"] = old_pin
    row["old_ok"] = old_h == old_pin
    if old_h != old_pin:
        row["status"] = "UNREPRODUCED"
        row["detail"] = f"old hash {old_h:016X} != pinned {old_pin:016X}"
        ctx.rows.append(row)
        return
    new_b = ctx.new.read(new_start, size)
    new_h = fnv1a64(new_b)
    row["new_hash"] = new_h
    row["reloc_old"] = len(ctx.old.reloc_hits(old_start, old_start + size))
    row["reloc_new"] = len(ctx.new.reloc_hits(new_start, new_start + size))
    if new_h == hval:
        row["status"] = "KEEP"
        ctx.rows.append(row)
        return
    good, bad, detail = _diff_analysis(IMAGE_BASE + old_start, IMAGE_BASE + new_start,
                                       list(old_b), list(new_b), [0] * size)
    row["detail"] = detail
    row["ndiff"] = len(good) + len(bad)
    row["status"] = "SUSPICIOUS" if bad else "NEW"
    row["newvalue"] = new_h
    ctx.rows.append(row)


def run_fnv_spec(ctx, s):
    line_text = read_lines(REPO / s["file"])[s["line"] - 1]
    hval = hash_literal(line_text)
    run_fnv(ctx, s["id"], s["sym"], f"{s['file']}:{s['line']}", hval,
            src_rva(s["base"]), s["lo"], s["hi"], REPO / s["base_file"], s["base_line"])


def run_fp_spec(ctx, s):
    line_text = read_lines(REPO / s["file"])[s["line"] - 1]
    rva, size, hval = fp_values(line_text)
    run_fnv(ctx, s["id"], s["sym"], f"{s['file']}:{s['line']}", hval, rva, 0, size,
            REPO / s["file"], s["line"], size=size)


def run_parsed_spec(ctx, s):
    rows = parsed_table(REPO / s["file"], s["lo"], s["hi"], s["table"])
    for ln, rva, vals in rows:
        pid = f"{s['id']}@{id_rva(rva):#x}"
        check_bytes(ctx, pid, s["sym"], f"{s['file']}:{ln}", vals, rva, 0, REPO / s["file"], ln)


def run_python_oculus(ctx):
    byte_checks, fps = oculus_python_pins()
    for rva_key, (vals, line) in sorted(byte_checks.items()):
        pid = f"py-byte-{id_rva(rva_key):#x}"
        is_ = PY_RVA_IS.get(rva_key, rva_kind())
        check_bytes(ctx, pid, "python _BYTE_CHECKS" + (" (old-build key)" if is_ == "old" else ""),
                    f"{OCULUS_PY.relative_to(REPO)}:{line}", vals, rva_key, 0, OCULUS_PY, line, rva_is=is_)
    for rva, size, h, line in fps:
        run_fnv(ctx, f"py-fp-{id_rva(rva):#x}", "python _FINGERPRINTS", f"{OCULUS_PY.relative_to(REPO)}:{line}",
                h, rva, 0, size, OCULUS_PY, line, size=size)


def run_terrain_site(ctx):
    """Call-site chain of the cull-plane builder: fragments A, then B, C, D with
    gaps <= kMaxGap, then E8 rel32 -> kTargetRva (terrain_cull_cover_core.h
    matchSite). Checked on both exes; the expected call and target are the
    source's (new) constants mapped to the old build."""
    tcc = REPO / TCC_H
    frag = {}
    for name, line in (("kFragA", 57), ("kFragB", 58), ("kFragC", 59), ("kFragD", 60)):
        frag[name] = bytes(cpp_array(tcc, line, name)[0])
    max_gap = 16
    site_lit, call_lit, tgt_lit = src_rva(0x4458D5D), src_rva(0x4458D87), src_rva(0x4441580)
    lines = read_lines(tcc)
    for lit, ln in ((site_lit, 44), (call_lit, 43), (tgt_lit, 42)):
        if lit not in hex_literals(lines[ln - 1]):
            ctx.rows.append(dict(id="tc-site", src=f"{TCC_H}:{ln}", status="SRC-MISMATCH",
                                 detail=f"{lit:#x} not on line {ln}"))
            return
    site_old, site_new, how = ctx.pair(site_lit, 0)
    call_old, call_new, _ = ctx.pair(call_lit, 0)
    tgt_old, tgt_new, _ = ctx.pair(tgt_lit, 0)

    def scan(pe, site):
        def rd(a, n):
            try:
                return pe.read(a, n)
            except ValueError:
                return None
        if rd(site, 6) != frag["kFragA"]:
            return None
        pos = site + 6
        layout = []
        for name in ("kFragB", "kFragC", "kFragD"):
            for gap in range(0, max_gap + 1):
                if rd(pos + gap, len(frag[name])) == frag[name]:
                    layout.append(gap)
                    pos = pos + gap + len(frag[name])
                    break
            else:
                return None
        for gap in range(0, max_gap + 1):
            b = rd(pos + gap, 5)
            if b and b[0] == 0xE8:
                rel = struct.unpack("<i", b[1:5])[0]
                return pos + gap, pos + gap + 5 + rel, tuple(layout)
        return None

    new_res = scan(ctx.new, site_new)
    old_res = scan(ctx.old, site_old)
    row = dict(id="tc-site", src=f"{TCC_H}:42-57", sym="cull-plane call-site chain", size=None,
               old=site_old, new=site_new, how=how)
    if new_res is None or old_res is None:
        row["status"] = "SITE-MISMATCH"
        row["detail"] = f"scan new={new_res} old={old_res}"
    elif (new_res[0], new_res[1]) != (call_new, tgt_new):
        row["status"] = "SITE-MISMATCH"
        row["detail"] = f"new call {new_res[0]:#x} -> {new_res[1]:#x}, expected {call_new:#x} -> {tgt_new:#x}"
    elif (old_res[0], old_res[1]) != (call_old, tgt_old):
        row["status"] = "SITE-MISMATCH"
        row["detail"] = f"old call {old_res[0]:#x} -> {old_res[1]:#x}, expected {call_old:#x} -> {tgt_old:#x}"
    elif new_res[2] != old_res[2]:
        row["status"] = "SITE-MISMATCH"
        row["detail"] = f"fragment gaps differ: old {old_res[2]} new {new_res[2]}"
    else:
        row["status"] = "SITE-OK"
        row["old_ok"] = True
        row["detail"] = (f"fragment gaps {new_res[2]} (same in both); E8 at {call_old:#x}->{old_res[1]:#x} old, "
                         f"{call_new:#x}->{new_res[1]:#x} new (rel32 moved, target kTargetRva)")
    ctx.rows.append(row)


def report(ctx):
    counts = {}
    for r in ctx.rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    out = []
    for r in ctx.rows:
        st = r["status"]
        head = f"{st:<13} {r['id']:<26} {r.get('src', ''):<52} {r.get('sym', '')[:30]:<30}"
        if r.get("old") is not None:
            head += f" old={r['old']:#x}"
        if r.get("new") is not None:
            head += f" new={r['new']:#x}"
        if r.get("how") not in (None, "map"):
            head += f" [{r['how']}]"
        if r.get("size") is not None:
            head += f" n={r['size']}"
        if "old_hash" in r:
            head += (f" hash pin={r['pinned']:016X} old={r['old_hash']:016X}"
                     f" new={r.get('new_hash', 0):016X} reloc(old/new)={r.get('reloc_old')}/{r.get('reloc_new')}")
        if "newvalue" in r and isinstance(r["newvalue"], int):
            head += f" NEWHASH={r['newvalue']:016X}"
        if "newvalue" in r and isinstance(r["newvalue"], list):
            head += " NEWBYTES=" + _hexstr(r["newvalue"])
        out.append(head)
        if r.get("old_ok"):
            out[-1] += "  old-exe-reproduces"
        if r.get("detail"):
            out.append("      " + r["detail"][:500])
    out.append("SUMMARY pins=%d " % len(ctx.rows) + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# --apply-new: write the NEW pins of a saved --mode pre run into the source
# ---------------------------------------------------------------------------

def array_spans(path: Path, decl_line: int, name: str):
    """[(start, end)] of every value token of the C++ array `name` (file text offsets)."""
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.split("\n")
    start = None
    for i in range(decl_line - 1, min(len(lines), decl_line + 40)):
        if re.search(r"\b" + re.escape(name) + r"\s*\[", lines[i]):
            start = i
            break
    if start is None:
        raise ValueError(f"{path.name}: array {name} not found near line {decl_line}")
    off = sum(len(l) + 1 for l in lines[:start])
    mt = masked_text(text)
    brace = mt.index("{", mt.index("=", off))
    depth, end = 0, None
    for k in range(brace, len(mt)):
        if mt[k] == "{":
            depth += 1
        elif mt[k] == "}":
            depth -= 1
            if depth == 0:
                end = k
                break
    return [(brace + 1 + m.start(), brace + 1 + m.end())
            for m in re.finditer(r"-?0[xX][0-9A-Fa-f]+|-?\d+", mt[brace + 1:end])]


def fmt_hex_like(tok: str, value: int, width: int) -> str:
    """`value` written like the hex token `tok` (prefix and letter case kept)."""
    digits = tok[2:]
    s = f"{value:0{width}X}"
    if any(c.isalpha() for c in digits) and digits == digits.lower():
        s = s.lower()
    return tok[:2] + s


def apply_new(pre_path: str):
    pre = {r["id"]: r for r in json.loads(Path(pre_path).read_text())}
    edits = {}      # path -> [(start, end, text)]
    done = []

    def want(pid):
        r = pre.get(pid)
        return r if r and r.get("status") == "NEW" and r.get("newvalue") is not None else None

    def add(path, start, end, text):
        edits.setdefault(path, []).append((start, end, text))

    def bytes_edit(path, spans, pinned_now, new_vals, pid):
        text = path.read_text(encoding="utf-8", errors="replace")
        if len(spans) != len(new_vals):
            raise ValueError(f"{pid}: {len(spans)} tokens for {len(new_vals)} bytes")
        n = 0
        for (a, b), cur, new in zip(spans, pinned_now, new_vals):
            if new is None or cur == new:
                continue
            tok = text[a:b]
            add(path, a, b, fmt_hex_like(tok, new, 2))
            n += 1
        done.append((pid, n))

    for s in SPECS:
        k = s["kind"]
        if k == "bytes":
            r = want(s["id"])
            if not r:
                continue
            path = REPO / s["file"]
            spans = array_spans(path, s["decl_line"], s["decl"])
            vals, _ = cpp_array(path, s["decl_line"], s["decl"])
            if s.get("row") is not None:
                lo, hi = s["row"] * s["width"], (s["row"] + 1) * s["width"]
                spans, vals = spans[lo:hi], vals[lo:hi]
            bytes_edit(path, spans, vals, r["newvalue"], s["id"])
        elif k == "bytes_parsed":
            path = REPO / s["file"]
            for ln, rva, vals, spans in table_entries(path, s["lo"], s["hi"], s["table"]):
                pid = f"{s['id']}@{id_rva(rva):#x}"
                r = want(pid)
                if r:
                    bytes_edit(path, spans, vals, r["newvalue"], pid)
        elif k in ("fnv", "fp"):
            r = want(s["id"])
            if not r:
                continue
            path = REPO / s["file"]
            text = path.read_text(encoding="utf-8", errors="replace")
            lines = text.split("\n")
            off = sum(len(l) + 1 for l in lines[:s["line"] - 1])
            m = re.search(r"0x([0-9A-Fa-f]{16})ull", lines[s["line"] - 1])
            digits = m.group(1)
            new = f"{r['newvalue']:016X}"
            if digits == digits.lower():
                new = new.lower()
            add(path, off + m.start(1), off + m.end(1), new)
            done.append((s["id"], 1))
    for path, es in edits.items():
        text = path.read_text(encoding="utf-8", errors="replace")
        for a, b, new in sorted(es, reverse=True):
            text = text[:a] + new + text[b:]
        path.write_bytes(text.encode("utf-8"))
    for pid, n in done:
        print(f"applied {pid}: {n} edit(s)")
    print(f"applied {len(done)} pin(s) in {len(edits)} file(s); elite_oculus.py pins are edited by hand")


def _arg(argv, name):
    """Value after `name` in argv, or None."""
    return argv[argv.index(name) + 1] if name in argv else None


def main(argv):
    global BASELINE, MODE
    MODE = _arg(argv, "--mode") or "pre"
    if MODE not in ("pre", "post"):
        raise SystemExit("--mode is pre or post")
    if "--apply-new" in argv:
        if MODE != "post":
            raise SystemExit("--apply-new runs on the retargeted source: --mode post")
        apply_new(_arg(argv, "--apply-new"))
        return 0
    if MODE == "post" and BASELINE_JSON.exists():
        BASELINE = {k: (v if isinstance(v, int) else [None if x is None else x for x in v])
                    for k, v in json.loads(BASELINE_JSON.read_text()).items()}
    ctx = Ctx()
    for s in SPECS:
        k = s["kind"]
        if k == "bytes":
            run_bytes_spec(ctx, s)
        elif k == "bytes_parsed":
            run_parsed_spec(ctx, s)
        elif k == "fnv":
            run_fnv_spec(ctx, s)
        elif k == "fp":
            run_fp_spec(ctx, s)
    run_python_oculus(ctx)
    run_terrain_site(ctx)
    if "--write-baseline" in argv:
        dump = {r["id"]: (r["pinned"] if isinstance(r.get("pinned"), int) else
                          [None if v is None else v for v in r["pinned"]])
                for r in ctx.rows if "pinned" in r}
        BASELINE_JSON.write_text(json.dumps(dump, indent=0, sort_keys=True))
        print(f"baseline written: {len(dump)} pins -> {BASELINE_JSON.name}")
        return 0
    print(report(ctx))
    save = _arg(argv, "--save-pre")
    if save:
        Path(save).write_text(json.dumps(
            [{k: v for k, v in r.items() if k in ("id", "status", "pinned", "newvalue", "old", "new", "how", "src")}
             for r in ctx.rows], indent=0))
    bad = {"UNREPRODUCED", "SUSPICIOUS", "SRC-MISMATCH", "NO-MAP", "NOT-CODE", "SITE-MISMATCH"}
    return 1 if any(r["status"] in bad for r in ctx.rows) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
