#!/usr/bin/env python3
"""Build a native-only EDVR release archive from validated build outputs."""
import argparse
import hashlib
import os
import re
import shutil
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _version(value):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?", value or ""):
        raise ValueError("version must be semantic, for example 0.3.0")
    return value


def _inside(path, base):
    try:
        return os.path.commonpath([os.path.realpath(path), os.path.realpath(base)]) == os.path.realpath(base)
    except ValueError:
        return False


def _files(root, no_dlss):
    build = root / "build"
    result = [(build / "edvr_openxr_graphics.dll", "d3d11.dll"),
              (build / "edvr_openxr_runtime.dll", "openvr/openvr_api.dll"),
              (build / "openxr_loader.dll", "openvr/openxr_loader.dll"),
              (build / "OPENXR-LOADER-LICENSE.txt", "openvr/OPENXR-LOADER-LICENSE.txt"),
              (build / "edvr-installer.exe", "edvr-installer.exe"),
              (root / "release" / "README.txt", "README.txt"),
              (root / "release" / "OPENVR.txt", "openvr/READ-ME-FIRST.txt")]
    if not no_dlss:
        result.append((build / "nvngx_dlss.dll", "nvngx_dlss.dll"))
        result.append((build / "NVIDIA-DLSS-LICENSE.txt", "NVIDIA-DLSS-LICENSE.txt"))
    result.extend([(root / "edvr.ini", "edvr.ini"), (root / "LICENSE", "LICENSE.txt")])
    return result


def _embedded_resource(executable, resource_id):
    """Read RCDATA with Windows datafile flags; never execute the installer."""
    if os.name != "nt":
        raise ValueError("installer resource validation requires Windows")
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HANDLE
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL
    handle = kernel.LoadLibraryExW(str(executable), None, 0x2 | 0x20)
    if not handle:
        raise OSError(ctypes.get_last_error(), "LoadLibraryExW failed")
    try:
        resource = kernel.FindResourceW(handle, resource_id, 10)
        if not resource: raise ValueError("installer resource %d is missing" % resource_id)
        size = kernel.SizeofResource(handle, resource); loaded = kernel.LoadResource(handle, resource)
        pointer = kernel.LockResource(loaded)
        if not size or not pointer: raise ValueError("installer resource %d is empty" % resource_id)
        return ctypes.string_at(pointer, size)
    finally:
        kernel.FreeLibrary(handle)


def _validate_installer_resources(executable, files):
    by_name = {name: source for source, name in files}
    ids = {101: "d3d11.dll", 102: "openvr/openvr_api.dll", 103: "edvr.ini",
           105: "openvr/openxr_loader.dll", 106: "openvr/OPENXR-LOADER-LICENSE.txt"}
    for resource_id, name in ids.items():
        actual = _embedded_resource(executable, resource_id)
        if hashlib.sha256(actual).digest() != hashlib.sha256(by_name[name].read_bytes()).digest():
            raise ValueError("installer resource %d differs from %s" % (resource_id, name))
    if "nvngx_dlss.dll" in by_name and _embedded_resource(executable, 104) != by_name["nvngx_dlss.dll"].read_bytes():
        raise ValueError("installer resource 104 differs from nvngx_dlss.dll")


