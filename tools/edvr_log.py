#!/usr/bin/env python3
"""Find the log a flight just wrote, and say whether it is the build you made.

    python tools/edvr_log.py --target steam --version
    python tools/edvr_log.py --target steam --expect-build HEAD
    python tools/edvr_log.py --target steam --grep "Stats\\[4[0-9]\\]"
    python tools/edvr_log.py --target steam --tail 80
    python tools/edvr_log.py --list

This is the sanctioned replacement for `Get-Content <some path> -Tail 200 |
Select-String ...`, retyped once a flight for a month.

The first line of any flight-log analysis is not "what do the counters
say", it is "is this log from the build I just installed". EDVR writes
that into every log it opens:

    version 0.14.1-93-gf78eba4 (build 68C0A1F2) -- this DLL was linked ...

--expect-build compares that against a git ref (HEAD by default) or a
literal string, and exits 2 when it does not match. A session has been
spent reading counters off a log that a stale DLL wrote; the exit code is
there so a script can refuse to go on.

Log names are edvr_<tag>_YYYYMMDD_HHMMSS.log, where the tag is `gfx` for
the d3d11 half and `vr` for the openvr half. They land in edvr_logs\\
beside the game unless log.dir in edvr.ini says otherwise, and this reads
that key rather than assuming the default -- a redirected log directory
is exactly when you would rather not be told there are no logs.

Read-only by construction: it opens files for reading and nothing else,
which is why it has no --dry-run.

Exit 0 when a log was read, 1 when none was found, 2 when --expect-build
did not match.
"""

import argparse
import os
import re
import sys
import tempfile

GAME_EXE = "EliteDangerous64.exe"
LOG_RE = re.compile(r"^edvr_(?P<tag>[a-z0-9]+)_(?P<stamp>\d{8}_\d{6})\.log$",
                    re.IGNORECASE)
# `version <string> (build <hex>)`, with the linked-at tail optional --
# log.cpp prints a shorter form when the timestamp will not convert.
#
# Every line Log::note() writes is prefixed `[HH:MM:SS.mmm] `, so the
# optional group is not decoration: anchored without it this matched the
# synthetic logs in the self-test and NOTHING in a real one. It still
# anchors at the start of the line rather than searching, so a sentence
# with the word "version" in it cannot be mistaken for the version note.
VERSION_RE = re.compile(r"^(?:\[[\d:.]+\]\s*)?version\s+(?P<ver>\S+)"
                        r"(?:\s+\(build\s+(?P<stamp>[0-9A-Fa-f]+)\))?")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_text(path):
    """Logs are ASCII in practice, but a shader name or a path can carry
    anything. Decode explicitly as UTF-8 -- never leave it to the console
    codepage, which is what turned an em-dash into mojibake on a public
    issue comment once."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def config_log_dir(game_dir):
    """log.dir out of the target's edvr.ini, if it sets one.

    A section-insensitive scan: the key is read as `log.dir` by the DLL,
    and the ini carries it under a [log] section as `dir`. Both spellings
    appear in the wild, so accept either rather than quietly finding
    nothing.
    """
    ini = os.path.join(game_dir, "edvr.ini")
    if not os.path.isfile(ini):
        return None
    section = ""
    try:
        text = read_text(ini)
    except OSError:
        return None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip().lower()
        val = val.strip().strip('"')
        full = key if "." in key else (section + "." + key if section else key)
        if full == "log.dir" and val:
            return val
    return None


def log_dir_for(game_dir):
    configured = config_log_dir(game_dir)
    if configured:
        return configured if os.path.isabs(configured) else \
            os.path.join(game_dir, configured)
    return os.path.join(game_dir, "edvr_logs")


def find_logs(directory, tag=None):
    """Newest first. The name carries the timestamp, so it sorts without
    stat()ing anything -- and a file copied off another rig keeps the time
    it was written rather than the time it was copied."""
    if not os.path.isdir(directory):
        return []
    out = []
    for name in os.listdir(directory):
        m = LOG_RE.match(name)
        if not m:
            continue
        if tag and m.group("tag").lower() != tag.lower():
            continue
        out.append((m.group("stamp"), m.group("tag"),
                    os.path.join(directory, name)))
    out.sort(reverse=True)
    return out


def version_line(text):
    """(line, version, link stamp) for the log's own version note."""
    for line in text.splitlines():
        m = VERSION_RE.search(line)
        if m:
            return line.strip(), m.group("ver"), m.group("stamp")
    return None, None, None


