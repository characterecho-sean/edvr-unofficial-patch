#!/usr/bin/env python3
"""Put a freshly built EDVR next to the game, and prove it landed.

    python tools/install_edvr.py --target steam --dry-run
    python tools/install_edvr.py --target steam
    python tools/install_edvr.py --target frontier --openvr --tag stepped
    python tools/install_edvr.py --target steam --verify-only

This is the sanctioned replacement for the copy-and-hope one-liner. That
one-liner is why both game directories on this rig carry dozens of backups
named six different ways (`pre-taa-20260903`, `bak-skip`, `bak-lodfix`),
and why "was that flight even on the build I just made" has had to be
answered by squinting at timestamps.

Three things it does that a `copy` does not:

  * REFUSES while Elite is running. A copy onto a loaded DLL either fails
    with a sharing violation or -- worse, if the game has not touched it
    yet -- succeeds and is then thrown away by the next launch. The check
    is by image name, so any install being open blocks any install: it
    over-refuses on purpose, and --force is the escape.
  * VERIFIES by SHA-256 after copying, source against destination, and
    says both hashes. An outdated DLL has invalidated a test flight
    before, and a hash is the only thing that can tell you it did.
  * BACKS UP under one naming scheme, `<name>.pre-<tag>-<stamp>.bak`,
    where the tag defaults to the short git hash of the tree being
    installed. A backup whose name says which commit it preceded is worth
    keeping; `bak-skip` is not.

edvr.ini is NOT copied unless --ini says so. It is the one file in the
payload that carries the settings of whoever flew last, a reinstall does
not undo an edit to it, and clobbering it has cost a session. --ini backs
it up first and says loudly what it did.

--dry-run prints the plan and writes nothing at all -- no copies, no
backups, no directories. The self-test asserts that, because a --dry-run
that wrote files is a bug this project has already shipped once.

Exit 0 when everything asked for landed and verified, 1 otherwise.
"""

import argparse
import datetime
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

GAME_EXE = "EliteDangerous64.exe"

