"""Fetch and verify EDVR's pinned AMD FidelityFX (FSR 4.1) SDK subset.

    python tools/fetch_fsr4.py [--dest DIR] [--dry-run]
    python tools/fetch_fsr4.py --verify DIR
    python tools/fetch_fsr4.py --self-test

Downloads the ffx-api headers (loader, types, DX12, upscale), AMD's two signed
D3D12 runtime DLLs (amd_fidelityfx_loader_dx12.dll, the one the app calls, and
amd_fidelityfx_upscaler_dx12.dll, the FSR 4.1.1 provider beside it) and the
SDK licence from AMD's official FidelityFX-SDK repository at one pinned commit
(tag v2.3.0). Every file is checked against its pinned SHA-256 before it is
installed. No import library is staged: EDVR loads the loader dynamically.

The repository stores the DLLs as ordinary git blobs (no Git LFS), so
raw.githubusercontent.com serves the binaries themselves; the SHA-256 pins
would refuse an LFS pointer file if that ever changed.

Staged layout (ffx_upscale.h includes "../../api/include/ffx_api.h", so the
relative structure under include\\FidelityFX is kept exactly):

    include\\FidelityFX\\api\\include\\{ffx_api.h, ffx_api_types.h, ffx_api_loader.h}
    include\\FidelityFX\\api\\include\\dx12\\ffx_api_dx12.h
    include\\FidelityFX\\upscalers\\include\\ffx_upscale.h
    bin\\amd_fidelityfx_loader_dx12.dll, bin\\amd_fidelityfx_upscaler_dx12.dll
    LICENSE.txt                      (the SDK's docs/license.md)

Default destination is %LOCALAPPDATA%\\EDVR\\fsr4-sdk, shared by checkouts;
pass --dest third_party\\fsr4 for a self-contained checkout. --dry-run is
strictly read-only. The SDK licence is included in the staged payload; this
tool does not request or record licence acceptance. The licence opens with a
binary-only redistribution grant and then lists the signed DLLs under MIT; it
is ambiguous, so nothing here ships the DLLs in an installer or release
package (design doc fsr-upscaler-design-2026-09-16.md, decision D8).
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

URL = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"
COMMIT = "60f4ea81909200d8542eca14dccb2628b763a9a3"  # FidelityFX SDK v2.3.0 (tag v2.3.0, 2026-06-24)
SDK_VERSION = "2.3.0"
RUNTIME_VERSION = "FSR 4.1.1"  # amd_fidelityfx_upscaler_dx12.dll FileVersion 4.1.1.2740; the loader is 2.3.0.2740
DLLS = ("bin/amd_fidelityfx_loader_dx12.dll", "bin/amd_fidelityfx_upscaler_dx12.dll")
FILES = {
    "include/FidelityFX/api/include/ffx_api.h": (
        "Kits/FidelityFX/api/include/ffx_api.h",
        "e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd"),
    "include/FidelityFX/api/include/ffx_api_types.h": (
        "Kits/FidelityFX/api/include/ffx_api_types.h",
        "cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49"),
    "include/FidelityFX/api/include/ffx_api_loader.h": (
        "Kits/FidelityFX/api/include/ffx_api_loader.h",
        "18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e"),
    "include/FidelityFX/api/include/dx12/ffx_api_dx12.h": (
        "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h",
        "c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"),
    "include/FidelityFX/upscalers/include/ffx_upscale.h": (
        "Kits/FidelityFX/upscalers/include/ffx_upscale.h",
        "65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf"),
    "bin/amd_fidelityfx_loader_dx12.dll": (
        "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll",
        "e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa"),
    "bin/amd_fidelityfx_upscaler_dx12.dll": (
        "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll",
        "d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46"),
    "LICENSE.txt": (
        "docs/license.md",
        "6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963"),
}


def default_dest():
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(base, "EDVR", "fsr4-sdk")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def pe_problem(name, data):
    """Why `data` is not an x64 PE image, or None when it is."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return "%s is not a valid PE image" % name
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 6 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return "%s has no valid PE header" % name
    if struct.unpack_from("<H", data, pe_offset + 4)[0] != 0x8664:
        return "%s is not x64" % name
    return None


