#!/usr/bin/env python3
"""Build a native-only EDVR release archive from validated build outputs.

Before anything is staged, the version stamped into each EDVR binary being
packaged (d3d11.dll, openvr_api.dll, the installer: the FileVersion and
ProductVersion strings of their VERSIONINFO, which build.bat sets from `git
describe --tags --always --dirty`) is compared with the release version asked
for. The rule: both strings must be exactly "v<version>" -- what git prints on
a clean checkout of the tag v<version> -- or exactly "<version>". Anything
that mentions "dirty" is refused first, as a build from a tree with
uncommitted changes (the kind that has reached a field tester twice); a build
a few commits past the tag ("v0.17.0-2-g650d8a9"), "unknown" (no git), another
release's tag and a binary with no version stamp are refused as mismatches.
--dry-run makes the same check and writes nothing.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# What a release carries under these archive names is stamped with EDVR's own
# version by build.bat (tools\gen_installer_rc.py writes the VERSIONINFO); the
# other files (NVIDIA's runtime, Microsoft's loader) are not ours to stamp.
STAMPED = ("d3d11.dll", "openvr/openvr_api.dll", "edvr-installer.exe", "edvr-flat-installer.exe")
STAMP_KEYS = ("FileVersion", "ProductVersion")


def _version(value):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?", value or ""):
        raise ValueError("version must be semantic, for example 0.3.0")
    return value


def _inside(path, base):
    try:
        return os.path.commonpath([os.path.realpath(path), os.path.realpath(base)]) == os.path.realpath(base)
    except ValueError:
        return False


def _ini_name(profile):
    """The settings file an edition ships and installs: the flat profile has its own."""
    return "edvr-flat.ini" if profile == "flat" else "edvr.ini"


def _files(root, no_dlss, profile="vr"):
    build = root / "build"
    if profile not in ("vr", "flat"):
        raise ValueError("unknown package profile")
    installer = "edvr-flat-installer.exe" if profile == "flat" else "edvr-installer.exe"
    result = [(build / "edvr_openxr_graphics.dll", "d3d11.dll"),
              (build / installer, installer),
              (build / ("edvr_profile_%s.ini" % profile), "edvr_profile.ini")]
    if profile == "vr":
        result += [(build / "edvr_openxr_runtime.dll", "openvr/openvr_api.dll"),
                   (build / "openxr_loader.dll", "openvr/openxr_loader.dll"),
                   (build / "OPENXR-LOADER-LICENSE.txt", "openvr/OPENXR-LOADER-LICENSE.txt"),
                   (root / "release" / "README.txt", "README.txt"),
                   (root / "release" / "OPENVR.txt", "openvr/READ-ME-FIRST.txt")]
    else:
        result.append((build / "edvr-flat-README.txt", "README.txt"))
    # Historical --no-dlss permits builds made without the SDK. It cannot
    # remove a runtime already embedded in the installer we distribute.
    if not no_dlss or (build / "nvngx_dlss.dll").is_file():
        result.append((build / "nvngx_dlss.dll", "nvngx_dlss.dll"))
        result.append((build / "NVIDIA-DLSS-LICENSE.txt", "NVIDIA-DLSS-LICENSE.txt"))
    # The flat edition keeps its settings in a file of its own (config.cpp reads
    # edvr-flat.ini first), so its archive carries that name: a flat archive that
    # shipped an "edvr.ini" told somebody unpacking it by hand to put the flat
    # defaults over the VR profile's tuning, where the flat runtime would not read them.
    result.extend([(build / "edvr-flat.ini" if profile == "flat" else root / "edvr.ini", _ini_name(profile)),
                   (root / "LICENSE", "LICENSE.txt")])
    result.append((root / "third_party" / "dxbc_hash" / "LICENSE.TXT", "DXBC-HASH-LICENSE.txt"))
    # AMD's FSR3 D3D11 port (MIT). Unlike NVIDIA's runtime it ships no DLL --
    # it is statically linked into d3d11.dll -- so its presence in a build is
    # exactly this notice, which build.bat copies beside the binaries when it
    # links the port and deletes on every no-port path. MIT's notice
    # requirement applies to the release because the code is in the binary we
    # distribute (the review of 2026-09-16, F10).
    ffx_notice = build / "FIDELITYFX-SDK-DX11-LICENSE.txt"
    if ffx_notice.is_file():
        result.append((ffx_notice, "FIDELITYFX-SDK-DX11-LICENSE.txt"))
    return result


def version_stamp_problems(version, stamps):
    """Why the binaries in `stamps` cannot be packaged as release `version`:
    a list of messages, empty when they can. `stamps` is [(name, mapping)],
    the mapping holding the FileVersion and ProductVersion strings a binary's
    VERSIONINFO carries, or None for a binary that has no stamp. The rule is
    in the module docstring: exactly "v<version>" or "<version>", and "dirty"
    named for what it is before anything else."""
    accepted = ("v" + version, version)
    problems = []
    for name, stamp in stamps:
        if not stamp:
            problems.append("%s carries no version stamp (no VERSIONINFO FileVersion or "
                            "ProductVersion), so it cannot be told from any other build" % name)
            continue
        by_value = {}
        for key in STAMP_KEYS:
            value = stamp.get(key)
            by_value.setdefault(None if value is None else str(value), []).append(key)
        for value, keys in by_value.items():
            which = "/".join(keys)
            if value is None:
                problems.append("%s has no %s" % (name, which))
            elif "dirty" in value.lower():
                problems.append("%s is stamped %s (%s): built from a tree with uncommitted "
                                "changes" % (name, value, which))
            elif value not in accepted:
                problems.append("%s is stamped %s (%s), not release v%s" % (name, value, which, version))
    return problems


def _stamped_versions(path):
    """{"FileVersion": ..., "ProductVersion": ...} as the VERSIONINFO of the
    file at `path` says (a key it lacks is left out), or None when the file has
    no version resource. Read with the Windows version API; the file is not
    loaded or run."""
    if os.name != "nt":
        raise ValueError("reading a binary's version stamp requires Windows")
    import ctypes
    from ctypes import wintypes
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    api.GetFileVersionInfoW.restype = wintypes.BOOL
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                   ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    api.VerQueryValueW.restype = wintypes.BOOL
    size = api.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    block = ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(path), 0, size, block):
        return None
    data, length = ctypes.c_void_p(), wintypes.UINT()

    def query(sub_block):
        if api.VerQueryValueW(block, sub_block, ctypes.byref(data), ctypes.byref(length)) \
                and length.value and data.value:
            return data.value, length.value
        return None

    languages = []
    found = query("\\VarFileInfo\\Translation")
    if found:
        raw = ctypes.string_at(found[0], found[1])
        languages = ["%04x%04x" % struct.unpack_from("<HH", raw, offset)
                     for offset in range(0, len(raw) - 3, 4)]
    for language in languages or ["040904b0", "040904e4", "000004b0"]:
        stamp = {}
        for key in STAMP_KEYS:
            found = query("\\StringFileInfo\\%s\\%s" % (language, key))
            if found:
                stamp[key] = ctypes.wstring_at(found[0])
        if stamp:
            return stamp
    return {}


def _check_version_stamps(version, files):
    """Refuse (ValueError) unless every EDVR binary in `files` is stamped as
    release `version` (version_stamp_problems)."""
    stamps = [(name, _stamped_versions(source)) for source, name in files if name in STAMPED]
    problems = version_stamp_problems(version, stamps)
    if problems:
        raise ValueError(
            "these binaries are not release %s:\n  %s\n  build.bat stamps them from `git describe "
            "--tags --always --dirty`: check out the tag v%s with a clean tree, run build.bat, "
            "then package." % (version, "\n  ".join(problems), version))
    print("[edvr] version stamps: %s all read v%s" % (", ".join(name for name, _ in stamps), version))


def _embedded_resource(executable, resource_id, required=True):
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
        if not resource:
            error = ctypes.get_last_error()
            # Mandatory RCDATA is checked first; only an absent named resource
            # is an expected result when probing the optional DLSS payload.
            if not required and error == 1814:  # ERROR_RESOURCE_NAME_NOT_FOUND
                return None
            raise ValueError("installer resource %d is missing (Windows error %d)" %
                             (resource_id, error))
        size = kernel.SizeofResource(handle, resource); loaded = kernel.LoadResource(handle, resource)
        pointer = kernel.LockResource(loaded)
        if not size or not pointer: raise ValueError("installer resource %d is empty" % resource_id)
        return ctypes.string_at(pointer, size)
    finally:
        kernel.FreeLibrary(handle)


def _validate_installer_resources(executable, files, profile="vr"):
    by_name = {name: source for source, name in files}
    ids = {101: "d3d11.dll", 103: _ini_name(profile), 107: "edvr_profile.ini"}
    if profile == "vr":
        ids.update({102: "openvr/openvr_api.dll", 105: "openvr/openxr_loader.dll",
                    106: "openvr/OPENXR-LOADER-LICENSE.txt"})
    for resource_id, name in ids.items():
        actual = _embedded_resource(executable, resource_id)
        if hashlib.sha256(actual).digest() != hashlib.sha256(by_name[name].read_bytes()).digest():
            raise ValueError("installer resource %d differs from %s" % (resource_id, name))
    expected_descriptor = ("[install]\r\nschema = 1\r\nprofile = %s\r\n" % profile).encode("ascii")
    if by_name["edvr_profile.ini"].read_bytes() != expected_descriptor:
        raise ValueError("loose descriptor does not declare the packaged profile")
    if profile == "flat":
        for resource_id in (102, 105, 106):
            if _embedded_resource(executable, resource_id, required=False) is not None:
                raise ValueError("flat installer unexpectedly embeds VR resource %d" % resource_id)
    embedded_dlss = _embedded_resource(executable, 104, required=False)
    if "nvngx_dlss.dll" in by_name:
        if embedded_dlss != by_name["nvngx_dlss.dll"].read_bytes():
            raise ValueError("installer resource 104 differs from nvngx_dlss.dll")
        notice = by_name.get("NVIDIA-DLSS-LICENSE.txt")
        if notice is None or not notice.read_bytes().strip():
            raise ValueError("the embedded DLSS runtime requires its NVIDIA license notice")
    elif embedded_dlss is not None:
        raise ValueError("installer embeds DLSS but its matching DLL and notice are missing from the package")


def package(root, version, no_dlss=False, dry_run=False, profile="vr"):
    version = _version(version); root = root.resolve()
    prefix = "edvr-flat" if profile == "flat" else "edvr"
    dist = root / "dist"; stage = dist / ("." + prefix + "-stage-" + version); final_stage = dist / (prefix + "-" + version)
    archive = dist / (prefix + "-" + version + ".zip"); temporary_archive = dist / ("." + prefix + "-" + version + ".zip.tmp")
    if not _inside(stage, dist) or not _inside(final_stage, dist) or not _inside(archive, dist):
        raise ValueError("resolved staging destination escapes repository dist")
    files = _files(root, no_dlss, profile)
    missing = [str(src) for src, _ in files if not src.is_file()]
    if missing:
        raise ValueError("missing release payload:\n  " + "\n  ".join(missing))
    _check_version_stamps(version, files)
    try:
        import openxr_pe
        openxr_pe.native_graphics_exports(str(files[0][0]))
        if profile == "vr": openxr_pe.native_exports(str(root / "build" / "edvr_openxr_runtime.dll"))
    except (ImportError, OSError, ValueError) as exc:
        raise ValueError("native payload validation failed: %s" % exc)
    try:
        if profile == "vr":
            from fetch_openxr_loader import verify
            verify(str(root / "build"))
    except (ImportError, OSError, ValueError) as exc:
        raise ValueError("bundled OpenXR loader validation failed: %s" % exc)
    installer = "edvr-flat-installer.exe" if profile == "flat" else "edvr-installer.exe"
    _validate_installer_resources(root / "build" / installer, files, profile)
    print("[edvr] %s package plan: %s" % (profile, archive))
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
    old_stage = dist / ("." + prefix + "-old-stage-" + version)
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
    installer_archive = dist / (installer[:-4] + "-" + version + ".zip")
    temporary_installer_archive = dist / ("." + installer[:-4] + "-" + version + ".zip.tmp")
    if temporary_installer_archive.exists(): temporary_installer_archive.unlink()
    with zipfile.ZipFile(temporary_installer_archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        output.write(final_stage / installer, installer)
    os.replace(temporary_installer_archive, installer_archive)
    print("[edvr] wrote %s (%d bytes)" % (archive, archive.stat().st_size)); return 0


def self_test():
    def stamp(value, product=None):
        return {"FileVersion": value, "ProductVersion": value if product is None else product}

    def refused(version, stamps, *needles):
        problems = version_stamp_problems(version, stamps)
        text = " | ".join(problems)
        assert problems and all(needle in text for needle in needles), (version, stamps, problems)
        return text

    # The release version check, on fixture strings (no binaries): the rule is
    # exactly "v<version>" or "<version>" in both FileVersion and ProductVersion.
    assert version_stamp_problems("1.2.3", [("d3d11.dll", stamp("v1.2.3")),
                                            ("openvr/openvr_api.dll", stamp("1.2.3"))]) == []
    assert version_stamp_problems("1.2.3-rc.1", [("d3d11.dll", stamp("v1.2.3-rc.1"))]) == []
    assert version_stamp_problems("1.2.3", []) == []
    text = refused("1.2.3", [("d3d11.dll", stamp("v1.2.2"))], "d3d11.dll is stamped v1.2.2",
                   "(FileVersion/ProductVersion)", "not release v1.2.3")
    assert "uncommitted" not in text
    for wrong in ("v1.2.30", "v1.2.3.1", "v1.2.3-1-g0123abc", "1.2.3-rc.1", "unknown", "", " v1.2.3"):
        refused("1.2.3", [("d3d11.dll", stamp(wrong))], "not release v1.2.3")
    refused("1.2.3-rc.1", [("d3d11.dll", stamp("v1.2.3"))], "d3d11.dll is stamped v1.2.3 ")
    for dirty in ("v1.2.3-dirty", "v1.2.3-2-g0123abc-dirty", "V1.2.3-DIRTY", "unknown-dirty"):
        text = refused("1.2.3", [("d3d11.dll", stamp(dirty))], "is stamped " + dirty, "uncommitted changes")
        assert "not release" not in text, text
    text = refused("1.2.3", [("d3d11.dll", stamp("v1.2.3", "v1.2.3-dirty"))], "(ProductVersion)", "uncommitted")
    assert "FileVersion" not in text, text
    refused("1.2.3", [("d3d11.dll", None)], "d3d11.dll carries no version stamp")
    refused("1.2.3", [("d3d11.dll", {})], "d3d11.dll carries no version stamp")
    refused("1.2.3", [("d3d11.dll", {"FileVersion": "v1.2.3"})], "d3d11.dll has no ProductVersion")
    text = refused("1.2.3", [("d3d11.dll", stamp("v1.2.3")), ("openvr/openvr_api.dll", stamp("v1.2.3-dirty")),
                             ("edvr-installer.exe", None)],
                   "openvr/openvr_api.dll is stamped", "edvr-installer.exe carries no version stamp")
    assert "d3d11.dll" not in text, "a binary that matches is not named: " + text
    if os.name == "nt":
        # The real reader: a system DLL carries strings, a file with no version
        # resource carries none.
        system = _stamped_versions(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "kernel32.dll")
        assert system and system.get("FileVersion") and system.get("ProductVersion"), system
        with tempfile.TemporaryDirectory(prefix="edvr-package-stamp-") as folder:
            bare = Path(folder) / "bare.dll"
            bare.write_bytes(b"MZ, but no version resource")
            assert _stamped_versions(bare) is None

    with tempfile.TemporaryDirectory(prefix="edvr-package-test-") as temp:
        root = Path(temp); (root / "build").mkdir(); (root / "release").mkdir()
        for source, _ in _files(root, True):
            source.parent.mkdir(parents=True, exist_ok=True); source.write_bytes(b"payload")
        vr_descriptor = b"[install]\r\nschema = 1\r\nprofile = vr\r\n"
        (root / "build" / "edvr_profile_vr.ini").write_bytes(vr_descriptor)
        (root / "build" / "OPENXR-LOADER-LICENSE.txt").write_bytes(b"notice")
        import openxr_pe
        old_native, old_graphics = openxr_pe.native_exports, openxr_pe.native_graphics_exports
        old_verify = None
        import fetch_openxr_loader
        old_verify = fetch_openxr_loader.verify
        old_resource = globals()['_embedded_resource']
        resources = {101: b"payload", 102: b"payload", 103: b"payload",
                     105: b"payload", 106: b"notice", 107: vr_descriptor}
        def fake_resource(executable, resource_id, required=True):
            if required and resource_id not in resources:
                raise ValueError("missing fixture resource")
            return resources.get(resource_id)
        openxr_pe.native_exports = lambda path: None; openxr_pe.native_graphics_exports = lambda path: None
        fetch_openxr_loader.verify = lambda path: None
        globals()['_embedded_resource'] = fake_resource
        # The fixture binaries are not real PEs. Each reads as stamped with the
        # release being packaged, unless a case overrides one by file name; the
        # local package() below tells the reader which release that is, so the
        # older cases (every version they package is different) need no change.
        old_stamps = globals()['_stamped_versions']
        stamps = {"version": "0.0.0", "override": {}}

        def fake_stamps(path):
            if path.name in stamps["override"]:
                return stamps["override"][path.name]
            return stamp("v" + stamps["version"])

        real_package = globals()['package']

        def package(root, version, *args, **kwargs):
            stamps["version"] = version
            return real_package(root, version, *args, **kwargs)

        globals()['_stamped_versions'] = fake_stamps
        try:
            assert package(root, "1.2.3", no_dlss=True, dry_run=True) == 0
            assert not (root / "dist").exists()
            try: package(root, "../escape", dry_run=True); raise AssertionError("bad version accepted")
            except ValueError: pass
            assert package(root, "1.2.3", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "edvr-installer-1.2.3.zip") as installer_archive:
                assert set(installer_archive.namelist()) == {"edvr-installer.exe"}
                assert installer_archive.read("edvr-installer.exe") == b"payload"
            assert not (root / "dist" / "edvr-installer-1.2.3.exe").exists()
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
                assert names == {"d3d11.dll", "edvr_profile.ini", "openvr/openvr_api.dll", "openvr/openxr_loader.dll",
                                 "openvr/OPENXR-LOADER-LICENSE.txt", "edvr-installer.exe",
                                 "README.txt", "openvr/READ-ME-FIRST.txt", "edvr.ini", "LICENSE.txt", "DXBC-HASH-LICENSE.txt"}
            (root / "build" / "edvr_openxr_runtime.dll").write_bytes(b"payload")
            dlss = root / "build" / "nvngx_dlss.dll"
            notice = root / "build" / "NVIDIA-DLSS-LICENSE.txt"
            resources[104] = b"embedded-dlss"
            try:
                package(root, "1.2.4", no_dlss=True)
                raise AssertionError("embedded runtime accepted without its loose payload")
            except ValueError:
                pass
            assert not (root / "dist" / ".edvr-stage-1.2.4").exists()
            dlss.write_bytes(resources[104])
            try:
                package(root, "1.2.4", no_dlss=True)
                raise AssertionError("embedded runtime accepted without its notice")
            except ValueError:
                pass
            notice.write_bytes(b"NVIDIA runtime notice")
            assert package(root, "1.2.4", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "edvr-1.2.4.zip") as archive:
                assert archive.read("nvngx_dlss.dll") == resources[104]
                assert archive.read("NVIDIA-DLSS-LICENSE.txt") == notice.read_bytes()
            for bad in (None, b"different-installer-runtime"):
                resources[104] = bad
                try:
                    package(root, "1.2.5", no_dlss=True)
                    raise AssertionError("mismatched embedded runtime accepted")
                except ValueError:
                    pass
            resources[104] = dlss.read_bytes()
            notice.write_bytes(b" \n")
            try:
                package(root, "1.2.5", no_dlss=True)
                raise AssertionError("empty DLSS notice accepted")
            except ValueError:
                pass
            # AMD's FSR3 port notice: absent from every archive above (no
            # such file in the fixture build), carried when build.bat has put
            # one there. The port links statically, so this notice IS the
            # release's only trace of it.
            notice.write_bytes(b"NVIDIA runtime notice")
            ffx = root / "build" / "FIDELITYFX-SDK-DX11-LICENSE.txt"
            assert not ffx.exists()
            ffx.write_bytes(b"MIT, FidelityFX SDK DX11 port")
            assert package(root, "1.2.6", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "edvr-1.2.6.zip") as archive:
                assert archive.read("FIDELITYFX-SDK-DX11-LICENSE.txt") == ffx.read_bytes()
            ffx.unlink()
            assert package(root, "1.2.7", no_dlss=True) == 0
            with zipfile.ZipFile(root / "dist" / "edvr-1.2.7.zip") as archive:
                assert "FIDELITYFX-SDK-DX11-LICENSE.txt" not in set(archive.namelist())

            # The release version check through package(): read before anything is
            # staged, so a refused release writes nothing -- dry run or not -- and
            # the message names the binary and what it is stamped.
            graphics, runtime = "edvr_openxr_graphics.dll", "edvr_openxr_runtime.dll"
            for label, override, needle in (
                    ("a dirty DLL", {graphics: stamp("v3.0.0-dirty")},
                     "d3d11.dll is stamped v3.0.0-dirty"),
                    ("a runtime from another release", {runtime: stamp("v2.9.9")},
                     "openvr/openvr_api.dll is stamped v2.9.9"),
                    ("a dev-build installer", {"edvr-installer.exe": stamp("v3.0.0-4-g0123abc")},
                     "edvr-installer.exe is stamped v3.0.0-4-g0123abc"),
                    ("an unstamped DLL", {graphics: None}, "d3d11.dll carries no version stamp")):
                stamps["override"] = {name: value for name, value in override.items()}
                for dry_run in (True, False):
                    try:
                        package(root, "3.0.0", no_dlss=True, dry_run=dry_run)
                        raise AssertionError("%s accepted (dry_run=%s)" % (label, dry_run))
                    except ValueError as error:
                        assert needle in str(error) and "3.0.0" in str(error), (label, str(error))
                assert not (root / "dist" / "edvr-3.0.0.zip").exists(), "a refused release wrote: " + label
                assert not (root / "dist" / ".edvr-stage-3.0.0").exists(), "a refused release staged: " + label
                assert not (root / "dist" / "edvr-3.0.0").exists(), "a refused release staged: " + label
            stamps["override"] = {}
            assert package(root, "3.0.0", no_dlss=True, dry_run=True) == 0
            assert not (root / "dist" / "edvr-3.0.0.zip").exists()
            assert package(root, "3.0.0", no_dlss=True) == 0 and (root / "dist" / "edvr-3.0.0.zip").is_file()

            flat_descriptor = b"[install]\r\nschema = 1\r\nprofile = flat\r\n"
            (root / "build" / "edvr_profile_flat.ini").write_bytes(flat_descriptor)
            (root / "build" / "edvr-flat.ini").write_bytes(b"[fix]\r\ntemporal_aa = off\r\n")
            (root / "build" / "edvr-flat-README.txt").write_bytes(b"Flat qualification build")
            (root / "build" / "edvr-flat-installer.exe").write_bytes(b"installer")
            resources.clear(); resources.update({101: b"payload", 103: b"[fix]\r\ntemporal_aa = off\r\n",
                                                 104: dlss.read_bytes(), 107: flat_descriptor})
            assert package(root, "1.2.8", no_dlss=True, profile="flat") == 0
            with zipfile.ZipFile(root / "dist" / "edvr-flat-1.2.8.zip") as archive:
                names = set(archive.namelist())
                assert "openvr/openvr_api.dll" not in names and "edvr-flat-installer.exe" in names
                assert archive.read("edvr_profile.ini") == flat_descriptor
                # The flat edition's settings file is edvr-flat.ini: the archive names
                # it so, and carries no edvr.ini (the VR profile's) at all.
                assert "edvr-flat.ini" in names and "edvr.ini" not in names, names
                assert archive.read("edvr-flat.ini") == b"[fix]\r\ntemporal_aa = off\r\n"
            # The installer's own copy of the settings is checked against that same
            # file, under its own name.
            resources[103] = b"[fix]\r\ntemporal_aa = dlss\r\n"
            try:
                package(root, "1.2.8", no_dlss=True, profile="flat")
                raise AssertionError("flat installer embedding other settings than edvr-flat.ini accepted")
            except ValueError as error:
                assert "edvr-flat.ini" in str(error), str(error)
            resources[103] = b"[fix]\r\ntemporal_aa = off\r\n"
            # And the VR archive still ships edvr.ini, under that name.
            with zipfile.ZipFile(root / "dist" / "edvr-1.2.7.zip") as archive:
                assert "edvr.ini" in set(archive.namelist()) and "edvr-flat.ini" not in set(archive.namelist())
            with zipfile.ZipFile(root / "dist" / "edvr-flat-installer-1.2.8.zip") as installer_archive:
                assert set(installer_archive.namelist()) == {"edvr-flat-installer.exe"}
                assert installer_archive.read("edvr-flat-installer.exe") == b"installer"
            resources[102] = b"wrong-edition-runtime"
            try:
                package(root, "1.2.9", no_dlss=True, profile="flat")
                raise AssertionError("flat installer with embedded VR runtime accepted")
            except ValueError:
                pass
            del resources[102]

            # The flat archive carries no runtime DLL, so only what it does carry is
            # checked: another release's runtime is not its business, its installer is.
            stamps["override"] = {runtime: stamp("v0.0.1-dirty")}
            assert package(root, "3.0.1", no_dlss=True, profile="flat") == 0
            stamps["override"] = {"edvr-flat-installer.exe": stamp("v3.0.1-dirty")}
            try:
                package(root, "3.0.2", no_dlss=True, profile="flat")
                raise AssertionError("flat installer stamped for another release accepted")
            except ValueError as error:
                assert "edvr-flat-installer.exe is stamped v3.0.1-dirty" in str(error), str(error)
            assert not (root / "dist" / "edvr-flat-3.0.2.zip").exists()
        finally:
            openxr_pe.native_exports, openxr_pe.native_graphics_exports = old_native, old_graphics
            fetch_openxr_loader.verify = old_verify
            globals()['_embedded_resource'] = old_resource
            globals()['_stamped_versions'] = old_stamps
    print("package_native: self-test passed"); return 0


def check_installer(root, profile="vr"):
    """Run after linking, so stale outputs cannot fail the early self-test."""
    root = root.resolve()
    executable = root / "build" / ("edvr-flat-installer.exe" if profile == "flat" else "edvr-installer.exe")
    _validate_installer_resources(executable, _files(root, True, profile), profile)
    if _embedded_resource(executable, 65535, required=False) is not None:
        raise ValueError("unexpected resource 65535 in the native installer")
    print("package_native: actual installer resources match the release files")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?"); parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--no-dlss", action="store_true",
                        help="allow a build without DLSS; retain the DLL and notice when embedded")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--profile", choices=("vr", "flat"), default="vr")
    parser.add_argument("--check-installer", action="store_true",
                        help="verify the built installer's actual resources without executing it")
    args = parser.parse_args(argv)
    if args.self_test: return self_test()
    if args.check_installer: return check_installer(args.root, args.profile)
    return package(args.root, args.version, args.no_dlss, args.dry_run, args.profile)


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print("[edvr] packaging failed: %s" % exc); raise SystemExit(1)
