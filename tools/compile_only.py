#!/usr/bin/env python3
"""Which tree a compile-only build came from, and what it produced.

A compile-only build is the Linux (Steam Deck, Proton) test-flight path:
tools/linux_toolchain.py runs `build.bat --compile-only` under Wine, and that
compiles the production DLLs into <root>/build/compile-build with none of the
full build's validation gates.  This module is the one definition of such a
build's identity, shared by the toolchain that writes its receipt and by
`install_edvr.py --compile-only`, which refuses a build whose tree has moved.

  source_fingerprint(root)  HEAD plus build_receipt.py's input fingerprint.  An
                            edit since the compile changes it, and so does any
                            commit, even a docs-only one: the DLLs report the
                            version `git describe` gave at compile time, and
                            `edvr_log.py --expect-build HEAD` would not match
                            them after one.
  write_receipt(...)        compile_only_receipt.json beside the outputs: kind,
                            validated=false, commit, describe, the fingerprint,
                            the toolchain, and the SHA-256 of every file an
                            install may take (the build directory's top-level
                            .dll and .txt files).

    python tools/compile_only.py --fingerprint [--root DIR]
    python tools/compile_only.py --self-test
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_receipt  # noqa: E402


KIND = "local-wine-production-dll-compile-only"
RECEIPT = "compile_only_receipt.json"
# What an install takes from the build directory; everything else there (gen\,
# obj\, .lib, .exp, .pdb, phase times) is the compile's own.
PAYLOAD_SUFFIXES = (".dll", ".txt")
# A receipt without these is not a build of EDVR.
REQUIRED = ("d3d11.dll", "openvr_api.dll")


def build_dir(root):
    """Where `build.bat --compile-only` puts a build of `root`: inside the
    tree's own ignored build\\, which the fingerprint never reads."""
    return Path(root) / "build" / "compile-build"


def _git(root, *args):
    try:
        result = subprocess.run(["git", "-C", str(root), *args], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def source_fingerprint(root):
    root = Path(root).resolve()
    head = _git(root, "rev-parse", "HEAD") or "nogit"
    inputs = build_receipt.input_fingerprint(root)
    return hashlib.sha256(("%s\n%s\n" % (head, inputs)).encode("ascii")).hexdigest()


def describe(root):
    return _git(root, "describe", "--tags", "--always", "--dirty") or "unknown"


def payload(bdir):
    """{name: {sha256, size}} for every file an install may take from `bdir`."""
    outputs = {}
    for path in sorted(Path(bdir).iterdir()):
        if path.is_file() and path.suffix.lower() in PAYLOAD_SUFFIXES:
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            outputs[path.name] = {"sha256": digest.hexdigest(), "size": path.stat().st_size}
    return outputs


def remove_receipt(root):
    """A compile about to start first removes the old receipt, so a failed or
    interrupted compile can never leave one beside outputs it does not describe."""
    path = build_dir(root) / RECEIPT
    if path.exists():
        path.unlink()


def write_receipt(root, fingerprint, toolchain):
    """Write the receipt for the build of `root` compiled from `fingerprint`.
    Refuses (ValueError) when the tree is no longer that one, or when the build
    directory lacks a required output."""
    root = Path(root).resolve()
    bdir = build_dir(root)
    if source_fingerprint(root) != fingerprint:
        raise ValueError("the tree changed during the compile; compile again")
    outputs = payload(bdir)
    missing = [name for name in REQUIRED if name not in outputs]
    if missing:
        raise ValueError("%s has no %s" % (bdir, ", ".join(missing)))
    receipt = {
        "kind": KIND,
        "validated": False,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "commit": _git(root, "rev-parse", "HEAD") or "nogit",
        "describe": describe(root),
        "source_fingerprint": fingerprint,
        "toolchain": toolchain,
        "outputs": outputs,
    }
    path = bdir / RECEIPT
    temporary = path.with_name(RECEIPT + ".tmp")
    temporary.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)
    return path


def self_test():
    temporary = Path(tempfile.mkdtemp(prefix="edvr-compile-only-"))
    try:
        root = temporary / "repo"
        root.mkdir()

        def git(*args):
            subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t",
                            "-c", "commit.gpgsign=false", *args], check=True, capture_output=True)

        def write(relative, data):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

        def expect(condition, why):
            if not condition:
                raise AssertionError(why)

        write("build.bat", b"@echo off\r\n")
        write("src/a.cpp", b"int a;\n")
        write(".gitignore", b"build/\n")
        subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
        git("add", "-A")
        git("commit", "-q", "-m", "fixture")
        first = source_fingerprint(root)
        expect(source_fingerprint(root) == first, "the fingerprint is not stable")

        write("src/a.cpp", b"int a = 1;\n")
        expect(source_fingerprint(root) != first, "an edit did not change the fingerprint")
        write("src/a.cpp", b"int a;\n")
        expect(source_fingerprint(root) == first, "undoing the edit did not restore the fingerprint")
        write("src/new.cpp", b"int b;\n")
        expect(source_fingerprint(root) != first, "an untracked source did not change the fingerprint")
        (root / "src" / "new.cpp").unlink()

        bdir = build_dir(root)
        for name, data in (("d3d11.dll", b"GRAPHICS"), ("openvr_api.dll", b"RUNTIME"),
                           ("INTEL-XESS-LICENSE.txt", b"LICENSE"), ("d3d11.pdb", b"PDB"),
                           ("d3d11.lib", b"LIB"), ("gen/edvr_version.h", b"#define V\n")):
            write(Path("build/compile-build") / name, data)
        expect(source_fingerprint(root) == first, "the build output changed the fingerprint")

        toolchain = {"msvc": "test"}
        path = write_receipt(root, first, toolchain)
        receipt = json.loads(path.read_text(encoding="utf-8"))
        expect(receipt["kind"] == KIND and receipt["validated"] is False, "the receipt is not marked compile-only")
        expect(receipt["source_fingerprint"] == first and receipt["toolchain"] == toolchain,
               "the receipt does not carry the fingerprint and toolchain")
        expect(sorted(receipt["outputs"]) == ["INTEL-XESS-LICENSE.txt", "d3d11.dll", "openvr_api.dll"],
               "the receipt lists %s, not the install payload" % sorted(receipt["outputs"]))
        expect(receipt["outputs"]["d3d11.dll"]["sha256"] == hashlib.sha256(b"GRAPHICS").hexdigest(),
               "the receipt's hash is not the output's")
        expect(not (bdir / (RECEIPT + ".tmp")).exists(), "the receipt's temporary file was left behind")

        remove_receipt(root)
        expect(not path.exists(), "remove_receipt left the receipt")
        write("src/a.cpp", b"int a = 2;\n")
        try:
            write_receipt(root, first, toolchain)
            raise AssertionError("a receipt was written for a tree that changed during the compile")
        except ValueError:
            pass
        expect(not path.exists(), "a refused receipt was written anyway")
        write("src/a.cpp", b"int a;\n")

        (bdir / "openvr_api.dll").unlink()
        try:
            write_receipt(root, first, toolchain)
            raise AssertionError("a receipt was written without openvr_api.dll")
        except ValueError:
            pass

        write("docs/notes.md", b"a note\n")
        git("add", "-A")
        git("commit", "-q", "-m", "docs only")
        expect(source_fingerprint(root) != first, "a docs-only commit did not change the fingerprint")
    finally:
        shutil.rmtree(temporary, ignore_errors=True)
    print("compile_only: self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fingerprint", action="store_true", help="print the tree's compile-only fingerprint")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.fingerprint:
        parser.error("one of --fingerprint or --self-test is required")
    print(source_fingerprint(args.root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
