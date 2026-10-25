#!/usr/bin/env python3
"""The Linux toolchain: Microsoft's compiler under Wine, for compile-only test flights.

EDVR's DLLs are built with MSVC.  On Linux (the Steam Deck, Proton) this tool
keeps a device-local copy of VS 2022's build tools and the Windows SDK, a
Windows Python and a Wine prefix under <main checkout>/.toolchains/windows/
(git-ignored; EDVR_TOOLCHAIN_HOME overrides it), and runs
`build.bat --compile-only` inside that prefix.  The result is a compile-only
build (tools/compile_only.py): the production DLLs in
<root>/build/compile-build, NOT Windows validated, for a test flight.  CI's
full Windows build stays the validation for a merge or a release (AGENTS.md,
"Build and verify before commit").

    python3 tools/linux_toolchain.py setup --dry-run
    python3 tools/linux_toolchain.py setup --accept-license
    python3 tools/linux_toolchain.py doctor
    python3 tools/linux_toolchain.py build-dlls [--root DIR] [--clean]
    python3 tools/linux_toolchain.py --self-test

setup needs wine, msitools and git from the system's package manager and never
runs sudo itself.  Its compiler download (msvc-wine's vsdownload.py, pinned
below) is VS 2022's Build Tools under Microsoft's licence (VS_LICENSE): using
them requires a valid Visual Studio licence, and the files must never be
shared, published or committed, which is why they live under .toolchains/.
--accept-license says you have read and accept those terms; without it setup
stops before downloading anything.  --dry-run writes nothing at all.

The SDKs come from the repository's own fetch tools, pinned there, into
.toolchains/windows/deps/, and build-dlls points build.bat at them; the FSR 3
DX11 port is built there under Wine with pinned CMake and Ninja.  build-dlls
fetches the pinned OpenXR loader into the tree being built when it lacks one.

What differs from CI's Windows build:
  - nothing in the compiler or SDK: MSVC 14.44.35207 (VS 2022 17.14) and
    Windows SDK 10.0.26100, the versions of CI's windows-2022 image;
  - shaders compile with the Windows SDK's own d3dcompiler_47.dll, installed
    in the prefix over Wine's (the shader self-test checks its bytes);
  - the proxy's export list comes from Wine's d3d11.dll, which lacks
    CreateDirect3D11DeviceFromDXGIDevice, CreateDirect3D11SurfaceFromDXGISurface,
    D3D11CreateDeviceForD3D12, EnableFeatureLevelUpgrade and D3DPerformance_*
    and adds two D3DKMT exports.  Elite imports D3D11CreateDevice alone from it
    (EliteDangerous64.exe, checked 2026-10-24);
  - no Python gates, rigs or installer: that is what makes it compile-only.
"""

import argparse
import contextlib
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compile_only  # noqa: E402


REPO = Path(__file__).resolve().parents[1]

MSVC_WINE_URL = "https://github.com/mstorsjo/msvc-wine.git"
MSVC_WINE_COMMIT = "514f8ea34842cd6d831804d0e9658d3a32870ae1"   # ISC; covers its scripts, not what they download
VS_MAJOR = "17"
VS_VERSION = "17.14"
MSVC_VERSION = "14.44.35207"
SDK_VERSION = "10.0.26100.0"
VS_LICENSE = ("Microsoft Software License Terms: Microsoft Visual Studio 2022 Remote Debugger, "
              "IntelliTrace Collector, other Debuggers, Agents and Build Tools (EULAID VS_2022_Tools_2024Mar)",
              "https://visualstudio.microsoft.com/license-terms/vs2022-ga-diagnosticbuildtools/")
VSDOWNLOAD_ARGS = ["--accept-license", "--major", VS_MAJOR, "--msvc-version", VS_VERSION,
                   "--architecture", "x64", "--host-arch", "x64",
                   "--with-asan", "no", "--with-dia", "no", "--with-msbuild", "no"]
