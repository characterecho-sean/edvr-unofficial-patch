#!/usr/bin/env python3
"""Where Steam keeps Elite Dangerous: one resolver for install_edvr.py and edvr_log.py.

On Windows the Steam root is in the registry, with C:\\Steam as the fallback
for a rig whose entry has gone missing. On Linux (Steam Deck, Proton) it is
~/.local/share/Steam, ~/.steam/steam (usually a symlink to the first) or the
Flatpak copy. Either way steamapps/libraryfolders.vdf names every other
library, because the game is routinely on a different drive from Steam itself,
and the game directory is
<library>/steamapps/common/Elite Dangerous/Products/<product>.

    python tools/steam_library.py            # print what this host resolves
    python tools/steam_library.py --self-test
"""

import os
import sys
import tempfile
import shutil

WINDOWS_FALLBACK = r"C:\Steam"
ELITE_DIR = "Elite Dangerous"
# steam_dirs(fallback=...) default: use WINDOWS_FALLBACK as it is when called.
_MODULE_FALLBACK = object()
# Relative to $HOME. The two first are the ones a Steam Deck has; the Flatpak
# copy keeps its own tree.
LINUX_ROOTS = (".local/share/Steam", ".steam/steam", ".steam/root",
               ".var/app/com.valvesoftware.Steam/.local/share/Steam")


def _registry_steam_path():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Valve\Steam") as k:
            return winreg.QueryValueEx(k, "SteamPath")[0]
    except (ImportError, OSError):
        return None


def steam_roots(windows=None, home=None, registry=_registry_steam_path):
    """Steam installs themselves (not yet their extra libraries)."""
    if windows is None:
        windows = os.name == "nt"
    if windows:
        path = registry()
        return [path] if path else []
    home = home or os.path.expanduser("~")
    return [os.path.join(home, rel) for rel in LINUX_ROOTS
            if os.path.isdir(os.path.join(home, rel, "steamapps"))]


def library_paths(roots):
    """`roots` plus every library their libraryfolders.vdf names."""
    libs = list(roots)
    for r in roots:
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
    return libs


def products_under(root, game_exe):
    """Product leaves under an install root that actually hold the game."""
    found = []
    products = os.path.join(root, "Products")
    if not os.path.isdir(products):
        return found
    for name in sorted(os.listdir(products)):
        leaf = os.path.join(products, name)
        if os.path.isfile(os.path.join(leaf, game_exe)):
            found.append(os.path.normpath(leaf))
    return found


def steam_dirs(game_exe, windows=None, home=None, registry=_registry_steam_path,
               fallback=_MODULE_FALLBACK):
    """Every Steam copy of the game, once each.

    `fallback` is the Windows last-resort Steam root; it defaults to
    WINDOWS_FALLBACK, and None means there is none. A test passes its own, so
    it never consults the real C:\\Steam of the machine it runs on."""
    if windows is None:
        windows = os.name == "nt"
    if fallback is _MODULE_FALLBACK:
        fallback = WINDOWS_FALLBACK
    libs = library_paths(steam_roots(windows, home, registry))
    if windows and fallback:
        libs.append(fallback)
    # Deduplicated case- and separator-insensitively. The registry hands
    # back `c:/steam` and the fallback is `C:\Steam`; compared as strings
    # those are two installs, and the tool then refuses to act because it
    # has "found two" -- the same directory, twice. On Linux the same trap
    # is ~/.steam/steam, a symlink to ~/.local/share/Steam, so compare the
    # resolved path there.
    out, seen = [], set()
    for lib in libs:
        base = os.path.join(lib, "steamapps", "common", ELITE_DIR)
        for leaf in products_under(base, game_exe):
            key = os.path.normcase(os.path.normpath(leaf)) if windows \
                else os.path.realpath(leaf)
            if key not in seen:
                seen.add(key)
                out.append(os.path.normpath(leaf))
    return out


