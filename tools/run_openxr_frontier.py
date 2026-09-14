#!/usr/bin/env python3
"""Launch a fresh EDLaunch with the verified, reversible native OpenXR package.

Never installs, restores, edits settings, supplies login tokens, or terminates
processes. EDLaunch startup is not evidence of a successful game session.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

try:
    from tools.openxr_pe import validate_frontier_imports
    from tools.run_openxr_native import child_environment
    from tools.install_edvr import resolve_target, native_paths, verify_native_receipt
except ImportError:
    from openxr_pe import validate_frontier_imports
    from run_openxr_native import child_environment
    from install_edvr import resolve_target, native_paths, verify_native_receipt

ROOT = Path(__file__).resolve().parents[1]
DENY = {'elitedangerous64.exe', 'edlaunch.exe', 'vrserver.exe',
        'vrcompositor.exe', 'vrmonitor.exe', 'openxr_native_test.exe',
        'openxr_module_test.exe'}
HIDDEN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def absolute_path(value, label):
    if not value or not os.path.isabs(value):
        raise ValueError(label + ' must be an absolute path')
    return Path(value).resolve()


def existing_file(value, label):
    path = absolute_path(value, label)
    if not path.is_file():
        raise ValueError('missing ' + label + ': ' + str(path))
    return path


def runtime_library(manifest):
    data = json.loads(manifest.read_text(encoding='utf-8-sig'))
    runtime = data.get('runtime') if isinstance(data, dict) else None
    value = runtime.get('library_path') if isinstance(runtime, dict) else None
    if not isinstance(value, str) or not value.strip():
        raise ValueError('runtime JSON lacks runtime.library_path')
    return existing_file(str((manifest.parent / value).resolve()), 'runtime library')


def parse_inventory(output):
    rows = list(csv.reader(output.splitlines()))
    if not rows or any(len(row) != 5 or not row[0].strip() or
                       not row[1].isdigit() or not row[3].isdigit() for row in rows):
        raise ValueError('malformed process inventory')
    return {row[0].strip().lower() for row in rows}


def inventory():
    try:
        result = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'],
                                capture_output=True, text=True, timeout=20,
                                check=True, creationflags=HIDDEN)
        return parse_inventory(result.stdout or '')
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        raise ValueError('could not prove process state: ' + str(exc)) from exc


def preflight(args):
    root = absolute_path(args.root, 'root') if args.root else ROOT
    target = Path(resolve_target(args.target)).resolve()
    launcher = existing_file(args.launcher, 'launcher')
    if launcher.name.lower() != 'edlaunch.exe':
        raise ValueError('launcher must be EDLaunch.exe')
    loader = existing_file(args.loader, 'loader')
    manifest = existing_file(args.runtime, 'runtime JSON')
    runtime = runtime_library(manifest)
    paths = {key: Path(value).resolve()
             for key, value in native_paths(str(root), str(target)).items()}
    for key in ('native_source', 'graphics_source', 'original'):
        existing_file(str(paths[key]), key)
    game = existing_file(str(target / 'EliteDangerous64.exe'), 'game')
    imports = validate_frontier_imports(game, paths['native_source'])
    inputs = dict(game=game, launcher=launcher, loader=loader,
                  runtime_manifest=manifest, runtime_library=runtime,
                  native=paths['native_source'], graphics=paths['graphics_source'],
                  original=paths['original'])
    hashes = {key: {'path': str(path), 'sha256': sha256(path)}
              for key, path in inputs.items()}
    receipt = None
    if args.native_receipt:
        receipt_path = existing_file(args.native_receipt, 'native receipt')
        receipt = verify_native_receipt(str(receipt_path), str(target))
        if Path(receipt['root']).resolve() != root:
            raise ValueError('receipt build root differs from selected root')
        for entry in receipt['files']:
            if entry['installed_sha256'].lower() != hashes[entry['key']]['sha256']:
                raise ValueError('installed ' + entry['key'] + ' differs from selected build')
        hashes['native_receipt'] = {'path': str(receipt_path), 'sha256': sha256(receipt_path)}
    elif not args.dry_run:
        raise ValueError('--native-receipt is required for a real launch')
    ini = paths['ini']
    hashes['ini'] = {'path': str(ini), 'sha256': sha256(ini) if ini.is_file() else None}
    # Bind the already loaded proxy beside Elite; loading another path to the
    # build could produce a second module without the game's hook/callback state.
    graphics = paths['graphics_target']
    env = child_environment(dict(os.environ), loader, graphics, manifest, True, True)
    out = absolute_path(args.output, 'output') if args.output else (
        root / 'build' / ('openxr-frontier-' + datetime.now().strftime('%Y%m%d-%H%M%S')))
    if out.exists() or not out.parent.is_dir():
        raise ValueError('output must be new with an existing parent: ' + str(out))
    return dict(version=1, root=str(root), target=str(target), launcher=str(launcher),
                cwd=str(launcher.parent), output=str(out), hashes=hashes,
                bootstrap={'EDVR_OPENXR_LOADER': str(loader),
                           'EDVR_OPENXR_GRAPHICS': str(graphics),
                           'EDVR_OPENXR_SEPARATE_DEVICE': '1',
                           'XR_RUNTIME_JSON': str(manifest)},
                imports=imports, pending_install=receipt is None), env


def write_json(path, value):
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')


def run(args):
    plan, env = preflight(args)
    busy = sorted(inventory() & DENY)
    plan.update(mode='dry-run' if args.dry_run else 'launch-plan', busy=busy,
                timestamp_utc=datetime.now(timezone.utc).isoformat())
    if args.dry_run:
        print(json.dumps(plan, sort_keys=True))
        return 0
    if busy:
        raise ValueError('close these processes before launch: ' + ', '.join(busy))
    out = Path(plan['output'])
    out.mkdir()
    write_json(out / 'launch-plan.json', plan)
    with (out / 'launcher.stdout.log').open('xb') as stdout, \
            (out / 'launcher.stderr.log').open('xb') as stderr:
        child = subprocess.Popen([plan['launcher']], cwd=plan['cwd'], env=env,
                                 stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                 creationflags=HIDDEN)
    launched = {'version': 1, 'mode': 'launcher-started', 'pid': child.pid,
                'plan': str(out / 'launch-plan.json'),
                'timestamp_utc': datetime.now(timezone.utc).isoformat(),
                'note': 'Use EDLaunch Play; game/session not yet verified.'}
    try:
        write_json(out / 'launch-receipt.json', launched)
    except OSError as exc:
        print(json.dumps(launched, sort_keys=True))
        raise ValueError('EDLaunch started, but its receipt could not be written: ' + str(exc)) from exc
    print(json.dumps(launched, sort_keys=True))
    return 0


def self_test():
    checks = 0

    def check(value):
        nonlocal checks
        checks += 1
        assert value, 'check %d' % checks

    def refused(fn):
        try:
            fn()
        except (OSError, ValueError):
            check(True)
        else:
            check(False)

    with tempfile.TemporaryDirectory(prefix='edvr-native-launch-test-') as temporary:
        base = Path(temporary).resolve()
        root, game = base / 'repo', base / 'game'
        (root / 'build').mkdir(parents=True)
        (game / 'Openvr' / 'win64').mkdir(parents=True)
        paths = native_paths(str(root), str(game))
        for value in paths.values():
            Path(value).write_bytes(b'fixture:' + Path(value).name.encode())
        exe, launcher = game / 'EliteDangerous64.exe', base / 'EDLaunch.exe'
        loader, manifest, runtime = base / 'loader.dll', base / 'runtime.json', base / 'runtime.dll'
        rec = base / 'native.json'
        for path in (exe, launcher, loader, runtime, rec):
            path.write_bytes(b'fixture')
        manifest.write_text('{"runtime":{"library_path":"runtime.dll"}}', encoding='utf-8')
        args = argparse.Namespace(root=str(root), target=str(game), launcher=str(launcher),
                                  loader=str(loader), runtime=str(manifest), native_receipt=None,
                                  output=str(base / 'output'), dry_run=True)
        valid_receipt = dict(root=str(root), files=[
            dict(key=key, installed_sha256=sha256(paths[key + '_source']).upper())
            for key in ('native', 'graphics')])

        def snapshot():
            return {str(path.relative_to(base)): path.read_bytes() if path.is_file() else None
                    for path in base.rglob('*')}

        with patch(__name__ + '.validate_frontier_imports', return_value=[]), \
                patch(__name__ + '.verify_native_receipt', return_value=valid_receipt), \
                patch(__name__ + '.inventory', return_value={'edlaunch.exe'}), \
                patch(__name__ + '.subprocess.Popen') as spawn:
            before = snapshot()
            check(run(args) == 0 and snapshot() == before and not spawn.called)
            args.dry_run = False
            refused(lambda: run(args))
            args.native_receipt = str(rec)
            refused(lambda: run(args))
            check(snapshot() == before and not spawn.called)
            valid_receipt['files'][0]['installed_sha256'] = '0' * 64
            refused(lambda: preflight(args))
            valid_receipt['files'][0]['installed_sha256'] = sha256(paths['native_source'])
            valid_receipt['root'] = str(base)
            refused(lambda: preflight(args))
            valid_receipt['root'] = str(root)
            with patch(__name__ + '.inventory', side_effect=ValueError('unavailable')):
                refused(lambda: run(args))
            with patch(__name__ + '.inventory', return_value=set()), \
                    patch(__name__ + '.write_json', side_effect=OSError('disk full')):
                refused(lambda: run(args))
                check(not spawn.called)
            Path(args.output).rmdir()
            with patch(__name__ + '.inventory', return_value=set()):
                spawn.return_value.pid = 42
                check(run(args) == 0 and spawn.call_count == 1)
                call = spawn.call_args
                check(call.kwargs['cwd'] == str(launcher.parent))
                check(call.kwargs['env']['EDVR_OPENXR_GRAPHICS'] == paths['graphics_target'])
                check(json.loads((Path(args.output) / 'launch-receipt.json').read_text())['pid'] == 42)

        for invalid in ('', 'garbage', '"EliteDangerous64.exe","abc","Console","1","1 K"'):
            refused(lambda: parse_inventory(invalid))
        check(parse_inventory('"EliteDangerous64.exe","123","Console","1","1,024 K"') == {'elitedangerous64.exe'})
        for text in ('[]', '{}', '{"runtime":5}', '{"runtime":{"library_path":""}}',
                     '{"runtime":{"library_path":"missing.dll"}}'):
            manifest.write_text(text, encoding='utf-8')
            refused(lambda: runtime_library(manifest))
        refused(lambda: existing_file('relative.dll', 'loader'))
        inherited = dict(os.environ)
        inherited.update(edvr_openxr_loader='stale', xr_runtime_json='stale')
        before = inherited.copy()
        env = child_environment(inherited, loader, Path(paths['graphics_target']), manifest, True, True)
        check(inherited == before and 'edvr_openxr_loader' not in env and 'xr_runtime_json' not in env)
        result = subprocess.run([sys.executable, '-c',
                                 'import os; print(os.environ["EDVR_OPENXR_SEPARATE_DEVICE"]); '
                                 'print(os.environ["XR_RUNTIME_JSON"])'],
                                env=env, capture_output=True, text=True, timeout=10, creationflags=HIDDEN)
        check(result.returncode == 0 and result.stdout.splitlines() == ['1', str(manifest)])
    print('run_openxr_frontier: %d checks, 0 failures' % checks)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--target', help='frontier or an explicit game directory')
    ap.add_argument('--root', help='absolute build repository/stage root')
    ap.add_argument('--launcher', help='absolute EDLaunch.exe')
    ap.add_argument('--loader', help='absolute openxr_loader.dll')
    ap.add_argument('--runtime', help='absolute runtime JSON')
    ap.add_argument('--native-receipt', help='absolute installed native receipt')
    ap.add_argument('--output', help='absolute new evidence directory')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    for name in ('target', 'launcher', 'loader', 'runtime'):
        if not getattr(args, name):
            ap.error('--' + name + ' is required')
    try:
        return run(args)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print('run_openxr_frontier: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
