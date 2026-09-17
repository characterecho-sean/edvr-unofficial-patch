"""Fetch and build the community Direct3D 11 port of AMD's FSR 3.1 upscaler.

    python tools/fetch_ffx_dx11.py [--dest DIR] [--commit SHA]
    python tools/fetch_ffx_dx11.py --verify DIR
    python tools/fetch_ffx_dx11.py --dry-run [--dest DIR]
    python tools/fetch_ffx_dx11.py --self-test

A pinned, shallow, sparse clone of https://github.com/optiscaler/FidelityFX-SDK-DX11
(HEAD 9b04fa49abae5aa341c0007af13c8ba1b4ad9913 as pinned 2026-09-16; MIT; a fork of
metarutaiga/FidelityFX-SDK-DX11 hardened by the OptiScaler project; FSR version
constants 3.1.2) -- fetched the way tools/fetch_ngx.py fetches the DLSS SDK: a
blobless, depth-1 fetch of one commit, then a cone-mode sparse checkout (which
also brings the repo-root LICENSE.txt along for free, same as fetch_ngx.py's).
Only sdk/ is checked out; framework/ and samples/ (the Cauldron sample browser)
are not needed to build the two static libraries EDVR links.

Built with CMake + VS2022's "Visual Studio 17 2022" generator and the fork's own
PREBUILT shader compiler (sdk/tools/binary_store/FidelityFX_SC.exe, committed in
the repo -- nothing here compiles a shader compiler, it is only ever invoked).
That compiler is what runs every FSR3 upscaler shader at -T cs_5_0
-DFFX_HLSL_SM=50 in four permutations (wave32, wave64, and the 16-bit variant of
each), driven entirely by the fork's own sdk/src/backends/dx11/CMakeLists.txt.

Then staged into a layout EDVR's build.bat can link against directly:

    <dest>\\src\\...                        the sparse checkout (sdk\\, LICENSE.txt)
    <dest>\\include\\FidelityFX\\host\\...   the 7-header closure below
    <dest>\\lib\\*.lib                      ffx_fsr3upscaler_x64.lib, ffx_backend_dx11_x64.lib
    <dest>\\LICENSE.txt
    <dest>\\VERSION.txt                     commit, FSR version, CRT, toolset

Where it goes: %LOCALAPPDATA%\\EDVR\\ffx-dx11 by default -- one build per machine,
shared by every checkout and worktree, matching fetch_ngx.py's ngx-sdk -- or
--dest somewhere else (third_party\\ffx-dx11 for a self-contained checkout).

The static CRT is forced to match EDVR's own /MT, which the fork's CMake does not
set on its own (CMake's un-forced MSVC default is /MD, which fails EDVR's link
with LNK2038): -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DCMAKE_POLICY_DEFAULT_CMP0091=NEW. Only the Release configuration is built.

--verify DIR checks an existing build: every staged file exists; dumpbin
/directives on each library shows the static CRT (LIBCMT, never MSVCRT);
dumpbin /symbols shows no by-name __imp_ reference to d3d11.dll, dxgi.dll or
d3dcompiler_47.dll (EDVR IS d3d11.dll, so a by-name import would bind to
itself -- the EDHM lesson; a d3dcompiler import would mean runtime shader
compilation, which the precompiled DXBC blobs should make unnecessary). One
summary line per check is printed; exits 2 on any mismatch.
"""
import argparse
import glob
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

URL = "https://github.com/optiscaler/FidelityFX-SDK-DX11"
COMMIT = "9b04fa49abae5aa341c0007af13c8ba1b4ad9913"  # optiscaler fork HEAD, read 2026-09-16
FSR_VERSION = "3.1.2"  # FFX_FSR3UPSCALER_VERSION_{MAJOR,MINOR,PATCH} in ffx_fsr3upscaler.h

SPARSE_PATHS = ["sdk"]  # cone mode also keeps LICENSE.txt: it sits at the repo root