PYTHON_VERSION = "3.12.10"   # CI's 3.12; the last 3.12 with Windows binaries
PYTHON_URL = "https://www.python.org/ftp/python/%s/python-%s-embed-amd64.zip" % (PYTHON_VERSION, PYTHON_VERSION)
PYTHON_SHA256 = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"
# The FSR 3 DX11 port builds with CMake, under Wine with Ninja: nmake.exe cannot
# parse the Makefiles CMake writes (tools/fetch_ffx_dx11.py, GENERATORS).
CMAKE_VERSION = "4.4.4"      # BSD-3-Clause; SHA-256 from the release's own list
CMAKE_URL = "https://github.com/Kitware/CMake/releases/download/v%s/cmake-%s-windows-x86_64.zip" % (
    CMAKE_VERSION, CMAKE_VERSION)
CMAKE_SHA256 = "bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536"
NINJA_VERSION = "1.13.2"     # Apache-2.0; SHA-256 is GitHub's asset digest
NINJA_URL = "https://github.com/ninja-build/ninja/releases/download/v%s/ninja-win.zip" % NINJA_VERSION
NINJA_SHA256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"
HOST_TOOLS = ("wine", "msiextract", "git")
HOST_INSTALL = "sudo pacman -S --needed wine msitools git"
WINE_OVERRIDES = "mscoree,mshtml=;winemenubuilder.exe=d;d3dcompiler_47=n"

# (folder under deps/, fetch tool, its destination flag, the build.bat variable
# that names it, a file whose presence means it is fetched).  A checkout whose
# tools/ lacks the fetch tool does not use that SDK, and skips it.  No flag:
# the SDK is built, under Wine (setup's fetch_ffx).
DEPS = (
    ("ngx", "fetch_ngx.py", "--into", "EDVR_NGX_SDK", "include/nvsdk_ngx.h"),
    ("xess", "fetch_xess.py", "--dest", "EDVR_XESS_SDK", "include/xess/xess.h"),
    ("fsr4", "fetch_fsr4.py", "--dest", "EDVR_FSR4_SDK", "include/FidelityFX/upscalers/include/ffx_upscale.h"),
    ("ffx-dx11", "fetch_ffx_dx11.py", None, "EDVR_FFX_DX11", "include/FidelityFX/host/ffx_fsr3upscaler.h"),
)


class Layout:
    def __init__(self, home):
        self.home = Path(home)
        self.msvc_wine = self.home / "msvc-wine"
        self.cache = self.home / "cache"
        self.msvc = self.home / "msvc"
        self.msvc_tools = self.msvc / "VC" / "Tools" / "MSVC" / MSVC_VERSION
        self.kits = self.msvc / "Windows Kits" / "10"
        self.cl = self.msvc_tools / "bin" / "HostX64" / "x64" / "cl.exe"
        self.d3dcompiler = self.kits / "Redist" / "D3D" / "x64" / "d3dcompiler_47.dll"
        self.downloads = self.home / "downloads"
        self.python = self.home / "python"
        self.prefix = self.home / "wineprefix"
        self.prefix_d3dcompiler = self.prefix / "drive_c" / "windows" / "system32" / "d3dcompiler_47.dll"
        self.deps = self.home / "deps"
        self.cmake_bin = self.home / "cmake" / ("cmake-%s-windows-x86_64" % CMAKE_VERSION) / "bin"
        self.ninja = self.home / "ninja"


def toolchain_home(root=REPO):
    """The main checkout's .toolchains/windows: one toolchain serves every worktree."""
    env = os.environ.get("EDVR_TOOLCHAIN_HOME")
    if env:
        return Path(env)
    main = Path(root)
    common = _capture(["git", "-C", str(root), "rev-parse", "--git-common-dir"])
    if common:
        main = (Path(root) / common).resolve().parent
    return main / ".toolchains" / "windows"


def to_wine(path):
    """A Linux absolute path as Wine's Z: drive names it."""
    text = Path(path).as_posix()
    if not text.startswith("/"):
        raise ValueError("not an absolute Linux path: %s" % text)
    return "Z:" + text.replace("/", "\\")


