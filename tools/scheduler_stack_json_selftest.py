"""Strict round-trip gate for the SchedulerStackProbe JSON writer.

Runs build\\scheduler_stack_json_test.exe --self-test, which serializes a
deterministic fixture through the production writeJson, then json.loads
the output (strict: any quoting regression raises here) and asserts every
fixture value round-trips. The kinematic writer shipped three quoting
failures before its gate existed; this gate exists so the scheduler
writer's first failure is caught in the build, not in a flight journal.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build' / 'scheduler_stack_json_test.exe'


def self_test():
    if not EXE.exists():
        raise AssertionError(f'{EXE} is missing; build.bat compiles it before this gate runs')
    r = subprocess.run([str(EXE), '--self-test'], capture_output=True, text=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode != 0:
        raise AssertionError(f'scheduler_stack_json_test --self-test failed:\n{r.stderr}')
    ss = json.loads(r.stdout)['schedulerStack']
    assert ss['status'] == 'installed', ss['status']
    assert ss['active'] == 0, ss['active']
    targets = ss['targets']
    assert len(targets) == 4, targets
    names = ['worker-entry-144321940', 'worker-entry-144320340',
             'record-drain-1442df940', 'reset-repopulate-1436a0f50']
    for i, t in enumerate(targets):
        assert t['name'] == names[i], t
        assert t['calls'] == 1000 + i * 100 + 1, t
        assert t['faults'] == i, t
        assert t['unique'] == 2, t
        assert t['overflow'] == 7 + i, t
        assert t['collisions'] == 3 + i, t
        sigs = t['signatures']
        assert len(sigs) == 2, sigs
        assert sigs[0] == dict(
            sig=f'0x{0x1111111100000000 + i * 0x100000000 + 0xAAAA:x}',
            depth=3, count=900 + i * 10 + 1,
            stack=[f'0x{0x1462B4A0 + i:x}',
                   f'0x{0x144321940 + i:x}',
                   f'0x{0x1436A1000 + i:x}']), sigs[0]
        assert sigs[1] == dict(
            sig=f'0x{0x2222222200000000 + i * 0x100000000 + 0xBBBB:x}',
            depth=1, count=80 + i * 10 + 2,
            stack=[f'0x{0x145DD120 + i:x}']), sigs[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print('scheduler_stack_json_selftest: ok')
        return
    parser.error('only --self-test is supported')


if __name__ == '__main__':
    main()
