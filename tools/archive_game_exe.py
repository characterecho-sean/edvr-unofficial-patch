"""Keep a copy of the game executable before Steam overwrites it.

    python tools\\archive_game_exe.py --target steam --dry-run
    python tools\\archive_game_exe.py --target steam
    python tools\\archive_game_exe.py --target "D:\\...\\elite-dangerous-odyssey-64"
    python tools\\archive_game_exe.py --self-test

A game update replaces EliteDangerous64.exe in place. Mapping EDVR's bound
addresses onto the new build (tools\\build_diff.py map) needs the old build
next to the new one, and once Steam has finished there is no old build left
on disk: the 332841 -> 334473 map only worked because an old copy happened
to be in analysis\\. install_edvr.py calls archive() on every real install, so
the build EDVR was last installed against is kept without anyone remembering
to run this; run it by hand before letting Steam update a game EDVR has not
been installed into since.

It copies the game exe to analysis\\EliteDangerous64-<build>.exe, the name
build_diff.py's examples use. analysis\\ is git-ignored: game files are never
committed. The build is the last field of VersionInfo.txt's "Version" beside
the exe (2026.10.13.334473 -> 334473). The copy is written to a temp name,
hashed, and published under the final name only if nothing holds it yet
(_publish), so a partial copy never carries the final name and two runs
racing on the same build (installs from two worktrees) never replace each
other's copy.

  * the destination already holds the same bytes -> "already archived", 0;
  * the destination holds different bytes under the same build -> refused,
    1, nothing written (a mislabelled or modified copy needs a person);
  * no VersionInfo.txt or no build in it -> refused, 1, unless --build;
  * --dry-run says which of these would happen and writes nothing at all:
    no directory, no temp file.

--target takes install_edvr.py's spellings: steam, frontier, or the directory
holding the exe; its resolver (and steam_library.py's search behind it) is
the one install search. Nothing is launched or modified; only the archive
file is written.
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

GAME_EXE = "EliteDangerous64.exe"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_dest(root=REPO):
    """analysis\\ in the main checkout: a worktree's own analysis\\ is not shared
    (worktree.py junctions only third_party) and goes when the worktree does."""
    try:
        common = subprocess.run(["git", "-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        main = os.path.dirname(common) if os.path.basename(common) == ".git" else root
    except (OSError, subprocess.CalledProcessError):
        main = root
    return os.path.normpath(os.path.join(main, "analysis"))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pe_identity(path):
    """(TimeDateStamp, SizeOfImage): the pair EDVR's build gates check."""
    with open(path, "rb") as f:
        head = f.read(4096)
    if head[:2] != b"MZ" or len(head) < 0x40:
        raise ValueError("%s: not a PE file (no MZ header)" % path)
    pe = struct.unpack_from("<I", head, 0x3C)[0]
    if pe + 24 + 60 > len(head) or head[pe:pe + 4] != b"PE\0\0":
        raise ValueError("%s: not a PE file (no PE signature)" % path)
    stamp = struct.unpack_from("<I", head, pe + 8)[0]
    size_of_image = struct.unpack_from("<I", head, pe + 24 + 56)[0]
    return stamp, size_of_image


def build_of(exe):
    """The build number from VersionInfo.txt beside the exe, or None."""
    info = os.path.join(os.path.dirname(exe), "VersionInfo.txt")
    try:
        with open(info, "r", encoding="utf-8-sig") as f:
            version = json.load(f).get("Version", "")
    except (OSError, ValueError):
        return None
    last = str(version).rsplit(".", 1)[-1]
    return last if last.isdigit() else None


def archive(exe, dest_dir, build=None, dry_run=False, out=print):
    """Copy exe to dest_dir/EliteDangerous64-<build>.exe. Returns an exit code."""
    build = build or build_of(exe)
    if not build:
        out("[archive] refused: no build number in VersionInfo.txt beside %s; pass --build." % exe)
        return 1
    stamp, size_of_image = pe_identity(exe)
    source_hash = sha256_of(exe)
    target = os.path.join(dest_dir, "EliteDangerous64-%s.exe" % build)
    identity = "build %s, sha256 %s, PE stamp 0x%08X, SizeOfImage 0x%X" % (
        build, source_hash, stamp, size_of_image)
    if os.path.exists(target):
        if sha256_of(target) == source_hash:
            out("[archive] already archived: %s (%s)" % (target, identity))
            return 0
        out("[archive] refused: %s exists with different bytes; the installed exe is %s. "
            "Check which copy is right before replacing it." % (target, identity))
        return 1
    if dry_run:
        out("[archive] would archive %s -> %s (%s) (dry run: nothing written)" % (exe, target, identity))
        return 0
    os.makedirs(dest_dir, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=".archive-", suffix=".tmp", dir=dest_dir)
    os.close(fd)
    try:
        shutil.copyfile(exe, temp)
        if sha256_of(temp) != source_hash:
            out("[archive] refused: the copy's hash differs from %s (changed while copying?)" % exe)
            return 1
        if not _publish(temp, target):
            # Another run (another worktree's install) published this build first.
            if sha256_of(target) == source_hash:
                out("[archive] already archived: %s (%s)" % (target, identity))
                return 0
            out("[archive] refused: %s was archived concurrently with different bytes; the installed "
                "exe is %s. Check which copy is right before replacing it." % (target, identity))
            return 1
    finally:
        if os.path.exists(temp):
            os.remove(temp)
    out("[archive] archived %s -> %s (%s)" % (exe, target, identity))
    return 0


