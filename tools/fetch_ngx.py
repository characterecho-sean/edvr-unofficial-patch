"""Fetch NVIDIA's DLSS SDK -- only what the build uses -- pinned and verified.

    python tools/fetch_ngx.py [--into DIR] [--dry-run]
    python tools/fetch_ngx.py --verify DIR
    python tools/fetch_ngx.py --self-test

A sparse, shallow clone of https://github.com/NVIDIA/DLSS at ONE pinned
commit (the 310.9.1 SDK), taking the headers, the x64 static library, the
release runtime and NVIDIA's licence: about 200 MB on disk, most of it the
runtimes. The runtime's SHA-256 is pinned here as well and checked after
every fetch and by every build, so an SDK that moved fails loudly instead of
shipping a different DLL than the one the flights verified. Updating the SDK
is a deliberate commit that changes COMMIT and DLL_SHA256 together, with the
flight that verified the new runtime.

Where it goes: %LOCALAPPDATA%\\EDVR\\ngx-sdk by default -- one copy per machine,
which every checkout and worktree finds (build.bat looks at EDVR_NGX_SDK,
then the checkout's own third_party\\ngx, then here) -- or --into somewhere
else, third_party\\ngx for a checkout that must be self-contained. The SDK's
licence keeps it out of this tree: it may ship inside an application (the
installer carries the runtime) and not on its own, and not under an open
source licence, which a copy in this repository would be.

--verify DIR checks an existing copy -- the files the build needs, the
runtime's hash -- and exits non-zero on any mismatch; build.bat runs it
before it links the SDK in.

--dry-run prints the git commands a fetch would run and runs none of them: no
directory, no clone, no network. --self-test checks the verification and the
fetch's command sequence against fixtures in the temp folder with subprocess
and the network replaced for the whole run, so it never fetches anything;
build.bat runs it before it trusts --verify with the build.
"""
import argparse
import hashlib
import os
import subprocess
import sys

URL = "https://github.com/NVIDIA/DLSS"
# The pinned SDK: the commit and the runtime it carries. Both change together.
COMMIT = "374959484e79a640feaba44c93ac8cfb0a03f5b5"   # "DLSS 310.9.1 SDK", 2026-09-21
DLL_SHA256 = "3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"
DLL_VERSION = "310.9.1"
PATHS = ["include", "lib/Windows_x86_64/x64", "lib/Windows_x86_64/rel"]

HEADER = os.path.join("include", "nvsdk_ngx.h")
LIB = os.path.join("lib", "Windows_x86_64", "x64", "nvsdk_ngx_s.lib")
DLL = os.path.join("lib", "Windows_x86_64", "rel", "nvngx_dlss.dll")
LICENCE = "LICENSE.txt"


def default_dest():
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        base = os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(base, "EDVR", "ngx-sdk")


def run(*args, dry_run=False):
    print("(dry run) +" if dry_run else "+", " ".join(args))
    if not dry_run:
        subprocess.check_call(args)


