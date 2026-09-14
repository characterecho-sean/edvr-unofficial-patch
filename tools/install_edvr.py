#!/usr/bin/env python3
"""Put a freshly built EDVR next to the game, and prove it landed.

    python tools/install_edvr.py --target steam --dry-run
    python tools/install_edvr.py --target steam
    python tools/install_edvr.py --target frontier --openvr --tag stepped
    python tools/install_edvr.py --target steam --verify-only
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
    over-refuses on purpose, and --force is the escape. A --dry-run is
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

NATIVE_RECEIPT_VERSION = 1
NATIVE_KIND = "edvr-native-openxr"


def native_paths(root, target):
    """The deliberately separate native package and its paired graphics DLL."""
    return {
        "native_source": os.path.join(root, "build", "edvr_openxr_runtime.dll"),
        "graphics_source": os.path.join(root, "build", "d3d11.dll"),
        "native_target": os.path.join(target, "Openvr", "win64", "openvr_api.dll"),
        "graphics_target": os.path.join(target, "d3d11.dll"),
        "original": os.path.join(target, "Openvr", "win64", "openvr_api_orig.dll"),
        "ini": os.path.join(target, "edvr.ini"),
    }


def strict_game_running():
    """Return (known, running); native staging fails closed on probe errors."""
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
    running = False
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
            running = True
    return True, running


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
    return p


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
    known, running = strict_game_running()
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
        if entry["source"] != os.path.abspath(expected[key + "_source"]) or \
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
        receipt = _load_native_receipt(receipt_path)
        target = receipt["target"]
        entries = {entry["key"]: entry for entry in receipt["files"]}
        if dry_run:
            print("[edvr] native restore plan (DRY RUN): %s" % target)
            print("[edvr] dry run: wrote nothing.")
            return 0
        known, running = strict_game_running()
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
    from openxr_pe import validate_frontier_imports
    validate_frontier_imports(game, paths["native_source"])
    config = "[openxr]\nversion=1\nloader=%s\ngraphics=%s\nruntime=%s\nseparate_device=1\n" % (loader, paths["graphics_target"], runtime)
    return paths, config.encode("utf-8"), lib

def native_direct(root, target, loader, runtime, dry_run=False):
    paths, config, lib = _native_direct_plan(root, target, loader, runtime)
    known, running = strict_game_running()
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
    ap.add_argument("--native-openxr", action="store_true",
                    help="stage the experimental native OpenXR DLL and its "
                         "paired d3d11.dll (requires --dll, plus a receipt "
                         "or explicit --no-backup local configuration)")
    ap.add_argument("--native-receipt", default=None,
                    help="absolute new receipt path for --native-openxr")
    ap.add_argument("--native-loader", default=None)
    ap.add_argument("--native-runtime", default=None)
    ap.add_argument("--restore-native", default=None, metavar="RECEIPT",
                    help="restore both files recorded by a native receipt")
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
                args.no_backup, args.tag)):
            ap.error("--restore-native cannot be combined with install options")
        if not os.path.isabs(args.restore_native):
            ap.error("--restore-native requires an absolute receipt path")
        return restore_native(args.restore_native, args.dry_run)

    if args.native_receipt and not args.native_openxr:
        ap.error("--native-receipt requires --native-openxr")
    if args.native_receipt and not os.path.isabs(args.native_receipt):
        ap.error("--native-receipt must be an absolute new path")
    if args.native_openxr:
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

    # The guard exists because copying onto a loaded DLL is what goes wrong
    # with the game open. A dry run copies nothing, so it is not refused:
    # `--target steam --dry-run` with Elite open prints the plan, as
    # CLAUDE.md says it does (it was refused before 2026-09-11, and the
    # self-test papered over that with --force).
    if not args.force and not args.dry_run and game_running():
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

    # Direct native route: use a complete temporary pair and replace the
    # contract validator/process probe so this remains a CPU-only test.
    direct_tmp = tempfile.mkdtemp(prefix="edvr_direct_test_")
    try:
        droot = os.path.join(direct_tmp, "repo"); dgame = os.path.join(direct_tmp, "game")
        os.makedirs(os.path.join(droot, "build")); os.makedirs(os.path.join(dgame, "Openvr", "win64"))
        for rel, data in (("build/edvr_openxr_runtime.dll", b"NATIVE"), ("build/d3d11.dll", b"GRAPHICS")):
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
        old_validate=_pe.validate_frontier_imports; old_probe=globals()["strict_game_running"]
        calls=[0]
        def fake_validate(game,native): calls[0]+=1
        _pe.validate_frontier_imports=fake_validate
        globals()["strict_game_running"]=lambda:(True,False)
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
        if main(["--root",droot,"--target",dgame,"--native-openxr","--dll","--no-backup","--native-loader",loader,"--dry-run"])!=0: ok=False
        if direct_snapshot()!=before: ok=False
        globals()["strict_game_running"]=lambda:(False,False)
        if native_direct(droot,dgame,loader,os.path.join(direct_tmp,"runtime.json"),True)!=0: ok=False
        after=direct_snapshot()
        if before!=after: ok=False
        for state in ((True,True), (False,False)):
            globals()["strict_game_running"]=lambda:state
            if native_direct(droot,dgame,loader,manifest,False)==0: ok=False
            if direct_snapshot()!=before: ok=False
        if open(os.path.join(dgame,"edvr.ini"),"rb").read()!=before_ini or open(os.path.join(dgame,"Openvr","win64","openvr_api_orig.dll"),"rb").read()!=before_orig: ok=False
        try: main(["--native-openxr","--dll","--no-backup","--native-loader",loader,"--native-runtime",manifest,"--openvr"]); ok=False
        except SystemExit: pass
        os.remove(lib) if os.path.exists(lib) else None
        _pe.validate_frontier_imports=old_validate; globals()["strict_game_running"]=old_probe
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

        # A missing source is refused BEFORE anything is copied.
        try:
            build_plan(root, game, ["openvr"])
            print("build_plan accepted a missing source")
            ok = False
        except SystemExit:
            pass

        # The property that matters most: --dry-run writes nothing. Not
        # the copy, not the backup, not a directory. This project has
        # shipped a --dry-run that wrote files. No --force here, on
        # purpose: a dry run is not refused while the game runs (it copies
        # nothing), and this call is what keeps that so -- the build gate
        # went red on 2026-09-11 with Elite open when the running-game
        # guard sat ahead of the dry-run branch. The listing and the bytes
        # are what prove the property, not the exit code.
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
        globals()["strict_game_running"] = lambda: (True, False)
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

            # A process probe error is a hard refusal and leaves both files
            # untouched, even though ordinary installs retain their legacy
            # fail-open probe for compatibility.
            globals()["strict_game_running"] = lambda: (False, False)
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
            globals()["strict_game_running"] = lambda: (True, False)
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
                        os.path.join(root, "build", "d3d11.dll")):
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
                        os.path.join(root, "build", "d3d11.dll")):
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
                f.write(b"NEW-DLL")
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
               open(graphics_target, "rb").read() != b"NEW-DLL":
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
               open(graphics_target, "rb").read() != b"NEW-DLL":
                print("restore receipt failure did not preserve installed pair")
                ok = False
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
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