# The transitive #include closure of ffx_fsr3upscaler.h, ffx_interface.h, ffx_types.h,
# ffx_error.h and backends/dx11/ffx_dx11.h -- read from the fork's own headers
# 2026-09-16. ffx_fx.h (the per-effect umbrella ffx_fsr1.h/ffx_cas.h/... header) is
# NOT part of this closure: nothing here includes it.
HEADERS = [
    "ffx_fsr3upscaler.h",
    "ffx_interface.h",
    "ffx_types.h",
    "ffx_error.h",
    "ffx_assert.h",
    "ffx_util.h",
    os.path.join("backends", "dx11", "ffx_dx11.h"),
]
HEADER_SRC = os.path.join("sdk", "include", "FidelityFX", "host")  # under <src>
HEADER_DEST = os.path.join("include", "FidelityFX", "host")  # under <dest>

LIBS = ["ffx_fsr3upscaler_x64.lib", "ffx_backend_dx11_x64.lib"]

# The fork's own prebuilt shader compiler (see the module docstring): under
# <src>, never staged into include\/lib\, never shipped. Its SHA-256 is
# recorded in VERSION.txt purely as provenance -- it is invoked once by the
# CMake build below and not touched again by anything EDVR itself builds or
# ships. 2026-09-16: NOT byte-identical to AMD's own upstream copy at tags
# v1.1.4 or v1.1.2 (which are identical to each other, and are plain
# committed binaries there too, not Git LFS) -- consistent with this fork
# having extended the compiler for DX11/cs_5_0, which stock AMD SDK tags
# never target.
SHADER_COMPILER_REL = os.path.join("sdk", "tools", "binary_store", "FidelityFX_SC.exe")

LICENCE = "LICENSE.txt"
VERSION_FILE = "VERSION.txt"

# sdk/CMakeLists.txt sets no CRT; BuildFidelityFXDX11.bat's own flags plus the
# one that turns the FSR3 upscaler component on (see the module docstring and
# the design doc's docs/fsr-upscaler-design-2026-09-16.md section 3.2) plus the
# forced static CRT.
#
# FFX_FSR3UPSCALER=ON alone is enough: passed via -D it is a cache entry from
# the start of the configure, and sdk/src/components/fsr3upscaler/CMakeLists.txt
# reads it directly (`if (FFX_FSR3UPSCALER OR FFX_ALL)`), regardless of whether
# sdk/CMakeLists.txt's own `if (FFX_FSR3) option(FFX_FSR3UPSCALER ON) ...` block
# ever runs. FFX_FSR3 is deliberately NOT set: sdk/src/components/fsr3/CMakeLists.txt
# guards on it too (`if (FFX_FSR3 OR FFX_ALL)`), and that is the COMBINED FSR3
# wrapper (ffxFsr3ContextCreate, bundling frame interpolation and optical flow) --
# a real extra build target measured 2026-09-16: passing FFX_FSR3=ON built an
# unwanted ffx_fsr3_x64.lib alongside the two EDVR actually wants. EDVR calls the
# upscaler-only API (ffxFsr3UpscalerContextCreate/Dispatch in ffx_fsr3upscaler.h),
# so only FFX_FSR3UPSCALER is set.
CMAKE_CONFIGURE_ARGS = [
    "-DFFX_API_CUSTOM=OFF", "-DFFX_API_VK=OFF", "-DFFX_API_DX12=OFF", "-DFFX_ALL=OFF",
    "-DFFX_API_DX11=ON", "-DFFX_FSR=ON", "-DFFX_FSR3UPSCALER=ON",
    "-DFFX_FSR1=OFF", "-DFFX_FSR2=OFF", "-DFFX_FI=OFF", "-DFFX_OF=OFF",
    "-DFFX_AUTO_COMPILE_SHADERS=1", "-DFFX_BUILD_AS_DLL=OFF",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
]