def _publish(temp, target):
    """Give temp the final name only if nothing holds it yet; False if something does.

    A hard link fails atomically on an existing name, so two runs racing past
    the existence check cannot replace each other's copy. os.replace would.
    Where a link is unsupported, Windows' rename is the fallback: it also never
    replaces an existing file."""
    try:
        os.link(temp, target)
        return True
    except FileExistsError:
        return False
    except OSError:
        if os.name != "nt":
            raise
    try:
        os.rename(temp, target)
        return True
    except FileExistsError:
        return False


def _fake_pe(path, stamp, size_of_image, filler=b""):
    pe = 0x80
    head = bytearray(0x200)
    head[0:2] = b"MZ"
    struct.pack_into("<I", head, 0x3C, pe)
    head[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<I", head, pe + 8, stamp)
    struct.pack_into("<I", head, pe + 24 + 56, size_of_image)
    with open(path, "wb") as f:
        f.write(bytes(head) + filler)


def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as td:
        game = os.path.join(td, "game")
        os.makedirs(game)
        exe = os.path.join(game, GAME_EXE)
        _fake_pe(exe, 0x6ACD513D, 0x640C000, b"one")
        with open(os.path.join(game, "VersionInfo.txt"), "w", encoding="utf-8") as f:
            f.write('{"Version":"2026.10.13.334473","executable":"EliteDangerous64.exe"}')
        dest = os.path.join(td, "analysis")
        target = os.path.join(dest, "EliteDangerous64-334473.exe")
        lines = []

        check(build_of(exe) == "334473", "build_of reads the last VersionInfo field")
        check(pe_identity(exe) == (0x6ACD513D, 0x640C000), "pe_identity reads stamp and SizeOfImage")

        check(archive(exe, dest, dry_run=True, out=lines.append) == 0 and "would archive" in lines[-1],
              "a dry run reports the copy it would make")
        check(not os.path.exists(dest), "a dry run creates no directory and no file")

        check(archive(exe, dest, out=lines.append) == 0, "first archive succeeds")
        check(os.path.isfile(target) and sha256_of(target) == sha256_of(exe), "archive copies the exact bytes")
        check(archive(exe, dest, out=lines.append) == 0 and "already archived" in lines[-1],
              "re-archiving the same bytes is a no-op")
        check(archive(exe, dest, dry_run=True, out=lines.append) == 0 and "already archived" in lines[-1],
              "a dry run of an archived build says so")

        _fake_pe(exe, 0x6ACD513D, 0x640C000, b"two")
        before = sha256_of(target)
        check(archive(exe, dest, out=lines.append) == 1 and "refused" in lines[-1],
              "different bytes under the same build are refused")
        check(archive(exe, dest, dry_run=True, out=lines.append) == 1 and "refused" in lines[-1],
              "a dry run reports the same refusal")
        check(sha256_of(target) == before, "a refused archive leaves the existing copy alone")
        check(not [n for n in os.listdir(dest) if n.startswith(".archive-")], "no temp file is left behind")

        # A concurrent run publishes the same build after this run's existence
        # check, while it copies: never replaced, whatever the racer's bytes.
        real_copy = shutil.copyfile
        for racer, want, word in ((b"two", 0, "already archived"), (b"three", 1, "refused")):
            race_dest = os.path.join(td, "race-%d" % want)
            os.makedirs(race_dest)
            race_target = os.path.join(race_dest, "EliteDangerous64-334473.exe")

            def racing_copy(src, dst, _racer=racer, _target=race_target):
                _fake_pe(_target, 0x6ACD513D, 0x640C000, _racer)
                return real_copy(src, dst)
            shutil.copyfile = racing_copy
            try:
                rc = archive(exe, race_dest, out=lines.append)
            finally:
                shutil.copyfile = real_copy
            with open(race_target, "rb") as f:
                kept = f.read()
            check(rc == want and word in lines[-1], "a racing archive with %r bytes reports %s" % (racer, word))
            check(kept.endswith(racer), "a racing archive never replaces the copy published first (%r)" % racer)
            check(os.listdir(race_dest) == ["EliteDangerous64-334473.exe"], "a lost race leaves no temp file")

        os.remove(os.path.join(game, "VersionInfo.txt"))
        check(archive(exe, dest, out=lines.append) == 1, "a missing VersionInfo.txt is refused")
        check(archive(exe, dest, build="999", out=lines.append) == 0
              and os.path.isfile(os.path.join(dest, "EliteDangerous64-999.exe")), "--build overrides VersionInfo")

        not_pe = os.path.join(td, "not_pe.exe")
        with open(not_pe, "wb") as f:
            f.write(b"\0" * 64)
        try:
            pe_identity(not_pe)
            check(False, "a non-PE file is rejected")
        except ValueError:
            pass

    for f in failures:
        print("[archive] self-test FAIL: %s" % f)
    print("[archive] self-test %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description="Copy the game exe to analysis\\ before an update replaces it.")
    p.add_argument("--target", default="steam",
                   help="steam, frontier, or the directory holding the game exe (install_edvr.py's spellings)")
    p.add_argument("--dest", help="archive folder (default: analysis\\ in the main checkout)")
    p.add_argument("--build", help="build number when VersionInfo.txt is missing")
    p.add_argument("--dry-run", action="store_true", help="say what would be archived; write nothing")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        from tools.install_edvr import resolve_target
    except ImportError:
        from install_edvr import resolve_target
    exe = os.path.join(resolve_target(args.target), GAME_EXE)
    return archive(exe, os.path.abspath(args.dest or default_dest()), args.build, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
