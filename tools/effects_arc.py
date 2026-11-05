#!/usr/bin/env python3
"""Read the game's compiled effects archives offline: which shaders an effect uses, and their bytecode.

    python tools/effects_arc.py list --arc "<game>\\Win64\\EffectsBinary\\Effects2_Win64_SM50.arc"
    python tools/effects_arc.py find --arc A.arc --arc B.arc --hash 70F5F7604083BA90
    python tools/effects_arc.py extract --arc A.arc --effect CockpitGlass_Forward --out DIR [--disasm] [--dry-run]

Every build ships Win64\\EffectsBinary\\*.arc: Odyssey has Effects_Win64_SM50.arc
and Effects2_Win64_SM50.arc, the legacy Horizons client one Effects.arc. Each is
a 16-byte header and then one raw-deflate stream. Inflated, the stream is a run
of records laid back to back:

    +0  u16 version   +2  two-character tag   +4  4cc   +8  u32 record size

The 4cc is b"sd3f" for a shader record and b"xd3f" for an effect record. A
shader record holds one DXBC blob and a 16-byte id (at +12 in the legacy
version-2 records, +16 in Odyssey's version-5 ones). An effect record starts
with its parameter, texture and sampler names, then its own name, then tables
that name each pass's shaders by that 16-byte id. The game strips RDEF, so the
bytecode carries no names: this is how a shader EDVR hashes at creation is put
to an effect name without running the game. Hashes are the ones EDVR logs and
dumps shaders by: fnv1a64 in src\\common\\log.cpp over the whole blob, whose
offset basis is 1469598103934665603, not the standard FNV-1a-64 basis
14695981039346656037 (game_build_core.h's). An archive blob is byte for byte
the blob the game creates, so only that basis puts the two together.

docs\\fss-scanner.md and docs\\bloom-artifact-2026-10-20.md found none of their
hashes in these archives because they hashed with the standard basis;
docs\\cockpit-glass-legacy-port.md records how the format was read.

list prints every effect with its record offset and how many shaders it names.
find names the effects whose shaders include --hash. extract writes the shaders
of --effect as <vs|ps|...>_<HASH>.dxbc into --out, and with --disasm a .txt
beside each from D3DDisassemble (Windows, d3dcompiler_47.dll). --dry-run writes
nothing at all and prints what it would write. Read-only on the archives.
Exit 0 on success, 1 when the effect or hash is not found, 2 on usage or read
errors.
"""
import argparse
import os
import re
import struct
import sys
import tempfile
import zlib

HEADER_BYTES = 16
SHADER_4CC = b"sd3f"
EFFECT_4CC = b"xd3f"
STAGES = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs"}
# src\common\log.cpp's basis (see the docstring); every shader hash in src\ uses it.
FNV_OFFSET = 1469598103934665603
FNV_PRIME = 0x100000001B3


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def inflate(raw):
    if len(raw) <= HEADER_BYTES:
        raise ValueError("archive shorter than its header")
    d = zlib.decompressobj(-15)
    data = d.decompress(raw[HEADER_BYTES:])
    if not d.eof:
        raise ValueError("archive stream is truncated: the deflate data ends before its end marker")
    return data


def records(data):
    """(offset, 4cc, version, size) for each record, in order. Stops at the first
    record that does not carry a d3f 4cc or a sane size."""
    out = []
    pos = 0
    while pos + 12 <= len(data):
        cc = data[pos + 4:pos + 8]
        size = struct.unpack_from("<I", data, pos + 8)[0]
        if cc[1:] != b"d3f" or size < 12 or pos + size > len(data):
            break
        out.append((pos, cc, struct.unpack_from("<H", data, pos)[0], size))
        pos += size
    return out


def dxbc_stage(blob):
    if len(blob) < 32 or blob[:4] != b"DXBC":
        return "?"
    count = struct.unpack_from("<I", blob, 28)[0]
    for i in range(count):
        off = struct.unpack_from("<I", blob, 32 + 4 * i)[0]
        if blob[off:off + 4] in (b"SHEX", b"SHDR"):
            return STAGES.get(struct.unpack_from("<I", blob, off + 8)[0] >> 16, "?")
    return "?"


def shader_id_offset(version):
    return 12 if version <= 2 else 16


def shader_table(data, recs):
    """16-byte id -> (record offset, DXBC blob)."""
    table = {}
    for off, cc, ver, size in recs:
        if cc != SHADER_4CC:
            continue
        at = data.find(b"DXBC", off, off + size)
        if at < 0:
            continue
        blob = data[at:at + struct.unpack_from("<I", data, at + 24)[0]]
        idoff = off + shader_id_offset(ver)
        table[data[idoff:idoff + 16]] = (off, blob)
    return table


NAME_RE = re.compile(rb"([A-Za-z_][A-Za-z0-9_.]{2,})\x00\x00\x00")


def effect_name(body):
    """The effect's own name: the first name in its string table followed by
    zero padding rather than another name. A legacy record names its source
    file (X.fx) there; the suffix is dropped."""
    m = NAME_RE.search(body, 0x20)
    if not m:
        return None
    name = m.group(1).decode("ascii")
    return name[:-3] if name.endswith(".fx") else name