def package(root, version, no_dlss=False, dry_run=False):
    version = _version(version); root = root.resolve()
    dist = root / "dist"; stage = dist / (".edvr-stage-" + version); final_stage = dist / ("edvr-" + version)
    archive = dist / ("edvr-" + version + ".zip"); temporary_archive = dist / (".edvr-" + version + ".zip.tmp")
    if not _inside(stage, dist) or not _inside(final_stage, dist) or not _inside(archive, dist):
        raise ValueError("resolved staging destination escapes repository dist")
    files = _files(root, no_dlss)
    missing = [str(src) for src, _ in files if not src.is_file()]
    if missing:
        raise ValueError("missing release payload:\n  " + "\n  ".join(missing))
    try:
        import openxr_pe
        openxr_pe.native_exports(str(files[1][0]))
        openxr_pe.native_graphics_exports(str(files[0][0]))
    except (ImportError, OSError, ValueError) as exc:
        raise ValueError("native payload validation failed: %s" % exc)
    try:
        from fetch_openxr_loader import verify
        verify(str(root / "build"))
    except (ImportError, OSError, ValueError) as exc:
        raise ValueError("bundled OpenXR loader validation failed: %s" % exc)
    _validate_installer_resources(root / "build" / "edvr-installer.exe", files)
    print("[edvr] native package plan: %s" % archive)
    for _, name in files: print("       %s" % name)
    if dry_run:
        print("[edvr] dry run: wrote nothing."); return 0
    dist.mkdir(parents=True, exist_ok=True)
    if stage.exists(): shutil.rmtree(stage)
    if temporary_archive.exists(): temporary_archive.unlink()
    try:
        for source, name in files:
            destination = stage / Path(name)
            if not _inside(destination, stage): raise ValueError("payload path traversal")
            destination.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, destination)
        with zipfile.ZipFile(temporary_archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for path in sorted(stage.rglob("*")):
                if path.is_file(): output.write(path, path.relative_to(stage).as_posix())
    except Exception:
        if temporary_archive.exists(): temporary_archive.unlink()
        raise
    old_stage = dist / (".edvr-old-stage-" + version)
    if old_stage.exists(): shutil.rmtree(old_stage)
    if final_stage.exists(): final_stage.replace(old_stage)
    try:
        stage.replace(final_stage)
        os.replace(temporary_archive, archive)
    except Exception:
        if final_stage.exists(): shutil.rmtree(final_stage)
        if old_stage.exists(): old_stage.replace(final_stage)
        raise
    if old_stage.exists(): shutil.rmtree(old_stage)
    shutil.copy2(final_stage / "edvr-installer.exe", dist / ("edvr-installer-" + version + ".exe"))
    print("[edvr] wrote %s (%d bytes)" % (archive, archive.stat().st_size)); return 0


def self_test():
    with tempfile.TemporaryDirectory(prefix="edvr-package-test-") as temp:
        root = Path(temp); (root / "build").mkdir(); (root / "release").mkdir()
        for source, _ in _files(root, True):
            source.parent.mkdir(parents=True, exist_ok=True); source.write_bytes(b"payload")
        (root / "build" / "OPENXR-LOADER-LICENSE.txt").write_bytes(b"notice")
        import openxr_pe
        old_native, old_graphics = openxr_pe.native_exports, openxr_pe.native_graphics_exports
        old_verify = None
        import fetch_openxr_loader
        old_verify = fetch_openxr_loader.verify
        old_resources = globals()['_validate_installer_resources']
        openxr_pe.native_exports = lambda path: None; openxr_pe.native_graphics_exports = lambda path: None
        fetch_openxr_loader.verify = lambda path: None
        globals()['_validate_installer_resources'] = lambda executable, files: None
        try:
            assert package(root, "1.2.3", no_dlss=True, dry_run=True) == 0
            assert not (root / "dist").exists()
            try: package(root, "../escape", dry_run=True); raise AssertionError("bad version accepted")
            except ValueError: pass
            assert package(root, "1.2.3", no_dlss=True) == 0
            old_archive = (root / "dist" / "edvr-1.2.3.zip").read_bytes()
            (root / "build" / "edvr_openxr_runtime.dll").unlink()
            try:
                package(root, "1.2.3", no_dlss=True)
                raise AssertionError("missing native payload accepted")
            except ValueError:
                pass
            assert (root / "dist" / "edvr-1.2.3.zip").read_bytes() == old_archive
            with zipfile.ZipFile(root / "dist" / "edvr-1.2.3.zip") as archive:
                names = set(archive.namelist())
                assert names == {"d3d11.dll", "openvr/openvr_api.dll", "openvr/openxr_loader.dll",
                                 "openvr/OPENXR-LOADER-LICENSE.txt", "edvr-installer.exe",
                                 "README.txt", "openvr/READ-ME-FIRST.txt", "edvr.ini", "LICENSE.txt"}
        finally:
            openxr_pe.native_exports, openxr_pe.native_graphics_exports = old_native, old_graphics
            fetch_openxr_loader.verify = old_verify
            globals()['_validate_installer_resources'] = old_resources
    print("package_native: self-test passed"); return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?"); parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--no-dlss", action="store_true"); parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test: return self_test()
    return package(args.root, args.version, args.no_dlss, args.dry_run)


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print("[edvr] packaging failed: %s" % exc); raise SystemExit(1)
