#!/usr/bin/env python3
"""Run EDVR's Windows installer with Elite's recorded Steam Proton runtime."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


APP_ID = "359320"
PRODUCT = "Products/elite-dangerous-odyssey-64"
MAX_VDF = 4 * 1024 * 1024


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--installer", type=Path, help="edvr-installer.exe (beside script or build/ by default)")
    p.add_argument("--steam-dir", type=Path, help="Steam root; otherwise use STEAM_DIR or known locations")
    p.add_argument("--proton", type=Path, help="Proton directory or proton script; overrides the prefix record")
    p.add_argument("--game-dir", type=Path, help="folder holding EliteDangerous64.exe inside the installed game")
    actions = p.add_mutually_exclusive_group()
    for name in ("install", "repair", "uninstall", "collect-logs"):
        actions.add_argument("--" + name, action="store_true")
    actions.add_argument("--preview", action="store_true",
                         help="start Proton/Wine to show the Windows install plan without changing game files")
    p.add_argument("--scope", choices=("both", "vr", "flat"))
    p.add_argument("--replace-settings", action="store_true")
    p.add_argument("--remove-settings", action="store_true")
    offline = p.add_mutually_exclusive_group()
    offline.add_argument("--dry-run", action="store_true",
                         help="resolve and print the exact command; start no processes and write nothing")
    offline.add_argument("--self-test", action="store_true", help=argparse.SUPPRESS)
    return p


def installer_args(args):
    action = next((name for name in ("install", "repair", "uninstall", "collect_logs", "preview")
                   if getattr(args, name)), None)
    if args.scope and action not in ("install", "repair", "preview"):
        raise ValueError("--scope requires --install, --repair or --preview")
    if args.replace_settings and action not in ("install", "repair", "preview"):
        raise ValueError("--replace-settings requires --install, --repair or --preview")
    if args.remove_settings and action != "uninstall":
        raise ValueError("--remove-settings requires --uninstall")
    result = (["--install", "--dry-run"] if action == "preview" else
              (["--" + action.replace("_", "-")] if action else []))
    if args.scope:
        result += ["--scope", args.scope]
    if args.replace_settings:
        result.append("--replace-settings")
    if args.remove_settings:
        result.append("--remove-settings")
    return result


def installer_path(args, script):
    if args.installer is not None:
        return args.installer.expanduser().resolve()
    adjacent = script.resolve().parent / "edvr-installer.exe"
    return adjacent if adjacent.is_file() else script.resolve().parents[1] / "build" / "edvr-installer.exe"


def vdf(path):
    """Read the small KeyValues subset in Steam's local manifests, without regex scoping."""
    if not path.is_file() or path.stat().st_size > MAX_VDF:
        raise ValueError("missing or oversized Steam manifest: %s" % path)
    data = path.read_text(encoding="utf-8-sig", errors="replace")
    tokens = []
    i = 0
    while i < len(data):
        if data[i].isspace():
            i += 1
        elif data.startswith("//", i):
            end = data.find("\n", i)
            i = len(data) if end < 0 else end + 1
        elif data[i] in "{}":
            tokens.append(data[i]); i += 1
        elif data[i] == '"':
            i += 1; value = []
            while i < len(data) and data[i] != '"':
                if data[i] == "\\" and i + 1 < len(data) and data[i + 1] in ('"', "\\"):
                    i += 1
                value.append(data[i]); i += 1
            if i == len(data):
                raise ValueError("unterminated quote in %s" % path)
            tokens.append("".join(value)); i += 1
        else:
            raise ValueError("unexpected VDF character in %s" % path)
    at = 0

    def section(nested=False):
        nonlocal at
        result = {}
        while at < len(tokens):
            if tokens[at] == "}":
                if not nested:
                    raise ValueError("unexpected VDF close in %s" % path)
                at += 1
                return result
            key = tokens[at]; at += 1
            if key == "{" or at == len(tokens):
                raise ValueError("missing VDF value in %s" % path)
            value = tokens[at]; at += 1
            if value == "{":
                value = section(True)
            elif value == "}":
                raise ValueError("unexpected VDF close in %s" % path)
            if key in result:
                raise ValueError("duplicate VDF key %s in %s" % (key, path))
            result[key] = value
        if nested:
            raise ValueError("unclosed VDF section in %s" % path)
        return result

    parsed = section()
    if at != len(tokens):
        raise ValueError("trailing VDF content in %s" % path)
    return parsed