# EDVR IS d3d11.dll: a static lib that imports these BY NAME would bind to itself
# (system_d3d11.h's lesson from the EDHM redirect fix) or, for d3dcompiler, would
# mean the SDK compiles shaders at runtime instead of using its precompiled blobs.
# kernel32 and the CRT's own imports are fine and not matched here.
FORBIDDEN_IMPORT_PREFIXES = (
    "__imp_D3D11", "__imp_CreateDXGI", "__imp_DXGI",
    "__imp_D3DCompile", "__imp_D3DReflect", "__imp_D3DDisassemble",
    "__imp_D3DStripShader", "__imp_D3DGetBlobPart", "__imp_D3DCreateBlob",
    "__imp_D3DGetDebugInfo", "__imp_D3DGetInputSignatureBlob", "__imp_D3DGetOutputSignatureBlob",
    "__imp_D3DGetInputAndOutputSignatureBlob", "__imp_D3DReadFileToBlob", "__imp_D3DWriteBlobToFile",
    "__imp_D3DPreprocess", "__imp_D3DLoadModule", "__imp_D3DCreateLinker", "__imp_D3DCreateFunctionLinkingGraph",
)


def default_dest():
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(base, "EDVR", "ffx-dx11")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(*args, cwd=None):
    print("+", " ".join(str(a) for a in args))
    subprocess.check_call([str(a) for a in args], cwd=cwd)


def run_capture(*args, cwd=None):
    return subprocess.check_output([str(a) for a in args], cwd=cwd,
                                    stderr=subprocess.STDOUT).decode("utf-8", "replace")


