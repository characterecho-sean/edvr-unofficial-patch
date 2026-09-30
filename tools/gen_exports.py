#!/usr/bin/env python3
"""Generate forwarding thunks for a proxy DLL from a real DLL's export table.

A proxy DLL has to export everything the original did or the process fails to
start. Hand-maintaining that list is how proxies break on OS updates, so we read
the export directory of the real binary and emit:

  <out>/edvr_thunks_<tag>.asm   MASM: a pointer array plus one jmp thunk per export
  <out>/edvr_<tag>.def          linker EXPORTS mapping real names -> thunk symbols
  <out>/edvr_exports_<tag>.inc  C string array of names, in pointer-array order

Exports named on the command line as --wrap are omitted from the thunks and
mapped in the .def to a C++ implementation of the same name, so we can intercept
a handful of entry points while everything else passes straight through.

Pure stdlib PE parsing: no dumpbin, no pefile, nothing to install.

--dry-run builds all three files in memory, prints the summary and writes
nothing (not the directory either). --self-test lays a small PE32+ out in the
temp folder -- named, forwarded, ordinal-only and empty exports -- and checks
the parse and every file the generator writes against it; build.bat runs it
before it trusts the tool with the system's real d3d11.dll.
"""

import argparse
import contextlib
import io
import os
import struct
import sys

IMAGE_DOS_SIGNATURE = 0x5A4D
IMAGE_NT_SIGNATURE = 0x00004550
PE32PLUS_MAGIC = 0x20B


class PeError(Exception):
    pass


class PeFile:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 0x40:
            raise PeError("file too small")
        if struct.unpack_from("<H", data, 0)[0] != IMAGE_DOS_SIGNATURE:
            raise PeError("not a PE file (bad MZ)")
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if struct.unpack_from("<I", data, e_lfanew)[0] != IMAGE_NT_SIGNATURE:
            raise PeError("not a PE file (bad PE signature)")

        coff = e_lfanew + 4
        (self.machine, self.num_sections, _, _, _, opt_size, _) = struct.unpack_from(
            "<HHIIIHH", data, coff
        )
        opt = coff + 20
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic != PE32PLUS_MAGIC:
            raise PeError("only PE32+ (x64) is supported; got magic 0x%X" % magic)

        # Data directory count sits at a fixed offset within the PE32+ optional
        # header; the export directory is entry 0.
        num_dirs = struct.unpack_from("<I", data, opt + 108)[0]
        if num_dirs < 1:
            raise PeError("no data directories")
        self.export_rva, self.export_size = struct.unpack_from("<II", data, opt + 112)

        self.sections = []
        sec = opt + opt_size
        for i in range(self.num_sections):
            base = sec + i * 40
            name = data[base : base + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, base + 8)
            self.sections.append((name, vaddr, vsize, rawptr, rawsize))

    def rva_to_offset(self, rva: int) -> int:
        for _, vaddr, vsize, rawptr, rawsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                delta = rva - vaddr
                if delta < rawsize:
                    return rawptr + delta
                raise PeError("RVA 0x%X is in uninitialised data" % rva)
        raise PeError("RVA 0x%X not in any section" % rva)

    def read_cstr(self, rva: int) -> str:
        off = self.rva_to_offset(rva)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")

    def exports(self):
        """Returns (dll_name, [(name_or_None, ordinal, is_forwarder)])."""
        if self.export_rva == 0:
            return ("", [])
        off = self.rva_to_offset(self.export_rva)
        # IMAGE_EXPORT_DIRECTORY: Characteristics, TimeDateStamp, MajorVersion,
        # MinorVersion, Name, Base, NumberOfFunctions, NumberOfNames,
        # AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals.
        (_char, _tds, _major, _minor, name_rva, ordinal_base, num_funcs,
         num_names, funcs_rva, names_rva,
         name_ords_rva) = struct.unpack_from("<IIHHIIIIIII", self.data, off)

        dll_name = self.read_cstr(name_rva) if name_rva else ""

        func_off = self.rva_to_offset(funcs_rva) if num_funcs else 0
        func_rvas = [
            struct.unpack_from("<I", self.data, func_off + 4 * i)[0]
            for i in range(num_funcs)
        ]

        ordinal_to_name = {}
        if num_names:
            names_off = self.rva_to_offset(names_rva)
            ords_off = self.rva_to_offset(name_ords_rva)
            for i in range(num_names):
                nrva = struct.unpack_from("<I", self.data, names_off + 4 * i)[0]
                idx = struct.unpack_from("<H", self.data, ords_off + 2 * i)[0]
                ordinal_to_name[idx] = self.read_cstr(nrva)

        out = []
        lo, hi = self.export_rva, self.export_rva + self.export_size
        for idx, rva in enumerate(func_rvas):
            if rva == 0:
                continue
            out.append(
                (ordinal_to_name.get(idx), ordinal_base + idx, lo <= rva < hi)
            )
        return (dll_name, out)