def section(mapping, key, path):
    value = mapping.get(key)
    if not isinstance(value, dict):
        raise ValueError("%s has no %s section" % (path, key))
    return value


def safe_leaf(value, label):
    if not isinstance(value, str) or value in ("", ".", "..") or re.search(r"[/\\\x00]", value):
        raise ValueError("unsafe %s in Steam manifest: %r" % (label, value))
    return value


def roots(args, home, environ):
    explicit = args.steam_dir or (Path(environ["STEAM_DIR"]) if environ.get("STEAM_DIR") else None)
    if explicit is not None:
        candidates = [explicit]
    else:
        candidates = [home / ".steam/steam", home / ".steam/root", home / ".local/share/Steam"]
        if environ.get("XDG_DATA_HOME"):
            candidates.append(Path(environ["XDG_DATA_HOME"]) / "Steam")
        candidates.append(home / ".var/app/com.valvesoftware.Steam/.local/share/Steam")
    found = []
    for candidate in candidates:
        path = candidate.expanduser().resolve()
        if path not in found and (path / "steamapps/libraryfolders.vdf").is_file():
            found.append(path)
    if not found:
        raise ValueError("Steam root not found; pass --steam-dir PATH (launch Steam once first)")
    return found


def libraries(steam_root):
    folders = section(vdf(steam_root / "steamapps/libraryfolders.vdf"), "libraryfolders", "libraryfolders.vdf")
    result = []
    for key, entry in folders.items():
        if not key.isdigit() or not isinstance(entry, dict):
            continue
        raw = entry.get("path")
        if not isinstance(raw, str) or not raw:
            continue
        library = Path(raw).expanduser().resolve()
        if library not in result and (library / "steamapps").is_dir():
            result.append(library)
    if steam_root not in result and (steam_root / "steamapps").is_dir():
        result.insert(0, steam_root)
    return result