def head_of(src):
    try:
        out = subprocess.check_output(["git", "-C", src, "rev-parse", "HEAD"], stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


# ---------------------------------------------------------------------------
# Fetch -- exactly fetch_ngx.py's approach: blobless depth-1 fetch of one
# pinned commit, then a cone-mode sparse checkout.
# ---------------------------------------------------------------------------

def fetch(src, commit):
    os.makedirs(src, exist_ok=True)
    if not os.path.isdir(os.path.join(src, ".git")):
        run("git", "init", "-q", src)
        run("git", "-C", src, "remote", "add", "origin", URL)
    if head_of(src) != commit:
        run("git", "-C", src, "fetch", "--depth", "1", "--filter=blob:none", "origin", commit)
    run("git", "-C", src, "sparse-checkout", "set", *SPARSE_PATHS)
    run("git", "-C", src, "checkout", "-q", "--detach", commit)
    got = head_of(src)
    if got != commit:
        sys.exit("fetch_ffx_dx11: checked out %s, not the pinned %s" % (got, commit))
    print("FidelityFX-SDK-DX11 %s ready at %s" % (commit[:12], src))


# ---------------------------------------------------------------------------
# Toolset discovery -- mirrors build.bat's own vswhere search (build.bat's
# :find_vs, around line 69-81), extended to also locate CMake and dumpbin,
# neither of which build.bat itself needs.
# ---------------------------------------------------------------------------

def find_vs_install():
    progfiles86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = os.path.join(progfiles86, "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vswhere):
        return None
    try:
        out = subprocess.check_output([
            vswhere, "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath",
        ]).decode().strip()
        return out or None
    except (subprocess.CalledProcessError, OSError):
        return None


def _pip_cmake_candidate():
    """The PyPI `cmake` package's own binary (`pip install --user cmake`), if
    importable. Tried two ways, since on this host they resolve to
    differently-spelled-but-both-real paths (Windows AppContainer path
    virtualization): the package's own advertised CMAKE_BIN_DIR, and the
    per-user Scripts directory pip installs its console-script shim into.
    Returns a path that may not exist, or None if neither is importable."""
    try:
        import cmake as cmake_pkg
        return os.path.join(cmake_pkg.CMAKE_BIN_DIR, "cmake.exe")
    except ImportError:
        pass
    try:
        import sysconfig
        scripts = sysconfig.get_path("scripts", "nt_user")
        if scripts:
            return os.path.join(scripts, "cmake.exe")
    except (ImportError, KeyError, TypeError):
        pass
    return None


def cmake_candidates():
    """Ordered (label, path) candidates for cmake.exe: PATH, VS2022's own
    bundled copy (if the "C++ CMake tools for Windows" component is
    installed), then the PyPI `cmake` package -- needed on any machine with
    neither of the other two (true of this one: no system CMake, and VS2022
    Community here does not carry the bundled component either). A path may
    not actually exist; pick_existing() below chooses the first that does."""
    candidates = []
    on_path = shutil.which("cmake")
    if on_path:
        candidates.append(("PATH", on_path))
    vs = find_vs_install()
    if vs:
        candidates.append(("VS2022 bundled", os.path.join(
            vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")))
    pip_cmake = _pip_cmake_candidate()
    if pip_cmake:
        candidates.append(("pip cmake package", pip_cmake))
    return candidates


def pick_existing(candidates):
    """The first (label, path) in order whose path actually exists on disk,
    or (None, None). The pure decision at the heart of find_cmake(), kept
    separate so --self-test can inject candidates offline -- no real PATH,
    registry, or pip install involved -- and still exercise the ordering."""
    for label, path in candidates:
        if path and os.path.isfile(path):
            return label, path
    return None, None


def find_cmake():
    label, path = pick_existing(cmake_candidates())
    if path:
        print("fetch_ffx_dx11: cmake: %s -> %s" % (label, path))
    return path


def find_dumpbin():
    vs = find_vs_install()
    if vs:
        hits = sorted(glob.glob(os.path.join(vs, "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "dumpbin.exe")))
        if hits:
            return hits[-1]
    return shutil.which("dumpbin")


def toolset_string(cmake_exe):
    parts = []
    try:
        parts.append(run_capture(cmake_exe, "--version").splitlines()[0].strip())
    except (subprocess.CalledProcessError, OSError, IndexError):
        pass
    vs = find_vs_install()
    if vs:
        msvc_dirs = sorted(glob.glob(os.path.join(vs, "VC", "Tools", "MSVC", "*")))
        if msvc_dirs:
            parts.append("MSVC " + os.path.basename(msvc_dirs[-1]))
        parts.append(os.path.basename(vs.rstrip("\\/")))
    parts.append('generator "Visual Studio 17 2022" -A x64')
    return "; ".join(parts) if parts else "unknown"


# ---------------------------------------------------------------------------
# Build -- CMake configure + build, Release only.
# ---------------------------------------------------------------------------

def build(src, cmake_exe, jobs=None):
    sdk = os.path.join(src, "sdk")
    build_dir = os.path.join(sdk, "build_edvr")
    run(cmake_exe, "-S", sdk, "-B", build_dir, "-G", "Visual Studio 17 2022", "-A", "x64",
        *CMAKE_CONFIGURE_ARGS)
    args = [cmake_exe, "--build", build_dir, "--config", "Release", "--parallel", str(jobs or os.cpu_count() or 4)]
    run(*args)
    return os.path.join(sdk, "bin", "ffx_sdk")


# ---------------------------------------------------------------------------
# Stage -- the curated include\/lib\ layout build.bat's EDVR_HAVE_FSR3 block
# links against.
# ---------------------------------------------------------------------------

def stage(src, lib_dir, dest, commit, cmake_exe):
    include_dest = os.path.join(dest, HEADER_DEST)
    os.makedirs(os.path.join(include_dest, "backends", "dx11"), exist_ok=True)
    for rel in HEADERS:
        s = os.path.join(src, HEADER_SRC, rel)
        d = os.path.join(include_dest, rel)
        if not os.path.isfile(s):
            sys.exit("fetch_ffx_dx11: expected header missing from the checkout: %s" % s)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(s, d)

    lib_dest = os.path.join(dest, "lib")
    os.makedirs(lib_dest, exist_ok=True)
    for name in LIBS:
        s = os.path.join(lib_dir, name)
        if not os.path.isfile(s):
            sys.exit("fetch_ffx_dx11: build did not produce %s (looked in %s)" % (name, lib_dir))
        shutil.copyfile(s, os.path.join(lib_dest, name))

    licence_src = os.path.join(src, LICENCE)
    if not os.path.isfile(licence_src):
        sys.exit("fetch_ffx_dx11: %s missing from the checkout" % licence_src)
    shutil.copyfile(licence_src, os.path.join(dest, LICENCE))

    dumpbin_exe = find_dumpbin()
    crt = "unknown"
    if dumpbin_exe:
        try:
            crt = crt_of(run_capture(dumpbin_exe, "/directives", os.path.join(lib_dest, LIBS[0])))
        except (subprocess.CalledProcessError, OSError):
            pass

    sc_path = os.path.join(src, SHADER_COMPILER_REL)
    sc_hash = sha256_of(sc_path) if os.path.isfile(sc_path) else "unknown (not found in the checkout)"

    version_text = (
        "commit: %s\n"
        "FSR: %s\n"
        "CRT: %s\n"
        "toolset: %s\n"
        "shader compiler (sdk\\tools\\binary_store\\FidelityFX_SC.exe) SHA-256: %s"
        " -- build-time only, never staged or shipped\n"
        % (commit, FSR_VERSION, crt, toolset_string(cmake_exe), sc_hash)
    )
    with open(os.path.join(dest, VERSION_FILE), "w", encoding="utf-8") as f:
        f.write(version_text)
    print("Staged FidelityFX DX11 %s into %s" % (FSR_VERSION, dest))


# ---------------------------------------------------------------------------
# Verify -- file existence, CRT, and the by-name-self-import check. The
# dumpbin runners are injectable so --self-test can exercise this offline.
# ---------------------------------------------------------------------------

def crt_of(directives_text):
    """'static (LIBCMT)' / 'dynamic (MSVCRT)' / 'unknown', read from a dumpbin
    /directives dump's /DEFAULTLIB tokens. MSVCRT (or MSVCRTD) anywhere beats a
    LIBCMT also present -- a mixed-CRT library is exactly the LNK2038 case."""
    up = directives_text.upper()
    if "MSVCRT" in up:
        return "dynamic (MSVCRT)"
    if "LIBCMT" in up:
        return "static (LIBCMT)"
    return "unknown"


def imports_in(symbols_text):
    return sorted(set(re.findall(r"__imp_[A-Za-z0-9_]+", symbols_text)))


def forbidden_imports(names):
    return [n for n in names if n.startswith(FORBIDDEN_IMPORT_PREFIXES)]


def _find_define(text, name):
    m = re.search(r"#define\s+" + re.escape(name) + r"\s*\((\d+)\)", text)
    return m.group(1) if m else None


def check_library(name, lib_path, run_directives, run_symbols, problems, summary):
    if not os.path.isfile(lib_path):
        problems.append("missing: %s" % lib_path)
        summary.append("%s: MISSING" % name)
        return
    directives = run_directives(lib_path)
    crt = crt_of(directives)
    if crt != "static (LIBCMT)":
        problems.append("%s: CRT is %s, not the static LIBCMT EDVR needs" % (name, crt))
    symbols = run_symbols(lib_path)
    imports = imports_in(symbols)
    bad = forbidden_imports(imports)
    if bad:
        problems.append("%s: by-name import of %s -- EDVR IS d3d11.dll, this binds to itself"
                         % (name, ", ".join(bad)))
    summary.append("%s: CRT=%s imports=%s" % (name, crt, ",".join(imports) if imports else "none"))


def verify(dest, run_directives=None, run_symbols=None, quiet=False):
    """Every staged file the build needs, the CRT, and the self-import check.
    Returns a list of problems, empty when the staged copy is clean."""
    problems = []
    summary = []

    missing_headers = [rel for rel in HEADERS if not os.path.isfile(os.path.join(dest, HEADER_DEST, rel))]
    if missing_headers:
        problems += ["missing: %s" % os.path.join(dest, HEADER_DEST, rel) for rel in missing_headers]
    summary.append("headers: %s" % ("ok (%d)" % len(HEADERS) if not missing_headers else "MISSING"))

    for name in (LICENCE, VERSION_FILE):
        if not os.path.isfile(os.path.join(dest, name)):
            problems.append("missing: %s" % os.path.join(dest, name))

    header_path = os.path.join(dest, HEADER_DEST, "ffx_fsr3upscaler.h")
    version = None
    if os.path.isfile(header_path):
        text = Path(header_path).read_text(encoding="utf-8", errors="replace")
        major = _find_define(text, "FFX_FSR3UPSCALER_VERSION_MAJOR")
        minor = _find_define(text, "FFX_FSR3UPSCALER_VERSION_MINOR")
        patch = _find_define(text, "FFX_FSR3UPSCALER_VERSION_PATCH")
        if major and minor and patch:
            version = "%s.%s.%s" % (major, minor, patch)
    if version:
        summary.append("FSR version: %s" % version)
    else:
        problems.append("could not read FSR version constants from %s" % header_path)

    if run_directives is None or run_symbols is None:
        dumpbin_exe = find_dumpbin()
        if not dumpbin_exe:
            problems.append("dumpbin.exe not found (no VS2022 C++ toolset on PATH or under Program Files) "
                             "-- cannot check the CRT or the import table")
            run_directives = run_symbols = None
        else:
            run_directives = run_directives or (lambda p: run_capture(dumpbin_exe, "/directives", p))
            run_symbols = run_symbols or (lambda p: run_capture(dumpbin_exe, "/symbols", p))

    if run_directives and run_symbols:
        for name in LIBS:
            check_library(name, os.path.join(dest, "lib", name), run_directives, run_symbols, problems, summary)
    else:
        for name in LIBS:
            lib_path = os.path.join(dest, "lib", name)
            summary.append("%s: %s" % (name, "present (CRT/imports unchecked)" if os.path.isfile(lib_path) else "MISSING"))
            if not os.path.isfile(lib_path):
                problems.append("missing: %s" % lib_path)

    if not quiet:
        for line in summary:
            print("fetch_ffx_dx11: " + line)
        if problems:
            for p in problems:
                print("fetch_ffx_dx11: PROBLEM: " + p)
        else:
            print("fetch_ffx_dx11: FidelityFX DX11 %s verified at %s" % (version or FSR_VERSION, dest))
    return problems


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=None, help="where to fetch/build/stage (default: %%LOCALAPPDATA%%\\EDVR\\ffx-dx11)")
    ap.add_argument("--commit", default=COMMIT, help="override the pinned commit")
    ap.add_argument("--verify", default=None, metavar="DIR", help="verify an existing staged copy and exit")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    dest = args.dest or default_dest()

    if args.verify:
        problems = verify(args.verify)
        return 2 if problems else 0

    if args.dry_run:
        print("Would fetch %s at %s into %s" % (URL, args.commit[:12], os.path.join(dest, "src")))
        print("Would configure+build sdk\\ with CMake + VS2022 (Release, static /MT)")
        print("Would stage include\\, lib\\, %s and %s under %s" % (LICENCE, VERSION_FILE, dest))
        return 0

    src = os.path.join(dest, "src")
    fetch(src, args.commit)
    cmake_exe = find_cmake()
    if not cmake_exe:
        sys.exit("fetch_ffx_dx11: no cmake.exe on PATH and none bundled under VS2022 "
                 "(Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin) -- "
                 "install the \"C++ CMake tools for Windows\" VS component, or put cmake on PATH")
    lib_dir = build(src, cmake_exe)
    stage(src, lib_dir, dest, args.commit, cmake_exe)
    problems = verify(dest)
    return 2 if problems else 0


# ---------------------------------------------------------------------------
# Self-test -- offline: no network, no build. Exercises the pin, the verify
# logic (against a synthetic staged tree with injected dumpbin output), and
# the --dry-run property.
# ---------------------------------------------------------------------------

def _write_synthetic_tree(dest):
    inc = os.path.join(dest, HEADER_DEST)
    os.makedirs(os.path.join(inc, "backends", "dx11"), exist_ok=True)
    for rel in HEADERS:
        p = os.path.join(inc, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if rel == "ffx_fsr3upscaler.h":
            Path(p).write_text(
                "#define FFX_FSR3UPSCALER_VERSION_MAJOR      (3)\n"
                "#define FFX_FSR3UPSCALER_VERSION_MINOR      (1)\n"
                "#define FFX_FSR3UPSCALER_VERSION_PATCH      (2)\n",
                encoding="utf-8")
        else:
            Path(p).write_text("// synthetic\n", encoding="utf-8")
    lib_dir = os.path.join(dest, "lib")
    os.makedirs(lib_dir, exist_ok=True)
    for name in LIBS:
        Path(os.path.join(lib_dir, name)).write_bytes(b"synthetic-lib-not-a-real-coff-archive")
    Path(os.path.join(dest, LICENCE)).write_text("MIT\n", encoding="utf-8")
    Path(os.path.join(dest, VERSION_FILE)).write_text("commit: 0\n", encoding="utf-8")


def self_test():
    checks = 0

    assert re.match(r"^[0-9a-f]{40}$", COMMIT), "COMMIT is not a 40-hex-char SHA"
    checks += 1

    assert crt_of('   /DEFAULTLIB:"LIBCMT"') == "static (LIBCMT)"; checks += 1
    assert crt_of('   /DEFAULTLIB:"MSVCRT"') == "dynamic (MSVCRT)"; checks += 1
    assert crt_of('   /DEFAULTLIB:"LIBCMT" /DEFAULTLIB:"MSVCRT"') == "dynamic (MSVCRT)"; checks += 1
    assert crt_of('   /DEFAULTLIB:"OLDNAMES"') == "unknown"; checks += 1

    clean_symbols = "008 00000000 UNDEF  notype       External     | __imp_HeapAlloc\n" \
                     "009 00000000 UNDEF  notype       External     | __imp_GetProcAddress\n"
    dirty_symbols = clean_symbols + "00A 00000000 UNDEF  notype       External     | __imp_D3D11CreateDevice\n"
    assert imports_in(clean_symbols) == ["__imp_GetProcAddress", "__imp_HeapAlloc"]; checks += 1
    assert forbidden_imports(imports_in(clean_symbols)) == []; checks += 1
    assert forbidden_imports(imports_in(dirty_symbols)) == ["__imp_D3D11CreateDevice"]; checks += 1
    assert forbidden_imports(imports_in(clean_symbols + "X | __imp_D3DCompile2\n")) == ["__imp_D3DCompile2"]; checks += 1

    # find_cmake()'s search order (PATH, then VS-bundled, then the pip
    # package) via pick_existing() with injected candidates -- no real PATH,
    # registry, or pip install touched.
    with tempfile.TemporaryDirectory(prefix="edvr-ffx-dx11-") as scratch:
        missing = os.path.join(scratch, "missing.exe")
        path_cmake = os.path.join(scratch, "path_cmake.exe")
        vs_cmake = os.path.join(scratch, "vs_cmake.exe")
        pip_cmake = os.path.join(scratch, "pip_cmake.exe")

        label, path = pick_existing([("PATH", missing), ("VS2022 bundled", missing), ("pip cmake package", missing)])
        assert (label, path) == (None, None), "pick_existing found something out of nowhere"
        checks += 1

        Path(pip_cmake).write_bytes(b"stub")
        label, path = pick_existing([("PATH", missing), ("VS2022 bundled", missing), ("pip cmake package", pip_cmake)])
        assert (label, path) == ("pip cmake package", pip_cmake), "the pip-package tier was not reached"
        checks += 1

        Path(vs_cmake).write_bytes(b"stub")
        label, path = pick_existing([("PATH", missing), ("VS2022 bundled", vs_cmake), ("pip cmake package", pip_cmake)])
        assert (label, path) == ("VS2022 bundled", vs_cmake), "VS-bundled did not win over the pip package"
        checks += 1

        Path(path_cmake).write_bytes(b"stub")
        label, path = pick_existing([("PATH", path_cmake), ("VS2022 bundled", vs_cmake), ("pip cmake package", pip_cmake)])
        assert (label, path) == ("PATH", path_cmake), "PATH did not win when all three exist"
        checks += 1

    # --dry-run writes nothing at all, and reflects a --commit override.
    with tempfile.TemporaryDirectory(prefix="edvr-ffx-dx11-") as scratch:
        dest = os.path.join(scratch, "ffx-dx11")
        buf = io.StringIO()
        old_stdout = sys.stdout
        sys.stdout = buf
        try:
            rc = main(["--dest", dest, "--dry-run", "--commit", "d" * 40])
        finally:
            sys.stdout = old_stdout
        assert rc == 0, "dry-run did not exit 0"
        assert not os.path.exists(dest), "dry-run created %s" % dest
        assert ("d" * 12) in buf.getvalue(), "dry-run did not reflect the --commit override"
        checks += 1

    ok_dir = lambda p: '/DEFAULTLIB:"LIBCMT"'
    ok_sym = lambda p: "External | __imp_HeapAlloc\n"

    with tempfile.TemporaryDirectory(prefix="edvr-ffx-dx11-") as scratch:
        dest = os.path.join(scratch, "staged")
        _write_synthetic_tree(dest)

        problems = verify(dest, run_directives=ok_dir, run_symbols=ok_sym, quiet=True)
        assert problems == [], "clean synthetic tree reported problems: %r" % problems
        checks += 1

        problems = verify(dest, run_directives=lambda p: '/DEFAULTLIB:"MSVCRT"', run_symbols=ok_sym, quiet=True)
        assert problems, "a dynamic CRT was not caught"
        checks += 1

        problems = verify(dest, run_directives=ok_dir,
                           run_symbols=lambda p: "External | __imp_D3D11CreateDevice\n", quiet=True)
        assert problems, "a by-name self-import was not caught"
        checks += 1

        problems = verify(dest, run_directives=ok_dir,
                           run_symbols=lambda p: "External | __imp_D3DCompile\n", quiet=True)
        assert problems, "a by-name d3dcompiler import was not caught"
        checks += 1

        os.remove(os.path.join(dest, HEADER_DEST, "ffx_error.h"))
        problems = verify(dest, run_directives=ok_dir, run_symbols=ok_sym, quiet=True)
        assert any("ffx_error.h" in p for p in problems), "a missing header was not caught"
        checks += 1

    # --dry-run's own property test above also covers "writes nothing"; confirm
    # the CLI's --verify surfaces exit code 2 on a directory that does not exist.
    with tempfile.TemporaryDirectory(prefix="edvr-ffx-dx11-") as scratch:
        buf = io.StringIO()
        old_stdout = sys.stdout
        sys.stdout = buf
        try:
            rc = main(["--verify", os.path.join(scratch, "absent")])
        finally:
            sys.stdout = old_stdout
        assert rc == 2, "verify of a missing directory did not exit 2"
        checks += 1

    print("fetch_ffx_dx11: %d checks passed (offline)" % checks)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("fetch_ffx_dx11: ERROR: " + str(error))
        raise SystemExit(1)
