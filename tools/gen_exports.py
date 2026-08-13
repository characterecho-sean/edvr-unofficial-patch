#!/usr/bin/env python3
# GENERATED from tools/gen_exports.py in the private edvr repo -- do not edit here.
# Edit there, then: python tools/sync_common.py --write   [body-sha256 e59e047d6fb9daa9]
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
"""

import argparse
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

# --- lazy variant -----------------------------------------------------------
#
# Same thunk, plus a check that the table has been filled, and a slow path that
# fills it on the first call.
#
# This exists so a proxy does not have to load the real DLL from DllMain.
# LoadLibrary of a module nothing else has mapped runs that module's own DllMain
# under the loader lock, re-entrantly, which Windows does not support -- it is
# what crashed the game for a user running ReShade 6.8.0 with EDHM. The d3d11
# side was rebuilt to defer for that reason; the openvr side could not, because
# its exports are bare `jmp [slot]` and something has to fill the slot before
# the first call. This is that something.
#
# The first call happens on an ordinary call stack with no loader lock held,
# which is the whole point.

ASM_LAZY_HEAD = """.DATA

; Non-zero once the export table has been filled. Written by C, read by every
; thunk below. Byte-sized so the check is one compare against memory.
PUBLIC edvr_ready_{tag}
edvr_ready_{tag} BYTE 0

.CODE

EXTERN edvr_lazyInit_{tag}:PROC

; Calls the C initialiser without disturbing anything the real function will
; expect to find.
;
; A thunk is transparent: it must not change the arguments, and on x64 the first
; four live in rcx/rdx/r8/r9 and xmm0-xmm3, all of which a call is free to
; clobber. So they are saved around it. Arguments five and up sit above the
; return address and are untouched, because rsp is restored exactly.
;
; Stack: a thunk entry has rsp = 8 (mod 16). The call into here makes it 0, and
; 128 is a multiple of 16, so rsp stays 0 (mod 16) -- correct for the call, and
; correct for the 16-byte movaps slots at rsp+64 and above. 32 bytes at the
; bottom are the shadow space the callee is entitled to.
; PROC FRAME, and the prologue annotated, so this function has unwind info.
;
; It did not, and that is not cosmetic. x64 exception handling walks the stack
; with the .pdata/.xdata tables; a function with no entry is assumed to be a leaf
; and its return address is read from [rsp]. This one moves rsp by 128 and then
; CALLS, so an unwinder would have taken the return address out of the middle of
; the shadow space below -- and every SEH handler above, including the game's,
; would fail to run. A fault in the initialiser became an uncatchable process
; kill rather than the degrade-to-vanilla this project promises.
;
; Confirmed by dumpbin: without these, thunks.obj contributes no .pdata at all.
edvr_lazyShim_{tag} PROC FRAME
    sub     rsp, 128
    .ALLOCSTACK 128
    .ENDPROLOG
    mov     QWORD PTR [rsp+32], rcx
    mov     QWORD PTR [rsp+40], rdx
    mov     QWORD PTR [rsp+48], r8
    mov     QWORD PTR [rsp+56], r9
    movaps  XMMWORD PTR [rsp+64], xmm0
    movaps  XMMWORD PTR [rsp+80], xmm1
    movaps  XMMWORD PTR [rsp+96], xmm2
    movaps  XMMWORD PTR [rsp+112], xmm3
    call    edvr_lazyInit_{tag}
    movaps  xmm3, XMMWORD PTR [rsp+112]
    movaps  xmm2, XMMWORD PTR [rsp+96]
    movaps  xmm1, XMMWORD PTR [rsp+80]
    movaps  xmm0, XMMWORD PTR [rsp+64]
    mov     r9,  QWORD PTR [rsp+56]
    mov     r8,  QWORD PTR [rsp+48]
    mov     rdx, QWORD PTR [rsp+40]
    mov     rcx, QWORD PTR [rsp+32]
    add     rsp, 128
    ret
edvr_lazyShim_{tag} ENDP

"""

# The thunk itself needs no unwind info: it never moves rsp, so unwinding its
# frame as a leaf reads the return address from the right place. Only the shim,
# which allocates, needs a real entry.
ASM_LAZY_THUNK = """PUBLIC {sym}
{sym} PROC
    cmp BYTE PTR [edvr_ready_{tag}], 0
    jne @F
    call edvr_lazyShim_{tag}
@@:
    jmp QWORD PTR [edvr_realProcs_{tag} + {offset}]
{sym} ENDP

"""

ASM_TEMPLATE_TAIL = """END
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="path to the real DLL")
    ap.add_argument("--tag", required=True, help="short identifier, e.g. d3d11")
    ap.add_argument("--out", required=True, help="output directory")
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
    ap.add_argument(
        "--lazy",
        action="store_true",
        help="fill the export table on the first call instead of from DllMain, "
             "so the real module is never loaded under the loader lock",
    )
    args = ap.parse_args()

    try:
        with open(args.source, "rb") as f:
            pe = PeFile(f.read())
        dll_name, exports = pe.exports()
    except (PeError, OSError) as exc:
        print("gen_exports: %s: %s" % (args.source, exc), file=sys.stderr)
        return 1

    wrapped = set(args.wrap)
    named = [(n, o, f) for (n, o, f) in exports if n]
    ordinal_only = [(n, o, f) for (n, o, f) in exports if not n]
    thunked = [(n, o, f) for (n, o, f) in named if n not in wrapped]

    missing = wrapped - {n for (n, _, _) in named}
    if missing:
        print(
            "gen_exports: warning: --wrap names not exported by %s: %s"
            % (dll_name or args.source, ", ".join(sorted(missing))),
            file=sys.stderr,
        )

    os.makedirs(args.out, exist_ok=True)
    tag = args.tag

    asm_path = os.path.join(args.out, "edvr_thunks_%s.asm" % tag)
    with open(asm_path, "w", newline="\r\n") as f:
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
        if args.lazy:
            f.write(ASM_LAZY_HEAD.format(tag=tag))
        template = ASM_LAZY_THUNK if args.lazy else ASM_THUNK
        for i, (name, _ordinal, _fwd) in enumerate(thunked):
            f.write(
                template.format(sym="edvr_%s_thunk_%d" % (tag, i), tag=tag, offset=i * 8)
            )
        f.write(ASM_TEMPLATE_TAIL)

    def_path = os.path.join(args.out, "edvr_%s.def" % tag)
    with open(def_path, "w", newline="\r\n") as f:
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
    with open(inc_path, "w", newline="\r\n") as f:
        f.write("// Generated by tools/gen_exports.py from %s\n"
                % os.path.basename(args.source))
        for name, _ordinal, _fwd in thunked:
            f.write('    "%s",\n' % name)

    forwarders = sum(1 for (_n, _o, fwd) in thunked if fwd)
    print(
        "gen_exports: %s -> %d thunks (%d forwarders), %d wrapped, %d ordinal-only"
        % (os.path.basename(args.source), len(thunked), forwarders,
           len(wrapped) - len(missing), len(ordinal_only))
    )
    if ordinal_only:
        print(
            "gen_exports: warning: %d ordinal-only exports were NOT reproduced; "
            "if the host imports any of them by ordinal the proxy will fail to load"
            % len(ordinal_only),
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