def verify(dest, quiet=False):
    problems = []
    root = Path(dest)
    for rel, (_source, expected) in FILES.items():
        path = root / Path(rel)
        if not path.is_file():
            problems.append("missing: %s" % path)
            continue
        data = path.read_bytes()
        got = sha256_bytes(data)
        if got != expected:
            problems.append("%s SHA-256 is %s, pin is %s" % (rel, got.upper(), expected.upper()))
        elif rel in DLLS:
            problem = pe_problem(rel, data)
            if problem:
                problems.append(problem)
    if not problems and not quiet:
        print("FidelityFX SDK %s (%s runtime) verified at %s (commit %s)" %
              (SDK_VERSION, RUNTIME_VERSION, dest, COMMIT[:12]))
    return problems


def download_file(source, timeout=300):
    request = urllib.request.Request("%s/%s/%s" % (URL, COMMIT, source),
                                     headers={"User-Agent": "EDVR-pinned-sdk-fetch/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def fetch(dest, dry_run=False, downloader=download_file):
    if dry_run:
        print("Would fetch AMD FidelityFX SDK %s (%s) at %s from %s" % (SDK_VERSION, RUNTIME_VERSION, COMMIT, URL))
        for rel, (source, digest) in FILES.items():
            print("Would stage %s from %s (SHA-256 %s)" % (rel, source, digest.upper()))
        print("(dry run) no network, directories, or files were touched")
        return 0

    problems = verify(dest, quiet=True)
    if not problems:
        print("FidelityFX SDK already verified at %s" % dest)
        return 0

    payload = {}
    for rel, (source, expected) in FILES.items():
        data = downloader(source)
        got = sha256_bytes(data)
        if got != expected:
            raise ValueError("downloaded %s SHA-256 is %s, pin is %s" %
                             (source, got.upper(), expected.upper()))
        payload[rel] = data

    # Validate the runtime DLLs before creating anything at the destination.
    for rel in DLLS:
        problem = pe_problem("downloaded " + os.path.basename(rel), payload[rel])
        if problem:
            raise ValueError(problem)

    parent = os.path.dirname(os.path.abspath(dest))
    staged = tempfile.mkdtemp(prefix=".fsr4-stage-")
    backup = None
    publish = None
    try:
        for rel, data in payload.items():
            path = Path(staged) / Path(rel)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        os.makedirs(parent, exist_ok=True)
        publish = tempfile.mkdtemp(prefix=".fsr4-publish-", dir=parent)
        shutil.rmtree(publish)
        shutil.copytree(staged, publish)
        if os.path.exists(dest):
            backup = dest + ".fsr4-old"
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
    ap.add_argument("--dest", default=None, help="SDK destination (default: %%LOCALAPPDATA%%\\EDVR\\fsr4-sdk)")
    ap.add_argument("--verify", default=None, metavar="DIR", help="verify an existing SDK copy and exit")
    ap.add_argument("--dry-run", action="store_true", help="show the pinned files without network or writes")
    ap.add_argument("--self-test", action="store_true", help="run offline fixture checks and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify:
        problems = verify(args.verify)
        for problem in problems:
            print("fetch_fsr4: " + problem)
        return 2 if problems else 0
    try:
        return fetch(args.dest or default_dest(), dry_run=args.dry_run)
    except (OSError, ValueError) as error:
        print("fetch_fsr4: " + str(error), file=sys.stderr)
        return 2


def self_test():
    import io
    import contextlib
    import re
    import tempfile
    from unittest import mock
    failures = []

    def expect(name, condition, detail=""):
        if not condition:
            failures.append("%s: %s" % (name, detail))

    def pe_fixture(machine=0x8664):
        return (b"MZ" + b"\0" * (0x3C - 2) + struct.pack("<I", 0x40) +
                b"\0" * (0x40 - 0x3C - 4) + b"PE\0\0" + struct.pack("<H", machine))

    # The production pins: well formed, one source and one destination per file, the licence staged under
    # its published name, and the include tree laid out the way ffx_upscale.h's own includes need.
    expect("pins", len(FILES) == 8 and re.fullmatch(r"[0-9a-f]{40}", COMMIT) is not None,
           "unexpected file count or commit pin")
    expect("pin-digests", all(re.fullmatch(r"[0-9a-f]{64}", digest) for _s, digest in FILES.values()),
           "a pinned digest is not 64 lowercase hex digits")
    expect("pin-sources", len({source for source, _d in FILES.values()}) == len(FILES),
           "two staged files share one source")
    expect("pin-layout", all(rel in FILES for rel in DLLS) and "LICENSE.txt" in FILES and
           "include/FidelityFX/upscalers/include/ffx_upscale.h" in FILES and
           "include/FidelityFX/api/include/ffx_api.h" in FILES and
           "include/FidelityFX/api/include/dx12/ffx_api_dx12.h" in FILES,
           "the staged layout lost a file the engine includes")

    with tempfile.TemporaryDirectory(prefix="edvr-fetch-fsr4-") as base:
        fixtures = {rel: (pe_fixture() if rel in DLLS else b"fixture:" + rel.encode()) for rel in FILES}
        # The self-test substitutes fixture pins without affecting production constants.
        saved_files = dict(FILES)
        try:
            for rel, data in fixtures.items():
                source, _digest = FILES[rel]
                FILES[rel] = (source, sha256_bytes(data))
            by_source = {src: fixtures[rel] for rel, (src, _h) in FILES.items()}
            dest = os.path.join(base, "install")
            code = fetch(dest, downloader=lambda source: by_source[source])
            expect("staging", code == 0 and not verify(dest, quiet=True), "valid fixture did not stage")
            expect("staged-include-tree",
                   (Path(dest) / "include/FidelityFX/upscalers/include" / "../../api/include/ffx_api.h").is_file(),
                   "ffx_upscale.h's ../../api/include/ffx_api.h does not resolve in the staged tree")
            expect("staged-dlls", all((Path(dest) / rel).is_file() for rel in DLLS), "a runtime DLL was not staged")
            Path(dest, "include/FidelityFX/upscalers/include/ffx_upscale.h").write_bytes(b"corrupt")
            expect("corrupt-header", bool(verify(dest, quiet=True)), "corrupt header passed")
            Path(dest, "include/FidelityFX/upscalers/include/ffx_upscale.h").write_bytes(
                fixtures["include/FidelityFX/upscalers/include/ffx_upscale.h"])
            for dll in DLLS:
                Path(dest, dll).write_bytes(b"wrong runtime")
                expect("corrupt-" + dll, bool(verify(dest, quiet=True)), "corrupt %s passed" % dll)
                Path(dest, dll).write_bytes(fixtures[dll])
            Path(dest, "LICENSE.txt").unlink()
            expect("missing-licence", bool(verify(dest, quiet=True)), "a missing licence passed")
            Path(dest, "LICENSE.txt").write_bytes(fixtures["LICENSE.txt"])
            expect("restored", not verify(dest, quiet=True), "restoring every file did not verify")

            # A DLL whose hash matches its pin but that is not an x64 PE image must still be refused.
            x86 = pe_fixture(0x14C)
            FILES[DLLS[1]] = (FILES[DLLS[1]][0], sha256_bytes(x86))
            Path(dest, DLLS[1]).write_bytes(x86)
            expect("x86-runtime", any("not x64" in p for p in verify(dest, quiet=True)),
                   "a pinned x86 runtime passed verification")
            x86_dest = os.path.join(base, "x86")
            try:
                fetch(x86_dest, downloader=lambda source: x86 if source == FILES[DLLS[1]][0] else by_source[source])
                expect("x86-fetch", False, "an x86 runtime was staged")
            except ValueError:
                expect("x86-fetch", not os.path.exists(x86_dest), "an x86 fetch left a destination")
            FILES[DLLS[1]] = (FILES[DLLS[1]][0], sha256_bytes(fixtures[DLLS[1]]))
            Path(dest, DLLS[1]).write_bytes(fixtures[DLLS[1]])

            bad_dest = os.path.join(base, "failed")

            def bad_download(source):
                if source == FILES["bin/amd_fidelityfx_upscaler_dx12.dll"][0]:
                    return b"wrong runtime"
                return by_source[source]
            try:
                fetch(bad_dest, downloader=bad_download)
                expect("failed-fetch", False, "bad runtime accepted")
            except ValueError:
                expect("failed-fetch", not os.path.exists(bad_dest), "failed fetch left destination")
            # An already-verified destination is not fetched again.
            expect("already-verified", fetch(dest, downloader=bad_download) == 0,
                   "a verified destination was fetched again")
            Path(dest, "LICENSE.txt").write_bytes(b"stale")
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
            code = fetch(dest, downloader=lambda source: by_source[source])
            expect("replacement", code == 0 and not verify(dest, quiet=True) and
                   not os.path.exists(dest + ".fsr4-old"), "a good replacement did not land cleanly")
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
            # The same through the command line, which must not reach the network either.
            output = io.StringIO()
            with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")), \
                    contextlib.redirect_stdout(output):
                code = main(["--dry-run", "--dest", dry_dest])
            expect("dry-run-cli", code == 0 and not os.path.exists(dry_parent), "--dry-run through main() wrote or fetched")
        finally:
            FILES.clear()
            FILES.update(saved_files)
    if failures:
        print("fetch_fsr4: self-test FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print("fetch_fsr4: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