def effect_shaders(body, table):
    """The shaders an effect record names, in first-reference order, without repeats."""
    seen = []
    for i in range(len(body) - 15):
        rid = body[i:i + 16]
        if rid in table and rid not in seen:
            seen.append(rid)
    return seen


class Archive:
    def __init__(self, path, raw=None):
        self.path = path
        self.data = inflate(raw if raw is not None else open(path, "rb").read())
        self.recs = records(self.data)
        self.shaders = shader_table(self.data, self.recs)

    def effects(self):
        for off, cc, ver, size in self.recs:
            if cc == EFFECT_4CC:
                body = self.data[off:off + size]
                yield off, effect_name(body), body


def load(paths):
    archives = [Archive(p) for p in paths]
    shaders = {}
    for a in archives:
        shaders.update(a.shaders)
    return archives, shaders


def effect_rows(archives, shaders, name=None, want_hash=None):
    """(archive, offset, effect name, [(stage, hash, blob)]) for matching effects.
    Shaders are looked up across every archive given: an effect can name a
    shader stored in the other archive."""
    rows = []
    for a in archives:
        for off, ename, body in a.effects():
            if name is not None and ename != name:
                continue
            refs = []
            for rid in effect_shaders(body, shaders):
                blob = shaders[rid][1]
                refs.append((dxbc_stage(blob), "%016X" % fnv1a64(blob), blob))
            if want_hash is not None and want_hash not in [r[1] for r in refs]:
                continue
            rows.append((a, off, ename, refs))
    return rows


