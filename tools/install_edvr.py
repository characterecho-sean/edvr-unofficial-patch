#!/usr/bin/env python3
"""Put a freshly built EDVR next to the game, and prove it landed.

    python tools/install_edvr.py --target steam --dry-run
    python tools/install_edvr.py --target steam
    python tools/install_edvr.py --target frontier --openvr --tag stepped
    python tools/install_edvr.py --target steam --verify-only
    python tools/install_edvr.py --target steam --profile flat --dry-run
    python tools/install_edvr.py --target frontier --native-openxr --dll \
        --native-receipt C:\\temp\\edvr-native.json
    python tools/install_edvr.py --restore-native C:\\temp\\edvr-native.json

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
    over-refuses on purpose. A --dry-run is
    never refused: it copies nothing, so there is nothing to protect.
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
import csv
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

GAME_EXE = "EliteDangerous64.exe"

NATIVE_RECEIPT_VERSION = 1
NATIVE_KIND = "edvr-native-openxr"
LEGACY_GRAPHICS_SOURCE = "build/d3d11.dll"
FLAT_KIND = "edvr-flat"
PROFILE_FILE = "edvr_profile.ini"


def profile_bytes(profile):
    return ("[install]\r\nschema = 1\r\nprofile = %s\r\n" % profile).encode("utf-8")


def native_paths(root, target):
    """The deliberately separate native package and its paired graphics DLL."""
    return {
        "native_source": os.path.join(root, "build", "edvr_openxr_runtime.dll"),
        # Native startup routing is compiled into a distinct graphics image.
        "graphics_source": os.path.join(root, "build", "edvr_openxr_graphics.dll"),
        "native_target": os.path.join(target, "Openvr", "win64", "openvr_api.dll"),
        "graphics_target": os.path.join(target, "d3d11.dll"),
        "original": os.path.join(target, "Openvr", "win64", "openvr_api_orig.dll"),
        "ini": os.path.join(target, "edvr.ini"),
    }


def strict_game_running(target=None):
    """Return (known, running); native staging fails closed on probe errors.

    With a target directory, only an EliteDangerous64.exe running from that
    directory counts: another install's game cannot hold this install's files
    open. A running game process whose image path cannot be resolved keeps
    the conservative refusal.
    """
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=20,
                             check=False,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW",
                                                    0x08000000))
    except (OSError, subprocess.SubprocessError):
        return False, False
    if out.returncode != 0 or not (out.stdout or "").strip():
        return False, False
    rows = list(csv.reader((out.stdout or "").splitlines()))
    if not rows:
        return False, False
    pids = []
    for row in rows:
        if len(row) != 5 or not row[0].strip() or not row[3].isdigit():
            return False, False
        try:
            pid = int(row[1].strip())
        except (TypeError, ValueError):
            return False, False
        if pid < 0:
            return False, False
        if row[0].strip().lower() == GAME_EXE.lower():
            pids.append(pid)
    if target is None:
        return True, bool(pids)
    return game_running_in(target, [_process_image_path(pid) for pid in pids])


def _process_image_path(pid):
    """Executable path of a process, or None when it cannot be queried."""
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32.dll")
    handle = kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buffer,
                                                   ctypes.byref(size)):
            return None
        return buffer.value
    finally:
        kernel32.CloseHandle(handle)


def game_running_in(target, images):
    """(known, running) for a target directory and resolved game image paths.

    images holds one entry per running EliteDangerous64.exe process: its
    executable path, or None when that path could not be queried. A proven
    match wins over an unresolved process; otherwise an unresolved process
    keeps the conservative refusal.
    """
    expected = os.path.normcase(os.path.realpath(os.path.join(target, GAME_EXE)))
    unresolved = False
    for image in images:
        if image is None:
            unresolved = True
            continue
        if os.path.normcase(os.path.realpath(image)) == expected:
            return True, True
    if unresolved:
        return False, False
    return True, False


def _hash_or_missing(path):
    return sha256(path) if os.path.isfile(path) else None


def _valid_hash(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9A-Fa-f]{64}", value) is not None


def _native_backup_name(dst, tag, stamp):
    """Choose a native receipt backup without ever overwriting an old one."""
    base = backup_name(dst, tag, stamp)
    candidate, suffix = base, 1
    while os.path.exists(candidate):
        candidate = "%s.%d" % (base, suffix)
        suffix += 1
    return candidate


def _native_preflight(root, target, receipt_path):
    p = native_paths(root, target)
    errors = []
    for key in ("native_source", "graphics_source"):
        if not os.path.isfile(p[key]):
            errors.append("       missing source %-14s %s" % (key, p[key]))
    for key in ("native_target", "graphics_target", "original"):
        if not os.path.isfile(p[key]):
            errors.append("       missing target %-13s %s" % (key, p[key]))
    game = os.path.join(target, GAME_EXE)
    if not os.path.isfile(game):
        errors.append("       missing target executable %s" % game)
    for key in ("native_target", "graphics_target", "original", "ini"):
        if not _under(p[key], target):
            errors.append("       target path escapes game directory: %s" % p[key])
    if receipt_path is not None:
        if not os.path.isabs(receipt_path):
            errors.append("       --native-receipt must be an absolute new path")
        elif os.path.exists(receipt_path):
            errors.append("       receipt already exists: %s" % receipt_path)
        elif not os.path.isdir(os.path.dirname(os.path.abspath(receipt_path))):
            errors.append("       receipt parent does not exist: %s" %
                          os.path.dirname(os.path.abspath(receipt_path)))
    if errors:
        raise SystemExit("[edvr] native preflight failed:\n" + "\n".join(errors))
    try:
        import openxr_pe
        openxr_pe.validate_native_pair(game, p["native_source"],
                                       p["graphics_source"])
        if validate_elite_game(game) is False:
            raise ValueError("Elite executable profile is not supported")
    except (OSError, ValueError) as exc:
        raise SystemExit("[edvr] native preflight failed:\n"
                         "       %s" % exc)
    return p


def validate_elite_game(path):
    """Validate the qualified executable before staging either profile."""
    try:
        from elite_oculus import validate_elite_oculus
    except ImportError as exc:
        raise ValueError("Elite executable profile validator unavailable: %s" % exc)
    return validate_elite_oculus(path)


def _native_receipt(root, target, paths, backups, before_hashes,
                    installed_hashes, state):
    ini_hash = _hash_or_missing(paths["ini"])
    return {
        "version": NATIVE_RECEIPT_VERSION,
        "kind": NATIVE_KIND,
        "target": os.path.abspath(target),
        "root": os.path.abspath(root),
        "state": state,
        "files": [
            {"key": "native", "source": os.path.abspath(paths["native_source"]),
             "target": os.path.abspath(paths["native_target"]),
             "backup": os.path.abspath(backups["native"]),
             "before_sha256": before_hashes["native"],
             "installed_sha256": installed_hashes["native"]},
            {"key": "graphics", "source": os.path.abspath(paths["graphics_source"]),
             "target": os.path.abspath(paths["graphics_target"]),
             "backup": os.path.abspath(backups["graphics"]),
             "before_sha256": before_hashes["graphics"],
             "installed_sha256": installed_hashes["graphics"]},
        ],
        "original": {"path": os.path.abspath(paths["original"]),
                      "sha256": sha256(paths["original"])},
        "ini": {"path": os.path.abspath(paths["ini"]),
                 "sha256": ini_hash,
                 "missing": ini_hash is None},
    }


def _write_new_receipt(path, receipt):
    """Create, never replace, the user-selected receipt path."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    fd = os.open(path, flags, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            fd = None
            json.dump(receipt, f, indent=2, sort_keys=True)
            f.write("\n")
    finally:
        if fd is not None:
            os.close(fd)


