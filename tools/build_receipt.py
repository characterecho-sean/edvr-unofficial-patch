"""Record and verify the input identity of a green native build.

The full build is allowed to run on a dirty tree so the test suite can check
an uncommitted change.  After that change is committed, the DLL-only
promotion build uses this receipt to prove that the compiled inputs are the
same while allowing the git commit metadata, and therefore EDVR's displayed
version, to change.

Documentation under docs/ is not a compiled input: the receipt's only
consumer is the DLL-only promotion, which compiles no doc content, so a
doc-only change must not force another full build.

THE INPUT RULE (2026-10-01).  The fingerprint covers exactly:
  1. every file git tracks;
  2. every untracked file git does not ignore (a new source file that has not
     been `git add`ed yet is still an input);
  3. every file under the named ignored dependency folders, DEPENDENCY_ROOTS:
     the fetched NVIDIA SDK, the FidelityFX checkout and the pinned OpenXR
     loader are git-ignored (licence, size) but ARE compiled or linked into the
     DLLs, so a change to one must change the fingerprint.
Everything else git ignores is not an input: reviews/ (local review write-ups),
logs, build output, scratch.  Writing or updating a local review therefore
cannot make `build.bat --dll-only` refuse when no compiled input changed.  The
tooling and output roots in EXCLUDED_ROOTS (and docs/, above) stay outside the
identity whatever git says about them.  Outside a git work tree there is no
ignore list to ask, so the older rule applies: everything but EXCLUDED_ROOTS.
A receipt written before this rule fingerprinted the ignored files too, so it
does not match a later check: the next promotion needs one full build.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from datetime import datetime, timezone


SCHEMA = 1
EXCLUDED_ROOTS = {".git", ".claude", ".codex", ".vs", "build", "build - Copy",
                  "dist", "analysis", "edvr_logs", "docs"}
EXCLUDED_PARTS = {"__pycache__", ".pytest_cache"}
# Git-ignored folders that are compiled inputs and so stay in the fingerprint
# (see THE INPUT RULE above).  Relative to the repository root, forward slashes.
DEPENDENCY_ROOTS = ("third_party/ngx", "third_party/ffx-dx11", "third_party/openxr/loader")


def _excluded(relative):
    return (relative.parts and relative.parts[0] in EXCLUDED_ROOTS) or \
        any(part in EXCLUDED_PARTS or part.endswith(".pyc") for part in relative.parts)


def git_listed(root):
    """Return the relative paths git lists as inputs (tracked, plus untracked
    files it does not ignore), or None when root is not itself a git work tree
    or git cannot answer: then the caller falls back to the older walk."""
    root = Path(root).resolve()
    try:
        top = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if top.returncode or Path(top.stdout.decode("utf-8", "replace").strip()).resolve() != root:
            return None
        listed = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
                                 "--exclude-standard"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError:
        return None
    if listed.returncode:
        return None
    return [name for name in listed.stdout.decode("utf-8", "surrogateescape").split("\0") if name]


def input_files(root):
    """Return every workspace file that can be a build input (THE INPUT RULE
    in the module docstring): git's tracked and untracked-not-ignored files plus
    the ignored dependency folders, minus the excluded roots; everything but
    the excluded roots where root is not a git work tree."""
    root = Path(root).resolve()
    listed = git_listed(root)
    candidates = []
    if listed is None:
        candidates = [path for path in root.rglob("*") if path.is_file()]
    else:
        candidates = [root / name for name in listed]
        for dependency in DEPENDENCY_ROOTS:
            base = root / dependency
            if base.is_dir():
                candidates.extend(path for path in base.rglob("*") if path.is_file())
    files = {}
    for path in candidates:
        if not path.is_file():
            continue   # tracked but deleted: its absence is the change
        relative = path.relative_to(root)
        if _excluded(relative):
            continue
        files[relative.as_posix()] = path
    return sorted(files.items())


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


def _remove_tree(path):
    """Delete a directory tree even where git left read-only object files in it (Windows)."""
    def clear_and_retry(function, target, _error):
        os.chmod(target, stat.S_IWRITE)
        function(target)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=clear_and_retry)
    else:
        shutil.rmtree(path, onerror=clear_and_retry)


def self_test():
    temporary = tempfile.mkdtemp(prefix="edvr-build-receipt-")
    try:
        _self_test_in_git_tree(Path(temporary) / "git")
        _self_test_without_git(Path(temporary) / "plain")
    finally:
        _remove_tree(temporary)
    print("build_receipt: self-test passed")
    return 0


def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(text, bytes):
        path.write_bytes(text)
    else:
        path.write_text(text, encoding="utf-8")


def _expect_same(root, expected, why):
    if input_fingerprint(root) != expected:
        raise AssertionError(why)


def _expect_changed(root, previous, why):
    if input_fingerprint(root) == previous:
        raise AssertionError(why)


def _self_test_in_git_tree(root):
    """THE INPUT RULE, in a real git work tree with the repository's ignore patterns."""
    root.mkdir(parents=True)

    def git(*args):
        result = subprocess.run(["git", "-C", str(root), *args], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        if result.returncode:
            raise AssertionError("git %s failed: %s" % (" ".join(args), result.stderr.decode("utf-8", "replace")))

    git("init", "-q")
    _write(root / ".gitignore", "build/\ndist/\n__pycache__/\n*.pyc\nreviews/\nscratch.log\n"
           "third_party/ngx/\nthird_party/ffx-dx11/\nthird_party/openxr/loader/\n")
    _write(root / "source.txt", "one")
    _write(root / "tools" / "tool.py", "one")
    git("add", ".gitignore", "source.txt", "tools/tool.py")
    # Not tracked, not ignored: a new source file that has not been added yet is an input.
    _write(root / "src" / "new_file.cpp", "one")
    # Outside the identity: outputs, docs, caches, local reviews and scratch.
    _write(root / "build" / "ignored.txt", "one")
    _write(root / "dist" / "ignored.txt", "one")
    _write(root / "docs" / "notes.md", "one")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"one")
    _write(root / "reviews" / "review.md", "one")
    _write(root / "scratch.log", "one")
    # Ignored by git and still compiled inputs.
    _write(root / "third_party" / "ngx" / "lib" / "sdk.lib", "one")
    _write(root / "third_party" / "ffx-dx11" / "ffx.h", "one")
    _write(root / "third_party" / "openxr" / "loader" / "openxr_loader.dll", "one")
    first = input_fingerprint(root)

    # Changes that must not matter.
    _write(root / "build" / "ignored.txt", "two")
    _write(root / "docs" / "notes.md", "two")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"two")
    _write(root / "reviews" / "review.md", "two")          # a local review updated ...
    _write(root / "reviews" / "second-review.md", "new")   # ... and a new one written
    _write(root / "scratch.log", "two")
    _expect_same(root, first, "build outputs, docs, caches, reviews or other ignored scratch affect the input fingerprint")

    # Changes that must matter, each undone before the next.
    for relative, why in (
            ("third_party/ngx/lib/sdk.lib", "a change under third_party/ngx does not affect the input fingerprint"),
            ("third_party/ffx-dx11/ffx.h", "a change under third_party/ffx-dx11 does not affect the input fingerprint"),
            ("third_party/openxr/loader/openxr_loader.dll",
             "a change under third_party/openxr/loader does not affect the input fingerprint"),
            ("source.txt", "a tracked source change does not affect the input fingerprint"),
            ("tools/tool.py", "a tracked tool change does not affect the input fingerprint"),
            ("src/new_file.cpp", "an untracked, not-ignored file's change does not affect the input fingerprint")):
        path = root / relative
        original = path.read_text(encoding="utf-8")
        _write(path, "two")
        _expect_changed(root, first, why)
        _write(path, original)
        _expect_same(root, first, "restoring %s did not restore the input fingerprint" % relative)
    _write(root / "src" / "another_new_file.cpp", "one")
    _expect_changed(root, first, "adding an untracked, not-ignored file does not affect the input fingerprint")
    (root / "src" / "another_new_file.cpp").unlink()
    _write(root / "third_party" / "ngx" / "lib" / "added.lib", "one")
    _expect_changed(root, first, "adding a file under third_party/ngx does not affect the input fingerprint")
    (root / "third_party" / "ngx" / "lib" / "added.lib").unlink()
    (root / "source.txt").unlink()
    _expect_changed(root, first, "deleting a tracked file does not affect the input fingerprint")
    _write(root / "source.txt", "one")
    _expect_same(root, first, "the input fingerprint is not restored once every change is undone")


def _self_test_without_git(root):
    """Outside a git work tree there is no ignore list: everything but the excluded roots counts."""
    (root / "build").mkdir(parents=True)
    (root / "docs").mkdir()
    (root / "tools" / "__pycache__").mkdir(parents=True)
    _write(root / "source.txt", "one")
    _write(root / "build" / "ignored.txt", "one")
    _write(root / "docs" / "notes.md", "one")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"one")
    first = input_fingerprint(root)
    _write(root / "build" / "ignored.txt", "two")
    _write(root / "docs" / "notes.md", "two")
    _write(root / "tools" / "__pycache__" / "ignored.pyc", b"two")
    _expect_same(root, first, "build outputs or docs affect the input fingerprint")
    _write(root / "source.txt", "two")
    _expect_changed(root, first, "source changes do not affect the input fingerprint")
    if parse_context(["z=last", "a=first"]) != {"a": "first", "z": "last"}:
        raise AssertionError("context keys are not canonicalized")


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