def _capture(argv, cwd=None):
    try:
        result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def _run(argv, cwd=None, env=None):
    """Run a step, its output straight to the console; returns the exit code."""
    print("+ " + " ".join(str(a) for a in argv), flush=True)
    return subprocess.run([str(a) for a in argv], cwd=cwd, env=env).returncode


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _download(layout, url, sha256):
    """The pinned archive at `url` in downloads/, fetched when missing; None when
    what arrived is not the pinned file (then nothing is kept)."""
    layout.downloads.mkdir(parents=True, exist_ok=True)
    archive = layout.downloads / url.rsplit("/", 1)[1]
    if archive.is_file() and _sha256(archive) == sha256:
        return archive
    print("+ download %s" % url, flush=True)
    partial = archive.with_name(archive.name + ".part")
    with urllib.request.urlopen(url, timeout=120) as response, open(partial, "wb") as out:
        shutil.copyfileobj(response, out)
    if _sha256(partial) != sha256:
        partial.unlink()
        print("linux_toolchain: %s does not match its pinned SHA-256" % url)
        return None
    os.replace(partial, archive)
    return archive


def _unpack(layout, url, sha256, dest):
    archive = _download(layout, url, sha256)
    if archive is None:
        return 1
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(dest)
    return 0


def _start_pdb_server(layout, env):
    """Start mspdbsrv.exe ourselves, detached from every pipe.  Left to cl.exe,
    it starts on first use, outlives the build by minutes and holds the build's
    stdout open, so a caller reading that through a pipe hangs.  cl.exe then
    finds this one running; a second start exits at once."""
    subprocess.Popen(["wine", to_wine(layout.msvc_tools / "bin" / "HostX64" / "x64" / "mspdbsrv.exe"),
                      "-start", "-spawn", "-shutdowntime", "300"], env=env, stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def _wine_version():
    return _capture(["wine", "--version"]) or "unknown"


def deps_for(root):
    """The DEPS rows this checkout uses: those whose fetch tool it has."""
    return [row for row in DEPS if (Path(root) / "tools" / row[1]).is_file()]


def _sdk_header(layout):
    return layout.kits / "Include" / SDK_VERSION / "um" / "d3d11.h"


def _msvc_sdk_ready(layout):
    """One download installs the compiler and the SDK, and an interrupted one can leave
    cl.exe without the SDK headers or the redistributable d3dcompiler, so setup is
    done only when all three are there."""
    return layout.cl.is_file() and _sdk_header(layout).is_file() and layout.d3dcompiler.is_file()


def checks(layout, root=REPO):
    """[(label, ok, required, detail)] for doctor and the build's precondition."""
    rows = []
    for tool in HOST_TOOLS:
        found = shutil.which(tool)
        rows.append(("host " + tool, bool(found), True, found or "install with: " + HOST_INSTALL))
    commit = _capture(["git", "-C", str(layout.msvc_wine), "rev-parse", "HEAD"]) \
        if (layout.msvc_wine / ".git").exists() else None
    rows.append(("msvc-wine", commit == MSVC_WINE_COMMIT, False, "%s (pinned %s)" % (commit, MSVC_WINE_COMMIT[:12])))
    rows.append(("MSVC " + MSVC_VERSION, layout.cl.is_file(), True, str(layout.cl)))
    sdk = _sdk_header(layout)
    rows.append(("Windows SDK " + SDK_VERSION, sdk.is_file(), True, str(sdk)))
    python_ok = (layout.python / "python.exe").is_file() and not list(layout.python.glob("python*._pth"))
    rows.append(("Windows Python " + PYTHON_VERSION, python_ok, True, str(layout.python)))
    rows.append(("Wine prefix", (layout.prefix / "system.reg").is_file(), True, str(layout.prefix)))
    native = layout.prefix_d3dcompiler.is_file() and layout.d3dcompiler.is_file() and \
        _sha256(layout.prefix_d3dcompiler) == _sha256(layout.d3dcompiler)
    rows.append(("SDK d3dcompiler_47 in the prefix", native, True, str(layout.prefix_d3dcompiler)))
    rows.append(("CMake %s and Ninja %s" % (CMAKE_VERSION, NINJA_VERSION),
                 (layout.cmake_bin / "cmake.exe").is_file() and (layout.ninja / "ninja.exe").is_file(), False,
                 "%s, %s" % (layout.cmake_bin, layout.ninja)))
    for name, tool, _flag, _var, probe in deps_for(root):
        path = layout.deps / name
        rows.append(("SDK " + name, (path / probe).is_file(), False, str(path)))
    return rows


def doctor(layout, root=REPO):
    rows = checks(layout, root)
    for label, ok, required, detail in rows:
        print("[%s] %-34s %s" % ("ok" if ok else ("MISSING" if required else "missing"), label, detail))
    if all(ok for _l, ok, _r, _d in rows):
        print("linux_toolchain: ready")
        return 0
    print("linux_toolchain: not ready; python3 tools/linux_toolchain.py setup --dry-run lists what setup would do")
    return 1


def _prefix_env(layout, base=None):
    env = dict(os.environ if base is None else base)
    env.update({"WINEPREFIX": layout.prefix.as_posix(), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": WINE_OVERRIDES})
    return env


def setup(layout, root=REPO, accept_license=False, dry_run=False):
    """Bring every component to its pin, skipping what is already there."""
    missing = [tool for tool in HOST_TOOLS if not shutil.which(tool)]
    if missing:
        print("linux_toolchain: %s not found. This tool never runs sudo; install them with:\n    %s"
              % (", ".join(missing), HOST_INSTALL))
        return 2
    if not _msvc_sdk_ready(layout) and not accept_license and not dry_run:
        print("linux_toolchain: the compiler download is VS 2022's Build Tools, under\n    %s\n    %s\n"
              "Using them requires a valid Visual Studio licence, and the files must never be shared.\n"
              "Read the terms, then run setup again with --accept-license." % VS_LICENSE)
        return 3

    def step(label, done, action):
        if done:
            print("[ok] %s" % label)
            return 0
        if dry_run:
            print("[would] %s" % label)
            return 0
        print("[doing] %s" % label, flush=True)
        rc = action()
        if rc:
            print("linux_toolchain: %s failed (exit %s)" % (label, rc))
        return rc

    def clone_msvc_wine():
        if not (layout.msvc_wine / ".git").exists():
            rc = _run(["git", "clone", "-q", MSVC_WINE_URL, layout.msvc_wine])
            if rc:
                return rc
        return _run(["git", "-C", layout.msvc_wine, "checkout", "-q", "--detach", MSVC_WINE_COMMIT])

    def download_msvc():
        return _run([sys.executable, layout.msvc_wine / "vsdownload.py", *VSDOWNLOAD_ARGS,
                     "--cache", layout.cache, "--dest", layout.msvc])

    def install_python():
        rc = _unpack(layout, PYTHON_URL, PYTHON_SHA256, layout.python)
        # The embeddable build's ._pth pins sys.path and leaves out the script's
        # folder, which build.bat's tools import their siblings from.
        for pth in layout.python.glob("python*._pth"):
            pth.unlink()
        return rc

    def install_cmake_ninja():
        return _unpack(layout, CMAKE_URL, CMAKE_SHA256, layout.cmake_bin.parent.parent) or \
            _unpack(layout, NINJA_URL, NINJA_SHA256, layout.ninja)

    def fetch_ffx(tool, dest):
        """The FSR 3 DX11 port: the host's git makes the pinned checkout (Wine
        has none), then the Windows Python builds and verifies it under Wine."""
        rc = _run([sys.executable, Path(root) / "tools" / tool, "--fetch-only", "--dest", dest])
        if rc:
            return rc
        env = wine_env(layout, "", [])
        env["WINEPATH"] += ";%s;%s" % (to_wine(layout.cmake_bin), to_wine(layout.ninja))
        _start_pdb_server(layout, env)
        python = to_wine(layout.python / "python.exe")
        script = to_wine(Path(root).resolve() / "tools" / tool)
        log = layout.deps / "ffx-dx11-build.log"
        print("+ (under Wine) %s --no-fetch --generator ninja, output in %s" % (tool, log), flush=True)
        with open(log, "w") as out:
            rc = subprocess.run(["wine", python, "-I", script, "--dest", to_wine(dest), "--no-fetch",
                                 "--generator", "ninja"], cwd=root, env=env, stdin=subprocess.DEVNULL,
                                stdout=out, stderr=subprocess.STDOUT).returncode
        if rc:
            print("linux_toolchain: the FSR 3 port's build failed; see %s" % log)
            return rc
        return _run(["wine", python, "-I", script, "--verify", to_wine(dest)], cwd=root, env=env)

    def make_prefix():
        if not (layout.prefix / "system.reg").is_file():
            rc = _run(["wineboot", "-i"], env=_prefix_env(layout))
            if rc:
                return rc
        if not layout.d3dcompiler.is_file():
            print("linux_toolchain: the SDK has no %s" % layout.d3dcompiler)
            return 1
        shutil.copyfile(layout.d3dcompiler, layout.prefix_d3dcompiler)
        return 0

    rc = step("msvc-wine at %s" % MSVC_WINE_COMMIT[:12],
              _capture(["git", "-C", str(layout.msvc_wine), "rev-parse", "HEAD"]) == MSVC_WINE_COMMIT,
              clone_msvc_wine)
    rc = rc or step("MSVC %s and Windows SDK %s (VS %s, about 3 GB)" % (MSVC_VERSION, SDK_VERSION, VS_VERSION),
                    _msvc_sdk_ready(layout), download_msvc)
    rc = rc or step("Windows Python %s" % PYTHON_VERSION,
                    (layout.python / "python.exe").is_file() and not list(layout.python.glob("python*._pth")),
                    install_python)
    native = layout.prefix_d3dcompiler.is_file() and layout.d3dcompiler.is_file() and \
        _sha256(layout.prefix_d3dcompiler) == _sha256(layout.d3dcompiler)
    rc = rc or step("Wine prefix with the SDK's d3dcompiler_47", native, make_prefix)
    rc = rc or step("CMake %s and Ninja %s" % (CMAKE_VERSION, NINJA_VERSION),
                    (layout.cmake_bin / "cmake.exe").is_file() and (layout.ninja / "ninja.exe").is_file(),
                    install_cmake_ninja)
    for name, tool, flag, _var, probe in deps_for(root):
        dest = layout.deps / name
        if flag is None:
            action = lambda tool=tool, dest=dest: fetch_ffx(tool, dest)
        else:
            action = lambda tool=tool, flag=flag, dest=dest: _run([sys.executable, Path(root) / "tools" / tool,
                                                                   flag, dest])
        rc = rc or step("SDK %s (tools/%s)" % (name, tool), (dest / probe).is_file(), action)
    if rc:
        return rc
    print("linux_toolchain: setup %s" % ("dry run, nothing written" if dry_run else "complete"))
    return 0


def wine_env(layout, describe, deps, base=None):
    """The environment build.bat runs in: vcvars64's INCLUDE, LIB and PATH for
    the pinned toolset, the prefix, and the SDKs in `deps` (folder names under
    deps/).  An SDK variable already set by the caller (=none, say) is kept.
    The host's PYTHON* variables are dropped: Windows Python would read them."""
    env = {k: v for k, v in (os.environ if base is None else base).items() if not k.startswith("PYTHON")}
    env = _prefix_env(layout, env)
    msvc, kits = to_wine(layout.msvc_tools), to_wine(layout.kits)
    sdk = "%s\\%%s\\%s" % (kits, SDK_VERSION)
    env["INCLUDE"] = ";".join([msvc + "\\include"] + [sdk % "Include" + "\\" + part
                                                      for part in ("ucrt", "um", "shared", "winrt", "cppwinrt")])
    env["LIB"] = ";".join([msvc + "\\lib\\x64", sdk % "Lib" + "\\ucrt\\x64", sdk % "Lib" + "\\um\\x64"])
    env["WINEPATH"] = ";".join([msvc + "\\bin\\HostX64\\x64", sdk % "bin" + "\\x64", to_wine(layout.python)])
    env["EDVR_GIT_DESCRIBE"] = describe
    for name, _tool, _flag, var, _probe in DEPS:
        if name in deps and var not in env:
            env[var] = to_wine(layout.deps / name)
    return env


@contextlib.contextmanager
def _tree_lock(root):
    """One compile of a tree at a time; build.bat skips build_lock.py under Wine."""
    import fcntl
    path = compile_only.build_dir(root).parent / "compile-build.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("linux_toolchain: another compile of %s is running; waiting for it" % root, flush=True)
            fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def _run_build(layout, root, env, clean):
    _start_pdb_server(layout, env)
    argv = ["wine", "cmd", "/c", to_wine(Path(root) / "build.bat"), "--compile-only"] + (["--clean"] if clean else [])
    return _run(argv, cwd=root, env=env)


def build_dlls(layout, root, clean=False):
    root = Path(root).resolve()
    if b"--compile-only" not in (root / "build.bat").read_bytes():
        print("linux_toolchain: %s\\build.bat predates --compile-only; merge main into it first" % root)
        return 1
    missing = [label for label, ok, required, _d in checks(layout, root) if required and not ok]
    if missing:
        print("linux_toolchain: not ready (%s); run python3 tools/linux_toolchain.py setup" % ", ".join(missing))
        return 1
    if not (root / "third_party" / "openxr" / "loader" / "openxr_loader.dll").is_file():
        rc = _run([sys.executable, root / "tools" / "fetch_openxr_loader.py"], cwd=root)
        if rc:
            return rc
    deps = [row[0] for row in deps_for(root) if (layout.deps / row[0] / row[4]).is_file()]
    for name, tool, *_rest in deps_for(root):
        if name not in deps:
            print("linux_toolchain: WARNING: no %s SDK in %s; this build goes without it" % (name, layout.deps))
    with _tree_lock(root):
        fingerprint = compile_only.source_fingerprint(root)
        compile_only.remove_receipt(root)
        describe = compile_only.describe(root)
        rc = _run_build(layout, root, wine_env(layout, describe, deps), clean)
        if rc:
            print("linux_toolchain: build.bat --compile-only failed (exit %s); no receipt written" % rc)
            return rc
        toolchain = {"msvc": MSVC_VERSION, "sdk": SDK_VERSION, "vs": VS_VERSION, "python": PYTHON_VERSION,
                     "wine": _wine_version(), "deps": deps}
        try:
            path = compile_only.write_receipt(root, fingerprint, toolchain)
        except (OSError, ValueError) as error:
            print("linux_toolchain: no receipt written: %s" % error)
            return 1
    print("linux_toolchain: COMPILE-ONLY build of %s (%s), NOT Windows validated: %s" % (root, describe, path.parent))
    if describe.endswith("-dirty"):
        print("linux_toolchain: WARNING: compiled from a dirty tree; commit, then compile, before a build "
              "that will be flown (edvr_log.py --expect-build HEAD will not match it)")
    hint = "" if root == REPO else " --root %s" % root
    print("Install it for a test flight (close Elite first):\n"
          "    python3 tools/install_edvr.py --target steam --compile-only%s --dry-run\n"
          "    python3 tools/install_edvr.py --target steam --compile-only%s" % (hint, hint))
    return 0


def self_test():
    temporary = Path(tempfile.mkdtemp(prefix="edvr-linux-toolchain-"))
    saved = {name: globals()[name] for name in ("_run", "_run_build", "_tree_lock", "_wine_version", "checks")}
    calls = []

    def expect(condition, why):
        if not condition:
            raise AssertionError(why)

    def snapshot(path):
        return sorted((str(p.relative_to(path)), p.read_bytes() if p.is_file() else None)
                      for p in Path(path).rglob("*")) if Path(path).exists() else None

    try:
        expect(to_wine("/home/u/a b/c.bat") == "Z:\\home\\u\\a b\\c.bat", "to_wine mis-maps a path")
        try:
            to_wine("relative/path")
            raise AssertionError("to_wine accepted a relative path")
        except ValueError:
            pass

        layout = Layout("/t/tc")
        env = wine_env(layout, "v1.2.3-4-gabc", ["ngx", "xess"],
                       base={"PYTHONHOME": "/usr", "PYTHONPATH": "/x", "EDVR_XESS_SDK": "none", "HOME": "/home/u"})
        msvc = "Z:\\t\\tc\\msvc\\VC\\Tools\\MSVC\\" + MSVC_VERSION
        kits = "Z:\\t\\tc\\msvc\\Windows Kits\\10"
        expect(env["INCLUDE"].split(";") == [msvc + "\\include"] + ["%s\\Include\\%s\\%s" % (kits, SDK_VERSION, part)
               for part in ("ucrt", "um", "shared", "winrt", "cppwinrt")], "INCLUDE is not vcvars64's: %s" % env["INCLUDE"])
        expect(env["LIB"].split(";") == [msvc + "\\lib\\x64", "%s\\Lib\\%s\\ucrt\\x64" % (kits, SDK_VERSION),
                                         "%s\\Lib\\%s\\um\\x64" % (kits, SDK_VERSION)], "LIB is wrong: %s" % env["LIB"])
        expect(env["WINEPATH"].split(";") == [msvc + "\\bin\\HostX64\\x64", "%s\\bin\\%s\\x64" % (kits, SDK_VERSION),
                                              "Z:\\t\\tc\\python"], "WINEPATH is wrong: %s" % env["WINEPATH"])
        expect("PYTHONHOME" not in env and "PYTHONPATH" not in env, "the host's PYTHON* variables leak into Wine")
        expect(env["EDVR_XESS_SDK"] == "none", "a caller's SDK choice was overridden")
        expect(env["EDVR_NGX_SDK"] == "Z:\\t\\tc\\deps\\ngx" and "EDVR_FSR4_SDK" not in env and
               "EDVR_FFX_DX11" not in env, "the SDK variables do not follow the fetched SDKs")
        expect(wine_env(layout, "", ["ffx-dx11"], base={})["EDVR_FFX_DX11"] == "Z:\\t\\tc\\deps\\ffx-dx11",
               "the built FSR 3 port is not handed to build.bat")
        expect(env["EDVR_GIT_DESCRIBE"] == "v1.2.3-4-gabc" and env["WINEPREFIX"] == "/t/tc/wineprefix" and
               "d3dcompiler_47=n" in env["WINEDLLOVERRIDES"] and env["HOME"] == "/home/u",
               "the prefix, describe or overrides are wrong")

        # doctor and setup on an empty home write nothing; setup without the
        # licence stops before any step, and --dry-run runs nothing.
        home = temporary / "home"
        empty = Layout(home)
        globals()["_run"] = lambda argv, cwd=None, env=None: calls.append(argv) or 0
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            expect(doctor(empty) == 1, "doctor called an empty toolchain ready")
            if all(shutil.which(tool) for tool in HOST_TOOLS):
                expect(setup(empty) == 3, "setup went ahead without --accept-license")
                expect(setup(empty, dry_run=True) == 0, "setup --dry-run failed")
        expect(not home.exists() and not calls, "doctor or setup wrote or ran something: %r" % calls)

        # build-dlls: the receipt follows a good compile of an unchanged tree only.
        # The toolchain is the Linux-path layout above, so this runs on Windows too.
        root = temporary / "repo"
        (root / "src").mkdir(parents=True)
        (root / "build.bat").write_bytes(b"@echo off\r\nREM --compile-only\r\n")
        (root / "src" / "a.cpp").write_bytes(b"int a;\n")
        (root / ".gitignore").write_bytes(b"build/\nthird_party/openxr/loader/\n")
        loader = root / "third_party" / "openxr" / "loader" / "openxr_loader.dll"
        loader.parent.mkdir(parents=True)
        loader.write_bytes(b"LOADER")
        subprocess.run(["git", "init", "-q", str(root)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", "-c",
                        "commit.gpgsign=false", "add", "-A"], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", "-c",
                        "commit.gpgsign=false", "commit", "-q", "-m", "fixture"], check=True, capture_output=True)
        globals()["checks"] = lambda layout, root=REPO: [("all", True, True, "")]
        globals()["_tree_lock"] = lambda root: contextlib.nullcontext()
        globals()["_wine_version"] = lambda: "wine-test"
        bdir = compile_only.build_dir(root)
        receipt = bdir / compile_only.RECEIPT

        def compiler(rc=0, edit=False):
            def run(layout, root_, env, clean):
                expect(env["EDVR_GIT_DESCRIBE"] == compile_only.describe(root), "build.bat was not given the describe")
                bdir.mkdir(parents=True, exist_ok=True)
                (bdir / "d3d11.dll").write_bytes(b"GRAPHICS")
                (bdir / "openvr_api.dll").write_bytes(b"RUNTIME")
                if edit:
                    (root / "src" / "a.cpp").write_bytes(b"int a = 1;\n")
                return rc
            return run

        with contextlib.redirect_stdout(io.StringIO()):
            globals()["_run_build"] = compiler()
            expect(build_dlls(layout, root) == 0 and receipt.is_file(), "a good compile wrote no receipt")
            import json
            data = json.loads(receipt.read_text(encoding="utf-8"))
            expect(data["source_fingerprint"] == compile_only.source_fingerprint(root) and
                   data["toolchain"]["wine"] == "wine-test" and data["toolchain"]["msvc"] == MSVC_VERSION,
                   "the receipt does not describe this tree and toolchain")
            globals()["_run_build"] = compiler(rc=2)
            expect(build_dlls(layout, root) == 2 and not receipt.exists(),
                   "a failed compile left the previous receipt beside its outputs")
            globals()["_run_build"] = compiler(edit=True)
            expect(build_dlls(layout, root) == 1 and not receipt.exists(),
                   "a tree edited during the compile got a receipt")
            (root / "build.bat").write_bytes(b"@echo off\r\n")
            expect(build_dlls(layout, root) == 1, "a build.bat without --compile-only was run")
        expect(not calls, "build-dlls ran a step it should not have: %r" % calls)

        # --dry-run belongs to setup: build-dlls refuses it before it removes or builds anything.
        ran = []
        globals()["_run_build"] = lambda *a, **k: ran.append(a) or 0
        receipt.write_bytes(b"{}")
        for command in ("build-dlls", "doctor"):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    main([command, "--root", str(root), "--clean", "--dry-run"])
                refused = False
            except SystemExit as stop:
                refused = stop.code == 2
            expect(refused and not ran and receipt.is_file(), "%s --dry-run was not refused before it acted" % command)

        # setup is done only when the compiler and the SDK it came with are both there.
        part = Layout(temporary / "partial")
        part.cl.parent.mkdir(parents=True)
        part.cl.write_bytes(b"cl")
        expect(not _msvc_sdk_ready(part), "a compiler with no SDK counted as installed")
        _sdk_header(part).parent.mkdir(parents=True)
        _sdk_header(part).write_bytes(b"h")
        expect(not _msvc_sdk_ready(part), "a missing d3dcompiler_47 counted as installed")
        part.d3dcompiler.parent.mkdir(parents=True)
        part.d3dcompiler.write_bytes(b"d")
        expect(_msvc_sdk_ready(part), "a complete install was not recognised")
    finally:
        globals().update(saved)
        shutil.rmtree(temporary, ignore_errors=True)
    print("linux_toolchain: self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", nargs="?", choices=("setup", "doctor", "build-dlls"))
    parser.add_argument("--root", type=Path, default=REPO, help="the tree build-dlls compiles (default: this checkout)")
    parser.add_argument("--clean", action="store_true", help="build-dlls: remove the compile-build first")
    parser.add_argument("--accept-license", action="store_true",
                        help="setup: you have read and accept VS 2022's Build Tools licence (see the module docstring)")
    parser.add_argument("--dry-run", action="store_true",
                        help="setup: say what would be done, write nothing (refused for the other commands)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.command:
        parser.error("a command or --self-test is required")
    if args.dry_run and args.command != "setup":
        parser.error("--dry-run is for setup only; %s has no dry run" % args.command)
    if os.name == "nt":
        print("linux_toolchain: on Windows, run build.bat itself")
        return 2
    layout = Layout(toolchain_home(args.root))
    if args.command == "doctor":
        return doctor(layout, args.root)
    if args.command == "setup":
        return setup(layout, args.root, accept_license=args.accept_license, dry_run=args.dry_run)
    return build_dlls(layout, args.root, clean=args.clean)


if __name__ == "__main__":
    sys.exit(main())
