"""Fetch NVIDIA's DLSS SDK -- only what the build uses -- pinned and verified.

    python tools/fetch_ngx.py [--into DIR]
    python tools/fetch_ngx.py --verify DIR

A sparse, shallow clone of https://github.com/NVIDIA/DLSS at ONE pinned
commit (the 310.7.0 SDK), taking the headers, the x64 static library, the
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
"""
import argparse
import hashlib
import os
import subprocess
import sys

URL = "https://github.com/NVIDIA/DLSS"
# The pinned SDK: the commit and the runtime it carries. Both change together.
COMMIT = "a291cc7d2cc642a51566f3dfd5376f635cd1b284"   # "DLSS 310.7.0 SDK", 2026-06-23
DLL_SHA256 = "be6e434a94ca32499515eb62ca0e6c274526055d568d0426e4c652dcdfb6ee6e"
DLL_VERSION = "310.7.0"
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


def run(*args):
    print("+", " ".join(args))
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


def fetch(dest):
    os.makedirs(dest, exist_ok=True)
    if not os.path.isdir(os.path.join(dest, ".git")):
        run("git", "init", "-q", dest)
        run("git", "-C", dest, "remote", "add", "origin", URL)
    if head_of(dest) != COMMIT:
        # A shallow, blob-less fetch of the one commit; GitHub serves a fetch
        # by full commit id. The sparse checkout then materialises only the
        # paths the build reads (cone mode keeps the top-level files, the
        # licence among them).
        run("git", "-C", dest, "fetch", "--depth", "1", "--filter=blob:none", "origin", COMMIT)
        run("git", "-C", dest, "sparse-checkout", "set", *PATHS)
        run("git", "-C", dest, "checkout", "-q", "--detach", COMMIT)
    else:
        run("git", "-C", dest, "sparse-checkout", "set", *PATHS)
        run("git", "-C", dest, "checkout", "-q", "--detach", COMMIT)
    problems = verify(dest)
    if problems:
        sys.exit("the fetched SDK is not the pinned one:\n  " + "\n  ".join(problems))
    print("DLSS SDK ready at %s -- build.bat finds it there." % dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--into", default=None, help="where to put the SDK (default: %%LOCALAPPDATA%%\\EDVR\\ngx-sdk)")
    ap.add_argument("--verify", default=None, metavar="DIR", help="check an existing copy and exit")
    args = ap.parse_args()
    if args.verify:
        problems = verify(args.verify)
        if problems:
            for p in problems:
                print("fetch_ngx: " + p)
            return 1
        return 0
    fetch(args.into or default_dest())
    return 0


if __name__ == "__main__":
    sys.exit(main())