def _reserve_backup(path):
    """Reserve a new backup path without touching an existing file."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    fd = os.open(path, flags, 0o600)
    os.close(fd)


def _replace_receipt(path, receipt):
    """Atomically update our already-created receipt, without a new path."""
    directory = os.path.dirname(os.path.abspath(path))
    fd, temp = tempfile.mkstemp(prefix="edvr-native-receipt-", suffix=".tmp",
                                dir=directory)
    os.close(fd)
    try:
        with open(temp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(receipt, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.remove(temp)


def native_install(root, target, receipt_path, tag, dry_run=False):
    if re.fullmatch(r"[A-Za-z0-9_-]+", tag) is None:
        raise SystemExit("[edvr] native backup tag must contain only letters, digits, _ or -")
    paths = _native_preflight(root, target, receipt_path)
    original_hash = sha256(paths["original"])
    before_hashes = {"native": sha256(paths["native_target"]),
                     "graphics": sha256(paths["graphics_target"])}
    installed_hashes = {"native": sha256(paths["native_source"]),
                        "graphics": sha256(paths["graphics_source"])}
    known, running = strict_game_running(target)
    if not dry_run and (not known or running):
        if not known:
            print("[edvr] ERROR: could not prove that %s is stopped; native "
                  "staging fails closed." % GAME_EXE)
        else:
            print("[edvr] ERROR: %s is running. Close it before native staging."
                  % GAME_EXE)
        return 1

    stamp = datetime.datetime.now()
    backups = {
        "native": _native_backup_name(paths["native_target"], tag, stamp),
        "graphics": _native_backup_name(paths["graphics_target"], tag, stamp),
    }
    if backups["graphics"] == backups["native"]:
        backups["graphics"] += ".graphics"
    print("[edvr] native target: %s" % target)
    print("[edvr] native plan%s:" %
          (" (DRY RUN -- nothing will be written)" if dry_run else ""))
    for key in ("native", "graphics"):
        print("       %-8s %s" % (key, paths[key + "_target"]))
        print("               replace -> %s" % os.path.basename(backups[key]))
    print("       receipt  %s" % (receipt_path or "(not requested in dry run)"))
    if dry_run:
        print("[edvr] dry run: wrote nothing.")
        return 0

    created_backups = []
    verified_backups = []
    active_mutations = []
    receipt = _native_receipt(root, target, paths, backups, before_hashes,
                              installed_hashes, "staging")
    receipt_owned = False
    try:
        # Journal the complete transaction before changing either live DLL.
        _write_new_receipt(receipt_path, receipt)
        receipt_owned = True
        for key in ("native", "graphics"):
            dst = paths[key + "_target"]
            _reserve_backup(backups[key])
            created_backups.append((key, backups[key]))
            shutil.copy2(dst, backups[key])
            if sha256(backups[key]) != before_hashes[key]:
                raise ValueError("backup verification failed: %s" % key)
            verified_backups.append((key, backups[key]))
        for key in ("native", "graphics"):
            dst = paths[key + "_target"]
            src = paths[key + "_source"]
            # Record the mutation before calling copy2: a failed copy may
            # have partially overwritten its destination.
            active_mutations.append((key, dst))
            shutil.copy2(src, dst)
            if sha256(dst) != installed_hashes[key]:
                raise ValueError("installed hash verification failed: %s" % key)
        if sha256(paths["original"]) != original_hash:
            raise ValueError("preserved original changed during staging")
        receipt["state"] = "installed"
        _replace_receipt(receipt_path, receipt)
        verify_native_receipt(receipt_path, target)
    except (OSError, IOError, ValueError) as exc:
        print("[edvr] ERROR: native staging failed: %s" % exc)
        # Restore every live file for which a verified backup exists. An
        # uncertain rollback retains every backup and the journal as recovery
        # evidence; it must never silently discard the only known originals.
        rollback_ok = True
        verified = {key: backup for key, backup in verified_backups}
        for key, dst in reversed(active_mutations):
            backup = verified.get(key)
            if backup is None:
                rollback_ok = False
                continue
            try:
                shutil.copy2(backup, dst)
                if sha256(dst) != before_hashes[key]:
                    raise OSError("rollback hash verification failed: %s" % key)
            except (OSError, IOError):
                rollback_ok = False
        if rollback_ok:
            try:
                if receipt_owned and os.path.isfile(receipt_path):
                    os.remove(receipt_path)
            except OSError:
                rollback_ok = False
        if rollback_ok:
            for _, backup in created_backups:
                try:
                    if os.path.exists(backup):
                        os.remove(backup)
                except OSError:
                    rollback_ok = False
        if not rollback_ok:
            receipt["state"] = "rollback_failed"
            receipt["error"] = str(exc)
            try:
                if receipt_owned:
                    _replace_receipt(receipt_path, receipt)
            except (OSError, IOError, ValueError):
                pass
            print("[edvr] ERROR: rollback is incomplete; retained receipt and "
                  "backups for recovery.")
        return 1
    print("[edvr] native package installed and receipt written: %s" % receipt_path)
    return 0


def _under(path, base):
    try:
        return os.path.normcase(os.path.commonpath(
            [os.path.realpath(path), os.path.realpath(base)])) == \
            os.path.normcase(os.path.realpath(base))
    except ValueError:
        return False


def _load_native_receipt(path):
    if not os.path.isabs(path):
        raise ValueError("receipt path must be absolute")
    with open(path, "r", encoding="utf-8") as f:
        r = json.load(f)
    if isinstance(r, dict) and r.get("version") == 2 and r.get("kind") in (NATIVE_KIND, FLAT_KIND):
        return _validate_v2_receipt(r)
    if not isinstance(r, dict) or r.get("version") != NATIVE_RECEIPT_VERSION or \
       r.get("kind") != NATIVE_KIND:
        raise ValueError("unsupported native receipt")
    target = r.get("target")
    files = r.get("files")
    original = r.get("original")
    ini = r.get("ini")
    if not isinstance(files, list) or len(files) != 2 or \
       not all(isinstance(x, dict) for x in files) or \
       not all(isinstance(x, dict) for x in (original, ini)):
        raise ValueError("malformed native receipt")
    if not isinstance(target, str) or not os.path.isabs(target):
        raise ValueError("receipt target must be absolute")
    target = os.path.abspath(target)
    root = r.get("root")
    if not isinstance(root, str) or not os.path.isabs(root):
        raise ValueError("receipt root must be absolute")
    expected = native_paths(root, target)
    target_real = os.path.realpath(target)
    if not os.path.isdir(target) or not target_real:
        raise ValueError("receipt target is not a directory")
    for key in ("native_target", "graphics_target", "original", "ini"):
        if not _under(expected[key], target):
            raise ValueError("expected path escapes receipt target: %s" % key)
    for entry in files:
        for field in ("key", "source", "target", "backup",
                      "before_sha256", "installed_sha256"):
            if not isinstance(entry.get(field), str):
                raise ValueError("native file field is not a string")
    by_key = {entry["key"]: entry for entry in files}
    if set(by_key) != {"native", "graphics"}:
        raise ValueError("receipt must contain native and graphics files")
    for key in ("native", "graphics"):
        entry = by_key[key]
        allowed_sources = {os.path.abspath(expected[key + "_source"])}
        # Receipts written before the native graphics split recorded the
        # qualification d3d11.dll.  Keep those receipts usable for deliberate
        # restore, while all new staging uses edvr_openxr_graphics.dll.
        if key == "graphics":
            allowed_sources.add(os.path.abspath(
                os.path.join(root, LEGACY_GRAPHICS_SOURCE)))
        if entry["source"] not in allowed_sources or \
           entry["target"] != os.path.abspath(expected[key + "_target"]) or \
           os.path.realpath(entry["target"]) != \
           os.path.realpath(expected[key + "_target"]) or \
           not _under(entry["backup"], target):
            raise ValueError("unsafe %s file entry" % key)
        for hkey in ("before_sha256", "installed_sha256"):
            if not _valid_hash(entry.get(hkey)):
                raise ValueError("bad %s hash" % key)
        if not os.path.isabs(entry["backup"]):
            raise ValueError("backup path must be absolute")
        backup_name_only = os.path.basename(entry["backup"])
        live_name = os.path.basename(entry["target"])
        if os.path.dirname(os.path.realpath(entry["backup"])) != \
           os.path.dirname(os.path.realpath(entry["target"])) or \
           re.fullmatch(re.escape(live_name) +
                        r"\.pre-[^\\/]+-\d{8}-\d{6}\.bak(?:\.\d+)?",
                        backup_name_only) is None:
            raise ValueError("backup is not a native sibling backup: %s" % key)
    if not isinstance(original.get("path"), str) or \
       not isinstance(original.get("sha256"), str) or \
       original.get("path") != os.path.abspath(expected["original"]) or \
       not _valid_hash(original.get("sha256")) or \
       os.path.realpath(original["path"]) != os.path.realpath(expected["original"]):
        raise ValueError("bad original record")
    if not isinstance(ini.get("path"), str) or \
       ini.get("path") != os.path.abspath(expected["ini"]) or \
       not isinstance(ini.get("missing"), bool) or \
       (ini.get("sha256") is not None and not _valid_hash(ini.get("sha256"))) or \
       os.path.realpath(ini["path"]) != os.path.realpath(expected["ini"]):
        raise ValueError("bad ini record")
    if r.get("state") != "installed":
        raise ValueError("receipt is not in installed state")
    protected = {os.path.normcase(os.path.realpath(expected[key]))
                 for key in ("native_target", "graphics_target", "original",
                              "ini", "native_source", "graphics_source")}
    protected.add(os.path.normcase(os.path.realpath(
        os.path.join(root, LEGACY_GRAPHICS_SOURCE))))
    backup_reals = set()
    for key in ("native", "graphics"):
        backup_real = os.path.normcase(os.path.realpath(by_key[key]["backup"]))
        if backup_real in protected or backup_real in backup_reals:
            raise ValueError("backup aliases a protected path")
        backup_reals.add(backup_real)
    # Check all recorded evidence without consulting or changing the INI.
    if sha256(original["path"]) != original["sha256"]:
        raise ValueError("preserved original changed")
    for key in ("native", "graphics"):
        entry = by_key[key]
        if sha256(entry["backup"]) != entry["before_sha256"]:
            raise ValueError("backup changed: %s" % key)
        if sha256(entry["target"]) != entry["installed_sha256"]:
            raise ValueError("active native file changed: %s" % key)
    return r


def verify_native_receipt(receipt_path, target=None):
    """Read-only validation for the native launcher and restore tooling.

    Raises ValueError/OSError for malformed, restored, stale, or externally
    changed files. The INI is intentionally recorded but never required to
    match: user edits remain outside the native transaction.
    """
    receipt = _load_native_receipt(receipt_path)
    if target is not None and os.path.normcase(os.path.realpath(target)) != \
       os.path.normcase(os.path.realpath(receipt["target"])):
        raise ValueError("receipt target mismatch")
    return receipt


def restore_native(receipt_path, dry_run=False):
    try:
        with open(receipt_path, encoding="utf-8") as stream:
            raw = json.load(stream)
        if isinstance(raw, dict) and raw.get("version") == 2:
            return restore_native_v2(receipt_path, dry_run)
        receipt = _load_native_receipt(receipt_path)
        target = receipt["target"]
        entries = {entry["key"]: entry for entry in receipt["files"]}
        if dry_run:
            print("[edvr] native restore plan (DRY RUN): %s" % target)
            print("[edvr] dry run: wrote nothing.")
            return 0
        known, running = strict_game_running(target)
        if not known:
            print("[edvr] ERROR: could not prove that %s is stopped; restore "
                  "fails closed." % GAME_EXE)
            return 1
        if running:
            print("[edvr] ERROR: %s is running; native restore refused." % GAME_EXE)
            return 1

        # Save the staged pair so a failure restoring the second file or
        # updating the receipt can return the directory to the installed
        # state described by the receipt.
        temps = {}
        active_mutations = []
        rollback_ok = True
        try:
            for key in ("native", "graphics"):
                fd, temp = tempfile.mkstemp(prefix="edvr-native-restore-",
                                             suffix=".tmp",
                                             dir=os.path.dirname(entries[key]["target"]))
                os.close(fd)
                temps[key] = temp
                shutil.copy2(entries[key]["target"], temp)
                if sha256(temp) != entries[key]["installed_sha256"]:
                    raise ValueError("staged restore copy changed: %s" % key)
            for key in ("native", "graphics"):
                active_mutations.append(key)
                shutil.copy2(entries[key]["backup"], entries[key]["target"])
            if sha256(entries["native"]["target"]) != entries["native"]["before_sha256"] or \
               sha256(entries["graphics"]["target"]) != entries["graphics"]["before_sha256"]:
                raise ValueError("restored hash verification failed")
        except (OSError, IOError, ValueError) as exc:
            print("[edvr] ERROR: native restore failed: %s" % exc)
            for key in reversed(active_mutations):
                temp = temps[key]
                try:
                    shutil.copy2(temp, entries[key]["target"])
                    if sha256(entries[key]["target"]) != entries[key]["installed_sha256"]:
                        raise OSError("restore rollback hash failed: %s" % key)
                except OSError:
                    rollback_ok = False
                except (IOError, ValueError):
                    rollback_ok = False
            if not rollback_ok:
                print("[edvr] ERROR: restore rollback is incomplete; retained "
                      "temporary recovery files.")
                return 1
            for temp in temps.values():
                try:
                    os.remove(temp)
                except OSError:
                    pass
            return 1
        receipt["state"] = "restored"
        receipt["restored_at"] = datetime.datetime.now().isoformat()
        try:
            # Replace only the receipt we just validated. If this fails,
            # revert the DLLs from the temporary staged copies and leave the
            # installed receipt as the valid recovery record.
            _replace_receipt(receipt_path, receipt)
        except (OSError, IOError, ValueError) as exc:
            for key, temp in temps.items():
                try:
                    shutil.copy2(temp, entries[key]["target"])
                    if sha256(entries[key]["target"]) != entries[key]["installed_sha256"]:
                        raise OSError("receipt rollback hash failed: %s" % key)
                except OSError:
                    rollback_ok = False
                except (IOError, ValueError):
                    rollback_ok = False
            if not rollback_ok:
                print("[edvr] ERROR: receipt update and rollback both failed; "
                      "retained receipt and temporary recovery files.")
            else:
                for temp in temps.values():
                    try:
                        os.remove(temp)
                    except OSError:
                        pass
                print("[edvr] ERROR: receipt update failed; native files were "
                      "restored to their installed state: %s" % exc)
            return 1
        for temp in temps.values():
            try:
                os.remove(temp)
            except OSError:
                pass
        print("[edvr] native files restored; receipt marked restored: %s" % receipt_path)
        return 0
    except (OSError, IOError, ValueError, json.JSONDecodeError) as exc:
        print("[edvr] ERROR: native restore refused: %s" % exc)
        return 1


def _validate_v2_receipt(r):
    if not isinstance(r, dict) or r.get("version") != 2 or r.get("kind") not in (NATIVE_KIND, FLAT_KIND) or r.get("state") != "installed":
        raise ValueError("receipt is not an installed EDVR v2 package")
    target, root, files = r.get("target"), r.get("root"), r.get("files")
    if not isinstance(target, str) or not os.path.isabs(target) or not os.path.isdir(target) or not isinstance(root, str) or not os.path.isabs(root):
        raise ValueError("bad native v2 target/root")
    if not isinstance(files, list) or not all(isinstance(e, dict) and isinstance(e.get("key"), str) for e in files):
        raise ValueError("bad native v2 files")
    paths = _standard_native_paths(root, target) if r["kind"] == NATIVE_KIND else _flat_paths(root, target)
    required = {"runtime", "graphics", "loader", "notice", "config"} if r["kind"] == NATIVE_KIND else {"graphics", "profile"}
    targets = {key: paths[key + "_target"] for key in ("runtime", "graphics", "loader", "notice") if key + "_target" in paths}
    if r["kind"] == NATIVE_KIND: targets["config"] = paths["config"]
    targets.update(profile=os.path.join(target, PROFILE_FILE), ini=os.path.join(target, "edvr.ini"), dlss=os.path.join(target, "nvngx_dlss.dll"))
    keys = [e["key"] for e in files]
    if len(keys) != len(set(keys)) or not required.issubset(keys) or set(keys) - targets.keys():
        raise ValueError("incomplete or duplicate EDVR v2 components")
    if r["kind"] == FLAT_KIND:
        sources = {"graphics": paths["graphics"],
                   "dlss": os.path.join(root, "build", "nvngx_dlss.dll"),
                   "ini": _ini_source(root, "flat"), "profile": "generated"}
        for e in files:
            expected = sources[e["key"]]
            if e.get("source") != (os.path.abspath(expected) if expected != "generated" else expected):
                raise ValueError("unsafe flat source: " + e["key"])
    if r["kind"] == FLAT_KIND and _flat_vr_leftovers(target):
        raise ValueError("VR components remain in flat installation")
    protected = {os.path.normcase(os.path.realpath(path)) for path in targets.values()}
    backups = set()
    for e in files:
        want = os.path.abspath(targets[e["key"]])
        if e.get("target") != want or not _under(want, target):
            raise ValueError("unsafe native v2 target")
        if not _valid_hash(e.get("installed_sha256")) or _hash_or_missing(want) != e["installed_sha256"]:
            raise ValueError("active native file changed: " + e["key"])
        backup, before = e.get("backup"), e.get("before_sha256")
        if backup is None:
            if before is not None:
                raise ValueError("absent backup has a prior hash")
            continue
        if not isinstance(backup, str) or not os.path.isabs(backup) or not _under(backup, target) or not _valid_hash(before):
            raise ValueError("bad native v2 backup")
        real = os.path.normcase(os.path.realpath(backup))
        if real in protected or real in backups or os.path.dirname(real) != os.path.normcase(os.path.dirname(os.path.realpath(want))):
            raise ValueError("native backup aliases a live file or escapes its directory")
        if re.fullmatch(re.escape(os.path.basename(want)) + r"\.pre-[^\\/]+-\d{8}-\d{6}\.bak(?:\.\d+)?", os.path.basename(backup)) is None:
            raise ValueError("backup is not a native sibling backup")
        if _hash_or_missing(backup) != before:
            raise ValueError("native backup changed: " + e["key"])
        backups.add(real)
    return r


def restore_native_v2(receipt_path, dry_run=False):
    try:
        receipt = _load_native_receipt(receipt_path)
        if dry_run:
            print("[edvr] native package restore plan; dry run wrote nothing")
            return 0
        known, running = strict_game_running(receipt["target"])
        if not known or running:
            raise ValueError("native restore requires a proven stopped game")
        snapshots, changed = [], []
        try:
            for entry in receipt["files"]:
                fd, path = tempfile.mkstemp(prefix="edvr-native-restore-", suffix=".tmp", dir=os.path.dirname(entry["target"]))
                os.close(fd)
                snapshots.append((entry, path))
                shutil.copy2(entry["target"], path)
                if sha256(path) != entry["installed_sha256"]:
                    raise ValueError("restore snapshot verification failed: " + entry["key"])
            for entry, path in snapshots:
                changed.append((entry, path))
                if entry["backup"]:
                    shutil.copy2(entry["backup"], entry["target"])
                else:
                    os.remove(entry["target"])
                if _hash_or_missing(entry["target"]) != entry["before_sha256"]:
                    raise ValueError("restore verification failed: " + entry["key"])
            restored = dict(receipt, state="restored", restored_at=datetime.datetime.now().isoformat())
            _replace_receipt(receipt_path, restored)
        except (OSError, ValueError) as exc:
            rollback_ok = True
            for entry, path in reversed(changed):
                try:
                    shutil.copy2(path, entry["target"])
                    if sha256(entry["target"]) != entry["installed_sha256"]:
                        raise ValueError("restore rollback hash mismatch")
                except (OSError, ValueError):
                    rollback_ok = False
            if rollback_ok:
                for _, path in snapshots:
                    try: os.remove(path)
                    except OSError: pass
            else:
                print("[edvr] ERROR: restore rollback incomplete; retained receipt and snapshots:")
                for _, path in snapshots: print("       " + path)
            print("[edvr] ERROR: native restore failed: " + str(exc))
            return 1
        for _, path in snapshots:
            try: os.remove(path)
            except OSError: pass
        print("[edvr] restored every native package component")
        return 0
    except (OSError, ValueError) as exc:
        print("[edvr] ERROR: native restore refused: " + str(exc))
        return 1


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


def _native_direct_plan(root, target, loader, runtime):
    if not os.path.isabs(loader) or not os.path.isfile(loader): raise ValueError("native loader must be an existing absolute file")
    runtime = runtime or "system"
    if runtime != "system" and (not os.path.isabs(runtime) or not os.path.isfile(runtime)): raise ValueError("native runtime must be an existing absolute file or system")
    loader = os.path.abspath(loader)
    if runtime != "system": runtime = os.path.abspath(runtime)
    paths = native_paths(root, target)
    for key in ("native_source", "graphics_source", "original", "native_target", "graphics_target"):
        if not os.path.isfile(paths[key]): raise ValueError("missing native path: %s" % paths[key])
    game = os.path.join(target, GAME_EXE)
    if not os.path.isfile(game): raise ValueError("missing game executable: %s" % game)
    lib = None
    if runtime != "system":
        try:
            with open(runtime, "r", encoding="utf-8") as stream:
                data = json.load(stream)
            lib = data["runtime"]["library_path"]
            if not isinstance(lib, str): raise ValueError
            lib = os.path.abspath(os.path.join(os.path.dirname(runtime), lib)) if not os.path.isabs(lib) else lib
            if not os.path.isfile(lib): raise ValueError("missing runtime library: %s" % lib)
        except (KeyError, TypeError, ValueError, OSError) as exc: raise ValueError("native direct preflight failed: %s" % exc)
    # The game's import contract is independent of runtime selection.
    from openxr_pe import validate_native_pair
    validate_native_pair(game, paths["native_source"], paths["graphics_source"])
    if validate_elite_game(game) is False:
        raise ValueError("Elite executable profile is not supported")
    config = "[openxr]\nversion=1\nloader=%s\ngraphics=%s\nruntime=%s\nseparate_device=1\n" % (loader, paths["graphics_target"], runtime)
    return paths, config.encode("utf-8"), lib

def native_direct(root, target, loader, runtime, dry_run=False):
    paths, config, lib = _native_direct_plan(root, target, loader, runtime)
    known, running = strict_game_running(target)
    if not known or running:
        if not dry_run: print("[edvr] ERROR: direct native route requires a proven stopped game")
        elif not known: print("[edvr] direct native dry run: process state unavailable")
        if not dry_run: return 1
    print("[edvr] direct native plan%s" % (" (DRY RUN)" if dry_run else ""))
    config_path = os.path.join(target, "Openvr", "win64", "edvr_openxr.ini")
    print("       %s -> %s" % (paths["native_source"], paths["native_target"]))
    print("       %s -> %s" % (paths["graphics_source"], paths["graphics_target"]))
    print("       local startup config -> %s" % config_path)
    print("       runtime selection: %s" % (lib if lib else "system discovery"))
    if dry_run: return 0
    shutil.copy2(paths["native_source"], paths["native_target"])
    shutil.copy2(paths["graphics_source"], paths["graphics_target"])
    with open(config_path, "wb") as f: f.write(config)
    expected = {paths["native_target"]: paths["native_source"], paths["graphics_target"]: paths["graphics_source"]}
    for p in (paths["native_target"], paths["graphics_target"]):
        if sha256(p) != sha256(expected[p]): print("[edvr] ERROR: post-copy hash mismatch: %s" % p); return 1
        print("       %s %s" % (p, sha256(p)))
    with open(config_path, "rb") as stream:
        if stream.read() != config:
            print("[edvr] ERROR: post-copy config mismatch")
            return 1
    print("       %s %s" % (config_path, sha256(config_path)))
    return 0

def native_direct_verify(root, target, loader, runtime):
    paths, config, _ = _native_direct_plan(root, target, loader, runtime)
    config_path = os.path.join(target, "Openvr", "win64", "edvr_openxr.ini")
    if not os.path.isfile(config_path): print("[edvr] direct native config mismatch"); return 1
    with open(config_path, "rb") as f: current_config = f.read()
    if current_config != config: print("[edvr] direct native config mismatch"); return 1
    if sha256(paths["native_target"]) != sha256(paths["native_source"]) or sha256(paths["graphics_target"]) != sha256(paths["graphics_source"]): return 1
    print("[edvr] direct native pair and config verified"); return 0

def _standard_native_paths(root, target):
    """Files owned by the normal native OpenXR installation."""
    xr = os.path.join(target, "Openvr", "win64")
    return {"runtime": os.path.join(root, "build", "edvr_openxr_runtime.dll"),
            "graphics": os.path.join(root, "build", "edvr_openxr_graphics.dll"),
            "loader": os.path.join(root, "build", "openxr_loader.dll"),
            "notice": os.path.join(root, "build", "OPENXR-LOADER-LICENSE.txt"),
            "runtime_target": os.path.join(xr, "openvr_api.dll"),
            "graphics_target": os.path.join(target, "d3d11.dll"),
            "loader_target": os.path.join(xr, "openxr_loader.dll"),
            "notice_target": os.path.join(xr, "OPENXR-LOADER-LICENSE.txt"),
            "config": os.path.join(xr, "edvr_openxr.ini"),
            "game": os.path.join(target, GAME_EXE)}


def _flat_paths(root, target):
    return {"graphics": os.path.join(root, "build", "d3d11.dll"),
            "graphics_target": os.path.join(target, "d3d11.dll"),
            "game": os.path.join(target, GAME_EXE)}


def _ini_source(root, profile):
    return os.path.join(root, "edvr.ini") if profile == "vr" else \
        os.path.join(root, "build", "edvr-flat.ini")


def _flat_vr_leftovers(target):
    """Recognized EDVR VR state needs the GUI's original-file recovery."""
    xr = os.path.join(target, "Openvr", "win64")
    candidates = (os.path.join(xr, "edvr_openxr.ini"),
                  os.path.join(xr, "openvr_api_orig.dll"),
                  os.path.join(xr, "openxr_loader.dll"),
                  os.path.join(target, "edvr_native_receipt.json"))
    return [path for path in candidates if os.path.exists(path)]