def inside(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def find_game(args, steam_roots):
    requested = args.game_dir.expanduser().resolve() if args.game_dir else None
    matches = []
    rejected = []
    for steam_root in steam_roots:
        for library in libraries(steam_root):
            manifest = library / "steamapps" / ("appmanifest_%s.acf" % APP_ID)
            if not manifest.is_file():
                continue
            state = section(vdf(manifest), "AppState", manifest)
            if state.get("appid") != APP_ID:
                raise ValueError("wrong appid in %s" % manifest)
            install_root = (library / "steamapps/common" / safe_leaf(state.get("installdir"), "installdir")).resolve()
            if not inside(install_root, (library / "steamapps/common").resolve()):
                raise ValueError("game install escapes Steam library: %s" % install_root)
            game = requested or (install_root / PRODUCT).resolve()
            if requested and not inside(requested, install_root):
                continue
            if not inside(game, install_root):
                rejected.append("game folder escapes Steam install: %s" % game)
                continue
            if not (game / "EliteDangerous64.exe").is_file():
                rejected.append("EliteDangerous64.exe missing in %s" % game)
                continue
            match = (steam_root, library, install_root, game)
            if match not in matches:
                matches.append(match)
    if not matches:
        detail = ("; " + "; ".join(rejected[:3])) if rejected else ""
        raise ValueError("Elite Steam app %s has no valid install%s; check --steam-dir and --game-dir" %
                         (APP_ID, detail))
    if len(matches) > 1:
        raise ValueError("multiple Elite installs found; pass --steam-dir and --game-dir to select one")
    return matches[0]


def compatdata_path(library, environ):
    raw = environ.get("STEAM_COMPAT_DATA_PATH")
    path = Path(raw).expanduser().resolve() if raw else library / "steamapps/compatdata" / APP_ID
    if not (path / "pfx/drive_c").is_dir():
        raise ValueError("Elite Proton prefix missing at %s; launch Elite once through Steam" % path)
    return path


def recorded_proton(compatdata):
    record = compatdata / "config_info"
    if not record.is_file() or record.stat().st_size > 1024 * 1024:
        raise ValueError("Proton record missing in %s; launch Elite once or pass --proton PATH" % compatdata)
    lines = record.read_text(encoding="utf-8", errors="replace").splitlines()
    if len(lines) < 8:
        raise ValueError("incomplete Proton record in %s; launch Elite once or pass --proton PATH" % record)

    def base_of(value, suffixes):
        cleaned = value.rstrip("/")
        for suffix in suffixes:
            if cleaned.startswith("/") and cleaned.endswith(suffix):
                return Path(cleaned[:-len(suffix)]).resolve()
        return None

    defaults = tuple("/%s/share/%s" % (tree, name)
                     for tree in ("files", "dist")
                     for name in ("default_pfx", "default_pfx_arm64"))
    base = base_of(lines[7], defaults)
    if base is None or not (base / "proton").is_file():
        raise ValueError("recorded Proton is missing or stale in %s; launch Elite once or pass --proton PATH" % record)
    fonts = base_of(lines[1], ("/files/share/fonts", "/dist/share/fonts"))
    library = base_of(lines[2], ("/files/lib", "/dist/lib"))
    if fonts != base or library != base:
        raise ValueError("inconsistent Proton paths in %s; launch Elite once or pass --proton PATH" % record)
    return base


def proton_path(args, compatdata):
    if args.proton is None:
        return recorded_proton(compatdata) / "proton"
    chosen = args.proton.expanduser().resolve()
    script = chosen / "proton" if chosen.is_dir() else chosen
    if script.name != "proton" or not script.is_file():
        raise ValueError("--proton needs a Proton directory or its proton script: %s" % chosen)
    return script


def runtime_path(proton, steam_roots):
    manifest = proton.parent / "toolmanifest.vdf"
    if not manifest.is_file():
        raise ValueError("Proton toolmanifest.vdf missing beside %s; reinstall it through Steam or pass --proton PATH" % proton)
    tool = section(vdf(manifest), "manifest", manifest)
    appid = tool.get("require_tool_appid")
    if appid is None:
        return None
    if not isinstance(appid, str) or not appid.isdigit():
        raise ValueError("invalid require_tool_appid in %s" % manifest)
    found = []
    for steam_root in steam_roots:
        for library in libraries(steam_root):
            app = library / "steamapps" / ("appmanifest_%s.acf" % appid)
            if not app.is_file():
                continue
            state = section(vdf(app), "AppState", app)
            if state.get("appid") != appid:
                raise ValueError("wrong appid in %s" % app)
            common = (library / "steamapps/common").resolve()
            runtime_root = (common / safe_leaf(state.get("installdir"), "runtime installdir")).resolve()
            if not inside(runtime_root, common):
                raise ValueError("runtime install escapes Steam library: %s" % runtime_root)
            candidate = (runtime_root / "_v2-entry-point").resolve()
            if not inside(candidate, runtime_root):
                raise ValueError("runtime entry escapes Steam install: %s" % candidate)
            if candidate.is_file() and candidate not in found:
                found.append(candidate)
    if len(found) != 1:
        raise ValueError("Proton requires Steam runtime app %s, but found %d; install it through Steam" %
                         (appid, len(found)))
    return found[0]


def windows_path(host_path, compatdata):
    dos = compatdata / "pfx/dosdevices"
    found = []
    if not dos.is_dir():
        raise ValueError("Proton prefix has no dosdevices: %s" % dos)
    for link in dos.iterdir():
        if not re.fullmatch(r"[a-zA-Z]:", link.name) or not link.is_symlink():
            continue
        target = link.resolve()
        if target.is_dir() and inside(host_path, target):
            found.append((len(target.parts), link.name[0].upper(), host_path.relative_to(target)))
    if not found:
        raise ValueError("no existing Wine drive maps %s in %s" % (host_path, dos))
    _, drive, relative = max(found, key=lambda item: (item[0], item[1]))
    tail = "\\".join(relative.parts)
    return drive + ":\\" + tail


def build_plan(args, *, script=Path(__file__), home=Path.home(), environ=None):
    env_in = os.environ if environ is None else environ
    forwarded = installer_args(args)
    installer = installer_path(args, script)
    if not installer.is_file():
        raise ValueError("installer not found: %s (pass --installer PATH)" % installer)
    steam_roots = roots(args, home, env_in)
    steam_root, library, install_root, game = find_game(args, steam_roots)
    compatdata = compatdata_path(library, env_in)
    proton = proton_path(args, compatdata)
    runtime = runtime_path(proton, steam_roots)
    game_windows = windows_path(game, compatdata)
    command = ([str(runtime), "--verb=run", "--"] if runtime else [])
    command += [str(proton), "runinprefix", str(installer), "--dir", game_windows] + forwarded
    env = dict(env_in)
    for key in list(env):
        if key.startswith("WINE") or key == "PROTON_VERSION":
            del env[key]
    env.update(SteamAppId=APP_ID, SteamGameId=APP_ID, STEAM_COMPAT_APP_ID=APP_ID,
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam_root), STEAM_COMPAT_DATA_PATH=str(compatdata),
               STEAM_COMPAT_INSTALL_PATH=str(install_root),
               STEAM_COMPAT_LIBRARY_PATHS=":".join(str(lib / "steamapps") for lib in libraries(steam_root)))
    return command, env, (game, compatdata, proton, runtime)