def self_test():
    ok = True

    def check(cond, why):
        nonlocal ok
        if not cond:
            print("steam_library self-test: " + why)
            ok = False

    exe = "EliteDangerous64.exe"
    tmp = tempfile.mkdtemp(prefix="edvr_steam_test_")
    try:
        def game(library, product="elite-dangerous-odyssey-64"):
            leaf = os.path.join(library, "steamapps", "common", ELITE_DIR,
                                "Products", product)
            os.makedirs(leaf, exist_ok=True)
            with open(os.path.join(leaf, exe), "wb") as f:
                f.write(b"GAME")
            return os.path.normpath(leaf)

        def vdf(root, *paths):
            os.makedirs(os.path.join(root, "steamapps"), exist_ok=True)
            body = "".join('\t"%d"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n' % (i, p.replace("\\", "\\\\"))
                           for i, p in enumerate(paths))
            with open(os.path.join(root, "steamapps", "libraryfolders.vdf"), "w") as f:
                f.write('"libraryfolders"\n{\n' + body + '}\n')

        # --- Linux: the Deck layout, ~/.steam/steam a symlink to the real root ---
        home = os.path.join(tmp, "deck")
        real = os.path.join(home, ".local", "share", "Steam")
        want = game(real)
        os.makedirs(os.path.join(home, ".steam"))
        try:
            os.symlink(real, os.path.join(home, ".steam", "steam"))
            linked = True
        except (OSError, NotImplementedError):
            linked = False        # no symlink rights: the dedup case just isn't exercised
        vdf(real, real)
        check(steam_dirs(exe, False, home) == [want],
              "the Deck layout (and its ~/.steam/steam symlink) should give the one game directory %r, got %r"
              % (want, steam_dirs(exe, False, home)))
        if linked:
            check(len(steam_roots(False, home)) == 2, "both Linux roots should be listed before dedup")

        # --- an extra library on another drive, named by libraryfolders.vdf ---
        extra = os.path.join(tmp, "sdcard")
        other = game(extra)
        vdf(real, real, extra)
        check(steam_dirs(exe, False, home) == [want, other],
              "an extra library should add its copy: %r" % steam_dirs(exe, False, home))
        vdf(real, real)
        check(steam_dirs(exe, False, home) == [want], "a library the vdf does not name must not be searched")

        # --- a library with no game, a Products leaf without the exe, no Steam at all ---
        empty = os.path.join(tmp, "empty")
        os.makedirs(os.path.join(empty, "steamapps", "common", ELITE_DIR, "Products", "stub"))
        vdf(real, real, empty)
        check(steam_dirs(exe, False, home) == [want], "a Products leaf without the game exe is not an install")
        check(steam_dirs(exe, False, os.path.join(tmp, "nobody")) == [], "no Steam must give no installs, not an error")
        check(steam_roots(False, os.path.join(tmp, "nobody")) == [], "no Steam roots on a bare home")

        # --- Windows: registry root, vdf libraries, the fallback, case-insensitive dedup ---
        # (the paths are POSIX temp dirs here; only the rules are under test.)
        # Every Windows call below names its own fallback. The real one is
        # C:\Steam, which holds a game on a PC that keeps Elite there, and this
        # test must give the same answer on that PC and on a bare CI runner.
        wroot = os.path.join(tmp, "wsteam")
        wgame = game(wroot)
        wextra = os.path.join(tmp, "wd")
        wother = game(wextra)
        vdf(wroot, wroot, wextra)
        got = steam_dirs(exe, True, None, lambda: wroot, None)
        check(got == [wgame, wother], "Windows: registry root then its vdf libraries: %r" % got)
        got = steam_dirs(exe, True, None, lambda: None, None)
        check(got == [], "Windows with no registry entry and no fallback has no installs: %r" % got)
        check(steam_roots(True, registry=lambda: None) == [], "Windows with no registry entry has no roots")

        # An empty fallback (a Steam tree with no game) adds nothing, with or without a registry entry.
        wempty = os.path.join(tmp, "wempty")
        os.makedirs(os.path.join(wempty, "steamapps", "common", ELITE_DIR, "Products", "stub"))
        got = steam_dirs(exe, True, None, lambda: None, wempty)
        check(got == [], "Windows: an empty fallback must give no installs: %r" % got)
        got = steam_dirs(exe, True, None, lambda: wroot, wempty)
        check(got == [wgame, wother], "Windows: an empty fallback must add nothing to the registry's: %r" % got)

        # A populated fallback is the install when the registry entry is missing, and follows the
        # registry's installs when it is a different library.
        wfall = os.path.join(tmp, "wfallback")
        wfgame = game(wfall)
        got = steam_dirs(exe, True, None, lambda: None, wfall)
        check(got == [wfgame], "Windows: a populated fallback is the install when the registry has none: %r" % got)
        got = steam_dirs(exe, True, None, lambda: wroot, wfall)
        check(got == [wgame, wother, wfgame], "Windows: registry installs, then the fallback's: %r" % got)
        # The fallback is a Windows rule: a Linux host never searches it.
        got = steam_dirs(exe, False, os.path.join(tmp, "nobody"), fallback=wfall)
        check(got == [], "a Linux host must not search the Windows fallback: %r" % got)

        # The same library reached through the registry and through the fallback is one install,
        # however the two spell it.
        for label, spelled in (("identically", wroot),
                               ("with a trailing separator", wroot + os.sep),
                               ("with forward slashes", wroot.replace(os.sep, "/")),
                               ("in upper case", wroot.upper() if os.path.normcase("A") != "A" else wroot)):
            got = steam_dirs(exe, True, None, lambda: wroot, spelled)
            check(got == [wgame, wother],
                  "Windows: one library via the registry and the fallback, spelled %s, must be one install: %r"
                  % (label, got))
        # The registry's own vdf can name the fallback's library too.
        vdf(wroot, wroot, wfall)
        got = steam_dirs(exe, True, None, lambda: wroot, wfall)
        check(got == [wgame, wfgame], "Windows: a library named by the vdf and by the fallback is one install: %r" % got)
        # The same library named twice by the vdf is one install.
        vdf(wroot, wroot, wroot)
        check(steam_dirs(exe, True, None, lambda: wroot, None) == [wgame],
              "the same library named twice must give one install")

        # Production reads WINDOWS_FALLBACK when no fallback is passed. Point the module at the
        # fixture, so the default path is covered without the real C:\Steam being read.
        real_fallback = WINDOWS_FALLBACK
        globals()["WINDOWS_FALLBACK"] = wfall
        try:
            got = steam_dirs(exe, True, None, lambda: None)
            check(got == [wfgame], "the default fallback must be WINDOWS_FALLBACK: %r" % got)
            got = steam_dirs(exe, True, None, lambda: None, None)
            check(got == [], "fallback=None must override the module default: %r" % got)
        finally:
            globals()["WINDOWS_FALLBACK"] = real_fallback
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("steam_library self-test: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        raise SystemExit(self_test())
    for p in steam_dirs("EliteDangerous64.exe"):
        print(p)
