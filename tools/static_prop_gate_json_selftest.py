"""Strict round-trip gate for the StaticPropGate JSON writer.

Runs build\\static_prop_gate_json_test.exe --self-test, which serializes a
deterministic fixture through the production writeJson, then json.loads
the output (strict: any quoting regression raises here) and asserts every
fixture value round-trips. The kinematic writer shipped three quoting
failures before its gate existed; this gate exists so the gate writer's
first failure is caught in the build, not in a flight journal.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build' / 'static_prop_gate_json_test.exe'


def self_test():
    if not EXE.exists():
        raise AssertionError(f'{EXE} is missing; build.bat compiles it before this gate runs')
    r = subprocess.run([str(EXE), '--self-test'], capture_output=True, text=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode != 0:
        raise AssertionError(f'static_prop_gate_json_test --self-test failed:\n{r.stderr}')
    g = json.loads(r.stdout)['staticPropGate']
    assert g['status'] == 'installed', g['status']
    assert g['active'] == 0, g['active']
    assert g['callsSeen'] == 4320, g
    assert g['callsSkipped'] == 3901, g
    assert g['callsRun'] == 419, g
    assert g['forcedRefreshes'] == 12, g
    assert g['invalidations'] == {'journal': 3, 'reset': 2, 'session': 1}, g
    assert g['cacheSize'] == 1572, g
    assert g['cacheCap'] == 4096, g
    assert g['evictions'] == 7, g
    assert g['oversize'] == 5, g
    assert g['verifyFaults'] == 1, g
    assert g['forcedRefreshFrames'] == 30, g
    assert g['recordsPerCollection'] == 16, g


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print('static_prop_gate_json_selftest: ok')
        return
    parser.error('only --self-test is supported')


if __name__ == '__main__':
    main()
