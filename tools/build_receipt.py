"""Record and verify the input identity of a green native build.

The full build is allowed to run on a dirty tree so the test suite can check
an uncommitted change.  After that change is committed, the DLL-only
promotion build uses this receipt to prove that the compiled inputs are the
same while allowing the git commit metadata, and therefore EDVR's displayed
version, to change.

Documentation under docs/ is not a compiled input: the receipt's only
consumer is the DLL-only promotion, which compiles no doc content, so a
doc-only change must not force another full build.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime, timezone


SCHEMA = 1
EXCLUDED_ROOTS = {".git", ".claude", ".codex", ".vs", "build", "build - Copy",
                  "dist", "analysis", "edvr_logs", "docs"}
EXCLUDED_PARTS = {"__pycache__", ".pytest_cache"}


def input_files(root):
    """Return every workspace file that can be a build input.

    Generated build products, documentation and local VCS/tooling state are
    outside the source identity.  Everything else is included, including
    ignored SDK files staged inside the checkout, so an input added outside
    git cannot silently evade the receipt.
    """
    root = Path(root).resolve()
    files = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if (relative.parts and relative.parts[0] in EXCLUDED_ROOTS) or \
                any(part in EXCLUDED_PARTS or part.endswith(".pyc") for part in relative.parts):
            continue
        files.append((relative.as_posix(), path))
    return sorted(files)


def input_fingerprint(root):
    digest = hashlib.sha256()
    for relative, path in input_files(root):
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        try:
            with path.open("rb") as stream:
                while True:
                    block = stream.read(1024 * 1024)
                    if not block:
                        break
                    digest.update(block)
        except OSError as error:
            raise ValueError("cannot read build input %s: %s" % (relative, error))
        digest.update(b"\0")
    return digest.hexdigest()


def command_output(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    if result.returncode:
        return ""
    return result.stdout.strip()


def git_state(root):
    return {
        "commit": command_output(root, "rev-parse", "HEAD") or "unknown",
        "describe": command_output(root, "describe", "--tags", "--always", "--dirty") or "unknown",
    }


def toolchain_state():
    located = subprocess.run(["where", "cl.exe"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, check=False)
    path = located.stdout.strip() if located.returncode == 0 else "unknown"
    version = subprocess.run(["cl.exe"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, check=False)
    lines = [line.strip() for line in version.stdout.splitlines() if line.strip()]
    return {"path": path, "version": lines[0] if lines else "unknown"}


def require_clean(root):
    result = subprocess.run(["git", "-C", str(root), "status", "--porcelain",
                             "--untracked-files=all"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    if result.returncode:
        raise ValueError("cannot inspect git status")
    if result.stdout.strip():
        raise ValueError("DLL-only promotion requires a clean working tree")


def parse_context(values):
    context = {}
    for value in values:
        key, separator, content = value.partition("=")
        if not separator or not key:
            raise ValueError("context must be KEY=VALUE: %s" % value)
        if key in context:
            raise ValueError("context key repeated: %s" % key)
        context[key] = content
    return dict(sorted(context.items()))


def make_receipt(root, context):
    state = git_state(root)
    return {
        "schema": SCHEMA,
        "status": "full-pass",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "commit": state["commit"],
        "describe": state["describe"],
        "toolchain": toolchain_state(),
        "inputs_sha256": input_fingerprint(root),
        "context": context,
    }


def write_receipt(path, root, context):
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    receipt = make_receipt(root, context)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=str(path.parent),
                                     prefix=path.name + ".", suffix=".tmp",
                                     delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(receipt, stream, indent=2, sort_keys=True)
        stream.write("\n")
    try:
        temporary.replace(path)
    except OSError:
        if temporary.exists():
            temporary.unlink()
        raise
    print("[edvr] full-build receipt: %s (%s)" % (path, receipt["inputs_sha256"]))
    return 0


def verify_receipt(path, root, context, clean):
    path = Path(path).resolve()
    try:
        with path.open(encoding="utf-8") as stream:
            receipt = json.load(stream)
    except (OSError, ValueError) as error:
        raise ValueError("cannot read full-build receipt %s: %s" % (path, error))
    if receipt.get("schema") != SCHEMA or receipt.get("status") != "full-pass":
        raise ValueError("receipt is not a full-pass receipt with schema %d" % SCHEMA)
    if receipt.get("context") != context:
        raise ValueError("build context differs from the last full build")
    if receipt.get("toolchain") != toolchain_state():
        raise ValueError("C++ toolchain differs from the last full build")
    actual = input_fingerprint(root)
    if receipt.get("inputs_sha256") != actual:
        raise ValueError("compiled inputs differ from the last full build")
    if clean:
        require_clean(root)
    state = git_state(root)
    print("[edvr] full-build receipt verified: %s" % path)
    print("[edvr] validated inputs from %s; promotion version is %s" %
          (receipt.get("describe", "unknown"), state["describe"]))
    return 0


def self_test():
    with tempfile.TemporaryDirectory(prefix="edvr-build-receipt-") as temporary:
        root = Path(temporary)
        (root / "build").mkdir()
        (root / "dist").mkdir()
        (root / "docs").mkdir()
        (root / "tools" / "__pycache__").mkdir(parents=True)
        (root / "source.txt").write_text("one", encoding="utf-8")
        (root / "build" / "ignored.txt").write_text("one", encoding="utf-8")
        (root / "dist" / "ignored.txt").write_text("one", encoding="utf-8")
        (root / "docs" / "notes.md").write_text("one", encoding="utf-8")
        (root / "tools" / "__pycache__" / "ignored.pyc").write_bytes(b"one")
        first = input_fingerprint(root)
        (root / "build" / "ignored.txt").write_text("two", encoding="utf-8")
        (root / "docs" / "notes.md").write_text("two", encoding="utf-8")
        (root / "tools" / "__pycache__" / "ignored.pyc").write_bytes(b"two")
        if input_fingerprint(root) != first:
            raise AssertionError("build outputs or docs affect the input fingerprint")
        (root / "source.txt").write_text("two", encoding="utf-8")
        if input_fingerprint(root) == first:
            raise AssertionError("source changes do not affect the input fingerprint")
        if parse_context(["z=last", "a=first"]) != {"a": "first", "z": "last"}:
            raise AssertionError("context keys are not canonicalized")
    print("build_receipt: self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--write", metavar="PATH", help="write a full-build receipt")
    action.add_argument("--verify", metavar="PATH", help="verify a full-build receipt")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--context", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.write and not args.verify:
        parser.error("one of --write, --verify, or --self-test is required")
    root = args.root.resolve()
    try:
        context = parse_context(args.context)
        if args.write:
            return write_receipt(args.write, root, context)
        return verify_receipt(args.verify, root, context, args.require_clean)
    except (OSError, ValueError) as error:
        print("build_receipt: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