# What can be installed, and where each piece goes. The two halves do NOT
# go in the same place: d3d11.dll sits beside the exe as an ADDED file,
# while openvr_api.dll REPLACES a file the game owns, in Openvr\win64,
# whose original must already be renamed openvr_api_orig.dll. Getting that
# backwards produces an install that looks complete and does nothing.
PAYLOAD = {
    "dll": ("build/d3d11.dll", "d3d11.dll"),
    "openvr": ("build/openvr_api.dll", "Openvr/win64/openvr_api.dll"),
    "dlss": ("build/nvngx_dlss.dll", "nvngx_dlss.dll"),
    "ini": ("edvr.ini", "edvr.ini"),
}


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def short_hash(root):
    """The commit being installed, for the backup name. Never fatal."""
    try:
        out = subprocess.run(["git", "-C", root, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "nogit"


def backup_name(dst, tag, now=None):
    """One scheme, always: <name>.pre-<tag>-<YYYYMMDD-HHMMSS>.bak"""
    now = now or datetime.datetime.now()
    return "%s.pre-%s-%s.bak" % (dst, tag, now.strftime("%Y%m%d-%H%M%S"))


def game_running():
    """True if any EliteDangerous64.exe is running.

    By image name, not by path. The installer proper is path-aware; this
    is not, and the difference only ever makes it refuse when it could
    have allowed, which is the safe direction for a tool that overwrites
    a DLL the game may be holding open.
    """
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + GAME_EXE],
                             capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return False
    return GAME_EXE.lower() in (out.stdout or "").lower()


def _products_under(root):
    """Product leaves under an install root that actually hold the game."""
    found = []
    products = os.path.join(root, "Products")
    if not os.path.isdir(products):
        return found
    for name in sorted(os.listdir(products)):
        leaf = os.path.join(products, name)
        if os.path.isfile(os.path.join(leaf, GAME_EXE)):
            found.append(os.path.normpath(leaf))
    return found


def steam_dirs():
    roots = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Valve\Steam") as k:
            roots.append(winreg.QueryValueEx(k, "SteamPath")[0])
    except (ImportError, OSError):
        pass
    # Every library, not just the default one: the game is routinely on a
    # different drive from Steam itself.
    libs = list(roots)
    for r in list(roots):
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        if not os.path.isfile(vdf):
            continue
        try:
            with open(vdf, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    parts = line.split('"')
                    if len(parts) >= 5 and parts[1] == "path":
                        libs.append(parts[3].replace("\\\\", "\\"))
        except OSError:
            pass
    # A plain fallback for a rig whose registry entry has gone missing.
    libs.append(r"C:\Steam")
    # Deduplicated case- and separator-insensitively. The registry hands
    # back `c:/steam` and the fallback is `C:\Steam`; compared as strings
    # those are two installs, and the tool then refuses to act because it
    # has "found two" -- the same directory, twice.
    out, seen = [], set()
    for lib in libs:
        base = os.path.join(lib, "steamapps", "common", "Elite Dangerous")
        for leaf in _products_under(base):
            key = os.path.normcase(os.path.normpath(leaf))
            if key not in seen:
                seen.add(key)
                out.append(os.path.normpath(leaf))
    return out


def frontier_dirs():
    local = os.environ.get("LOCALAPPDATA", "")
    if not local:
        return []
    return _products_under(os.path.join(local, "Frontier_Developments"))


def resolve_target(spec):
    """A store name, or a path to the directory holding the game exe."""
    if spec == "steam":
        found = steam_dirs()
    elif spec == "frontier":
        found = frontier_dirs()
    else:
        p = os.path.abspath(spec)
        if not os.path.isfile(os.path.join(p, GAME_EXE)):
            raise SystemExit(
                "[edvr] %s holds no %s -- point --target at the directory\n"
                "       that has the game executable in it." % (p, GAME_EXE))
        return p
    if not found:
        raise SystemExit("[edvr] no %s install found. Pass a path to "
                         "--target instead." % spec)
    if len(found) > 1:
        raise SystemExit("[edvr] %d %s installs found; name one with "
                         "--target <path>:\n       %s"
                         % (len(found), spec, "\n       ".join(found)))
    return found[0]


def build_plan(root, target, want):
    """[(key, src, dst)] for the pieces asked for. Missing sources are fatal
    here rather than half way through copying."""
    plan, missing = [], []
    for key in want:
        rel_src, rel_dst = PAYLOAD[key]
        src = os.path.join(root, rel_src.replace("/", os.sep))
        dst = os.path.join(target, rel_dst.replace("/", os.sep))
        if not os.path.isfile(src):
            missing.append((key, src))
        plan.append((key, src, dst))
    if missing:
        lines = ["[edvr] ERROR: nothing to install from:"]
        for key, src in missing:
            lines.append("       %-7s %s" % (key, src))
        lines.append("       Run build.bat first (by absolute path).")
        raise SystemExit("\n".join(lines))
    return plan


def check_openvr_original(target):
    """Our openvr_api.dll is a proxy: it forwards to the game's original,
    which the install step renames once. Without that rename the proxy has
    nothing to forward to and the VR half is dead on arrival -- silently,
    since the game still starts."""
    orig = os.path.join(target, "Openvr", "win64", "openvr_api_orig.dll")
    return os.path.isfile(orig)


def do_verify(plan):
    ok = True
    print("[edvr] verify (build vs installed):")
    for key, src, dst in plan:
        if not os.path.isfile(dst):
            print("       %-7s ABSENT   %s" % (key, dst))
            ok = False
            continue
        a, b = sha256(src), sha256(dst)
        if a == b:
            print("       %-7s match    %s" % (key, a))
        else:
            print("       %-7s MISMATCH" % key)
            print("               build     %s" % a)
            print("               installed %s" % b)
            ok = False
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Install a built EDVR into a game directory, verified.")
    ap.add_argument("--target", default="steam",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--root", default=None,
                    help="the tree to install FROM; defaults to this "
                         "script's own repository, which is what you want "
                         "unless you are installing another worktree's build")
    ap.add_argument("--dll", action="store_true",
                    help="install build/d3d11.dll (the default payload)")
    ap.add_argument("--openvr", action="store_true",
                    help="also install build/openvr_api.dll into Openvr/win64")
    ap.add_argument("--dlss", action="store_true",
                    help="also install build/nvngx_dlss.dll")
    ap.add_argument("--ini", action="store_true",
                    help="also overwrite the target's edvr.ini with the "
                         "repository's -- this discards tuned settings")
    ap.add_argument("--all", action="store_true",
                    help="dll + openvr + dlss (never ini)")
    ap.add_argument("--tag", default=None,
                    help="word for the backup name; defaults to the short "
                         "git hash of this tree")
    ap.add_argument("--no-backup", action="store_true",
                    help="replace without keeping the previous file")
    ap.add_argument("--force", action="store_true",
                    help="install even though the game appears to be running")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan and write nothing")
    ap.add_argument("--verify-only", action="store_true",
                    help="compare installed against built; write nothing")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    want = []
    if args.all:
        want = ["dll", "openvr", "dlss"]
    else:
        if args.dll or not (args.openvr or args.dlss or args.ini):
            want.append("dll")
        if args.openvr:
            want.append("openvr")
        if args.dlss:
            want.append("dlss")
    if args.ini:
        want.append("ini")

    root = os.path.abspath(args.root) if args.root else repo_root()
    target = resolve_target(args.target)
    plan = build_plan(root, target, want)

    print("[edvr] target: %s" % target)

    if args.verify_only:
        return 0 if do_verify(plan) else 1

    if not args.force and game_running():
        print("[edvr] ERROR: %s is running. Close the game, or pass --force\n"
              "       if you know this install is not the one that is open."
              % GAME_EXE)
        return 1

    tag = args.tag or short_hash(root)
    stamp = datetime.datetime.now()

    print("[edvr] plan%s:" % (" (DRY RUN -- nothing will be written)"
                              if args.dry_run else ""))
    for key, src, dst in plan:
        exists = os.path.isfile(dst)
        what = "replace" if exists else "add"
        print("       %-7s %s" % (key, dst))
        print("               %s %s" % (what, "" if not exists else
                                        ("-> " + os.path.basename(
                                            backup_name(dst, tag, stamp)))))
        if key == "ini" and exists:
            print("               NOTE: this discards the settings in the "
                  "target's edvr.ini.")

    if "openvr" in want and not check_openvr_original(target):
        print("[edvr] ERROR: Openvr\\win64\\openvr_api_orig.dll is missing.\n"
              "       Our openvr_api.dll forwards to the game's original, so\n"
              "       it must be renamed to openvr_api_orig.dll first. Without\n"
              "       that the VR half loads and does nothing, and the game\n"
              "       still starts, so nothing tells you.")
        return 1

    if args.dry_run:
        print("[edvr] dry run: wrote nothing.")
        return 0

    for key, src, dst in plan:
        parent = os.path.dirname(dst)
        if parent and not os.path.isdir(parent):
            os.makedirs(parent)
        if os.path.isfile(dst) and not args.no_backup:
            bak = backup_name(dst, tag, stamp)
            shutil.copy2(dst, bak)
            print("[edvr] kept   %s" % os.path.basename(bak))
        shutil.copy2(src, dst)
        print("[edvr] copied %s -> %s" % (os.path.relpath(src, root), dst))

    ok = do_verify(plan)
    if ok:
        print("[edvr] installed and verified. Log to look for after the "
              "flight:\n"
              "       python tools/edvr_log.py --target %s --version"
              % args.target)
    else:
        print("[edvr] ERROR: a file does not match what was built. Do not "
              "treat the next flight as evidence.")
    return 0 if ok else 1


def self_test():
    ok = True

    # The backup scheme is one string in one place; if it drifts, the
    # litter comes back.
    when = datetime.datetime(2026, 9, 10, 4, 23, 13)
    got = backup_name(r"C:\g\d3d11.dll", "f78eba4", when)
    want = r"C:\g\d3d11.dll.pre-f78eba4-20260910-042313.bak"
    if got != want:
        print("backup_name -> %s, want %s" % (got, want))
        ok = False

    tmp = tempfile.mkdtemp(prefix="edvr_install_test_")
    try:
        # A fake repo and a fake game directory.
        root = os.path.join(tmp, "repo")
        os.makedirs(os.path.join(root, "build"))
        for rel, body in (("build/d3d11.dll", b"NEW-DLL"),
                          ("edvr.ini", b"[fix]\n")):
            with open(os.path.join(root, rel.replace("/", os.sep)), "wb") as f:
                f.write(body)
        game = os.path.join(tmp, "game")
        os.makedirs(game)
        with open(os.path.join(game, GAME_EXE), "wb") as f:
            f.write(b"exe")
        with open(os.path.join(game, "d3d11.dll"), "wb") as f:
            f.write(b"OLD-DLL")

        if sha256(os.path.join(root, "build", "d3d11.dll")) == \
           sha256(os.path.join(game, "d3d11.dll")):
            print("sha256 called two different files equal")
            ok = False

        # resolve_target on an explicit path finds the exe.
        if resolve_target(game) != os.path.abspath(game):
            print("resolve_target did not accept a real game directory")
            ok = False

        # A missing source is refused BEFORE anything is copied.
        try:
            build_plan(root, game, ["openvr"])
            print("build_plan accepted a missing source")
            ok = False
        except SystemExit:
            pass

        # The property that matters most: --dry-run writes nothing. Not
        # the copy, not the backup, not a directory. This project has
        # shipped a --dry-run that wrote files.
        before = sorted(os.listdir(game))
        before_bytes = open(os.path.join(game, "d3d11.dll"), "rb").read()
        rc = main(["--root", root, "--target", game, "--dll", "--dry-run"])
        after = sorted(os.listdir(game))
        after_bytes = open(os.path.join(game, "d3d11.dll"), "rb").read()
        if rc != 0:
            print("dry run exited %d" % rc)
            ok = False
        if before != after:
            print("dry run changed the directory: %s -> %s" % (before, after))
            ok = False
        if before_bytes != after_bytes:
            print("dry run overwrote the installed file")
            ok = False

        # And the real thing does copy, back up under the scheme, and
        # verify. --force because a real game may be running on this rig.
        rc = main(["--root", root, "--target", game, "--dll",
                   "--tag", "selftest", "--force"])
        if rc != 0:
            print("install exited %d" % rc)
            ok = False
        if open(os.path.join(game, "d3d11.dll"), "rb").read() != b"NEW-DLL":
            print("install did not replace the file")
            ok = False
        baks = [n for n in os.listdir(game) if ".pre-selftest-" in n]
        if len(baks) != 1:
            print("expected one backup, found %r" % baks)
            ok = False
        elif open(os.path.join(game, baks[0]), "rb").read() != b"OLD-DLL":
            print("the backup does not hold the previous file")
            ok = False

        # verify-only agrees, and disagrees once the file is tampered with.
        if main(["--root", root, "--target", game, "--dll",
                 "--verify-only"]) != 0:
            print("verify-only failed on a good install")
            ok = False
        with open(os.path.join(game, "d3d11.dll"), "wb") as f:
            f.write(b"STALE")
        if main(["--root", root, "--target", game, "--dll",
                 "--verify-only"]) == 0:
            print("verify-only passed a stale install")
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