def run(args, *, script=Path(__file__), home=Path.home(), environ=None,
        launch=subprocess.run, platform=sys.platform, geteuid=getattr(os, "geteuid", lambda: 1)):
    if platform != "linux" and not args.dry_run:
        raise ValueError("run this wrapper on Linux; use edvr-installer.exe on Windows")
    command, env, (game, compatdata, proton, runtime) = build_plan(
        args, script=script, home=home, environ=environ)
    if args.dry_run:
        print("EDVR resolved launch plan (nothing executed or written):")
        print("  game: %s" % game)
        print("  prefix: %s" % compatdata)
        print("  Proton: %s" % proton)
        print("  runtime: %s" % (runtime or "none required by toolmanifest"))
        for key in ("SteamAppId", "SteamGameId", "STEAM_COMPAT_APP_ID",
                    "STEAM_COMPAT_CLIENT_INSTALL_PATH", "STEAM_COMPAT_DATA_PATH",
                    "STEAM_COMPAT_INSTALL_PATH", "STEAM_COMPAT_LIBRARY_PATHS"):
            print("  %s=%s" % (key, shlex.quote(env[key])))
        print("  command: %s" % shlex.join(command))
        return 0
    if geteuid() == 0:
        raise ValueError("run as your Steam user without sudo")
    try:
        return launch(command, env=env, check=False).returncode
    except OSError as exc:
        raise ValueError("could not start Steam's Proton/runtime: %s" % exc) from exc