ASM_TEMPLATE_HEAD = """; Generated by tools/gen_exports.py from {source}
; {named} named exports, {ordinal_only} ordinal-only, {wrapped} wrapped in C++.
; Do not edit: regenerate with build.bat.
;
; Each thunk is a single indirect jmp through a slot the DLL fills at load time
; with GetProcAddress against the real module. Leaf, no prologue, no unwind info
; needed, and the tail jump leaves the caller's frame and arguments untouched
; whatever the target's signature turns out to be.

.DATA

PUBLIC edvr_realProcs_{tag}
edvr_realProcs_{tag} QWORD {count} DUP(0)

.CODE

; Substituted for any export the real module does not provide: returning zero
; beats jumping through a null slot.
PUBLIC edvr_unresolved_{tag}
edvr_unresolved_{tag} PROC
    xor eax, eax
    ret
edvr_unresolved_{tag} ENDP

"""

ASM_THUNK = """PUBLIC {sym}
{sym} PROC
    jmp QWORD PTR [edvr_realProcs_{tag} + {offset}]
{sym} ENDP

"""

ASM_TEMPLATE_TAIL = """END
"""


@contextlib.contextmanager
def buffered(outputs, path):
    """A write handle whose text is kept rather than written: main() writes the
    outputs once everything is built, unless --dry-run says nothing may be."""
    buf = io.StringIO()
    yield buf
    outputs.append((path, buf.getvalue()))


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    if "--self-test" in argv:
        return self_test()
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="path to the real DLL")
    ap.add_argument("--tag", required=True, help="short identifier, e.g. d3d11")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--dry-run", action="store_true",
                    help="build the outputs, print the summary, write nothing")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against a fixture PE and exit")
    ap.add_argument(
        "--wrap",
        action="append",
        default=[],
        help="export implemented in C++ instead of thunked (repeatable)",
    )
    ap.add_argument(
        "--extra-export",
        action="append",
        default=[],
        help="additional symbol to export, implemented in C++ (repeatable). Used for "
             "build-check hooks the real DLL does not have; additive, so nothing that "
             "imports the real exports is affected",
    )
    args = ap.parse_args(argv)

    try:
        with open(args.source, "rb") as f:
            pe = PeFile(f.read())
        dll_name, exports = pe.exports()
    except (PeError, OSError) as exc:
        print("gen_exports: %s: %s" % (args.source, exc), file=sys.stderr)
        return 1

    wrapped = set(args.wrap)
    named = [(n, o, f) for (n, o, f) in exports if n]

    # A source that exports edvr_* symbols IS an EDVR proxy, not the runtime
    # it stands in for. Generating from it builds a proxy of our own proxy:
    # the thunk table inherits our extra exports as if they were the real
    # DLL's, the .def then names them twice, and the linker's "first
    # specification wins" quietly aliases a C++ implementation to a
    # forwarding thunk -- measured 2026-08-18, when a rebuild after an
    # install picked up an installed proxy as its source and a selftest
    # export answered with the do-nothing stub. The fix is the source, not
    # the symptom: pass --source pointing at the real, original DLL.
    ours = sorted(n for (n, _, _) in named if n.startswith("edvr_"))
    if ours:
        print(
            "gen_exports: ERROR: %s exports %s -- that is an EDVR proxy, not "
            "the real DLL. Pass --source pointing at the true original DLL."
            % (args.source, ", ".join(ours)),
            file=sys.stderr,
        )
        return 1

    ordinal_only = [(n, o, f) for (n, o, f) in exports if not n]
    thunked = [(n, o, f) for (n, o, f) in named if n not in wrapped]

    missing = wrapped - {n for (n, _, _) in named}
    if missing:
        print(
            "gen_exports: warning: --wrap names not exported by %s: %s"
            % (dll_name or args.source, ", ".join(sorted(missing))),
            file=sys.stderr,
        )

    tag = args.tag
    outputs = []   # (path, text), written below unless this is a dry run

    asm_path = os.path.join(args.out, "edvr_thunks_%s.asm" % tag)
    with buffered(outputs, asm_path) as f:
        f.write(
            ASM_TEMPLATE_HEAD.format(
                source=os.path.basename(args.source),
                named=len(named),
                ordinal_only=len(ordinal_only),
                wrapped=len(wrapped) - len(missing),
                tag=tag,
                count=max(len(thunked), 1),
            )
        )
        template = ASM_THUNK
        for i, (name, _ordinal, _fwd) in enumerate(thunked):
            f.write(
                template.format(sym="edvr_%s_thunk_%d" % (tag, i), tag=tag, offset=i * 8)
            )
        f.write(ASM_TEMPLATE_TAIL)

    def_path = os.path.join(args.out, "edvr_%s.def" % tag)
    with buffered(outputs, def_path) as f:
        f.write("; Generated by tools/gen_exports.py from %s\n" % os.path.basename(args.source))
        f.write("EXPORTS\n")
        for i, (name, _ordinal, _fwd) in enumerate(thunked):
            f.write("    %s = edvr_%s_thunk_%d\n" % (name, tag, i))
        # Wrapped exports map to an edvr_impl_-prefixed C++ symbol so our
        # definition can never collide with a declaration in a system header.
        for name in sorted(wrapped - missing):
            f.write("    %s = edvr_impl_%s\n" % (name, name))
        for extra in args.extra_export:
            # Additive only. The proxy must export everything the original did;
            # exporting one more is inert, because nothing imports by that name
            # except our own build check.
            f.write("    %s\n" % extra)
        for _name, ordinal, _fwd in ordinal_only:
            # Kept so the ordinal space stays intact for anything importing by
            # ordinal; resolved at runtime like the rest.
            f.write("    ; ordinal-only export @%d not forwarded\n" % ordinal)

    inc_path = os.path.join(args.out, "edvr_exports_%s.inc" % tag)
    with buffered(outputs, inc_path) as f:
        f.write("// Generated by tools/gen_exports.py from %s\n"
                % os.path.basename(args.source))
        for name, _ordinal, _fwd in thunked:
            f.write('    "%s",\n' % name)

    if not args.dry_run:
        os.makedirs(args.out, exist_ok=True)
        for path, text in outputs:
            with open(path, "w", newline="\r\n") as f:
                f.write(text)

    forwarders = sum(1 for (_n, _o, fwd) in thunked if fwd)
    print(
        "gen_exports: %s -> %d thunks (%d forwarders), %d wrapped, %d ordinal-only"
        % (os.path.basename(args.source), len(thunked), forwarders,
           len(wrapped) - len(missing), len(ordinal_only))
    )
    if args.dry_run:
        print("gen_exports: dry run: would write %s; wrote nothing"
              % ", ".join(os.path.basename(p) for p, _ in outputs))
    if ordinal_only:
        print(
            "gen_exports: warning: %d ordinal-only exports were NOT reproduced; "
            "if the host imports any of them by ordinal the proxy will fail to load"
            % len(ordinal_only),
            file=sys.stderr,
        )
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def _fixture_pe(dll_name, funcs, magic=PE32PLUS_MAGIC, export_dir=True):
    """A minimal PE32+ image with one section holding an export directory.

    funcs is one (name or None, kind) per function slot, in ordinal order
    starting at 1: kind "code" is an RVA outside the directory, "forward" one
    inside it (at a forwarder string) and "empty" is RVA 0, a hole in the
    table. The name table is written sorted, as the format wants."""
    rva, raw, opt_size = 0x1000, 0x200, 112 + 16 * 8
    named = sorted((n, i) for i, (n, _kind) in enumerate(funcs) if n)
    body = bytearray(40)   # the directory itself, filled in last

    def add(data):
        at = len(body)
        body.extend(data)
        return at

    name_at = add(dll_name.encode() + b"\0")
    funcs_at = add(bytes(4 * len(funcs)))
    names_at = add(bytes(4 * len(named)))
    ords_at = add(bytes(2 * len(named)))
    for k, (n, i) in enumerate(named):
        struct.pack_into("<I", body, names_at + 4 * k, rva + add(n.encode() + b"\0"))
        struct.pack_into("<H", body, ords_at + 2 * k, i)
    for i, (_n, kind) in enumerate(funcs):
        value = {"code": 0x5000 + 0x10 * i, "empty": 0}.get(kind)
        if kind == "forward":
            value = rva + add(b"KERNEL32.Sleep\0")
        struct.pack_into("<I", body, funcs_at + 4 * i, value)
    struct.pack_into("<IIHHIIIIIII", body, 0, 0, 0, 0, 0, rva + name_at, 1,
                     len(funcs), len(named), rva + funcs_at, rva + names_at, rva + ords_at)

    head = bytearray(raw)
    struct.pack_into("<H", head, 0, IMAGE_DOS_SIGNATURE)
    struct.pack_into("<I", head, 0x3C, 0x80)
    struct.pack_into("<I", head, 0x80, IMAGE_NT_SIGNATURE)
    struct.pack_into("<HHIIIHH", head, 0x84, 0x8664, 1, 0, 0, 0, opt_size, 0x2022)
    opt = 0x98
    struct.pack_into("<H", head, opt, magic)
    struct.pack_into("<I", head, opt + 108, 16)
    struct.pack_into("<II", head, opt + 112, rva if export_dir else 0,
                     len(body) if export_dir else 0)
    sec = opt + opt_size
    head[sec:sec + 8] = b".edata\0\0"
    struct.pack_into("<IIII", head, sec + 8, len(body), rva, len(body), raw)
    return bytes(head) + bytes(body)


