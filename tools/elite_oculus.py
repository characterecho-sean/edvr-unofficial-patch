#!/usr/bin/env python3
"""Strict, read-only validation for the qualified Elite LibOVR profile.

The profile is deliberately revision-specific.  It proves the executable
identity, the regular KERNEL32 LoadLibraryW import, the SDK call/return pair,
and the selector's LibOVR-to-OpenVR fallback before an installer or hook can
enable the route.  No executable is launched or modified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
import struct
import sys

try:
    from openxr_pe import Image, PEError
except ImportError:  # Allows a repository-root import in unit tests.
    from tools.openxr_pe import Image, PEError


GAME_SHA256 = "ed5a8f8cd10f9a80d3bda5badd7ba1d4fcfc15996c8c7e018852cb170e264b63"
PROFILE_ID = "elite-odyssey-ed5a8f8c-libovr-r1"
IMAGE_BASE = 0x140000000
LOAD_LIBRARY_IAT_RVA = 0x4DB62E0
LOADER_CALL_RVA = 0x4E7156
LOADER_RETURN_RVA = 0x4E715C

_BYTE_CHECKS = {
    # Selector writes its ordered backend kinds and exits on success.
    0x8D540B: bytes.fromhex("c744242801000000"),
    0x8D5418: bytes.fromhex("c744242c02000000"),
    0x8D5420: bytes.fromhex("c744243003000000"),
    0x8D5428: bytes.fromhex("84c07545"),
    0x8D543C: bytes.fromhex("ffc74883c60483ff0372e1"),
    # Backend factory's virtual initialize call and the LibOVR failure path.
    0x4E4DF0: bytes.fromhex("ff5008"),
    0x4E471C: bytes.fromhex("84c07431"),
    0x4E4751: bytes.fromhex("32c0"),
    0x4E71AE: bytes.fromhex("4885c0751db847f4ffff"),
    0x4E82F6: bytes.fromhex("c70347f4ffff"),
    0x4E875B: bytes.fromhex("4885c0740bffd0"),
}

_FINGERPRINTS = (
    (0x8D53A0, 0xF3, 0xFF6BF29E3788FC3C),
    (0x4E4B40, 0x2C7, 0x60262CD96F885F66),
    (0x4E46D0, 0x23C, 0xE72D36967A10BFA8),
    (0x4E4910, 0x230, 0x7B3B4C4070FB007A),
    (0x4E85D0, 0x155, 0x1BEB5B69C7BF932E),
    (0x4E7170, 0x984, 0x66B422313A80E9AE),
    (0x4E6DE0, 0x390, 0xB9F0AF721320C54D),
    (0x4E0F00, 0xC7, 0xAEE1539FB5BE2968),
    (0x4E82D0, 0x3A, 0xB69AB25FEC7A1F6A),
    (0x4E5D40, 0x65, 0x9F6C59F05E0A6390),
    (0x4E8750, 0x20, 0x72BD38A6D3FB9681),
)


@dataclass(frozen=True)
class EliteOculusProfile:
    profile_id: str
    game_sha256: str
    image_base: int
    load_library_iat_rva: int
    loader_call_rva: int
    loader_return_rva: int
    selector_rva: int = 0x8D53A0
    factory_rva: int = 0x4E4B40

    def as_dict(self):
        return {
            "profile_id": self.profile_id,
            "game_sha256": self.game_sha256,
            "image_base": hex(self.image_base),
            "load_library_iat_rva": hex(self.load_library_iat_rva),
            "loader_call_rva": hex(self.loader_call_rva),
            "loader_return_rva": hex(self.loader_return_rva),
            "selector_rva": hex(self.selector_rva),
            "factory_rva": hex(self.factory_rva),
        }


def _fail(message):
    raise ValueError("Elite Oculus profile rejected: " + message)


def _rva_bytes(im, rva, size):
    try:
        offset, available = im.mapping(rva, size)
    except PEError as exc:
        _fail("RVA " + hex(rva) + " is not mapped: " + str(exc))
    if available < size or offset < 0 or offset + size > len(im.data):
        _fail("RVA " + hex(rva) + " exceeds the file")
    return im.data[offset:offset + size]


def _ascii_equal(a, b):
    return a.replace("\\", "/").rsplit("/", 1)[-1].lower() == b.lower()


def _all_imports(im):
    """Yield (dll, function, IAT RVA), with strict descriptor/thunk bounds."""
    for directory, width, kind in ((1, 20, "import"), (13, 32, "delay")):
        rva, size = im.directory(directory)
        if not rva:
            continue
        if size < width:
            _fail(kind + " import directory is too small")
        terminated = False
        for off in range(0, size - width + 1, width):
            desc = rva + off
            values = [im.number(desc + n, 4) for n in range(0, width, 4)]
            if not any(values):
                terminated = True
                break
            if kind == "delay":
                if values[0] != 1:
                    _fail("delay import VA attributes unsupported")
                name_rva, first, lookup = values[1], values[3], values[4]
            else:
                _original, _timestamp, _forwarder, name_rva, first = values
                lookup = _original or first
            if not name_rva or not first or not lookup:
                _fail(kind + " import descriptor has no name or thunk")
            dll = im.string(name_rva)
            im.mapping(first, 8)
            im.mapping(lookup, 8)
            for index in range(65536):
                value = im.number(lookup + index * 8, 8)
                if not value:
                    break
                slot = first + index * 8
                im.mapping(slot, 8)
                if value & (1 << 63):
                    if value & ~((1 << 63) | 0xFFFF):
                        _fail("import ordinal has high bits")
                    yield dll, "#" + str(value & 0xFFFF), slot, kind
                    continue
                im.mapping(value, 2)
                yield dll, im.string(value + 2), slot, kind
            else:
                _fail("import thunk limit exceeded")
        if not terminated:
            _fail(kind + " import descriptors lack a bounded terminator")


def _text_section(im, rva, size=1):
    # Image intentionally keeps a compact section tuple, so read the section
    # characteristics here.  This remains bounded by Image's validated PE
    # header and table, and makes a non-code section unable to satisfy proof.
    nt = int.from_bytes(im.data[0x3C:0x40], "little")
    optional = nt + 24
    optional_size = int.from_bytes(im.data[nt + 20:nt + 22], "little")
    count = int.from_bytes(im.data[nt + 6:nt + 8], "little")
    table = optional + optional_size
    for index in range(count):
        off = table + index * 40
        start = int.from_bytes(im.data[off + 12:off + 16], "little")
        virtual = int.from_bytes(im.data[off + 8:off + 12], "little")
        raw_size = int.from_bytes(im.data[off + 16:off + 20], "little")
        span = max(virtual, raw_size)
        characteristics = int.from_bytes(im.data[off + 36:off + 40], "little")
        if (characteristics & 0x20000000) and start <= rva and rva + size <= start + span:
            return True
    return False


def _rel32_target(im, rva, expected, label):
    code = _rva_bytes(im, rva, 5)
    if code[0] != 0xE8:
        _fail(label + " is not a direct call")
    displacement = struct.unpack_from("<i", code, 1)[0]
    target = rva + 5 + displacement
    if target != expected:
        _fail(label + " targets " + hex(target) + ", expected " + hex(expected))


def _fnv1a(im, rva, size):
    return _fnv_bytes(_rva_bytes(im, rva, size))


def _fnv_bytes(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def _indirect_iat_call(im, rva, expected_iat):
    code = _rva_bytes(im, rva, 6)
    if code[:2] != b"\xff\x15":
        _fail("SDK loader call is not an RIP-relative IAT call")
    displacement = struct.unpack_from("<i", code, 2)[0]
    target = rva + 6 + displacement
    if target != expected_iat:
        _fail("SDK loader call resolves to " + hex(target) + ", expected " + hex(expected_iat))


def _relocated_pointer(im, rva, target, label):
    actual = int.from_bytes(_rva_bytes(im, rva, 8), "little")
    if actual != IMAGE_BASE + target:
        _fail(label + " does not point at the qualified function")


def _validate_image(im, digest, strict_identity=True):
    # Image has already validated PE32+, section overlap and all basic RVA
    # spans.  Verify the identity fields which are relevant to the profile.
    nt = int.from_bytes(im.data[0x3C:0x40], "little")
    optional = nt + 24
    machine = int.from_bytes(im.data[nt + 4:nt + 6], "little")
    timestamp = int.from_bytes(im.data[nt + 8:nt + 12], "little")
    image_size = int.from_bytes(im.data[optional + 56:optional + 60], "little")
    headers_size = int.from_bytes(im.data[optional + 60:optional + 64], "little")
    section_count = int.from_bytes(im.data[nt + 6:nt + 8], "little")
    if strict_identity and (machine, timestamp, image_size, headers_size, section_count) != (
            0x8664, 0x6ADA8048, 0x640C000, 0x400, 7):
        _fail("PE header identity differs from the qualified revision")
    image_base = int.from_bytes(im.data[optional + 24:optional + 32], "little")
    if image_base != IMAGE_BASE:
        _fail("image base is " + hex(image_base) + ", expected " + hex(IMAGE_BASE))

    imports = list(_all_imports(im))
    load_slots = [(dll, slot) for dll, name, slot, kind in imports
                  if kind == "import" and _ascii_equal(dll, "KERNEL32.dll")
                  and name == "LoadLibraryW"]
    if len(load_slots) != 1 or load_slots[0][1] != LOAD_LIBRARY_IAT_RVA:
        _fail("regular KERNEL32 LoadLibraryW IAT does not match the qualified slot")
    if any("libovr" in dll.lower() for dll, _name, _slot, _kind in imports):
        _fail("executable unexpectedly has a LibOVR import")

    if strict_identity:
        for rva, size, expected in _FINGERPRINTS:
            if not _text_section(im, rva, size):
                _fail("fingerprint block " + hex(rva) + " is outside executable code")
            if _fnv1a(im, rva, size) != expected:
                _fail("fingerprint differs at " + hex(rva))

    # Every proof point is a mapped code range.  The file parser's section
    # table and mapping checks ensure malformed inputs fail before slicing.
    for rva, expected in _BYTE_CHECKS.items():
        if not _text_section(im, rva, len(expected)):
            _fail("proof point " + hex(rva) + " is outside an executable section")
        actual = _rva_bytes(im, rva, len(expected))
        if actual != expected:
            _fail("proof bytes differ at " + hex(rva))
    _rel32_target(im, 0x8D5437, 0x4E4B40, "selector factory call")
    _rel32_target(im, 0x4E4706, 0x4E85D0, "Oculus SDK init call")
    _rel32_target(im, 0x4E86AF, 0x4E7170, "SDK shim init call")
    _rel32_target(im, 0x4E71A2, 0x4E6DE0, "SDK search loader call")
    _rel32_target(im, 0x4E5D93, 0x4E8750, "SDK cleanup call")
    _relocated_pointer(im, 0x4E27438, 0x4E46D0, "LibOVR vtable init")
    _relocated_pointer(im, 0x4E27440, 0x4E5D40, "LibOVR vtable shutdown")
    _relocated_pointer(im, 0x4E27600, 0x4E4910, "OpenVR vtable init")
    _indirect_iat_call(im, LOADER_CALL_RVA, LOAD_LIBRARY_IAT_RVA)
    if _rva_bytes(im, LOADER_CALL_RVA + 6, 6) != bytes.fromhex("488bcb488bf0"):
        _fail("SDK loader call has an unexpected caller continuation")
    if not _text_section(im, LOADER_RETURN_RVA, 1):
        _fail("SDK loader return is outside executable code")
    return EliteOculusProfile(PROFILE_ID, digest, image_base,
                              LOAD_LIBRARY_IAT_RVA, LOADER_CALL_RVA,
                              LOADER_RETURN_RVA)


def validate_elite_oculus(path):
    """Return the qualified profile or raise ValueError without side effects."""
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ValueError("Elite Oculus profile cannot read executable: " + str(exc)) from exc
    digest = hashlib.sha256(data).hexdigest()
    if digest != GAME_SHA256:
        _fail("SHA-256 " + digest + " is not a known executable revision")
    try:
        image = Image(data)
        return _validate_image(image, digest)
    except PEError as exc:
        _fail(str(exc))


def version_strings(path):
    """Best-effort version-resource strings for an executable.

    Returns whichever of ProductName, FileDescription and FileVersion it can
    read, and {} when the file cannot be read or carries no usable resource.
    This tells the user WHICH Elite Dangerous they pointed at; it never
    qualifies an executable -- the gate is the pinned digest above.
    """
    try:
        data = Path(path).read_bytes()
        image = Image(data)
        rva, size = image.directory(2)  # the resource table
        if not rva:
            return {}
        base, _avail = image.mapping(rva, size)
        found = _version_resource(image, base, size)
        if not found:
            return {}
        off, rsize = found
        strings = _version_block_strings(data, off, rsize)
        return {k: strings[k] for k in ("ProductName", "FileDescription", "FileVersion")
                if k in strings}
    except (OSError, PEError):
        return {}


def _u16(data, off):
    return int.from_bytes(data[off:off + 2], "little")


def _u32(data, off):
    return int.from_bytes(data[off:off + 4], "little")


def _version_resource(image, base, size):
    """Locate the RT_VERSION data entry inside the resource table."""
    data = image.data
    limit = base + size

    def entries(off):
        if off < base or off + 16 > limit:
            return []
        count = _u16(data, off + 12) + _u16(data, off + 14)
        if count > 64 or off + 16 + count * 8 > limit:
            return []
        return [(_u32(data, off + 16 + i * 8), _u32(data, off + 16 + i * 8 + 4))
                for i in range(count)]

    for name, target in entries(base):
        if name != 16 or not (target & 0x80000000):  # RT_VERSION, a directory
            continue
        for _name, t2 in entries(base + (target & 0x7fffffff)):
            if not (t2 & 0x80000000):
                continue
            for _lang, t3 in entries(base + (t2 & 0x7fffffff)):
                if t3 & 0x80000000:
                    continue
                entry = base + t3
                if entry + 16 > limit:
                    return None
                rva = _u32(data, entry)
                rsize = _u32(data, entry + 4)
                if not rva or not rsize:
                    return None
                try:
                    off, _avail = image.mapping(rva, rsize)
                except PEError:
                    return None
                return off, rsize
    return None


def _version_blocks(data, start, end):
    """Iterate the blocks of a version-info tree within [start, end).

    Yields (key, type, value_text, children_start, block_end); value_text is
    None for binary blocks.
    """
    off = start
    while off + 6 <= end:
        length = _u16(data, off)
        if length < 8 or off + length > end:
            break
        block_end = off + length
        value_length = _u16(data, off + 2)
        btype = _u16(data, off + 4)
        key_end = off + 6
        while key_end + 1 < block_end and _u16(data, key_end) != 0:
            key_end += 2
        key = data[off + 6:key_end].decode("utf-16-le", errors="replace")
        pos = (key_end + 2 + 3) & ~3
        value_size = value_length * 2 if btype == 1 else value_length
        value = None
        if btype == 1 and value_length and pos + value_size <= block_end:
            value = data[pos:pos + value_size].decode("utf-16-le", errors="replace")
        children = (pos + value_size + 3) & ~3
        yield key, btype, value, children, block_end
        # wLength excludes the padding that aligns the NEXT block to 4 bytes
        # (the game and notepad both write values whose block ends mid-word).
        off = (block_end + 3) & ~3


def _version_block_strings(data, off, size):
    """Every StringTable entry of a VS_VERSIONINFO block, first table wins."""
    found = {}
    end = off + size
    for key, _t, _v, children, block_end in _version_blocks(data, off, end):
        # Resource compilers disagree on the top-level key: "VS_VERSIONINFO"
        # is the documented name, the game ships "VS_VERSION_INFO".
        if key not in ("VS_VERSIONINFO", "VS_VERSION_INFO"):
            continue
        for fi_key, _ft, _fv, fi_children, fi_end in _version_blocks(data, children, block_end):
            if fi_key != "StringFileInfo":
                continue
            for _tk, _tt, _tv, t_children, t_end in _version_blocks(data, fi_children, fi_end):
                for skey, _st, svalue, _sc, _se in _version_blocks(data, t_children, t_end):
                    if svalue is not None and skey not in found:
                        found[skey] = svalue.rstrip("\0")
    return found


def _vs_block(key, value=b"", btype=0, children=b""):
    """One VS_VERSIONINFO-format block: length, type, key, value, children.

    Like the real files, the value is not padded inside wLength; the padding
    that aligns the next block is written between blocks, not counted.
    """
    keyb = key.encode("utf-16-le") + b"\0\0"
    head = struct.pack("<HHH", 0,
                       len(value) // 2 if btype == 1 else len(value),
                       btype) + keyb
    head += b"\0" * (-len(head) % 4)
    body = value
    if children:
        body += b"\0" * (-len(body) % 4)
    block = head + body + children
    return struct.pack("<H", len(block)) + block[2:]


def _vs_version_info(strings):
    """A VS_VERSIONINFO block carrying one StringTable of `strings`."""
    entries = b""
    for name, text in strings.items():
        block = _vs_block(name, text.encode("utf-16-le") + b"\0\0", btype=1)
        entries += block + b"\0" * (-len(block) % 4)
    table = _vs_block("040904b0", b"", btype=1, children=entries)
    sfi = _vs_block("StringFileInfo", b"", btype=1, children=table)
    return _vs_block("VS_VERSIONINFO", bytes(52), btype=0, children=sfi)


def _resource_dir(entries):
    out = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(entries))
    for name, target in entries:
        out += struct.pack("<II", name, target)
    return out


def _resource_table(vs_rva, vs_size):
    """Resource tables pointing at one RT_VERSION blob: type 16, name 1, lang 1033."""
    root = _resource_dir([(16, 0x80000000 | 0x20)])
    name = _resource_dir([(1, 0x80000000 | 0x40)])
    lang = _resource_dir([(1033, 0x60)])
    data_entry = struct.pack("<IIII", vs_rva, vs_size, 0, 0)
    return (root.ljust(0x20, b"\0") + name.ljust(0x20, b"\0") +
            lang.ljust(0x20, b"\0") + data_entry)


def _fixture(product_name="Elite Dangerous: Odyssey",
             file_description="Elite Dangerous: Odyssey Executable",
             file_version="334925"):
    """Small synthetic PE with the qualified proof points for parser tests."""
    # The real IAT RVA is high in the image.  Keep the fixture sparse in
    # meaning while giving the bounded parser the same address relationships.
    image_size = 0x640C000
    data = bytearray(image_size)
    section_raw, section_rva, section_size = 0x200, 0x1000, image_size - 0x1000

    def put(offset, value, size=4):
        data[offset:offset + size] = int(value).to_bytes(size, "little", signed=False)

    def file_offset(rva):
        return section_raw + (rva - section_rva)

    def rv(rva, value, size=4):
        put(file_offset(rva), value, size)

    def blob(rva, value):
        at = file_offset(rva)
        data[at:at + len(value)] = value

    data[:2] = b"MZ"
    put(0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    put(0x84, 0x8664, 2)
    put(0x86, 1, 2)
    put(0x94, 240, 2)
    optional = 0x98
    put(optional, 0x20B, 2)
    put(optional + 24, IMAGE_BASE, 8)
    put(optional + 56, image_size)
    put(optional + 60, 0x1000)
    put(optional + 108, 16)
    section = optional + 240
    put(section + 8, section_size)
    put(section + 12, section_rva)
    put(section + 16, section_size)
    put(section + 20, section_raw)
    put(section + 36, 0x60000020)
    dirs = optional + 112
    put(dirs + 8, 0x3000)
    put(dirs + 12, 40)

    # A single strict regular KERNEL32 import, with an INT and an IAT.
    rv(0x3000 + 12, 0x3400)
    rv(0x3000 + 16, LOAD_LIBRARY_IAT_RVA)
    rv(0x3000, 0x3500)
    blob(0x3400, b"KERNEL32.dll\0")
    rv(0x3500, 0x3700, 8)
    blob(0x3700, b"\0\0LoadLibraryW\0")
    rv(LOAD_LIBRARY_IAT_RVA, 0x12345678, 8)

    for rva, value in _BYTE_CHECKS.items():
        blob(rva, value)
    # selector factory direct call
    blob(0x8D5437, b"\xE8" + struct.pack("<i", 0x4E4B40 - (0x8D5437 + 5)))
    for rva, target in ((0x4E4706, 0x4E85D0), (0x4E86AF, 0x4E7170),
                        (0x4E71A2, 0x4E6DE0), (0x4E5D93, 0x4E8750)):
        blob(rva, b"\xE8" + struct.pack("<i", target - (rva + 5)))
    for rva, target in ((0x4E27438, 0x4E46D0), (0x4E27440, 0x4E5D40),
                        (0x4E27600, 0x4E4910)):
        rv(rva, IMAGE_BASE + target, 8)
    # SDK LoadLibraryW IAT call, followed by its exact return continuation.
    blob(LOADER_CALL_RVA, b"\xff\x15" + struct.pack("<i", LOAD_LIBRARY_IAT_RVA - (LOADER_CALL_RVA + 6)))
    blob(LOADER_RETURN_RVA, bytes.fromhex("488bcb488bf0"))
    # A version resource, so the code that reads ProductName and friends has a
    # fixture with the same shape as the real executables. The strings default
    # to Odyssey's; the legacy build's are "Elite:Dangerous" and friends.
    vs = _vs_version_info({"ProductName": product_name,
                           "FileDescription": file_description,
                           "FileVersion": file_version})
    resource = _resource_table(0x5000, len(vs))
    blob(0x4000, resource)
    blob(0x5000, vs)
    put(dirs + 16, 0x4000)
    put(dirs + 20, len(resource))
    return bytes(data)


def self_test():
    checks = 0

    def check(condition):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError("check %d failed" % checks)

    fixture = _fixture()
    image = Image(fixture)
    check(_validate_image(image, GAME_SHA256, strict_identity=False).profile_id == PROFILE_ID)
    check(_validate_image(image, GAME_SHA256, strict_identity=False).loader_return_rva == LOADER_RETURN_RVA)
    check(_fnv_bytes(b"abc") == 0xE71FA2190541574B)
    check(_fnv_bytes(b"abd") != _fnv_bytes(b"abc"))
    for mutation in (bytearray(b""), bytearray(b"MZ" + bytes(62))):
        try:
            Image(mutation)
        except PEError:
            check(True)
        else:
            check(False)
    altered = bytearray(fixture)
    altered[0x80] = ord("X")
    try:
        Image(altered)
    except PEError:
        check(True)
    else:
        check(False)
    def rejects(mutator):
        candidate = bytearray(fixture)
        mutator(candidate)
        try:
            _validate_image(Image(candidate), GAME_SHA256, strict_identity=False)
        except (PEError, ValueError):
            check(True)
        else:
            check(False)

    def at(data, rva):
        return 0x200 + rva - 0x1000

    rejects(lambda d: d.__setitem__(at(d, LOADER_CALL_RVA), 0x90))
    rejects(lambda d: d.__setitem__(at(d, 0x8D5437), 0x90))
    rejects(lambda d: d.__setitem__(at(d, 0x3508), 1))
    rejects(lambda d: d.__setitem__(at(d, 0x4E27438), 0))
    rejects(lambda d: d.__setitem__(slice(0x84, 0x86), (0x14C).to_bytes(2, "little")))
    rejects(lambda d: d.__setitem__(0x98 + 27, 0x41))
    # The public entry point refuses an unknown digest before any profile
    # interpretation, which is the installer safety property.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        unknown = Path(td) / "EliteDangerous64.exe"
        unknown.write_bytes(fixture)
        try:
            validate_elite_oculus(unknown)
        except ValueError as exc:
            check("SHA-256" in str(exc))
            check("not a known executable revision" in str(exc))
        else:
            check(False)
        strings = version_strings(unknown)
        check(strings.get("ProductName") == "Elite Dangerous: Odyssey")
        check(strings.get("FileDescription") == "Elite Dangerous: Odyssey Executable")
        check(strings.get("FileVersion") == "334925")
        legacy = Path(td) / "legacy.exe"
        legacy.write_bytes(_fixture(product_name="Elite:Dangerous",
                                    file_description="Elite:Dangerous Executable",
                                    file_version="269978"))
        strings = version_strings(legacy)
        check(strings.get("ProductName") == "Elite:Dangerous")
        check("odyssey" not in strings.get("ProductName", "").lower())
        check(version_strings(Path(td) / "absent.exe") == {})
    print("elite_oculus: %d checks, 0 failures" % checks)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", help="EliteDangerous64.exe")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate and print only; never writes or launches")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.path:
        parser.error("an executable path is required (or --self-test)")
    try:
        profile = validate_elite_oculus(args.path)
        print(json.dumps(profile.as_dict(), sort_keys=True))
        return 0
    except (OSError, ValueError, PEError) as exc:
        print("elite_oculus: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
