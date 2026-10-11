"""Fetch and verify EDVR's pinned Intel XeSS SDK subset.

    python tools/fetch_xess.py [--dest DIR] [--dry-run]
    python tools/fetch_xess.py --verify DIR
    python tools/fetch_xess.py --self-test

Downloads the two D3D12 headers, Intel's runtime DLL, and the SDK license from
the official Intel XeSS repository at one pinned commit. Every file is checked
against its pinned SHA-256 before it is installed. No import library is staged:
EDVR loads XeSS dynamically.

Default destination is %LOCALAPPDATA%\\EDVR\\xess-sdk, shared by checkouts;
pass --dest third_party\\xess for a self-contained checkout. --dry-run is
strictly read-only. The SDK license is included in the staged payload; this
tool does not request or record license acceptance.
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys
import tempfile
import urllib.request
from pathlib import Path

URL = "https://raw.githubusercontent.com/intel/xess"
COMMIT = "8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0"  # XeSS SDK v3.0.2
SDK_VERSION = "3.0.2"
RUNTIME_VERSION = "SR2.0.2"
FILES = {
    "include/xess/xess.h": ("inc/xess/xess.h", "98dc435f5aed252d83a161fc1f86223f20c67eb863214fe3b5cb215a3f6e3319"),
    "include/xess/xess_d3d12.h": ("inc/xess/xess_d3d12.h", "a582a60c3e39ee6fd6aa4871e23082cfb5ec510f98a19fc04acf7a7edb2f6b6c"),
    "bin/libxess.dll": ("bin/libxess.dll", "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7"),
    "LICENSE.txt": ("LICENSE.txt", "784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df"),
    "third-party-programs.txt": ("third-party-programs.txt", "e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c"),
}


def default_dest():
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(base, "EDVR", "xess-sdk")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def verify(dest, quiet=False):
    problems = []
    root = Path(dest)
    for rel, (_source, expected) in FILES.items():
        path = root / Path(rel)
        if not path.is_file():
            problems.append("missing: %s" % path)
            continue
        got = sha256_bytes(path.read_bytes())
        if got != expected:
            problems.append("%s SHA-256 is %s, pin is %s" % (rel, got.upper(), expected.upper()))
    dll_path = root / "bin/libxess.dll"
    if dll_path.is_file():
        data = dll_path.read_bytes()
        if len(data) < 0x40 or data[:2] != b"MZ":
            problems.append("bin/libxess.dll is not a valid PE image")
        else:
            pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
            if pe_offset + 6 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
                problems.append("bin/libxess.dll has no valid PE header")
            elif struct.unpack_from("<H", data, pe_offset + 4)[0] != 0x8664:
                problems.append("bin/libxess.dll is not x64")
    if not problems and not quiet:
        print("XeSS SDK %s (runtime %s) verified at %s (commit %s)" %
              (SDK_VERSION, RUNTIME_VERSION, dest, COMMIT[:12]))
    return problems


def download_file(source, timeout=120):
    request = urllib.request.Request("%s/%s/%s" % (URL, COMMIT, source),
                                     headers={"User-Agent": "EDVR-pinned-sdk-fetch/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def fetch(dest, dry_run=False, downloader=download_file):
    if dry_run:
        print("Would fetch Intel XeSS SDK %s at %s from %s" % (SDK_VERSION, COMMIT, URL))
        for rel, (source, digest) in FILES.items():
            print("Would stage %s from %s (SHA-256 %s)" % (rel, source, digest.upper()))
        print("(dry run) no network, directories, or files were touched")
        return 0

    problems = verify(dest, quiet=True)
    if not problems:
        print("XeSS SDK already verified at %s" % dest)
        return 0

    payload = {}
    for rel, (source, expected) in FILES.items():
        data = downloader(source)
        got = sha256_bytes(data)
        if got != expected:
            raise ValueError("downloaded %s SHA-256 is %s, pin is %s" %
                             (source, got.upper(), expected.upper()))
        payload[rel] = data

    # Validate the runtime before creating anything at the destination.
    dll = payload["bin/libxess.dll"]
    if len(dll) < 0x40 or dll[:2] != b"MZ":
        raise ValueError("downloaded libxess.dll is not a valid PE image")
    pe_offset = struct.unpack_from("<I", dll, 0x3C)[0]
    if pe_offset + 6 > len(dll) or dll[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("downloaded libxess.dll has no valid PE header")
    if struct.unpack_from("<H", dll, pe_offset + 4)[0] != 0x8664:
        raise ValueError("downloaded libxess.dll is not x64")

    parent = os.path.dirname(os.path.abspath(dest))
    staged = tempfile.mkdtemp(prefix=".xess-stage-")
    backup = None
    publish = None
    try:
        for rel, data in payload.items():
            path = Path(staged) / Path(rel)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        os.makedirs(parent, exist_ok=True)
        publish = tempfile.mkdtemp(prefix=".xess-publish-", dir=parent)
        shutil.rmtree(publish)
        shutil.copytree(staged, publish)
        if os.path.exists(dest):
            backup = dest + ".xess-old"
            if os.path.exists(backup):
                raise OSError("refusing to overwrite existing backup %s" % backup)
            os.replace(dest, backup)
        try:
            os.replace(publish, dest)
        except Exception:
            if backup is not None:
                os.replace(backup, dest)
                backup = None
            raise
        if backup is not None:
            shutil.rmtree(backup)
            backup = None
    finally:
        shutil.rmtree(staged, ignore_errors=True)
        if publish is not None:
            shutil.rmtree(publish, ignore_errors=True)
    problems = verify(dest)
    if problems:
        raise ValueError("staged SDK failed verification: " + "; ".join(problems))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=None, help="SDK destination (default: %%LOCALAPPDATA%%\\EDVR\\xess-sdk)")
    ap.add_argument("--verify", default=None, metavar="DIR", help="verify an existing SDK copy and exit")
    ap.add_argument("--dry-run", action="store_true", help="show the pinned files without network or writes")
    ap.add_argument("--self-test", action="store_true", help="run offline fixture checks and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify:
        problems = verify(args.verify)
        for problem in problems:
            print("fetch_xess: " + problem)
        return 2 if problems else 0
    try:
        return fetch(args.dest or default_dest(), dry_run=args.dry_run)
    except (OSError, ValueError) as error:
        print("fetch_xess: " + str(error), file=sys.stderr)
        return 2


def self_test():
    import io
    import contextlib
    import tempfile
    from unittest import mock
    failures = []

    def expect(name, condition, detail=""):
        if not condition:
            failures.append("%s: %s" % (name, detail))

    with tempfile.TemporaryDirectory(prefix="edvr-fetch-xess-") as base:
        fixtures = {rel: (b"MZ" + b"\0" * (0x3C - 2) + struct.pack("<I", 0x40) +
                          b"\0" * (0x40 - 0x3C - 4) + b"PE\0\0" + struct.pack("<H", 0x8664)
                          if rel == "bin/libxess.dll" else b"fixture:" + rel.encode())
                    for rel in FILES}
        # The self-test substitutes fixture pins without affecting production constants.
        saved_files = dict(FILES)
        try:
            for rel, data in fixtures.items():
                source, _digest = FILES[rel]
                FILES[rel] = (source, sha256_bytes(data))
            dest = os.path.join(base, "install")
            code = fetch(dest, downloader=lambda source: next(fixtures[rel] for rel, (src, _h) in FILES.items() if src == source))
            expect("staging", code == 0 and not verify(dest, quiet=True), "valid fixture did not stage")
            Path(dest, "include/xess/xess.h").write_bytes(b"corrupt")
            expect("corrupt-header", bool(verify(dest, quiet=True)), "corrupt header passed")
            Path(dest, "include/xess/xess.h").write_bytes(fixtures["include/xess/xess.h"])
            Path(dest, "bin/libxess.dll").write_bytes(b"wrong runtime")
            expect("corrupt-runtime", bool(verify(dest, quiet=True)), "corrupt runtime passed")
            bad_dest = os.path.join(base, "failed")
            def bad_download(source):
                if source == FILES["bin/libxess.dll"][0]:
                    return b"wrong runtime"
                return next(fixtures[rel] for rel, (src, _h) in FILES.items() if src == source)
            try:
                fetch(bad_dest, downloader=bad_download)
                expect("failed-fetch", False, "bad runtime accepted")
            except ValueError:
                expect("failed-fetch", not os.path.exists(bad_dest), "failed fetch left destination")
            before = {str(p.relative_to(dest)): p.read_bytes()
                      for p in Path(dest).rglob("*") if p.is_file()}
            try:
                fetch(dest, downloader=bad_download)
                expect("failed-replacement", False, "bad replacement accepted")
            except ValueError:
                after = {str(p.relative_to(dest)): p.read_bytes()
                         for p in Path(dest).rglob("*") if p.is_file()}
                expect("failed-replacement", before == after,
                       "failed fetch changed existing destination")
            dry_parent = os.path.join(base, "dry-parent")
            dry_dest = os.path.join(dry_parent, "dry")
            output = io.StringIO()
            # Guard writes everywhere, not just inside the proposed destination:
            # a cache, temporary file or backup is also forbidden in dry-run.
            with contextlib.ExitStack() as guards:
                for name in ("os.mkdir", "os.makedirs", "os.replace", "os.rename",
                             "os.remove", "os.unlink", "os.rmdir",
                             "tempfile.mkdtemp", "tempfile.mkstemp",
                             "shutil.copyfile", "shutil.copytree", "shutil.rmtree",
                             "pathlib.Path.write_bytes", "pathlib.Path.write_text"):
                    guards.enter_context(mock.patch(name, side_effect=AssertionError("dry-run write: " + name)))
                guards.enter_context(contextlib.redirect_stdout(output))
                fetch(dry_dest, dry_run=True, downloader=lambda _source: (_ for _ in ()).throw(AssertionError("network")))
            expect("dry-run", not os.path.exists(dry_parent) and "no network" in output.getvalue(), "dry-run touched filesystem/network")
        finally:
            FILES.clear()
            FILES.update(saved_files)
    if failures:
        print("fetch_xess: self-test FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("fetch_xess: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