def self_test():
    import io
    from contextlib import redirect_stderr, redirect_stdout
    from tempfile import TemporaryDirectory

    parse = lambda *words: parser().parse_args(words)
    assert installer_args(parse("--preview", "--scope", "flat", "--replace-settings")) == [
        "--install", "--dry-run", "--scope", "flat", "--replace-settings"]
    assert installer_args(parse()) == []
    assert installer_args(parse("--uninstall", "--remove-settings")) == ["--uninstall", "--remove-settings"]
    for invalid in (("--dry-run", "--self-test"), ("--install", "--preview")):
        with redirect_stderr(io.StringIO()):
            try:
                parse(*invalid)
                raise AssertionError("accepted mutually exclusive arguments")
            except SystemExit as exc:
                assert exc.code == 2
    for invalid in (("--scope", "vr"), ("--remove-settings",), ("--preview", "--remove-settings")):
        try:
            installer_args(parse(*invalid))
            raise AssertionError("accepted invalid action flags")
        except ValueError:
            pass

    with TemporaryDirectory() as tmp:
        # Windows TEMP may use an 8.3 alias; match build_plan()'s resolved paths.
        root = Path(tmp).resolve()
        home = root / "home"
        steam = home / ".local/share/Steam"
        other = home / "other Steam"
        script = root / "tools/install_proton.py"
        script.parent.mkdir()
        exe = script.parent / "edvr-installer.exe"
        exe.write_bytes(b"exe")
        proton = steam / "steamapps/common/Proton's $() Experimental"
        if os.name == "nt":
            proton = Path("//?/" + proton.resolve().as_posix())
        (proton / "files/share/default_pfx").mkdir(parents=True)
        (proton / "proton").write_bytes(b"script")
        (proton / "toolmanifest.vdf").write_text(
            '"manifest" { "require_tool_appid" "4183110" }', encoding="utf-8")
        runtime = steam / "steamapps/common/SteamLinuxRuntime_4"
        runtime.mkdir(parents=True)
        (runtime / "_v2-entry-point").write_bytes(b"script")
        (steam / "steamapps/appmanifest_4183110.acf").write_text(
            '"AppState" { "appid" "4183110" "installdir" "SteamLinuxRuntime_4" }', encoding="utf-8")
        game_root = steam / "steamapps/common/Elite's $() Dangerous"
        game = game_root / PRODUCT
        game.mkdir(parents=True)
        (game / "EliteDangerous64.exe").write_bytes(b"game")
        (steam / "steamapps/appmanifest_359320.acf").write_text(
            '"AppState" { "appid" "359320" "installdir" "Elite\'s $() Dangerous" }', encoding="utf-8")
        (steam / "steamapps/libraryfolders.vdf").write_text(
            '"libraryfolders" { "0" { "path" "' + str(steam) + '" "apps" { "359320" "1" } } }',
            encoding="utf-8")
        compat = steam / "steamapps/compatdata/359320"
        (compat / "pfx/drive_c").mkdir(parents=True)
        record_lines = ["11.0-100", str(proton / "files/share/fonts"),
                        str(proton / "files/lib"), str(steam), "0", "0", "0",
                        str(proton / "files/share/default_pfx")]
        (compat / "config_info").write_text(
            "\n".join(path.replace("\\", "/") for path in record_lines) + "\n", encoding="utf-8")
        dos = compat / "pfx/dosdevices"
        dos.mkdir()
        if os.name != "nt":
            (dos / "z:").symlink_to("/", target_is_directory=True)
            (dos / "s:").symlink_to(steam, target_is_directory=True)
        else:
            # Build gate on Windows has no POSIX symlink privilege. Mock the
            # mapping at the function seam; the parser/runner tests still run.
            original_windows_path = globals()["windows_path"]
            globals()["windows_path"] = lambda path, _compat: "S:\\" + "\\".join(path.relative_to(steam).parts)
        env = {"WINEPREFIX": "/wrong", "WINESERVER": "/wrong/server", "PROTON_VERSION": "old",
               "STEAM_DIR": str(steam), "PATH": "/mock"}
        args = parse("--dry-run", "--preview", "--scope", "flat")
        before = set(root.rglob("*"))
        buf = io.StringIO()
        calls = []

        def launch(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 23)

        try:
            with redirect_stdout(buf):
                assert run(args, script=script, home=home, environ=env, launch=launch,
                           platform="linux", geteuid=lambda: 1000) == 0
            assert not calls and set(root.rglob("*")) == before
            command, process_env, details = build_plan(args, script=script, home=home, environ=env)
            assert command[:3] == [str(runtime / "_v2-entry-point"), "--verb=run", "--"]
            assert command[3:7] == [str(proton / "proton"), "runinprefix", str(exe), "--dir"]
            assert "waitforexitandrun" not in command
            assert command[-4:] == ["--install", "--dry-run", "--scope", "flat"]
            assert str(game) in buf.getvalue() and shlex.join(command) in buf.getvalue()
            assert "WINEPREFIX" not in process_env and "WINESERVER" not in process_env
            assert "PROTON_VERSION" not in process_env and process_env["SteamAppId"] == APP_ID
            assert process_env["STEAM_COMPAT_DATA_PATH"] == str(compat)
            inconsistent = list(record_lines)
            inconsistent[1] = str(other / "files/share/fonts")
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in inconsistent) + "\n", encoding="utf-8")
            try:
                recorded_proton(compat)
                raise AssertionError("inconsistent Proton record accepted")
            except ValueError as exc:
                assert "inconsistent" in str(exc)
            arm64_record = list(record_lines)
            arm64_record[7] = str(proton / "files/share/default_pfx_arm64")
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in arm64_record) + "\n", encoding="utf-8")
            assert recorded_proton(compat) == proton
            (compat / "config_info").write_text(
                "\n".join(path.replace("\\", "/") for path in record_lines) + "\n", encoding="utf-8")
            assert run(parse("--preview"), script=script, home=home, environ=env,
                       launch=launch, platform="linux", geteuid=lambda: 1000) == 23
            assert calls[-1][0][-2:] == ["--install", "--dry-run"]
            if os.name != "nt":
                original_game = game.with_name("original-product")
                game.rename(original_game)
                outside = root / "outside product"
                outside.mkdir()
                game.symlink_to(outside, target_is_directory=True)
                try:
                    find_game(parse(), [steam])
                    raise AssertionError("default game symlink escape accepted")
                except ValueError as exc:
                    assert "escapes Steam install" in str(exc)
                game.unlink()
                original_game.rename(game)
                entry = runtime / "_v2-entry-point"
                entry.unlink()
                outside_entry = root / "outside runtime"
                outside_entry.write_bytes(b"script")
                entry.symlink_to(outside_entry)
                try:
                    runtime_path(proton / "proton", [steam])
                    raise AssertionError("runtime entry symlink escape accepted")
                except ValueError as exc:
                    assert "runtime entry escapes" in str(exc)
                entry.unlink()
                entry.write_bytes(b"script")
            try:
                run(parse("--preview"), script=script, home=home, environ=env,
                    launch=launch, platform="linux", geteuid=lambda: 0)
                raise AssertionError("root accepted")
            except ValueError as exc:
                assert "sudo" in str(exc)
            assert len(calls) == 1
            (compat / "config_info").unlink()
            try:
                build_plan(args, script=script, home=home, environ=env)
                raise AssertionError("missing record accepted")
            except ValueError as exc:
                assert "--proton" in str(exc)
            assert build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                              home=home, environ=env)[0][3] == str(proton / "proton")
            (compat / "pfx/drive_c").rmdir()
            try:
                build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                           home=home, environ=env)
                raise AssertionError("missing prefix accepted")
            except ValueError as exc:
                assert "launch Elite once" in str(exc)
            (compat / "pfx/drive_c").mkdir()
            (proton / "toolmanifest.vdf").write_text(
                '"manifest" { "require_tool_appid" "999999" }', encoding="utf-8")
            try:
                build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                           home=home, environ=env)
                raise AssertionError("missing runtime accepted")
            except ValueError as exc:
                assert "999999" in str(exc)
            (proton / "toolmanifest.vdf").unlink()
            try:
                build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                           home=home, environ=env)
                raise AssertionError("missing toolmanifest accepted")
            except ValueError as exc:
                assert "toolmanifest.vdf" in str(exc)
            (proton / "toolmanifest.vdf").write_text('"manifest" { "version" "2" }', encoding="utf-8")
            assert build_plan(parse("--proton", str(proton), "--dry-run"), script=script,
                              home=home, environ=env)[0][0] == str(proton / "proton")
            second = other / "steamapps/common/Elite Dangerous" / PRODUCT
            second.mkdir(parents=True)
            (second / "EliteDangerous64.exe").write_bytes(b"game")
            (other / "steamapps/appmanifest_359320.acf").write_text(
                '"AppState" { "appid" "359320" "installdir" "Elite Dangerous" }', encoding="utf-8")
            (steam / "steamapps/libraryfolders.vdf").write_text(
                '"libraryfolders" { "0" { "path" "' + str(steam) + '" } '
                '"1" { "path" "' + str(other) + '" } }', encoding="utf-8")
            (second / "EliteDangerous64.exe").unlink()
            assert find_game(parse(), [steam])[3] == game  # stale second manifest is skipped
            (second / "EliteDangerous64.exe").write_bytes(b"game")
            try:
                find_game(parse(), [steam])
                raise AssertionError("ambiguous libraries accepted")
            except ValueError as exc:
                assert "multiple" in str(exc)
            assert find_game(parse("--game-dir", str(game)), [steam])[3] == game
            try:
                safe_leaf("../escape", "test")
                raise AssertionError("unsafe leaf accepted")
            except ValueError:
                pass
            try:
                vdf(root / "missing.vdf")
                raise AssertionError("missing VDF accepted")
            except ValueError:
                pass
        finally:
            if os.name == "nt":
                globals()["windows_path"] = original_windows_path
    print("install_proton: self-test passed")


def main(argv=None):
    args = parser().parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    try:
        return run(args)
    except ValueError as exc:
        print("EDVR: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