def head_of(dest):
    try:
        out = subprocess.check_output(["git", "-C", dest, "rev-parse", "HEAD"],
                                      stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify(dest, quiet=False):
    """The files the build needs, and the runtime's hash. Returns a list of
    problems, empty when the copy is the pinned SDK."""
    problems = []
    for rel in (HEADER, LIB, DLL, LICENCE):
        if not os.path.isfile(os.path.join(dest, rel)):
            problems.append("missing: %s" % os.path.join(dest, rel))
    dll = os.path.join(dest, DLL)
    if os.path.isfile(dll):
        got = sha256_of(dll)
        if got != DLL_SHA256:
            problems.append("nvngx_dlss.dll is not the pinned %s runtime: its SHA-256 is %s, "
                            "the pin is %s" % (DLL_VERSION, got.upper(), DLL_SHA256.upper()))
    head = head_of(dest)
    if head and head != COMMIT:
        # Not fatal on its own -- the hash above is what matters -- but said.
        if not quiet:
            print("note: the clone at %s is at commit %s, not the pinned %s" % (dest, head[:12], COMMIT[:12]))
    if not problems and not quiet:
        print("DLSS SDK %s verified at %s (nvngx_dlss.dll SHA-256 %s)" % (DLL_VERSION, dest, DLL_SHA256.upper()))
    return problems


def fetch(dest, dry_run=False):
    """Clone the pinned SDK into dest and verify it. With dry_run, print the
    commands and run none: the directory is not made and nothing is fetched."""
    if not dry_run:
        os.makedirs(dest, exist_ok=True)
    if not os.path.isdir(os.path.join(dest, ".git")):
        run("git", "init", "-q", dest, dry_run=dry_run)
        run("git", "-C", dest, "remote", "add", "origin", URL, dry_run=dry_run)
    if head_of(dest) != COMMIT:
        # A shallow, blob-less fetch of the one commit; GitHub serves a fetch
        # by full commit id. The sparse checkout then materialises only the
        # paths the build reads (cone mode keeps the top-level files, the
        # licence among them).
        run("git", "-C", dest, "fetch", "--depth", "1", "--filter=blob:none", "origin", COMMIT,
            dry_run=dry_run)
        run("git", "-C", dest, "sparse-checkout", "set", *PATHS, dry_run=dry_run)
        run("git", "-C", dest, "checkout", "-q", "--detach", COMMIT, dry_run=dry_run)
    else:
        run("git", "-C", dest, "sparse-checkout", "set", *PATHS, dry_run=dry_run)
        run("git", "-C", dest, "checkout", "-q", "--detach", COMMIT, dry_run=dry_run)
    if dry_run:
        print("(dry run) nothing was fetched or verified, and nothing was written")
        return
    problems = verify(dest)
    if problems:
        sys.exit("the fetched SDK is not the pinned one:\n  " + "\n  ".join(problems))
    print("DLSS SDK ready at %s -- build.bat finds it there." % dest)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--into", default=None, help="where to put the SDK (default: %%LOCALAPPDATA%%\\EDVR\\ngx-sdk)")
    ap.add_argument("--verify", default=None, metavar="DIR", help="check an existing copy and exit")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the git commands a fetch would run and run none of them")
    ap.add_argument("--self-test", action="store_true",
                    help="check this script against fixtures, never touching the network, and exit")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify:
        problems = verify(args.verify)
        if problems:
            for p in problems:
                print("fetch_ngx: " + p)
            return 1
        return 0
    fetch(args.into or default_dest(), dry_run=args.dry_run)
    return 0


# ---------------------------------------------------------------------------
# --self-test
# ---------------------------------------------------------------------------

def self_test():
    """The verification, the destination and the fetch's command sequence,
    against fixtures in the temp folder. Never fetches: subprocess and the
    network are replaced for the whole run, and a call that reaches either
    fails the test instead of going out."""
    import contextlib
    import io
    import shutil
    import socket
    import tempfile

    global DLL_SHA256
    failures = []
    ran = []                     # every command the code under test tried to run
    touched = []                 # every attempt to reach the network
    git_head = [""]              # what `git rev-parse HEAD` answers (empty: not a repo)
    materialise = [None]         # what a `git checkout` leaves in the directory

    def expect(name, condition, what=""):
        if not condition:
            failures.append("%s: %s" % (name, what))

    def fake_check_call(args, *_a, **_k):
        args = list(args)
        ran.append(args)
        if args[:2] == ["git", "-C"] and args[3] == "checkout" and materialise[0]:
            materialise[0](args[2])
        return 0

    def fake_check_output(args, *_a, **_k):
        ran.append(list(args))
        if not git_head[0]:
            raise subprocess.CalledProcessError(128, args)
        return (git_head[0] + "\n").encode()

    def no_network(*a, **k):
        touched.append(a)
        raise AssertionError("the self-test tried to use the network")

    def capture(fn, *a, **k):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            try:
                result = fn(*a, **k)
            except SystemExit as exc:
                result = "SystemExit: %s" % exc
        return result, out.getvalue()

    def sdk(root, dll=b"the runtime"):
        """A directory that has every file the build reads."""
        for rel in (HEADER, LIB, DLL, LICENCE):
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(dll if rel == DLL else b"x")
        return root

    base = tempfile.mkdtemp(prefix="edvr-fetch-ngx-")
    saved = (subprocess.check_call, subprocess.check_output, subprocess.Popen,
             socket.create_connection, socket.getaddrinfo, DLL_SHA256)
    own_connect = "connect" in vars(socket.socket)
    saved_connect = socket.socket.connect
    subprocess.check_call, subprocess.check_output = fake_check_call, fake_check_output
    subprocess.Popen = no_network
    socket.socket.connect = no_network
    socket.create_connection = no_network
    socket.getaddrinfo = no_network
    try:
        # Where it goes when --into does not say.
        name = "default-dest"
        env = dict(os.environ)
        try:
            os.environ["LOCALAPPDATA"] = os.path.join(base, "local")
            expect(name, default_dest() == os.path.join(base, "local", "EDVR", "ngx-sdk"), default_dest())
            del os.environ["LOCALAPPDATA"]
            expect(name, default_dest().endswith(os.path.join("AppData", "Local", "EDVR", "ngx-sdk")),
                   default_dest())
        finally:
            os.environ.clear()
            os.environ.update(env)

        # The hash reads a file that is longer than one chunk, exactly.
        name = "sha256"
        big = os.path.join(base, "big.bin")
        data = bytes(range(256)) * (2 * 4096 + 1) + b"tail"       # just over two 1 MiB chunks
        with open(big, "wb") as f:
            f.write(data)
        expect(name, sha256_of(big) == hashlib.sha256(data).hexdigest(), sha256_of(big))

        # verify(): what is missing is named, a runtime that is not the pinned one
        # is named with both hashes, and the pinned one passes quietly or loudly.
        name = "verify-missing"
        empty = os.path.join(base, "empty")
        os.makedirs(empty)
        problems = verify(empty, quiet=True)
        expect(name, len(problems) == 4 and all(p.startswith("missing: ") for p in problems), repr(problems))
        expect(name, any(os.path.join(empty, DLL) in p for p in problems), repr(problems))

        name = "verify-wrong-runtime"
        wrong = sdk(os.path.join(base, "wrong"), dll=b"another runtime")
        problems = verify(wrong, quiet=True)
        expect(name, len(problems) == 1 and "is not the pinned %s runtime" % DLL_VERSION in problems[0], repr(problems))
        expect(name, hashlib.sha256(b"another runtime").hexdigest().upper() in problems[0]
               and DLL_SHA256.upper() in problems[0], repr(problems))

        name = "verify-pinned"
        DLL_SHA256 = hashlib.sha256(b"the runtime").hexdigest()      # pin the fixture's runtime
        right = sdk(os.path.join(base, "right"))
        problems, said = capture(verify, right)
        expect(name, problems == [] and "verified at %s" % right in said and DLL_SHA256.upper() in said,
               "%r %r" % (problems, said))
        problems, said = capture(verify, right, True)
        expect(name, problems == [] and said == "", "quiet: %r %r" % (problems, said))
        # A clone at another commit is a note, not a failure: the hash decides.
        git_head[0] = "0123456789abcdef" * 2 + "01234567"
        problems, said = capture(verify, right)
        expect(name, problems == [] and "note: the clone at %s is at commit 0123456789ab" % right in said,
               "%r %r" % (problems, said))
        git_head[0] = ""

        # --verify: the exit code the build reads, and a message per problem.
        name = "verify-cli"
        code, said = capture(main, ["--verify", right])
        expect(name, code == 0, "%r %r" % (code, said))
        code, said = capture(main, ["--verify", wrong])
        expect(name, code == 1 and "fetch_ngx: nvngx_dlss.dll is not the pinned" in said, "%r %r" % (code, said))
        code, said = capture(main, ["--verify", empty])
        expect(name, code == 1 and said.count("fetch_ngx: missing: ") == 4, "%r %r" % (code, said))

        # run(): a dry run says the command and does not run it.
        name = "run"
        del ran[:]
        _, said = capture(run, "git", "status")
        expect(name, ran == [["git", "status"]] and said == "+ git status\n", "%r %r" % (ran, said))
        del ran[:]
        _, said = capture(run, "git", "status", dry_run=True)
        expect(name, ran == [] and said == "(dry run) + git status\n", "%r %r" % (ran, said))

        # The fetch's commands, pinned: init and remote for a new directory, the
        # shallow blob-less fetch of the one commit, the sparse paths, the detach.
        fresh = os.path.join(base, "fresh")
        expected = [
            ["git", "init", "-q", fresh],
            ["git", "-C", fresh, "remote", "add", "origin", URL],
            ["git", "-C", fresh, "fetch", "--depth", "1", "--filter=blob:none", "origin", COMMIT],
            ["git", "-C", fresh, "sparse-checkout", "set", *PATHS],
            ["git", "-C", fresh, "checkout", "-q", "--detach", COMMIT],
        ]

        name = "fetch-dry-run"
        del ran[:]
        _, said = capture(fetch, fresh, dry_run=True)
        expect(name, [c for c in ran if "rev-parse" not in c] == [], "a command ran: %r" % ran)
        expect(name, not os.path.exists(fresh), "--dry-run made %s" % fresh)
        for cmd in expected:
            expect(name, "(dry run) + " + " ".join(cmd) in said, "%r not planned in\n%s" % (cmd, said))
        expect(name, "nothing was fetched or verified" in said, said)
        # ... and through the command line.
        del ran[:]
        code, said = capture(main, ["--into", fresh, "--dry-run"])
        expect(name, code == 0 and not os.path.exists(fresh) and
               [c for c in ran if "rev-parse" not in c] == [], "%r %r %r" % (code, said, ran))

        name = "fetch"
        del ran[:]

        def clone(dest):
            sdk(dest)
        materialise[0] = clone
        _, said = capture(fetch, fresh)
        expect(name, [c for c in ran if "rev-parse" not in c] == expected, "ran %r" % ran)
        expect(name, "DLSS SDK ready at %s" % fresh in said, said)

        # A directory that is already a clone of the pin only re-asserts the
        # paths and the checkout: no init, no remote, no fetch.
        name = "fetch-already-cloned"
        again = os.path.join(base, "again")
        os.makedirs(os.path.join(again, ".git"))
        git_head[0] = COMMIT
        del ran[:]
        _, said = capture(fetch, again)
        expect(name, [c for c in ran if "rev-parse" not in c] ==
               [["git", "-C", again, "sparse-checkout", "set", *PATHS],
                ["git", "-C", again, "checkout", "-q", "--detach", COMMIT]], "ran %r" % ran)
        git_head[0] = ""

        # A fetch that lands another runtime fails loudly and says so.
        name = "fetch-wrong-runtime"
        materialise[0] = lambda dest: sdk(dest, dll=b"moved on")
        result, said = capture(fetch, os.path.join(base, "moved"))
        expect(name, str(result).startswith("SystemExit: the fetched SDK is not the pinned one:")
               and "is not the pinned %s runtime" % DLL_VERSION in str(result), "%r %r" % (result, said))
        materialise[0] = None

        expect("no-network", touched == [], "the network was touched: %r" % (touched,))
        bad = [c for c in ran if c[0] != "git"]
        expect("no-network", bad == [], "a command that is not git ran: %r" % (bad,))
    finally:
        (subprocess.check_call, subprocess.check_output, subprocess.Popen,
         socket.create_connection, socket.getaddrinfo, DLL_SHA256) = saved
        if own_connect:
            socket.socket.connect = saved_connect
        else:
            del socket.socket.connect
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("fetch_ngx: self-test FAILED")
        for f in failures:
            print("  " + f.replace("\n", "\n    "))
        return 1
    print("fetch_ngx: self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
