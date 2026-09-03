"""Fetch NVIDIA's DLSS SDK into third_party/ngx -- only what the build uses.

    python tools/fetch_ngx.py

A sparse, shallow clone of https://github.com/NVIDIA/DLSS taking the
headers, the x64 static libraries and the release runtime (about 200 MB
on disk, most of it the runtimes). build.bat finds it there and builds
the dlaa mode in; the installer places nvngx_dlss.dll beside the game.
The SDK's licence (LICENSE.txt in the clone) permits shipping the runtime
with an application and keeps the SDK itself out of this tree.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "ngx")
URL = "https://github.com/NVIDIA/DLSS"
PATHS = ["include", "lib/Windows_x86_64/x64", "lib/Windows_x86_64/rel"]


def run(*args):
    print("+", " ".join(args))
    subprocess.check_call(args)


def main():
    os.makedirs(os.path.dirname(DEST), exist_ok=True)
    if not os.path.isdir(os.path.join(DEST, ".git")):
        run("git", "clone", "--depth", "1", "--filter=blob:none", "--sparse", URL, DEST)
    run("git", "-C", DEST, "sparse-checkout", "set", *PATHS)
    run("git", "-C", DEST, "checkout")
    header = os.path.join(DEST, "include", "nvsdk_ngx.h")
    lib = os.path.join(DEST, "lib", "Windows_x86_64", "x64", "nvsdk_ngx_s.lib")
    dll = os.path.join(DEST, "lib", "Windows_x86_64", "rel", "nvngx_dlss.dll")
    for f in (header, lib, dll):
        if not os.path.isfile(f):
            sys.exit(f"missing after the checkout: {f}")
    print(f"DLSS SDK ready at {DEST}")


if __name__ == "__main__":
    main()
