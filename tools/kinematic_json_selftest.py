"""Strict round-trip gate for the KinematicEvalProbe JSON writer.

Runs build\\kinematic_json_test.exe --self-test, which serializes a
deterministic fixture through the production writeJson, then json.loads
the output (strict: any quoting regression raises here) and asserts every
fixture value round-trips. Three writer failures -- a dropped closing
quote after a hex value, a misplaced quote inside an array, a broken
record termination -- reached the flight journal before this gate
existed; compilation alone caught none of them.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build' / 'kinematic_json_test.exe'


def self_test():
    if not EXE.exists():
        raise AssertionError(f'{EXE} is missing; build.bat compiles it before this gate runs')
    r = subprocess.run([str(EXE), '--self-test'], capture_output=True, text=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode != 0:
        raise AssertionError(f'kinematic_json_test --self-test failed:\n{r.stderr}')
    ke = json.loads(r.stdout)['kinematicEval']
    assert ke['status'] == 'not_run', ke['status']
    assert ke['summary'] == dict(
        observed=987654321, records=2, transitions=1, vtables=1,
        read_faults=3, record_overflow=1, transition_overflow=2, vtable_overflow=4,
        epoch_matches=11, epoch_mismatches=22, epoch_changes=33,
        ownership_checks=44, owner_backptr_match=45, owner_state4=46, ownership_overflow=47,
        riglink_checks=48, riglink_state4=49, riglink_null_collection=50, riglink_overflow=51,
        xf_movers=52, xf_changes=53,
        pose_samples=2, pose_sample_overflow=5, identity_event_overflow=7,
        dup_in_frame=2, gap_events=1, node_change_events=1, quat_change_frames=6,
        frames_counted=40, zero_record_frames=1,
        min_frame_records=2, max_frame_records=17,
        clock_sample_overflow=3, gap_relog_skipped=11, non_finite_pose=9), ke['summary']
    recs = ke['records']
    assert len(recs) == 2, recs
    mover, still = recs
    first = [f'0x{0x1000 + k:x}' for k in range(11)]
    latest = [f'0x{0x1000 + k:x}' for k in range(8)] + ['0x800000008000', '0xffff00008000', '0x80000000ffff']
    assert mover == dict(
        id=0, record='0x1234abcd5678ef00', first_frame=10, last_frame=42, calls=123,
        node='0x1111222233334444', pose_ctx='0xaabbccddeeff0011',
        pred='0x7777888899990000', pred_vtable_rva='0x4301234',
        pred2='0x5555666677778888', pred2_vtable_rva='0x4305678',
        hash='0xdeadbeefcafef00d', count298=31, flags='0xc3',
        bool234=1, pred_byte=90, gate2=2, job_mask=0x15,
        epoch1b8='0x1122334455667788', desc_epoch38='0x8877665544332211',
        xf_changes=7, last_xf_change=42,
        frames_sampled=40, dup_in_frame=2, gaps=1, max_gap=3,
        node_changes=1, quat_change_frames=6, max_jump=0.125, total_jump=4.5,
        xf_first=first, xf_latest=latest), mover
    assert still == dict(
        id=1, record='0x2222333344445555', first_frame=10, last_frame=42, calls=120,
        node='0x6666777788889999', pose_ctx='0x0', pred='0x0', pred_vtable_rva='0x0',
        pred2='0x0', pred2_vtable_rva='0x0', hash='0x0', count298=0, flags='0x41',
        bool234=0, pred_byte=0, gate2=0, job_mask=1, epoch1b8='0x0', desc_epoch38='0x0',
        xf_changes=0, last_xf_change=0,
        frames_sampled=40, dup_in_frame=0, gaps=0, max_gap=0,
        node_changes=0, quat_change_frames=0, max_jump=0, total_jump=0,
        xf_first=['0x800000008000'] * 11, xf_latest=['0x800000008000'] * 11), still
    assert ke['transitions'] == [dict(
        record=0, frame=42, old_flags='0x1', new_flags='0xc3',
        old_hash='0x1111111111111111', new_hash='0xdeadbeefcafef00d')], ke['transitions']
    assert ke['vtables'] == [dict(rva='0x4301234', first_frame=10, hits=123)], ke['vtables']
    jobs = ke['jobs']
    assert len(jobs) == 6, jobs
    assert jobs[0] == dict(name='Kinematic::UpdateRenderDataJob', calls=2,
                           total_ns=1000, mean_ns=500, max_ns=700), jobs[0]
    assert jobs[3] == dict(name='Kinematic::PrePhysicsAdvanceJob', calls=1,
                           total_ns=42, mean_ns=42, max_ns=42), jobs[3]
    for i in (1, 2, 4, 5):
        assert jobs[i]['calls'] == 0 and jobs[i]['total_ns'] == 0 \
            and jobs[i]['mean_ns'] == 0 and jobs[i]['max_ns'] == 0, jobs[i]
    assert ke['ownership'] == [dict(
        collection='0xaaaa0000bbbb0001', owner='0xaaaa0000bbbb0002',
        alias20='0xaaaa0000bbbb0003', epoch90='0xeeee',
        backptr='0xaaaa0000bbbb0001', owner_state=4, job=1,
        first_frame=10, hits=55)], ke['ownership']
    assert ke['riglinks'] == [dict(
        rig='0xf00d000000000001', pose_ctx='0xf00d000000000002',
        collection='0xaaaa0000bbbb0001', game_obj='0xf00d000000000003',
        descriptor='0xf00d000000000004', owner='0xf00d000000000005',
        coll_epoch90='0xeeee', rig_state=4, first_frame=10, hits=58,
        collection_known=1)], ke['riglinks']
    assert ke['pose_events'] == [dict(
        record=1, frame=30, kind=1, gap_len=3,
        old_node='0x6666777788889999', new_node='0x6666777788889999'),
        dict(record=0, frame=41, kind=2, gap_len=0,
             old_node='0x1111222233334444', new_node='0x99990000aaaabbbb')], ke['pose_events']
    assert ke['mover_samples'] == [dict(
        record=0, frame=42,
        t=['0x3f800000', '0x40000000', '0x40400000'],
        q=[32768, 32768, 32768, 65535]),
        dict(record=0, frame=43,
             t=['0x3f000000', '0x40000000', '0x40400000'],
             q=[32768, 32768, 32768, 65534])], ke['mover_samples']
    # Two consecutive present ticks (the mesh clock retired 2026-09-23).
    assert ke['clock_samples'] == [dict(present=1001),
                                   dict(present=1002)], ke['clock_samples']
    # Physics dirty-queue counts: max_delta 5 exceeds every kept sample's
    # delta (4, 1), proving the counter is independent of the sample cap.
    assert ke['phys_queue'] == dict(runs=3, appended=7, max_delta=5, resets=1,
                                    samples=[dict(entry=10, exit=14),
                                             dict(entry=20, exit=21)]), ke['phys_queue']
    # Dirty-queue node capture: total == ring + overflow by construction
    # (519 == 3 + 516); the native gate drives the invariant dynamically.
    assert ke['phys_nodes'] == dict(total=519, overflow=516, nodes=[
        '0x1111111111111111', '0x2222222222222222', '0x3333333333333333']), ke['phys_nodes']
    # Draw-item-builder bucket counts (the census join, perf arc).
    assert ke['bucket_items'] == dict(calls=9001, items=123456, empty_calls=31,
                                      entry_wild=7, exit_fault=5, neg_deltas=11,
                                      overflow_calls=3, max_buckets=17,
                                      max_items_per_call=4711), ke['bucket_items']
    # Direct producers (bucket = param_2): per-producer stat sets, named.
    assert ke['bucket_items_direct'] == [
        dict(producer='fun_144312e00', calls=6101, items=777777, neg_deltas=13,
             read_faults=2, max_items_per_call=909),
        dict(producer='fun_14369c9c0', calls=6202, items=888888, neg_deltas=14,
             read_faults=4, max_items_per_call=808)], ke['bucket_items_direct']
    print('kinematic json self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return
    ap.error('Nothing to do without --self-test')


if __name__ == '__main__':
    main()