def describe_cmd(ref, root):
    """The `git describe` to run for --expect-build.

    `--dirty` and a commit-ish "cannot be used together" -- git calls that
    fatal. Asking for both made every `--expect-build HEAD` fail silently
    and fall back to comparing the log against the literal string "HEAD",
    which can never match: an instrument that reports a mismatch on every
    run looks exactly like one that works. So --dirty is asked for only
    where it means something, which is HEAD.
    """
    cmd = ["git", "-C", root, "describe", "--tags", "--always"]
    if ref in ("HEAD", "", None):
        cmd.append("--dirty")
    else:
        cmd.append(ref)
    return cmd


def expected_version(ref, root):
    """A git ref becomes `git describe`; anything else is taken literally,
    so a version copied out of a release note works too."""
    import subprocess
    try:
        out = subprocess.run(describe_cmd(ref, root), capture_output=True,
                             text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return ref


def version_matches(actual, expect):
    """A build's version is `git describe` output; the log may carry a
    -dirty or -N-g<hash> suffix the expectation does not, and the commit
    hash is the part that decides. Compare on the trailing g<hash> when
    both have one, and fall back to substring either way."""
    if not actual or not expect:
        return False
    if actual == expect:
        return True
    a = re.search(r"g([0-9a-f]{7,})", actual)
    e = re.search(r"g([0-9a-f]{7,})", expect)
    if a and e:
        short = min(len(a.group(1)), len(e.group(1)))
        return a.group(1)[:short] == e.group(1)[:short]
    # `git describe` on a tagged commit prints the bare tag, with no hash.
    return expect in actual or actual in expect


def _products_under(root):
    found = []
    products = os.path.join(root, "Products")
    if not os.path.isdir(products):
        return found
    for name in sorted(os.listdir(products)):
        leaf = os.path.join(products, name)
        if os.path.isfile(os.path.join(leaf, GAME_EXE)):
            found.append(os.path.normpath(leaf))
    return found


def resolve_target(spec):
    if spec not in ("steam", "frontier"):
        p = os.path.abspath(spec)
        if os.path.isdir(p):
            return p
        raise SystemExit("[edvr] no such directory: %s" % p)
    # Deliberately shares no code with install_edvr.py's fuller search:
    # this one only has to find a log, and a wrong guess here costs a
    # message rather than an overwritten DLL.
    if spec == "frontier":
        local = os.environ.get("LOCALAPPDATA", "")
        found = _products_under(os.path.join(local, "Frontier_Developments")) \
            if local else []
    else:
        found = []
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Valve\Steam") as k:
                steam = winreg.QueryValueEx(k, "SteamPath")[0]
        except (ImportError, OSError):
            steam = r"C:\Steam"
        found = _products_under(os.path.join(steam, "steamapps", "common",
                                             "Elite Dangerous"))
    if not found:
        raise SystemExit("[edvr] no %s install found; pass a path to "
                         "--target." % spec)
    if len(found) > 1:
        raise SystemExit("[edvr] %d installs found; name one:\n       %s"
                         % (len(found), "\n       ".join(found)))
    return found[0]


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Locate and read EDVR flight logs.")
    ap.add_argument("--target", default="steam",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--dir", default=None,
                    help="read this log directory directly, ignoring --target")
    ap.add_argument("--file", default=None, help="read exactly this log file")
    ap.add_argument("--tag", default="gfx",
                    help="gfx (the d3d11 half), vr (the openvr half), or all")
    ap.add_argument("--nth", type=int, default=0,
                    help="0 is the newest log, 1 the one before it")
    ap.add_argument("--list", action="store_true",
                    help="list the logs found and stop")
    ap.add_argument("--version", action="store_true",
                    help="print the log's version line and stop")
    ap.add_argument("--expect-build", default=None,
                    help="a git ref (HEAD) or literal version; exit 2 if the "
                         "log was not written by that build")
    ap.add_argument("--grep", default=None,
                    help="print only lines matching this regular expression")
    ap.add_argument("--tail", type=int, default=None,
                    help="print only the last N lines (after --grep)")
    ap.add_argument("--root", default=None,
                    help="repository to resolve --expect-build against")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against itself and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.file:
        path = os.path.abspath(args.file)
        if not os.path.isfile(path):
            print("[edvr] no such log: %s" % path)
            return 1
        logs = [("", "", path)]
    else:
        directory = args.dir or log_dir_for(resolve_target(args.target))
        logs = find_logs(directory, None if args.tag == "all" else args.tag)
        if not logs:
            print("[edvr] no edvr_%s_*.log in %s"
                  % (args.tag, directory))
            return 1
        if args.list:
            print("[edvr] %d log(s) in %s:" % (len(logs), directory))
            for stamp, tag, p in logs:
                print("       %-4s %s  %s"
                      % (tag, stamp, os.path.basename(p)))
            return 0
        if args.nth >= len(logs):
            print("[edvr] only %d log(s); --nth %d is past the end"
                  % (len(logs), args.nth))
            return 1
        logs = [logs[args.nth]]

    path = logs[0][2]
    text = read_text(path)
    print("[edvr] %s  (%d lines, %.1f KB)"
          % (path, text.count("\n") + 1, len(text) / 1024.0))

    line, ver, stamp = version_line(text)
    if line:
        print("[edvr] %s" % line)
    else:
        print("[edvr] WARNING: this log has no version line -- it may be "
              "truncated, or not an EDVR log.")

    if args.expect_build:
        want = expected_version(args.expect_build,
                                os.path.abspath(args.root) if args.root
                                else repo_root())
        if version_matches(ver, want):
            print("[edvr] build matches: %s" % want)
        else:
            print("[edvr] BUILD MISMATCH\n"
                  "       log says   %s\n"
                  "       expected   %s\n"
                  "       This flight is not evidence about that build. "
                  "Reinstall and fly again."
                  % (ver or "(nothing)", want))
            return 2

    if args.version:
        return 0

    lines = text.splitlines()
    if args.grep:
        try:
            rx = re.compile(args.grep)
        except re.error as e:
            print("[edvr] bad --grep pattern: %s" % e)
            return 1
        lines = [l for l in lines if rx.search(l)]
        print("[edvr] %d line(s) match %s" % (len(lines), args.grep))
    if args.tail is not None:
        lines = lines[-args.tail:]
    if args.grep or args.tail is not None:
        for l in lines:
            print(l)
    return 0


def self_test():
    ok = True

    for name, want in (("edvr_gfx_20260910_042313.log", "gfx"),
                       ("edvr_vr_20260910_042313.log", "vr"),
                       ("notalog.txt", None)):
        m = LOG_RE.match(name)
        got = m.group("tag") if m else None
        if got != want:
            print("LOG_RE on %s -> %r, want %r" % (name, got, want))
            ok = False

    # The exact line log.cpp writes, long form and short.
    long_form = ("[00:00:00.001] version 0.14.1-93-gf78eba4 (build 68C0A1F2) "
                 "-- this DLL was linked 2026-09-09 20:34:39 UTC")
    _, ver, stamp = version_line("first line\n" + long_form + "\nmore\n")
    if ver != "0.14.1-93-gf78eba4" or stamp != "68C0A1F2":
        print("version_line long form -> %r %r" % (ver, stamp))
        ok = False
    _, ver2, stamp2 = version_line("[00:00:00.001] version unversioned test "
                                   "build\n")
    if ver2 != "unversioned":
        print("version_line short form -> %r" % ver2)
        ok = False
    # And a sentence that merely contains the word is not the version note.
    _, ver3, _ = version_line("[00:00:01.500] the version of the shader is 3\n")
    if ver3 is not None:
        print("version_line matched prose: %r" % ver3)
        ok = False

    # Matching has to tolerate the suffixes `git describe` adds, and must
    # still refuse a genuinely different commit -- the whole point.
    # HEAD asks about the working tree; a named ref must not, because
    # git refuses the combination outright.
    if "--dirty" not in describe_cmd("HEAD", "."):
        print("describe_cmd(HEAD) does not ask about a dirty tree")
        ok = False
    named = describe_cmd("v0.14.0", ".")
    if "--dirty" in named or named[-1] != "v0.14.0":
        print("describe_cmd(v0.14.0) -> %r" % named)
        ok = False

    for a, e, want in (
            ("0.14.1-93-gf78eba4", "0.14.1-93-gf78eba4", True),
            ("0.14.1-93-gf78eba4", "0.14.1-93-gf78eba4-dirty", True),
            ("0.14.1-93-gf78eba4", "0.14.1-40-gc468661", False),
            ("v0.14.0", "v0.14.0", True),
            (None, "v0.14.0", False)):
        if version_matches(a, e) != want:
            print("version_matches(%r, %r) != %s" % (a, e, want))
            ok = False

    tmp = tempfile.mkdtemp(prefix="edvr_log_test_")
    try:
        game = os.path.join(tmp, "game")
        logs = os.path.join(game, "edvr_logs")
        os.makedirs(logs)
        for stamp_s in ("20260910_040000", "20260910_050000"):
            with open(os.path.join(logs, "edvr_gfx_%s.log" % stamp_s),
                      "wb") as f:
                # With the timestamp prefix Log::note() really writes: a
                # fixture without it is what hid a regex that matched
                # nothing in the field.
                f.write(("[00:00:00.001] version 0.14.1-93-gf78eba4 "
                         "(build 68C0A1F2) -- this DLL was linked "
                         "2026-09-09 20:34:39 UTC\n"
                         "[00:00:12.400] Stats[40] ships 3\n"
                         "[00:00:12.400] Stats[41] ships 0\n"
                         "[00:00:12.401] something else\n").encode("utf-8"))
        with open(os.path.join(logs, "edvr_vr_20260910_060000.log"),
                  "wb") as f:
            f.write(b"[00:00:00.001] version 0.14.1-93-gf78eba4 "
                    b"(build 68C0A1F2)\n")

        # The default log directory, and the ini's override, both found.
        if log_dir_for(game) != logs:
            print("log_dir_for did not default to edvr_logs")
            ok = False
        other = os.path.join(tmp, "elsewhere")
        os.makedirs(other)
        with open(os.path.join(game, "edvr.ini"), "wb") as f:
            f.write(("[log]\ndir = %s\n" % other).encode("utf-8"))
        if log_dir_for(game) != other:
            print("log_dir_for ignored log.dir in the ini")
            ok = False
        os.remove(os.path.join(game, "edvr.ini"))

        # Newest first, and the tag filter separates the two halves.
        found = find_logs(logs, "gfx")
        if len(found) != 2 or not found[0][2].endswith("050000.log"):
            print("find_logs did not return the newest gfx log first: %r"
                  % [os.path.basename(p) for _, _, p in found])
            ok = False
        if len(find_logs(logs, "vr")) != 1:
            print("find_logs tag filter is wrong")
            ok = False
        if len(find_logs(logs)) != 3:
            print("find_logs with no tag did not return all three")
            ok = False

        newest = os.path.join(logs, "edvr_gfx_20260910_050000.log")
        if main(["--file", newest, "--version"]) != 0:
            print("--version on a good log did not exit 0")
            ok = False
        # The exit code a caller keys off: 2, distinct from 1.
        rc = main(["--file", newest, "--expect-build",
                   "0.14.1-40-gc468661", "--version"])
        if rc != 2:
            print("a build mismatch exited %d, want 2" % rc)
            ok = False
        rc = main(["--file", newest, "--expect-build",
                   "0.14.1-93-gf78eba4", "--version"])
        if rc != 0:
            print("a matching build exited %d, want 0" % rc)
            ok = False
        if main(["--dir", logs, "--tag", "gfx", "--grep", r"Stats\[4[01]\]"]) \
                != 0:
            print("--grep exited nonzero on a readable log")
            ok = False
        if main(["--file", os.path.join(logs, "nope.log")]) != 1:
            print("a missing log did not exit 1")
            ok = False
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