def self_test():
    """The PE parse and every file the generator writes, against a fixture image
    in the temp folder: named, forwarded, ordinal-only and empty export slots,
    wrapped and extra exports, and each way a source can be refused."""
    import shutil
    import tempfile

    failures = []

    def expect(name, condition, what=""):
        if not condition:
            failures.append("%s: %s" % (name, what))

    funcs = [("Alpha", "code"), ("Beta", "forward"), (None, "code"),
             (None, "empty"), ("Gamma", "code")]
    good = _fixture_pe("fake.dll", funcs)

    # The parse: names, ordinals from the table's base, the forwarder found by
    # its RVA landing inside the directory, the ordinal-only slot kept and the
    # empty one skipped.
    name = "parse"
    dll, exports = PeFile(good).exports()
    expect(name, dll == "fake.dll", repr(dll))
    expect(name, exports == [("Alpha", 1, False), ("Beta", 2, True), (None, 3, False),
                             ("Gamma", 5, False)], repr(exports))
    expect(name, PeFile(_fixture_pe("x.dll", [], export_dir=False)).exports() == ("", []),
           "a PE with no export directory")

    # Each way an image is refused, by message.
    bad_signature = bytearray(good)
    struct.pack_into("<I", bad_signature, 0x80, 0)
    lost_section = bytearray(good)
    struct.pack_into("<II", lost_section, 0x98 + 112, 0x9000, 0x100)
    for name, data, needle in (
            ("too-small", b"MZ", "file too small"),
            ("bad-mz", bytes(0x100), "bad MZ"),
            ("bad-pe-signature", bytes(bad_signature), "bad PE signature"),
            ("pe32", _fixture_pe("x.dll", funcs, magic=0x10B), "only PE32+"),
            ("export-rva-outside-sections", bytes(lost_section), "not in any section")):
        try:
            PeFile(data).exports()
            failures.append("%s: was not refused" % name)
        except PeError as exc:
            expect(name, needle in str(exc), str(exc))

    base = tempfile.mkdtemp(prefix="edvr-gen-exports-")

    def write(leaf, data):
        path = os.path.join(base, leaf)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def run(*argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def files(path):
        return sorted(os.listdir(path)) if os.path.isdir(path) else None

    def text_of(path):
        with open(path, "r", newline="") as f:
            return f.read()

    try:
        source = write("fake.dll", good)
        args = ["--source", source, "--tag", "t", "--wrap", "Gamma", "--wrap", "Nope",
                "--extra-export", "edvr_selftest_x"]

        # A full run: the summary, the two warnings, and the three files.
        name = "run"
        out = os.path.join(base, "gen")
        code, said, err = run(*args, "--out", out)
        expect(name, code == 0, "exit %r\n%s%s" % (code, said, err))
        expect(name, "gen_exports: fake.dll -> 2 thunks (1 forwarders), 1 wrapped, 1 ordinal-only" in said, said)
        expect(name, "warning: --wrap names not exported by fake.dll: Nope" in err, err)
        expect(name, "warning: 1 ordinal-only exports were NOT reproduced" in err, err)
        expect(name, files(out) == ["edvr_exports_t.inc", "edvr_t.def", "edvr_thunks_t.asm"], repr(files(out)))
        if files(out) == ["edvr_exports_t.inc", "edvr_t.def", "edvr_thunks_t.asm"]:
            # thunks in table order, the wrapped export mapped to its C++ name,
            # the extra export bare, the ordinal-only slot left as a comment.
            expect(name, text_of(os.path.join(out, "edvr_t.def")) ==
                   "; Generated by tools/gen_exports.py from fake.dll\r\n"
                   "EXPORTS\r\n"
                   "    Alpha = edvr_t_thunk_0\r\n"
                   "    Beta = edvr_t_thunk_1\r\n"
                   "    Gamma = edvr_impl_Gamma\r\n"
                   "    edvr_selftest_x\r\n"
                   "    ; ordinal-only export @3 not forwarded\r\n",
                   repr(text_of(os.path.join(out, "edvr_t.def"))))
            expect(name, text_of(os.path.join(out, "edvr_exports_t.inc")) ==
                   '// Generated by tools/gen_exports.py from fake.dll\r\n'
                   '    "Alpha",\r\n    "Beta",\r\n',
                   repr(text_of(os.path.join(out, "edvr_exports_t.inc"))))
            asm = text_of(os.path.join(out, "edvr_thunks_t.asm"))
            for needle in ("edvr_realProcs_t QWORD 2 DUP(0)",
                           "; 3 named exports, 1 ordinal-only, 1 wrapped in C++.",
                           "edvr_t_thunk_0 PROC\r\n    jmp QWORD PTR [edvr_realProcs_t + 0]\r\n",
                           "edvr_t_thunk_1 PROC\r\n    jmp QWORD PTR [edvr_realProcs_t + 8]\r\n",
                           "edvr_unresolved_t PROC"):
                expect(name, needle in asm, "%r not in the asm" % needle)
            expect(name, "edvr_t_thunk_2" not in asm and asm.endswith("END\r\n"), "the asm's tail")
            expect(name, "\n" not in asm.replace("\r\n", ""), "a bare LF in the asm")
            # The same inputs give the same bytes.
            again = os.path.join(base, "gen-again")
            run(*args, "--out", again)
            expect(name, all(text_of(os.path.join(out, n)) == text_of(os.path.join(again, n))
                             for n in files(out)), "a second run differs")

        # Everything wrapped leaves no thunks, and the pointer array still has a
        # slot (a zero-length MASM array is an error).
        name = "all-wrapped"
        out = os.path.join(base, "gen-wrapped")
        code, said, _ = run("--source", source, "--tag", "t", "--out", out,
                            "--wrap", "Gamma", "--wrap", "Alpha", "--wrap", "Beta")
        expect(name, code == 0 and "0 thunks (0 forwarders), 3 wrapped" in said, "exit %r: %s" % (code, said))
        expect(name, "QWORD 1 DUP(0)" in text_of(os.path.join(out, "edvr_thunks_t.asm")), "the pointer array")
        expect(name, "    Alpha = edvr_impl_Alpha\r\n    Beta = edvr_impl_Beta\r\n    Gamma = edvr_impl_Gamma\r\n"
               in text_of(os.path.join(out, "edvr_t.def")), "the wrapped names")

        # --dry-run builds and reports, and writes nothing: not a file, not the
        # directory; an existing directory stays empty.
        name = "dry-run"
        out = os.path.join(base, "gen-dry")
        code, said, err = run(*args, "--out", out, "--dry-run")
        expect(name, code == 0 and "would write edvr_thunks_t.asm, edvr_t.def, edvr_exports_t.inc; wrote nothing" in said,
               "exit %r: %s" % (code, said))
        expect(name, files(out) is None, "--dry-run made %s" % out)
        os.makedirs(out)
        run(*args, "--out", out, "--dry-run")
        expect(name, files(out) == [], "--dry-run wrote %r" % files(out))

        # A source that is our own proxy is refused, naming the exports that
        # gave it away: generating from it aliases a C++ export to a thunk.
        name = "own-proxy"
        proxy = write("proxy.dll", _fixture_pe("proxy.dll", [("edvr_selftest_hooks", "code"), ("Real", "code")]))
        out = os.path.join(base, "gen-proxy")
        code, said, err = run("--source", proxy, "--tag", "t", "--out", out)
        expect(name, code == 1 and "exports edvr_selftest_hooks -- that is an EDVR proxy" in err, "exit %r: %s" % (code, err))
        expect(name, files(out) is None, "an output was written for a refused source")

        # A source that is not a PE, and one that is not there.
        name = "bad-source"
        junk = write("junk.dll", b"this is not a PE file at all" * 4)
        out = os.path.join(base, "gen-junk")
        code, said, err = run("--source", junk, "--tag", "t", "--out", out)
        expect(name, code == 1 and "not a PE file (bad MZ)" in err, "exit %r: %s" % (code, err))
        code, said, err = run("--source", os.path.join(base, "nothing.dll"), "--tag", "t", "--out", out)
        expect(name, code == 1 and "nothing.dll" in err, "exit %r: %s" % (code, err))
        expect(name, files(out) is None, "an output was written for a bad source")
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("gen_exports: self-test FAILED")
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("gen_exports: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