def _equivalent_startup_config(actual, expected):
    """Accept the same startup config with LF or CRLF line endings only."""
    if actual == expected:
        return True
    normalized = actual.replace(b"\r\n", b"\n")
    return b"\r" not in normalized and normalized == expected


def standard_native_install(root, target, tag, dry_run=False, verify_only=False,
                            include_dlss=False, include_ini=False, profile="vr"):
    """Stage a profile's graphics payload and optional native components."""
    native = profile == "vr"
    p = _standard_native_paths(root, target) if native else _flat_paths(root, target)
    payload = ("runtime", "graphics", "loader", "notice") if native else ("graphics",)
    errors = ["missing %s source %s" % ("native" if native else "flat", p[k]) for k in payload
              if not os.path.isfile(p[k])]
    if include_dlss and not os.path.isfile(os.path.join(root, "build", "nvngx_dlss.dll")):
        errors.append("missing optional DLSS source %s" % os.path.join(root, "build", "nvngx_dlss.dll"))
    ini_source = _ini_source(root, profile)
    if include_ini and not os.path.isfile(ini_source):
        errors.append("missing %s INI source %s" % (profile, ini_source))
    if not os.path.isfile(p["game"]):
        errors.append("missing target executable %s" % p["game"])
    if errors:
        raise SystemExit("[edvr] %s preflight failed:\n       %s" %
                         (profile, "\n       ".join(errors)))
    if native:
        try:
            import fetch_openxr_loader
            fetch_openxr_loader.verify(os.path.join(root, "build"))
        except (ImportError, OSError, ValueError) as exc:
            raise SystemExit("[edvr] pinned OpenXR loader verification failed: %s" % exc)
        try:
            import openxr_pe
            openxr_pe.validate_native_pair(p["game"], p["runtime"], p["graphics"])
            if validate_elite_game(p["game"]) is False:
                raise ValueError("Elite executable profile is not supported")
        except (ImportError, OSError, ValueError) as exc:
            raise SystemExit("[edvr] native preflight failed:\n       %s" % exc)
    else:
        try:
            if validate_elite_game(p["game"]) is False:
                raise ValueError("Elite executable profile is not supported")
        except (ImportError, OSError, ValueError) as exc:
            raise SystemExit("[edvr] flat preflight failed:\n       %s" % exc)
    config_bytes = (("[openxr]\nversion=1\nloader=%s\ngraphics=%s\n"
                     "runtime=system\nseparate_device=1\n" %
                     (os.path.abspath(p["loader_target"]),
                      os.path.abspath(p["graphics_target"]))).encode("utf-8") if native else None)
    descriptor = os.path.join(target, PROFILE_FILE)
    descriptor_bytes = profile_bytes(profile)
    if not verify_only and not native:
        leftovers = _flat_vr_leftovers(target)
        if os.path.isfile(descriptor) and Path(descriptor).read_bytes() != descriptor_bytes:
            leftovers.append(descriptor)
        if leftovers:
            raise SystemExit("[edvr] flat preflight refused existing VR or unknown profile state; "
                             "use edvr-installer.exe to convert and restore originals:\n       " +
                             "\n       ".join(leftovers))
    if verify_only:
        ok = True
        for key in payload:
            dst = p[key + "_target"]
            if not os.path.isfile(dst) or sha256(dst) != sha256(p[key]):
                print("[edvr] %s verify mismatch: %s" % (profile, dst)); ok = False
        if native and (not os.path.isfile(p["config"]) or
                not _equivalent_startup_config(open(p["config"], "rb").read(), config_bytes)):
            print("[edvr] native verify mismatch: %s" % p["config"]); ok = False
        if not os.path.isfile(descriptor) or Path(descriptor).read_bytes() != descriptor_bytes:
            print("[edvr] %s verify mismatch: %s" % (profile, descriptor)); ok = False
        if not native and _flat_vr_leftovers(target):
            print("[edvr] flat verify mismatch: VR components remain"); ok = False
        if include_dlss:
            dlss = os.path.join(root, "build", "nvngx_dlss.dll")
            dst = os.path.join(target, "nvngx_dlss.dll")
            if not os.path.isfile(dst) or sha256(dst) != sha256(dlss):
                print("[edvr] %s verify mismatch: %s" % (profile, dst)); ok = False
        if include_ini:
            ini_target = os.path.join(target, "edvr.ini")
            if (not os.path.isfile(ini_target) or not os.path.isfile(ini_source) or
                    open(ini_target, "rb").read() != open(ini_source, "rb").read()):
                print("[edvr] %s verify mismatch: %s" % (profile, ini_target)); ok = False
        if ok: print("[edvr] %s payload and profile verified" % profile)
        return 0 if ok else 1
    if os.path.isfile(p["graphics_target"]):
        try:
            import openxr_pe
            names, _ = openxr_pe._exports(openxr_pe.Image(Path(p["graphics_target"]).read_bytes()))
            if not any(name.startswith("edvr") for name in names.values()):
                raise ValueError("existing d3d11.dll is a graphics mod")
        except (OSError, ValueError) as exc:
            raise SystemExit("[edvr] Use edvr-installer.exe to preserve and chain the existing d3d11.dll: %s" % exc)
    known, running = strict_game_running(target)
    if not dry_run and (not known or running):
        print("[edvr] ERROR: native staging requires a proven stopped game")
        return 1
    stamp = datetime.datetime.now()
    receipt = os.path.join(target, "edvr_native_receipt.json" if native else "edvr_flat_receipt.json")
    if os.path.exists(receipt): receipt = _native_backup_name(receipt, tag, stamp)
    entries = []
    for key in payload:
        dst = p[key + "_target"]
        entries.append({"key": key, "source": os.path.abspath(p[key]), "target": os.path.abspath(dst),
                        "backup": _native_backup_name(dst, tag, stamp) if os.path.isfile(dst) else None,
                        "before_sha256": sha256(dst) if os.path.isfile(dst) else None,
                    "installed_sha256": sha256(p[key])})
    if include_dlss:
        src = os.path.join(root, "build", "nvngx_dlss.dll"); dst = os.path.join(target, "nvngx_dlss.dll")
        entries.append({"key": "dlss", "source": os.path.abspath(src), "target": os.path.abspath(dst),
                        "backup": _native_backup_name(dst, tag, stamp) if os.path.isfile(dst) else None,
                        "before_sha256": sha256(dst) if os.path.isfile(dst) else None,
                        "installed_sha256": sha256(src)})
    if include_ini:
        src = ini_source; dst = os.path.join(target, "edvr.ini")
        entries.append({"key": "ini", "source": os.path.abspath(src), "target": os.path.abspath(dst),
                        "backup": _native_backup_name(dst, tag, stamp) if os.path.isfile(dst) else None,
                        "before_sha256": sha256(dst) if os.path.isfile(dst) else None,
                        "installed_sha256": sha256(src)})
    if native:
        entries.append({"key": "config", "source": "generated", "target": os.path.abspath(p["config"]),
                        "backup": _native_backup_name(p["config"], tag, stamp) if os.path.isfile(p["config"]) else None,
                        "before_sha256": sha256(p["config"]) if os.path.isfile(p["config"]) else None,
                        "installed_sha256": hashlib.sha256(config_bytes).hexdigest().upper()})
    entries.append({"key": "profile", "source": "generated", "target": os.path.abspath(descriptor),
                    "backup": _native_backup_name(descriptor, tag, stamp) if os.path.isfile(descriptor) else None,
                    "before_sha256": sha256(descriptor) if os.path.isfile(descriptor) else None,
                    "installed_sha256": hashlib.sha256(descriptor_bytes).hexdigest().upper()})
    print("[edvr] %s plan%s: %s" % (profile, " (DRY RUN -- nothing will be written)" if dry_run else "", target))
    for e in entries: print("       %-7s %s" % (e["key"], e["target"]))
    print("       receipt  %s" % receipt)
    if dry_run:
        print("[edvr] dry run: wrote nothing."); return 0
    journal = {"version": 2, "kind": NATIVE_KIND if native else FLAT_KIND, "target": os.path.abspath(target),
               "root": os.path.abspath(root), "state": "staging", "files": entries}
    made, changed = [], []
    receipt_owned = False
    try:
        if native: os.makedirs(os.path.dirname(p["config"]), exist_ok=True)
        _write_new_receipt(receipt, journal)
        receipt_owned = True
        for e in entries:
            if e["backup"]:
                _reserve_backup(e["backup"]); shutil.copy2(e["target"], e["backup"])
                if sha256(e["backup"]) != e["before_sha256"]: raise ValueError("backup verification failed: %s" % e["key"])
                made.append(e)
        for e in entries:
            changed.append(e)
            if e["key"] == "config":
                with open(e["target"], "wb") as stream: stream.write(config_bytes)
            elif e["key"] == "profile":
                with open(e["target"], "wb") as stream: stream.write(descriptor_bytes)
            else: shutil.copy2(e["source"], e["target"])
            if sha256(e["target"]) != e["installed_sha256"]: raise ValueError("installed hash verification failed: %s" % e["key"])
        journal["state"] = "installed"; _replace_receipt(receipt, journal)
        verify_native_receipt(receipt, target)
    except (OSError, IOError, ValueError) as exc:
        print("[edvr] ERROR: native staging failed: %s" % exc); rollback_ok = True
        if not receipt_owned:
            # A concurrent installer may have won exclusive creation. Its
            # receipt belongs to that transaction, and we changed no files.
            return 1
        for e in reversed(changed):
            try:
                if e["backup"]: shutil.copy2(e["backup"], e["target"])
                elif os.path.exists(e["target"]): os.remove(e["target"])
                if _hash_or_missing(e["target"]) != e["before_sha256"]:
                    raise OSError("rollback verification failed: " + e["key"])
            except OSError: rollback_ok = False
        if rollback_ok:
            for e in made:
                try: os.remove(e["backup"])
                except OSError: rollback_ok = False
            try: os.remove(receipt)
            except OSError: rollback_ok = False
        if not rollback_ok:
            journal["state"] = "rollback_failed"; journal["error"] = str(exc)
            try: _replace_receipt(receipt, journal)
            except (OSError, IOError, ValueError): pass
        return 1
    print("[edvr] %s package installed and verified; receipt: %s" % (profile, receipt))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Install a built EDVR into a game directory, verified.")
    ap.add_argument("--target", default="steam",
                    help="steam, frontier, or a path to the game directory")
    ap.add_argument("--profile", choices=("vr", "flat"), default="vr",
                    help="install profile (default: vr)")
    ap.add_argument("--root", default=None,
                    help="the tree to install FROM; defaults to this "
                         "script's own repository, which is what you want "
                         "unless you are installing another worktree's build")
    ap.add_argument("--dll", action="store_true",
                    help="compatibility alias: install the complete native package")
    ap.add_argument("--openvr", action="store_true",
                    help="compatibility alias: native runtime is always installed")
    ap.add_argument("--dlss", action="store_true",
                    help="also install build/nvngx_dlss.dll")
    ap.add_argument("--ini", action="store_true",
                    help="also overwrite the target's edvr.ini with the "
                         "selected profile's template -- this discards tuned settings")
    ap.add_argument("--all", action="store_true",
                    help="selected profile plus available DLSS (never ini)")
    ap.add_argument("--native-openxr", action="store_true",
                     help="stage the experimental native OpenXR DLL and its "
                         "paired native graphics DLL as d3d11.dll (requires --dll, plus a receipt "
                         "or explicit --no-backup local configuration)")
    ap.add_argument("--native-receipt", default=None,
                    help="absolute new receipt path for --native-openxr")
    ap.add_argument("--native-loader", default=None)
    ap.add_argument("--native-runtime", default=None)
    ap.add_argument("--restore-native", default=None, metavar="RECEIPT",
                    help="restore files recorded by a native or flat receipt")
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

    if (args.native_loader or args.native_runtime) and not args.native_openxr:
        ap.error("--native-loader/--native-runtime require --native-openxr")

    if args.restore_native:
        if any((args.native_openxr, args.native_receipt, args.dll, args.openvr,
                args.dlss, args.ini, args.all, args.verify_only, args.force,
                args.no_backup, args.tag)) or args.profile != "vr":
            ap.error("--restore-native cannot be combined with install options")
        if not os.path.isabs(args.restore_native):
            ap.error("--restore-native requires an absolute receipt path")
        return restore_native(args.restore_native, args.dry_run)

    if args.native_receipt and not args.native_openxr:
        ap.error("--native-receipt requires --native-openxr")
    if args.native_receipt and not os.path.isabs(args.native_receipt):
        ap.error("--native-receipt must be an absolute new path")
    if args.native_openxr:
        if args.profile != "vr":
            ap.error("--native-openxr requires --profile vr")
        if not args.dll:
            ap.error("--native-openxr requires --dll; it stages the paired "
                     "graphics DLL as part of the native package")
        direct = args.no_backup
        if (args.native_loader or args.native_runtime) and not direct:
            ap.error("--native-loader/--native-runtime require --no-backup direct mode")
        if any((args.openvr, args.all, args.ini, args.dlss, args.force)):
            ap.error("--native-openxr is mutually exclusive with --openvr, "
                     "--all, --ini, --dlss, and --force")
        if direct:
            if not args.native_loader:
                ap.error("direct native route requires --native-loader")
            if args.native_receipt:
                ap.error("--native-receipt is incompatible with direct native route")
            root = os.path.abspath(args.root) if args.root else repo_root()
            target = resolve_target(args.target)
            try:
                if args.verify_only: return native_direct_verify(root, target, args.native_loader, args.native_runtime or "system")
                return native_direct(root, target, args.native_loader, args.native_runtime or "system", args.dry_run)
            except (OSError, ValueError) as exc:
                print("[edvr] ERROR: direct native operation failed: %s" % exc)
                return 1
        if args.verify_only:
            if not args.native_receipt:
                ap.error("native --verify-only requires --native-receipt")
            try:
                verified = verify_native_receipt(
                    args.native_receipt,
                    resolve_target(args.target))
            except (OSError, ValueError) as exc:
                print("[edvr] ERROR: native receipt verification failed: %s" % exc)
                return 1
            print("[edvr] native receipt verified: %s (%s)" %
                  (args.native_receipt, verified["target"]))
            return 0
        if not args.dry_run and not args.native_receipt:
            ap.error("--native-openxr requires --native-receipt for a real install")
        root = os.path.abspath(args.root) if args.root else repo_root()
        target = resolve_target(args.target)
        tag = args.tag or short_hash(root)
        return native_install(root, target,
                              args.native_receipt,
                              tag, args.dry_run)

    root = os.path.abspath(args.root) if args.root else repo_root()
    target = resolve_target(args.target)
    tag = args.tag or short_hash(root)
    dlss_source = os.path.join(root, "build", "nvngx_dlss.dll")
    if args.force or args.no_backup:
        ap.error("normal native installation requires a stopped game and transactional backups")
    if args.profile == "flat" and (args.dll or args.openvr):
        ap.error("--profile flat cannot be combined with --dll or --openvr")
    if args.dlss and not os.path.isfile(dlss_source):
        ap.error("--dlss requested but build/nvngx_dlss.dll is missing")
    return standard_native_install(root, target, tag, args.dry_run,
                                   args.verify_only,
                                   include_dlss=(args.dlss or (args.all and os.path.isfile(dlss_source))),
                                   include_ini=args.ini, profile=args.profile)