def disassemble(blob):
    import ctypes
    d3d = ctypes.WinDLL("d3dcompiler_47.dll")
    out = ctypes.c_void_p()
    if d3d.D3DDisassemble(ctypes.c_char_p(blob), ctypes.c_size_t(len(blob)), 0, None, ctypes.byref(out)) != 0:
        return None
    vt = ctypes.cast(out, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(out)
    n = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(out)
    text = ctypes.string_at(ptr, n).decode("latin1")
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(out)
    return text


def cmd_list(args):
    archives, shaders = load(args.arc)
    for a, off, name, refs in effect_rows(archives, shaders):
        print("%-60s %s +0x%X shaders=%d" % (name, os.path.basename(a.path), off, len(refs)))
    return 0


def cmd_find(args):
    want = args.hash.upper().removeprefix("0X").rjust(16, "0")
    archives, shaders = load(args.arc)
    rows = effect_rows(archives, shaders, want_hash=want)
    for a, off, name, refs in rows:
        print("%s  %s +0x%X" % (name, os.path.basename(a.path), off))
    if not rows:
        print("no effect names shader %s" % want)
        return 1
    return 0


def cmd_extract(args):
    archives, shaders = load(args.arc)
    rows = effect_rows(archives, shaders, name=args.effect)
    if not rows:
        print("no effect named %s" % args.effect)
        return 1
    plan = []
    for a, off, name, refs in rows:
        for stage, h, blob in refs:
            plan.append((os.path.join(args.out, "%s_%s.dxbc" % (stage, h)), blob))
        print("%s  %s +0x%X: %s" % (name, os.path.basename(a.path), off,
                                    " ".join("%s_%s" % (s, h) for s, h, _ in refs)))
    for path, blob in plan:
        if args.dry_run:
            print("would write %s (%d bytes)%s" % (path, len(blob), " and .txt" if args.disasm else ""))
            continue
        os.makedirs(args.out, exist_ok=True)
        with open(path, "wb") as f:
            f.write(blob)
        if args.disasm:
            text = disassemble(blob)
            with open(path[:-5] + ".txt", "w", encoding="utf-8") as f:
                f.write(text if text is not None else "D3DDisassemble failed\n")
        print("wrote %s" % path)
    return 0


def fake_dxbc(stage, payload):
    """A minimal container: header, one SHEX chunk whose version token names the stage."""
    version = (next(k for k, v in STAGES.items() if v == stage) << 16) | 0x50
    shex = b"SHEX" + struct.pack("<II", 8 + len(payload), version) + struct.pack("<I", 2 + len(payload) // 4) + payload
    total = 32 + 4 + len(shex)
    return b"DXBC" + b"\0" * 20 + struct.pack("<II", total, 1) + struct.pack("<I", 36) + shex


def fake_record(version, tag, cc, body):
    return struct.pack("<H", version) + tag + cc + struct.pack("<I", 12 + len(body)) + body


def fake_archive(version):
    idoff = shader_id_offset(version)
    shaders, ids = [], []
    for i, stage in enumerate(("vs", "ps", "ps")):
        rid = bytes([0xA0 + i]) * 16
        pre = b"\0" * (idoff - 12) + rid + b"\0" * 8
        shaders.append(fake_record(version, b"ah", SHADER_4CC, pre + fake_dxbc(stage, bytes([i]) * 8)))
        ids.append(rid)
    names = b"\0" * 20 + b"DiffuseSampler\0gCockpitGlassState\0"
    glass = fake_record(version, b"xf", EFFECT_4CC, names + b"GlassFx.fx\0\0\0\0" + b"\x23\x02\0\0" + ids[0] + ids[1] + ids[0])
    other = fake_record(version, b"xf", EFFECT_4CC, b"\0" * 20 + b"Other_Forward\0\0\0\0" + ids[2])
    stream = b"".join(shaders) + glass + other
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return b"\0" * HEADER_BYTES + c.compress(stream) + c.flush()


def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    for version in (2, 5):
        a = Archive("fake-v%d.arc" % version, raw=fake_archive(version))
        check(len(a.recs) == 5, "v%d: five records walked" % version)
        check(len(a.shaders) == 3, "v%d: three shaders indexed" % version)
        rows = effect_rows([a], a.shaders)
        check([r[2] for r in rows] == ["GlassFx", "Other_Forward"], "v%d: effect names, .fx dropped" % version)
        glass = rows[0][3]
        check([s for s, _, _ in glass] == ["vs", "ps"], "v%d: glass names vs then ps, once each" % version)
        ps2 = "%016X" % fnv1a64(a.shaders[bytes([0xA2]) * 16][1])
        found = effect_rows([a], a.shaders, want_hash=ps2)
        check([r[2] for r in found] == ["Other_Forward"], "v%d: find by hash" % version)

    # An effect may name a shader stored in another archive.
    a = Archive("a.arc", raw=fake_archive(5))
    b_raw = fake_archive(5)
    b = Archive("b.arc", raw=b_raw)
    b.shaders = {}
    check(effect_rows([b], b.shaders) and not effect_rows([b], b.shaders)[0][3], "cross: alone b names nothing")
    merged = dict(a.shaders)
    check(len(effect_rows([b], merged)[0][3]) == 2, "cross: shaders resolved across archives")

    # Pinned to log.cpp's basis: the standard basis would hash "a" to 0xAF63DC4C8601EC8C.
    check(fnv1a64(b"") == 1469598103934665603 and fnv1a64(b"a") == 0x44BD8AD473CD9906, "fnv1a64 uses log.cpp's basis")

    # --dry-run writes nothing at all.
    with tempfile.TemporaryDirectory() as tmp:
        arc = os.path.join(tmp, "fake.arc")
        with open(arc, "wb") as f:
            f.write(fake_archive(5))
        out = os.path.join(tmp, "out")
        before = sorted(os.listdir(tmp))
        rc = main(["extract", "--arc", arc, "--effect", "GlassFx", "--out", out, "--dry-run"])
        check(rc == 0 and sorted(os.listdir(tmp)) == before, "--dry-run wrote nothing")
        rc = main(["extract", "--arc", arc, "--effect", "GlassFx", "--out", out])
        check(rc == 0 and len(os.listdir(out)) == 2, "extract wrote two blobs")
        check(main(["extract", "--arc", arc, "--effect", "Missing", "--out", out]) == 1, "missing effect exits 1")

    try:
        inflate(b"\0" * 4)
        check(False, "short archive refused")
    except ValueError:
        pass

    # A truncated stream is refused, not read as a shorter archive.
    good = fake_archive(5)
    try:
        inflate(good[:-6])
        check(False, "truncated stream refused")
    except ValueError as exc:
        check("truncated" in str(exc), "truncated stream says so")
    check(inflate(good) is not None, "the whole stream still inflates")

    # A bad archive gets the tool's message and exit 2, not a traceback: a truncated stream, and a record the walker accepts that holds a DXBC header cut short.
    with tempfile.TemporaryDirectory() as tmp:
        cut = os.path.join(tmp, "cut.arc")
        with open(cut, "wb") as f:
            f.write(good[:-6])
        check(main(["list", "--arc", cut]) == 2, "a truncated archive exits 2")
        short = os.path.join(tmp, "short.arc")
        stub = fake_record(5, b"ah", SHADER_4CC, b"DXBC")
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        with open(short, "wb") as f:
            f.write(b"\0" * HEADER_BYTES + c.compress(stub) + c.flush())
        try:
            rc = main(["list", "--arc", short])
        except struct.error:
            rc = "struct.error escaped"
        check(rc == 2, "a record too short to read exits 2 (got %s)" % (rc,))

    for f in failures:
        print("FAIL: %s" % f)
    print("effects_arc self-test: %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--self-test", action="store_true")
    sub = p.add_subparsers(dest="cmd")
    for name in ("list", "find", "extract"):
        s = sub.add_parser(name)
        s.add_argument("--arc", action="append", required=True, help="an Effects*.arc; repeat for both")
        if name == "find":
            s.add_argument("--hash", required=True, help="FNV-1a-64 of the shader, as EDVR logs it")
        if name == "extract":
            s.add_argument("--effect", required=True)
            s.add_argument("--out", required=True)
            s.add_argument("--disasm", action="store_true")
            s.add_argument("--dry-run", action="store_true")
    args = p.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.cmd:
        p.print_usage()
        return 2
    try:
        return {"list": cmd_list, "find": cmd_find, "extract": cmd_extract}[args.cmd](args)
    except (OSError, ValueError, zlib.error, struct.error) as exc:
        print("effects_arc: %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