def self_test():
    ok = True

    # The path-aware game probe: only a game running from the install target
    # blocks staging; an unresolvable image path fails closed.
    probe_tmp = tempfile.mkdtemp(prefix="edvr_probe_test_")
    try:
        probe_game = os.path.join(probe_tmp, "game")
        os.makedirs(probe_game)
        for images, want in (
                ([], (True, False)),
                ([None], (False, False)),
                ([os.path.join(probe_tmp, "other", GAME_EXE)], (True, False)),
                ([os.path.join(probe_game, GAME_EXE)], (True, True)),
                ([os.path.join(probe_tmp, "other", GAME_EXE),
                  os.path.join(probe_game, GAME_EXE)], (True, True)),
                ([None, os.path.join(probe_game, GAME_EXE)], (True, True)),
                ([os.path.join(probe_game, GAME_EXE), None], (True, True)),
                ([os.path.join(probe_tmp, "other", GAME_EXE), None],
                 (False, False))):
            got = game_running_in(probe_game, images)
            if got != want:
                print("path-aware probe mismatch: %r gave %r, want %r" %
                      (images, got, want))
                ok = False
        if game_running_in(probe_game,
                           [os.path.join(probe_game, ".", GAME_EXE.lower())]) != (True, True):
            print("path-aware probe lost a case/separator variant")
            ok = False
    finally:
        shutil.rmtree(probe_tmp, ignore_errors=True)

    # VR remains the default and must work on a fresh stock directory
    # (there is no original OpenVR DLL to rename).
    standard_tmp = tempfile.mkdtemp(prefix="edvr_standard_test_")
    try:
        sroot = os.path.join(standard_tmp, "repo"); sgame = os.path.join(standard_tmp, "game")
        os.makedirs(os.path.join(sroot, "build")); os.makedirs(os.path.join(sgame, "Openvr", "win64"))
        for rel, data in (("build/edvr_openxr_runtime.dll", b"RUNTIME"),
                          ("build/edvr_openxr_graphics.dll", b"GRAPHICS"),
                          ("build/openxr_loader.dll", b"LOADER"),
                          ("build/OPENXR-LOADER-LICENSE.txt", b"LICENSE"),
                          ("build/nvngx_dlss.dll", b"DLSS")):
            with open(os.path.join(sroot, rel.replace("/", os.sep)), "wb") as f: f.write(data)
        with open(os.path.join(sgame, GAME_EXE), "wb") as f: f.write(b"GAME")
        with open(os.path.join(sgame, "edvr.ini"), "wb") as f: f.write(b"[user]\nkeep=1\n")
        import openxr_pe as _standard_pe
        import fetch_openxr_loader as _standard_loader
        old_loader_verify = _standard_loader.verify
        _standard_loader.verify = lambda path: None
        old_standard_pair = _standard_pe.validate_native_pair
        old_standard_profile = globals()["validate_elite_game"]
        old_standard_probe = globals()["strict_game_running"]
        _standard_pe.validate_native_pair = lambda *args: None
        globals()["validate_elite_game"] = lambda path: None
        globals()["strict_game_running"] = lambda *a: (True, False)
        try:
            before = sorted((os.path.relpath(os.path.join(b, n), sgame), open(os.path.join(b, n), "rb").read())
                            for b, _, ns in os.walk(sgame) for n in ns)
            if main(["--root", sroot, "--target", sgame, "--all", "--dry-run"]) != 0: ok = False
            after = sorted((os.path.relpath(os.path.join(b, n), sgame), open(os.path.join(b, n), "rb").read())
                           for b, _, ns in os.walk(sgame) for n in ns)
            if before != after: print("standard dry run wrote files"); ok = False
            if main(["--root", sroot, "--target", sgame, "--all"]) != 0: ok = False
            assert Path(sgame, PROFILE_FILE).read_bytes() == profile_bytes("vr")
            sp = _standard_native_paths(sroot, sgame)
            if any(sha256(sp[k + "_target"]) != sha256(sp[k]) for k in ("runtime", "graphics", "loader", "notice")):
                print("standard native payload mismatch"); ok = False
            expected_standard_config = ("[openxr]\nversion=1\nloader=%s\ngraphics=%s\nruntime=system\nseparate_device=1\n" %
                    (os.path.abspath(sp["loader_target"]), os.path.abspath(sp["graphics_target"]))).encode("utf-8")
            if open(sp["config"], "rb").read() != expected_standard_config:
                print("standard config mismatch"); ok = False
            # The GUI installer serializes this same config with CRLF. CLI
            # verification must accept that equivalent text without writing.
            gui_config = expected_standard_config.replace(b"\n", b"\r\n")
            Path(sp["config"]).write_bytes(gui_config)
            before_verify = sorted(
                (os.path.relpath(os.path.join(b, n), sgame),
                 Path(b, n).read_bytes(),
                 os.stat(os.path.join(b, n)).st_mtime_ns)
                for b, _, ns in os.walk(sgame) for n in ns)
            if main(["--root", sroot, "--target", sgame, "--all", "--verify-only"]) != 0:
                print("GUI CRLF config failed standard verify-only")
                ok = False
            after_verify = sorted(
                (os.path.relpath(os.path.join(b, n), sgame),
                 Path(b, n).read_bytes(),
                 os.stat(os.path.join(b, n)).st_mtime_ns)
                for b, _, ns in os.walk(sgame) for n in ns)
            if before_verify != after_verify:
                print("standard verify-only wrote files")
                ok = False
            # A real config change remains an error even when its line endings
            # match the GUI serialization.
            Path(sp["config"]).write_bytes(gui_config.replace(b"runtime=system", b"runtime=changed"))
            if main(["--root", sroot, "--target", sgame, "--all", "--verify-only"]) == 0:
                print("divergent GUI config accepted by standard verify-only")
                ok = False
            # Receipt recovery continues to compare the installed config's
            # exact bytes, so restore the CLI serialization for that path.
            Path(sp["config"]).write_bytes(expected_standard_config)
            if open(os.path.join(sgame, "edvr.ini"), "rb").read() != b"[user]\nkeep=1\n":
                print("standard install changed user INI"); ok = False
            if not os.path.isfile(os.path.join(sgame, "nvngx_dlss.dll")): print("--all omitted DLSS"); ok = False
            receipt_path = os.path.join(sgame, "edvr_native_receipt.json")
            assert verify_native_receipt(receipt_path, sgame)["version"] == 2
            before_restore = sorted((os.path.relpath(os.path.join(b,n),sgame),Path(b,n).read_bytes()) for b,_,ns in os.walk(sgame) for n in ns)
            assert restore_native(receipt_path, True) == 0
            assert before_restore == sorted((os.path.relpath(os.path.join(b,n),sgame),Path(b,n).read_bytes()) for b,_,ns in os.walk(sgame) for n in ns)
            # A receipt-write failure restores the installed package as well.
            replace_saved = globals()["_replace_receipt"]
            def fail_receipt(*args): raise OSError("injected receipt write failure")
            globals()["_replace_receipt"] = fail_receipt
            try:
                assert restore_native(receipt_path) == 1
                verify_native_receipt(receipt_path)
            finally: globals()["_replace_receipt"] = replace_saved
            assert restore_native(receipt_path) == 0
            assert all(not os.path.exists(sp[key+"_target"]) for key in ("runtime","graphics","loader","notice"))
            assert not os.path.exists(sp["config"])
            assert Path(sgame,"edvr.ini").read_bytes() == b"[user]\nkeep=1\n"
            # Reinstall with existing runtime/config exercises all backup paths.
            Path(sp["runtime_target"]).write_bytes(b"OLD-RUNTIME")
            Path(sp["config"]).write_bytes(b"OLD-CONFIG")
            assert main(["--root",sroot,"--target",sgame,"--tag","second"]) == 0
            receipt_path = next(str(path) for path in Path(sgame).glob("edvr_native_receipt.json.pre-second-*.bak"))
            verify_native_receipt(receipt_path)
            assert restore_native(receipt_path) == 0
            assert Path(sp["runtime_target"]).read_bytes() == b"OLD-RUNTIME"
            assert Path(sp["config"]).read_bytes() == b"OLD-CONFIG"
            # Losing exclusive journal creation must not erase the winner's
            # evidence. No live files have been touched at that point.
            create_saved = globals()["_write_new_receipt"]
            raced = []
            def collide_receipt(path, journal):
                Path(path).write_bytes(b"OTHER-INSTALLER-RECEIPT")
                raced.append(path)
                raise FileExistsError("simulated receipt creation race")
            globals()["_write_new_receipt"] = collide_receipt
            try:
                assert main(["--root",sroot,"--target",sgame,"--tag","race"]) == 1
                assert Path(raced[0]).read_bytes() == b"OTHER-INSTALLER-RECEIPT"
                assert Path(sp["runtime_target"]).read_bytes() == b"OLD-RUNTIME"
            finally: globals()["_write_new_receipt"] = create_saved
            # A partially failed copy followed by a corrupt rollback retains
            # the verified old bytes and its journal instead of claiming success.
            copy_saved = shutil.copy2
            def corrupt_stage_and_restore(source, destination, *args, **kwargs):
                if str(destination) == sp["runtime_target"]:
                    Path(destination).write_bytes(b"PARTIAL")
                    if str(source) == sp["runtime"]: raise OSError("injected copy failure")
                    return destination
                return copy_saved(source, destination, *args, **kwargs)
            shutil.copy2 = corrupt_stage_and_restore
            try:
                assert main(["--root",sroot,"--target",sgame,"--tag","corrupt"]) == 1
            finally: shutil.copy2 = copy_saved
            failure_path = next(Path(sgame).glob("edvr_native_receipt.json.pre-corrupt-*.bak"))
            failure = json.loads(failure_path.read_text())
            assert failure["state"] == "rollback_failed"
            runtime_backup = next(e["backup"] for e in failure["files"] if e["key"] == "runtime")
            assert Path(runtime_backup).read_bytes() == b"OLD-RUNTIME"
            # Foreign graphics must be preserved, even in a dry run.
            Path(sp["graphics_target"]).write_bytes(b"FOREIGN-GRAPHICS")
            try:
                main(["--root",sroot,"--target",sgame,"--dry-run"])
                raise AssertionError("foreign graphics accepted without chaining")
            except SystemExit: pass
            assert Path(sp["graphics_target"]).read_bytes() == b"FOREIGN-GRAPHICS"
            # Pin verification must also run for an explicitly selected other root.
            _standard_loader.verify = old_loader_verify
            try:
                main(["--root",sroot,"--target",sgame,"--dry-run"])
                raise AssertionError("tampered loader accepted")
            except SystemExit: pass
        finally:
            _standard_loader.verify = old_loader_verify
            _standard_pe.validate_native_pair = old_standard_pair
            globals()["validate_elite_game"] = old_standard_profile
            globals()["strict_game_running"] = old_standard_probe
    except (OSError, IOError, ValueError, SystemExit) as exc:
        print("standard native self-test failed: %s" % exc); ok = False
    finally:
        shutil.rmtree(standard_tmp, ignore_errors=True)

    # Flat staging needs only the graphics build and must leave the game's
    # stock OpenVR directory byte-for-byte untouched.
    flat_tmp = tempfile.mkdtemp(prefix="edvr_flat_test_")
    try:
        froot = os.path.join(flat_tmp, "repo")
        fgame = os.path.join(flat_tmp, "game")
        fxr = os.path.join(fgame, "Openvr", "win64")
        os.makedirs(os.path.join(froot, "build"))
        os.makedirs(fxr)
        Path(froot, "build", "d3d11.dll").write_bytes(b"FLAT-GRAPHICS")
        Path(froot, "build", "nvngx_dlss.dll").write_bytes(b"FLAT-DLSS")
        Path(froot, "edvr.ini").write_bytes(b"[fix]\ntemporal_aa=dlss\n")
        Path(fgame, GAME_EXE).write_bytes(b"GAME")
        Path(fgame, "edvr.ini").write_bytes(b"[user]\nkeep=1\n")
        Path(fxr, "openvr_api.dll").write_bytes(b"STOCK-OPENVR")
        old_profile_validate = globals()["validate_elite_game"]
        old_probe = globals()["strict_game_running"]
        globals()["validate_elite_game"] = lambda path: None
        globals()["strict_game_running"] = lambda *a: (True, False)
        try:
            snapshot = lambda: sorted((str(p.relative_to(fgame)), p.read_bytes())
                                      for p in Path(fgame).rglob("*") if p.is_file())
            xr_snapshot = lambda: sorted((str(p.relative_to(fxr)), p.read_bytes())
                                         for p in Path(fxr).rglob("*") if p.is_file())
            before = snapshot()
            before_xr = xr_snapshot()
            args = ["--root", froot, "--target", fgame, "--profile", "flat", "--all"]
            assert main(args + ["--dry-run"]) == 0
            assert snapshot() == before, "flat dry run wrote files"
            assert main(args) == 0
            assert xr_snapshot() == before_xr, "flat install changed Openvr"
            assert Path(fgame, "d3d11.dll").read_bytes() == b"FLAT-GRAPHICS"
            assert Path(fgame, "nvngx_dlss.dll").read_bytes() == b"FLAT-DLSS"
            assert Path(fgame, "edvr.ini").read_bytes() == b"[user]\nkeep=1\n"
            descriptor_path = Path(fgame, PROFILE_FILE)
            assert descriptor_path.read_bytes() == profile_bytes("flat")
            receipt_path = Path(fgame, "edvr_flat_receipt.json")
            assert verify_native_receipt(str(receipt_path), fgame)["kind"] == FLAT_KIND
            assert main(args + ["--verify-only"]) == 0
            descriptor_path.write_bytes(profile_bytes("vr"))
            assert main(args + ["--verify-only"]) == 1
            descriptor_path.write_bytes(profile_bytes("flat"))
            leftover = Path(fxr, "edvr_openxr.ini")
            leftover.write_bytes(b"VR-LEFTOVER")
            before = snapshot()
            try:
                main(args + ["--dry-run"])
                raise AssertionError("flat dry run accepted VR leftovers")
            except SystemExit: pass
            assert snapshot() == before, "refused flat dry run wrote files"
            assert main(args + ["--verify-only"]) == 1
            leftover.unlink()
            assert main(args + ["--verify-only"]) == 0
            assert restore_native(str(receipt_path)) == 0
            assert xr_snapshot() == before_xr, "flat restore changed Openvr"
            assert not Path(fgame, "d3d11.dll").exists()
            assert not descriptor_path.exists()
            # --ini must require the generated flat template, never copy the
            # full VR INI in the repository root, and restore the user's INI.
            ini_args = args + ["--ini", "--tag", "flatini"]
            before = snapshot()
            try:
                main(ini_args + ["--dry-run"])
                raise AssertionError("flat --ini accepted a missing flat template")
            except SystemExit: pass
            assert snapshot() == before, "missing flat template dry run wrote files"
            flat_template = Path(froot, "build", "edvr-flat.ini")
            flat_template.write_bytes(b"[fix]\r\ntemporal_aa = off\r\n")
            assert main(ini_args + ["--dry-run"]) == 0
            assert snapshot() == before, "flat --ini dry run wrote files"
            assert main(ini_args) == 0
            assert Path(fgame, "edvr.ini").read_bytes() == flat_template.read_bytes()
            ini_receipt = next(Path(fgame).glob("edvr_flat_receipt.json.pre-flatini-*.bak"))
            ini_entry = next(e for e in verify_native_receipt(str(ini_receipt), fgame)["files"]
                             if e["key"] == "ini")
            assert ini_entry["source"] == str(flat_template)
            assert Path(ini_entry["backup"]).read_bytes() == b"[user]\nkeep=1\n"
            receipt_bytes = ini_receipt.read_bytes()
            changed_receipt = json.loads(receipt_bytes)
            next(e for e in changed_receipt["files"] if e["key"] == "ini")["source"] = str(Path(froot, "edvr.ini"))
            ini_receipt.write_text(json.dumps(changed_receipt), encoding="utf-8")
            try:
                verify_native_receipt(str(ini_receipt), fgame)
                raise AssertionError("flat receipt accepted full VR INI source")
            except ValueError: pass
            ini_receipt.write_bytes(receipt_bytes)
            assert main(ini_args + ["--verify-only"]) == 0
            Path(fgame, "edvr.ini").write_bytes(b"[fix]\ntemporal_aa=dlss\n")
            assert main(ini_args + ["--verify-only"]) == 1
            Path(fgame, "edvr.ini").write_bytes(flat_template.read_bytes())
            assert restore_native(str(ini_receipt)) == 0
            assert Path(fgame, "edvr.ini").read_bytes() == b"[user]\nkeep=1\n"
            assert xr_snapshot() == before_xr, "flat --ini changed Openvr"
            Path(fgame, "d3d11.dll").write_bytes(b"FOREIGN-GRAPHICS")
            before = snapshot()
            try:
                main(args + ["--dry-run"])
                raise AssertionError("flat dry run accepted foreign graphics")
            except SystemExit: pass
            assert snapshot() == before, "foreign DLL refusal wrote files"
        finally:
            globals()["validate_elite_game"] = old_profile_validate
            globals()["strict_game_running"] = old_probe
    except (OSError, IOError, ValueError, SystemExit, AssertionError) as exc:
        print("flat self-test failed: %s" % exc); ok = False
    finally:
        shutil.rmtree(flat_tmp, ignore_errors=True)

    # Direct native route: use a complete temporary pair and replace the
    # contract validator/process probe so this remains a CPU-only test.
    direct_tmp = tempfile.mkdtemp(prefix="edvr_direct_test_")
    try:
        droot = os.path.join(direct_tmp, "repo"); dgame = os.path.join(direct_tmp, "game")
        os.makedirs(os.path.join(droot, "build")); os.makedirs(os.path.join(dgame, "Openvr", "win64"))
        for rel, data in (("build/edvr_openxr_runtime.dll", b"NATIVE"), ("build/edvr_openxr_graphics.dll", b"GRAPHICS")):
            with open(os.path.join(droot, rel.replace("/", os.sep)), "wb") as f: f.write(data)
        for rel, data in ((GAME_EXE, b"GAME"), ("d3d11.dll", b"OLDG"),
                          ("Openvr/win64/openvr_api.dll", b"OLDN"),
                          ("Openvr/win64/openvr_api_orig.dll", b"ORIGINAL"),
                          ("edvr.ini", b"[user]\nkeep=1\n")):
            p=os.path.join(dgame,rel.replace("/",os.sep)); os.makedirs(os.path.dirname(p),exist_ok=True)
            with open(p,"wb") as f:f.write(data)
        loader=os.path.join(direct_tmp,"loader.dll"); lib=os.path.join(direct_tmp,"runtime.dll"); manifest=os.path.join(direct_tmp,"runtime.json")
        for p in (loader,lib):
            with open(p,"wb") as f:f.write(b"X")
        with open(manifest,"w",encoding="utf-8") as f: json.dump({"runtime":{"library_path":"runtime.dll"}},f)
        import openxr_pe as _pe
        old_validate=_pe.validate_native_pair; old_profile=globals()["validate_elite_game"]; old_probe=globals()["strict_game_running"]
        calls=[0]
        def fake_validate(game,native,graphics): calls[0]+=1
        _pe.validate_native_pair=fake_validate
        globals()["validate_elite_game"] = lambda game: None
        globals()["strict_game_running"]=lambda *a:(True,False)
        before_ini=open(os.path.join(dgame,"edvr.ini"),"rb").read(); before_orig=open(os.path.join(dgame,"Openvr","win64","openvr_api_orig.dll"),"rb").read()
        if native_direct(droot,dgame,loader,manifest,False)!=0 or calls[0]!=1: ok=False
        paths=native_paths(droot,dgame)
        if native_direct_verify(droot,dgame,loader,manifest)!=0: ok=False
        validations_before=calls[0]
        system_paths, system_config, system_lib = _native_direct_plan(droot,dgame,loader,None)
        if b"runtime=system" not in system_config or system_lib is not None or calls[0]!=validations_before+1: ok=False
        if native_direct(droot,dgame,loader,None,True)!=0: ok=False
        with open(paths["graphics_target"],"wb") as f:f.write(b"STALE")
        if native_direct_verify(droot,dgame,loader,manifest)==0: ok=False
        with open(paths["graphics_target"],"wb") as f:f.write(b"GRAPHICS")
        with open(os.path.join(dgame,"Openvr","win64","edvr_openxr.ini"),"wb") as f:f.write(b"stale")
        if native_direct_verify(droot,dgame,loader,manifest)==0: ok=False
        try: native_direct(droot,dgame,"relative-loader",manifest,True); ok=False
        except ValueError: pass
        os.remove(lib)
        try: native_direct(droot,dgame,loader,manifest,True); ok=False
        except ValueError: pass
        with open(lib,"wb") as f:f.write(b"X")
        # Dry run is write-free even when the game probe is unknown/busy.
        def direct_snapshot():
            return {os.path.relpath(os.path.join(dp,n),direct_tmp):sha256(os.path.join(dp,n))
                    for dp,dn,fn in os.walk(direct_tmp) for n in fn}
        before=direct_snapshot()
        # Direct native mode shares the same static pair and profile gates;
        # both ordinary and dry-run requests must fail before any write.
        saved_direct_pair = _pe.validate_native_pair
        def reject_direct_pair(game, native, graphics):
            raise ValueError("mismatched native graphics")
        _pe.validate_native_pair = reject_direct_pair
        for dry in (False, True):
            try:
                native_direct(droot, dgame, loader, manifest, dry)
                print("direct native mismatch was accepted")
                ok=False
            except ValueError:
                pass
            if direct_snapshot() != before:
                print("direct native mismatch changed files")
                ok=False
        _pe.validate_native_pair = saved_direct_pair
        saved_direct_profile = globals()["validate_elite_game"]
        def reject_direct_profile(path):
            raise ValueError("unknown Elite executable revision")
        globals()["validate_elite_game"] = reject_direct_profile
        for dry in (False, True):
            try:
                native_direct(droot, dgame, loader, manifest, dry)
                print("direct native unknown executable was accepted")
                ok=False
            except ValueError:
                pass
            if direct_snapshot() != before:
                print("direct native unknown executable changed files")
                ok=False
        globals()["validate_elite_game"] = saved_direct_profile
        if main(["--root",droot,"--target",dgame,"--native-openxr","--dll","--no-backup","--native-loader",loader,"--dry-run"])!=0: ok=False
        if direct_snapshot()!=before: ok=False
        globals()["strict_game_running"]=lambda *a:(False,False)
        if native_direct(droot,dgame,loader,os.path.join(direct_tmp,"runtime.json"),True)!=0: ok=False
        after=direct_snapshot()
        if before!=after: ok=False
        for state in ((True,True), (False,False)):
            globals()["strict_game_running"]=lambda *a:state
            if native_direct(droot,dgame,loader,manifest,False)==0: ok=False
            if direct_snapshot()!=before: ok=False
        if open(os.path.join(dgame,"edvr.ini"),"rb").read()!=before_ini or open(os.path.join(dgame,"Openvr","win64","openvr_api_orig.dll"),"rb").read()!=before_orig: ok=False
        try: main(["--native-openxr","--dll","--no-backup","--native-loader",loader,"--native-runtime",manifest,"--openvr"]); ok=False
        except SystemExit: pass
        os.remove(lib) if os.path.exists(lib) else None
        _pe.validate_native_pair=old_validate; globals()["validate_elite_game"]=old_profile; globals()["strict_game_running"]=old_probe
    finally:
        shutil.rmtree(direct_tmp, ignore_errors=True)

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
                          ("build/edvr_openxr_graphics.dll", b"NATIVE-GRAPHICS"),
                          ("build/edvr_openxr_runtime.dll", b"NATIVE-NEW"),
                          ("edvr.ini", b"[fix]\n")):
            with open(os.path.join(root, rel.replace("/", os.sep)), "wb") as f:
                f.write(body)
        game = os.path.join(tmp, "game")
        os.makedirs(game)
        with open(os.path.join(game, GAME_EXE), "wb") as f:
            f.write(b"exe")
        with open(os.path.join(game, "d3d11.dll"), "wb") as f:
            f.write(b"OLD-DLL")
        os.makedirs(os.path.join(game, "Openvr", "win64"))
        with open(os.path.join(game, "Openvr", "win64", "openvr_api.dll"), "wb") as f:
            f.write(b"OLD-OPENVR")
        with open(os.path.join(game, "Openvr", "win64", "openvr_api_orig.dll"), "wb") as f:
            f.write(b"STOCK-OPENVR")

        if sha256(os.path.join(root, "build", "d3d11.dll")) == \
           sha256(os.path.join(game, "d3d11.dll")):
            print("sha256 called two different files equal")
            ok = False

        # resolve_target on an explicit path finds the exe.
        if resolve_target(game) != os.path.abspath(game):
            print("resolve_target did not accept a real game directory")
            ok = False

        # Native staging is an explicit paired transaction. Its dry run does
        # not create the receipt, backup files, or any directories.
        receipt_path = os.path.join(tmp, "native-receipt.json")
        native_target = os.path.join(game, "Openvr", "win64", "openvr_api.dll")
        graphics_target = os.path.join(game, "d3d11.dll")
        old_native = open(native_target, "rb").read()
        old_graphics = open(graphics_target, "rb").read()
        before_native_tree = []
        for base, dirs, names in os.walk(game):
            for name in sorted(names):
                path = os.path.join(base, name)
                before_native_tree.append((os.path.relpath(path, game),
                                           open(path, "rb").read()))
        old_strict_game_running = globals()["strict_game_running"]
        import openxr_pe as _native_pe
        old_pair_validate = _native_pe.validate_native_pair
        old_profile_validate = globals()["validate_elite_game"]
        _native_pe.validate_native_pair = lambda game, native, graphics: None
        globals()["validate_elite_game"] = lambda game: None
        globals()["strict_game_running"] = lambda *a: (True, False)
        try:
            if main(["--root", root, "--target", game, "--native-openxr",
                     "--dll", "--dry-run"]) != 0:
                print("native dry run failed")
                ok = False
            after_native_tree = []
            for base, dirs, names in os.walk(game):
                for name in sorted(names):
                    path = os.path.join(base, name)
                    after_native_tree.append((os.path.relpath(path, game),
                                              open(path, "rb").read()))
            if before_native_tree != after_native_tree or os.path.exists(receipt_path):
                print("native dry run wrote files")
                ok = False

            # A static pair mismatch is rejected before the receipt, backups,
            # or either live DLL can be touched.
            before_mismatch = list(before_native_tree)
            saved_pair_validate = _native_pe.validate_native_pair
            def reject_pair(game, native, graphics):
                raise ValueError("mismatched native graphics")
            _native_pe.validate_native_pair = reject_pair
            try:
                try:
                    main(["--root", root, "--target", game, "--native-openxr",
                          "--dll", "--native-receipt", receipt_path])
                    print("native mismatch was accepted")
                    ok = False
                except SystemExit:
                    pass
            finally:
                _native_pe.validate_native_pair = saved_pair_validate
            after_mismatch = []
            for base, dirs, names in os.walk(game):
                for name in sorted(names):
                    path = os.path.join(base, name)
                    after_mismatch.append((os.path.relpath(path, game),
                                           open(path, "rb").read()))
            if before_mismatch != after_mismatch or os.path.exists(receipt_path):
                print("native mismatch changed files")
                ok = False

            # An unrecognized executable profile is equally a preflight
            # refusal, so migration cannot claim success while Elite would
            # still choose LibOVR.
            old_profile_gate = globals()["validate_elite_game"]
            def reject_profile(path):
                raise ValueError("unknown Elite executable revision")
            globals()["validate_elite_game"] = reject_profile
            try:
                try:
                    main(["--root", root, "--target", game, "--native-openxr",
                          "--dll", "--native-receipt", receipt_path])
                    print("unknown executable was accepted")
                    ok = False
                except SystemExit:
                    pass
            finally:
                globals()["validate_elite_game"] = old_profile_gate
            after_unknown = []
            for base, dirs, names in os.walk(game):
                for name in sorted(names):
                    path = os.path.join(base, name)
                    after_unknown.append((os.path.relpath(path, game),
                                          open(path, "rb").read()))
            if before_mismatch != after_unknown or os.path.exists(receipt_path):
                print("unknown executable changed files")
                ok = False

            # A process probe error is a hard refusal and leaves both files
            # untouched, even though ordinary installs retain their legacy
            # fail-open probe for compatibility.
            globals()["strict_game_running"] = lambda *a: (False, False)
            if main(["--root", root, "--target", game, "--native-openxr",
                     "--dll", "--native-receipt", receipt_path]) == 0:
                print("native install accepted an unknown process state")
                ok = False
            if open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics:
                print("process refusal changed native files")
                ok = False

            # A failed second copy rolls both destinations back and removes
            # the transaction's newly-created backup.
            globals()["strict_game_running"] = lambda *a: (True, False)
            real_copy2 = shutil.copy2
            def fail_native_source(src, dst, *copy_args, **copy_kwargs):
                if os.path.basename(src) == "edvr_openxr_runtime.dll":
                    raise OSError("self-test injected copy failure")
                return real_copy2(src, dst, *copy_args, **copy_kwargs)
            shutil.copy2 = fail_native_source
            try:
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--tag", "rollback"]) == 0:
                    print("injected native copy failure was accepted")
                    ok = False
            finally:
                shutil.copy2 = real_copy2
            if os.path.exists(receipt_path) or open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics:
                print("native rollback did not restore the paired files")
                ok = False

            # Exercise failure after the first live copy as well as before it.
            def fail_graphics_source(src, dst, *copy_args, **copy_kwargs):
                if os.path.abspath(src) == os.path.abspath(
                        os.path.join(root, "build", "edvr_openxr_graphics.dll")):
                    raise OSError("self-test second-copy failure")
                return real_copy2(src, dst, *copy_args, **copy_kwargs)
            shutil.copy2 = fail_graphics_source
            try:
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--tag", "second-copy"]) == 0:
                    print("second native copy failure was accepted")
                    ok = False
            finally:
                shutil.copy2 = real_copy2
            if os.path.exists(receipt_path) or open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics:
                print("second-copy rollback did not restore both files")
                ok = False

            # A successful copy call that leaves corrupted destination bytes
            # is rejected by the post-copy hash check and rolled back.
            def corrupt_native_destination(src, dst, *copy_args, **copy_kwargs):
                result = real_copy2(src, dst, *copy_args, **copy_kwargs)
                if os.path.abspath(src) == os.path.abspath(
                        os.path.join(root, "build", "edvr_openxr_runtime.dll")):
                    with open(dst, "wb") as f:
                        f.write(b"CORRUPTED")
                return result
            shutil.copy2 = corrupt_native_destination
            try:
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--tag", "corrupt-destination"]) == 0:
                    print("corrupted native destination was accepted")
                    ok = False
            finally:
                shutil.copy2 = real_copy2
            if os.path.exists(receipt_path) or open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics:
                print("corrupt destination rollback did not restore both files")
                ok = False

            # A corrupt backup is rejected before either live destination is
            # entered, and its reserved file is cleaned transactionally.
            def corrupt_native_backup(src, dst, *copy_args, **copy_kwargs):
                result = real_copy2(src, dst, *copy_args, **copy_kwargs)
                if ".pre-corrupt-backup-" in os.path.basename(dst):
                    with open(dst, "wb") as f:
                        f.write(b"CORRUPTED-BACKUP")
                return result
            shutil.copy2 = corrupt_native_backup
            try:
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--tag", "corrupt-backup"]) == 0:
                    print("corrupted native backup was accepted")
                    ok = False
            finally:
                shutil.copy2 = real_copy2
            if os.path.exists(receipt_path) or open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics:
                print("corrupt backup changed live files")
                ok = False

            # A receipt commit failure is also rolled back before any stale
            # receipt can claim that the pair is installed.
            real_replace_receipt = _replace_receipt
            def fail_receipt_commit(path, value):
                raise OSError("self-test receipt commit failure")
            globals()["_replace_receipt"] = fail_receipt_commit
            try:
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--tag", "receipt-failure"]) == 0:
                    print("receipt commit failure was accepted")
                    ok = False
            finally:
                globals()["_replace_receipt"] = real_replace_receipt
            if os.path.exists(receipt_path) or open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics:
                print("receipt failure did not roll back both files")
                ok = False

            # If rollback itself fails, retain the journal and every backup
            # as recovery evidence instead of deleting originals.
            stage_failed = [False]
            def fail_rollback(src, dst, *copy_args, **copy_kwargs):
                if os.path.abspath(src) == os.path.abspath(
                        os.path.join(root, "build", "edvr_openxr_graphics.dll")):
                    stage_failed[0] = True
                    raise OSError("self-test staged failure")
                if stage_failed[0] and os.path.abspath(dst) == os.path.abspath(native_target):
                    raise OSError("self-test rollback failure")
                return real_copy2(src, dst, *copy_args, **copy_kwargs)
            shutil.copy2 = fail_rollback
            try:
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--tag", "rollback-evidence"]) == 0:
                    print("rollback failure was accepted")
                    ok = False
            finally:
                shutil.copy2 = real_copy2
            if not os.path.exists(receipt_path):
                print("rollback failure discarded its receipt")
                ok = False
            else:
                retained = {}
                try:
                    with open(receipt_path, "r", encoding="utf-8") as f:
                        retained = json.load(f)
                    if retained.get("state") != "rollback_failed" or \
                       not all(os.path.isfile(entry["backup"])
                               for entry in retained["files"]):
                        print("rollback failure did not retain evidence")
                        ok = False
                except (OSError, ValueError, TypeError):
                    print("rollback failure left an unreadable receipt")
                    ok = False
                with open(native_target, "wb") as f:
                    f.write(old_native)
                with open(graphics_target, "wb") as f:
                    f.write(old_graphics)
                os.remove(receipt_path)
                for entry in retained.get("files", []):
                    try:
                        os.remove(entry["backup"])
                    except OSError:
                        pass

            if main(["--root", root, "--target", game, "--native-openxr",
                     "--dll", "--native-receipt", receipt_path,
                     "--tag", "native-selftest"]) != 0:
                print("native install failed")
                ok = False
            else:
                try:
                    verified = verify_native_receipt(receipt_path, game)
                    if verified["state"] != "installed" or \
                       len(verified["files"]) != 2:
                        print("native receipt validation returned wrong state")
                        ok = False
                except (OSError, ValueError) as exc:
                    print("native receipt did not validate: %s" % exc)
                    ok = False
                if main(["--root", root, "--target", game, "--native-openxr",
                         "--dll", "--native-receipt", receipt_path,
                         "--verify-only"]) != 0:
                    print("native paired verify-only failed")
                    ok = False

                # Receipt validation rejects traversal and a backup aliasing
                # the INI, without changing the valid installed transaction.
                with open(receipt_path, "r", encoding="utf-8") as f:
                    valid_receipt = json.load(f)
                for label, mutate in (
                        ("bad-type", lambda r: r.update({"files": "bad"})),
                        ("bad-backup", lambda r: r["files"][0].update(
                            {"backup": os.path.join(game, "edvr.ini")})),
                        ("bad-traversal", lambda r: r.update(
                            {"target": os.path.join(game, "..", "outside")}))):
                    bad_path = os.path.join(tmp, "native-%s.json" % label)
                    bad = json.loads(json.dumps(valid_receipt))
                    mutate(bad)
                    with open(bad_path, "w", encoding="utf-8") as f:
                        json.dump(bad, f)
                    try:
                        verify_native_receipt(bad_path, game)
                        print("malformed receipt accepted: %s" % label)
                        ok = False
                    except (OSError, ValueError, TypeError):
                        pass
            with open(os.path.join(game, "edvr.ini"), "wb") as f:
                f.write(b"[user-edit]\nkeep=1\n")
            with open(graphics_target, "wb") as f:
                f.write(b"EXTERNAL-CHANGE")
            if restore_native(receipt_path) == 0:
                print("stale native restore was accepted")
                ok = False
            with open(graphics_target, "wb") as f:
                f.write(b"NATIVE-GRAPHICS")
            # A corrupt staged restore copy must never be copied back onto
            # a live DLL: no live mutation has occurred at this point.
            real_copy2 = shutil.copy2
            def corrupt_restore_temp(src, dst, *copy_args, **copy_kwargs):
                result = real_copy2(src, dst, *copy_args, **copy_kwargs)
                if os.path.basename(dst).startswith("edvr-native-restore-"):
                    with open(dst, "wb") as f:
                        f.write(b"BAD-TEMP")
                return result
            shutil.copy2 = corrupt_restore_temp
            try:
                if restore_native(receipt_path) == 0:
                    print("corrupt restore staging accepted")
                    ok = False
            finally:
                shutil.copy2 = real_copy2
            if open(native_target, "rb").read() != b"NATIVE-NEW" or \
               open(graphics_target, "rb").read() != b"NATIVE-GRAPHICS":
                print("corrupt staging changed a live DLL")
                ok = False
            real_replace_receipt = _replace_receipt
            def fail_restore_receipt(path, value):
                raise OSError("self-test restore receipt failure")
            globals()["_replace_receipt"] = fail_restore_receipt
            try:
                if restore_native(receipt_path) == 0:
                    print("restore receipt failure was accepted")
                    ok = False
            finally:
                globals()["_replace_receipt"] = real_replace_receipt
            if open(native_target, "rb").read() != b"NATIVE-NEW" or \
               open(graphics_target, "rb").read() != b"NATIVE-GRAPHICS":
                print("restore receipt failure did not preserve installed pair")
                ok = False
            # Receipts from before the graphics split named build/d3d11.dll;
            # restore must continue to accept that source identity.
            with open(receipt_path, "r", encoding="utf-8") as stream:
                legacy_receipt = json.load(stream)
            for entry in legacy_receipt["files"]:
                if entry["key"] == "graphics":
                    entry["source"] = os.path.abspath(
                        os.path.join(root, LEGACY_GRAPHICS_SOURCE))
            with open(receipt_path, "w", encoding="utf-8") as stream:
                json.dump(legacy_receipt, stream)
            if restore_native(receipt_path) != 0:
                print("native restore failed")
                ok = False
            if open(native_target, "rb").read() != old_native or \
               open(graphics_target, "rb").read() != old_graphics or \
               open(os.path.join(game, "edvr.ini"), "rb").read() != \
               b"[user-edit]\nkeep=1\n":
                print("native restore changed the wrong files")
                ok = False
            try:
                verify_native_receipt(receipt_path, game)
                print("restored native receipt was accepted as installed")
                ok = False
            except (OSError, ValueError):
                pass
        finally:
            globals()["strict_game_running"] = old_strict_game_running
            _native_pe.validate_native_pair = old_pair_validate
            globals()["validate_elite_game"] = old_profile_validate
            if _native_pe.validate_native_pair is not old_pair_validate:
                print("native pair validator was not restored")
                ok = False
            if globals()["validate_elite_game"] is not old_profile_validate:
                print("Elite profile validator was not restored")
                ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
