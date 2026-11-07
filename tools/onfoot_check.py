#!/usr/bin/env python3
"""Offline checker for EDVR's on-foot flight recorder (F213): which stage moved first?

    python tools\\onfoot_check.py --target <game dir> --expect-build HEAD [--mark N]
    python tools\\onfoot_check.py --dir <edvr_logs\\onfoot> [--stamp 20261105_140000] [--list]
    python tools\\onfoot_check.py --target <game dir> --mark 2 --window-s 3 1
    python tools\\onfoot_check.py --target <game dir> --dump pin --fields e,stance,settle_why [--since 14:03:00 --until 14:04:00]
    python tools\\onfoot_check.py --self-test
    python tools\\onfoot_check.py --verify-fixture <folder the recorder rig wrote>

The recording needs log.on_foot_detail on; F9 (hotkey.on_foot_mark) drops a numbered mark at the moment the pilot saw something wrong. A session
is every onfoot_<stamp>_<nnn>.rec of one stamp in <game>\\edvr_logs\\onfoot; the newest stamp is read unless --stamp names one. The files describe
themselves (a JSON header carries every record's fields), so this reader is driven by the header and never by hard-coded offsets. A rule whose
field the file lacks prints "cannot evaluate: <record>.<field> missing" and the rest still run.

Reports: 1 session header (build, joins, seq gaps, runtime records, marks), 2 height excursions with the stage that moved first, 3 bob, 4 the
raise-leak gains (the F171 line's definitions, from the recording), 5 arm IK events with a cause, 6 the marks. Every finding carries local time, so
it can be compared with the text log. Every limit below is a named constant and a starting guess, until a flown recording says otherwise.

The height ledger, per valid pin record that is not cam_external (cam_raw is the camera as the eyes were rendered; E is pin.e, after the removals):
    cam_raw   = E + SEATED_Y_SIGN * (sub_raise + sub_vneck_y + sub_ads_y) + sub_weapon_view_y
    body_view = cam_raw - gain * (head_y - head_ref)             the height the pilot gets; gain is HEAD_GAIN = 0, pin.e already excludes the head
    game      = E - gain * (head_y - head_ref) - SEATED_Y_SIGN * (eye_y - sub_raise - sub_vneck_y - sub_ads_y) + sub_weapon_view_y
    so  body_view = game + SEATED_Y_SIGN * eye_y   exactly, and a term's effect on the view is SEATED_Y_SIGN times its change.
SEATED_Y_SIGN is -1 because the pin takes the camera LESS the camera's body-axes motion for a seated shift S, which is (-S0, -S1, +S2)
(native_on_foot_core.h adsCamShiftBody; F171's raise, "up positive", is -sub_raise).

Exit 0 no finding past a limit, 3 a finding past a limit, 2 the recording is of another build than --expect-build, 1 no recording or usage.
It writes no files. Read-only, like character_motion_check.py.
"""
import argparse
import collections
import contextlib
import datetime
import io
import json
import math
import os
import re
import shutil
import struct
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ---- limits: every one a starting guess (exit 3 on a finding past one) -------------------------------------------------------------------------------------------
SEATED_Y_SIGN = -1.0        # camera effect of a seated y term (see the docstring)
EXC_M = 0.015               # height excursion: |error| over this (m) ...
EXC_MIN_S = 0.100           # ... for this long ...
EXC_MERGE_S = 0.100         # ... merging gaps shorter than this
TRANSITION_S = 0.6          # an excursion beginning within this after a stance, item or ADS change is a transition
SLOW_SETTLE_S = 1.0         # a transition taking longer than this to come back is slow_settle
EVIDENCE_AFTER_S = 0.5      # the evidence window for a cause ends this long after the onset (or when the episode does)
LOOKBACK_S = 0.3            # the pre-onset baseline, and the evidence window's start
STALL_S = 0.100             # a gap in pin or align records over this is a stall
STALL_AFTER_S = 0.100       # a stall counts when it ends this close after (or anywhere before) the onset
RUNTIME_DEFERRED_S = 0.050  # rt_eye deferred for this long
RUNTIME_DIFF_M = 0.005      # |wanted - applied|
GAME_MOVED_M = 0.005        # "the game moved" for subtraction_miss
SAT_ROOM_M = 0.005          # legs' room left under this is used up ...
SAT_ROOM_SHARE = 0.5        # ... for this share of the window's calls (a running stride touches zero room on its own, 19% of the flown calls)
STORE_MOVED_M = 0.03        # a stored neck or cam moved this much
STARVED_S = 1.0             # not settled this long ...
STARVED_AGE_MS = 250.0      # ... and the raise target this old
GAME_SHARE = 0.60           # game_camera: the game holds this share of the move, with EDVR terms flat
TERM_FLAT_M = 0.005         # "flat" for a term
TERM_SHARE = 0.50           # term:<name>: the largest same-signed term holds this share
LEVEL_DEG = 10.0            # settled level frames: |aim pitch| under this
REF_MIN_ROWS = 20           # rows a class needs for a reference
HEADREF_MIN_ROWS = 30       # rows a recentre segment needs for its own head_ref
HEAD_GAIN = 0.0             # the gain of pin.e[1] on the physical head: none (F87h's hold cancels the sustained offset, pinEyeYAdjust the transient)
GAIN_TOL = 0.15             # the fit may differ from HEAD_GAIN by this before it is a finding
GAIN_FIT_MIN_RMS_M = 0.01   # a recentre segment is fitted only where the head moved this much rms (one flown segment moved 0.25 cm and gave a slope of +4.6)
JOIN_MAX_AGE_S = 0.25       # a p1 older than this is not the pin's camera
BOB_WINDOW_S = 0.25         # the moving mean
BOB_LIMIT_M = 0.002         # a flown good value was 0.0003
BOB_MIN_S = 1.0             # a group needs this much settled time
EXC_EXCLUDE_S = 0.3         # bob leaves out this far around an excursion
LEAK_BAND_LO, LEAK_BAND_HI = 10.0, 40.0      # F171's |aim pitch| bands
LEAK_MIN_STEP_M = 0.002     # F171 kMinStepM: a pair counts when the carried raise changed this much
LEAK_MAX_GAP = 2            # F171 kMaxP1Gap
LEAK_FLAG_SLOPE = 0.3       # F171 kFlagSlope: the 40+ band with k_up under this says the raise is not in the hook's camera
LEAK_THIN_PAIRS = 30        # a cell with fewer pairs prints as thin, not as a finding
IK_ELBOW_JUMP_M, IK_HAND_STILL_M = 0.05, 0.02
IK_SWIVEL_JUMP_DEG = 15.0
IK_GRIP_ERR_CM, IK_GRIP_CALLS, IK_GRIP_W = 3.0, 3, 0.5
IK_REACH_RATIO, IK_SHORTFALL_CM, IK_OVERREACH_S = 1.0, 1.0, 0.100
IK_NECK_JUMP_M, IK_PIN_STILL_M = 0.03, 0.01
IK_ELBOW_HIGH_S = 0.300
IK_ELBOW_LO_DEG, IK_ELBOW_HI_DEG, IK_ELBOW_LIMIT_S = 30.0, 178.0, 0.100
IK_LOOKBACK_S = 0.1         # evidence window before an event's onset
IK_POST_S = 0.1             # and after it, at most
IK_FP_AGE_MS = 100.0
IK_INFEASIBLE_RATIO = 0.995
IK_SHORTFALL_SEEN_CM = 0.1
IK_ANIM_SHARE = 0.8         # the animation moved this share of what the IK output moved
IK_ANIM_MIN_M = 0.01        # and the output moved at least this
IK_EXEMPT_EXITS = ('ik_off',)   # not a refusal: the feature is off
IK_JOIN_S = 0.05            # an ik record this close before a pin row is that row's joints
VN_SMOOTH_S = 0.3           # view minus neck is averaged over a stride before it is judged
VN_MIN_S = 0.300            # view_under_neck: under 0 this long
HANDS_MIN_S = 0.100         # hands_above_view: the higher hand above the view this long
STALE_M = 0.03              # raise_stale: the raise this far from its value at the last settled upright standing row ...
STALE_S = 1.0               # ... while not settled, this long
PATH_WINDOW_S = 0.5         # the path of the model root, for slope and ground speed
FOOT_ROLL_S = 0.4           # the lowest foot is a rolling minimum over this
FOOT_MATCH = 0.35           # the lowest foot fell by E's drop within this share: the model origin rose
FOOT_HOLD = 0.25            # the lowest foot moved under this share of E's drop: the camera fell
PITCH_CHANGED_DEG = 3.0
MOVING_MPS = 2.5            # the E over neck table: moving faster than this along the ground ...
FACING_COS = 0.95           # ... and along the facing: the path's horizontal part and the body's forward (pin.fwd) within this cosine
PHYS_HEAD_SHARE = 0.60      # physical_head: the head's own change (head_local y) over the same window holds this share of the move, with the same sign
FEET_NECK_HELD_M = 0.03     # the feet verdict is read only where the animated neck held within this over the window
F214_ERR_CM = 5.0           # F214 finding: the view over the neck is off neck + eye height by more than this on average where the follow ran (a starting guess)
F214_BOB_MM = 5.0           # F214 finding: the raise's own bob, rms after a moving mean, over this where the follow ran (a starting guess)
F214_BOB_WINDOW_S = 0.3     # the moving mean for the raise's own bob
F215_CONST_CM = 8.0         # F215: the level-ground constant, the game's camera over the mean animated neck (nofoot::f94::kCamOverUprightNeckM)
F215_R0_CM = 3.0            # F215 finding: the measured level E over the mean neck is off the constant by more than this (a starting guess)
F215_NECK_MEAN_S = 0.6      # F215: the neck's mean window
F215_LEVEL_PATH_M = 0.5     # F215 level rows: the last this much of the root's path in the plane ...
F215_LEVEL_COS = 0.95       # ... the facing along the path (cos over this) ...
F215_STEADY_DOT = 0.98      # ... the facing steady over the path (dot of the facing now and at its start) ...
F215_LEVEL_TAN = 0.05       # ... and the slope along the path under this (|tan|)
F215_LEVEL_MIN_ROWS = 20    # level rows needed for the comparison
F215_TURN_DEG = 8.0         # an in-place turn: the heading changes over this ...
F215_TURN_S = 0.25          # ... within this long
F215_TURN_SIDE_MPS = 0.6    # ... with the plane speed of the root under this ...
F215_TURN_SIDE_S = 0.3      # ... for this long each side
F215_TURN_AFTER_S = 1.2     # ... and the height moves are read to this long after the turn, because a raise that follows the neck catches up as much as a second after it ...
F215_TURN_LEFT_M = 0.15     # ... or until the root has left by this in the plane (the pilot set off)
F215_TURN_MOVE_CM = 3.0     # F215 finding: the median view move across the turns is over this (a starting guess)
F215_TURN_CAM_JUDGE_CM = 2.0   # F217: a turn is judged only when the camera, the physical head out, moved under this (else the camera moved, not the body: not judged)
F217_LEVEL_CAM_M = 1.60     # F217: E (true) on level ground with an item at the hip (a starting guess; the arc cutter read 1.60-1.61 m, the laser rifle 1.56-1.59)
F217_LEVEL_TOL_CM = 5.0     # F217 finding: the median E on level ground with an item is further than this from that height
F216_STRETCH_ROWS = 128     # F216: a straight stretch is this many rows ...
F216_FACING_DOT = 0.998     # ... with the facing steady (dot with the first row's over this) ...
F216_MIN_MPS = 2.0          # ... at a plane speed of at least this
F216_SPEED_SPLIT_MPS = 5.0  # the speed bins
F216_SLOPE_SPLIT = 0.05     # the slope bins (|tan| under, over)
F216_BAND_HZ = (2.5, 6.0)   # the bob's band
F216_SIDE_BAND_HZ = (1.0, 6.0)   # the side's band: the sway runs at half the stride's rate, about 1.7 Hz at a sprint
F216_SPRINT_MM = 3.0        # F216 finding: a level sprint leaves more than this in the view with the key off (a starting guess; 4.0 for the moving-mean filter)
F216_CLAMP_M = 0.025        # nofoot::f216::kClampM, for the clamp share ...
F216_CLAMP_OLD_M = 0.05     # ... 5 cm for a recording whose build predates the band-pass filter (no p1.hud_shift in it)
F216_LOW_BAND_HZ = (0.0, 1.5)   # the up term's slow part: what a change of slope fills
F218_SPEED_S = 0.5          # F218/F219 section: the root's plane speed is taken over the last this many seconds
F218_WALK_MPS = 0.3         # walk: from this speed ...
F218_SPRINT_MPS = 4.0       # ... sprint: from this speed
F218_MAX_MPS = 12.0         # a speed over this is the root jumping (a ship, a teleport), not a class
F218_MIN_CLASS_S = 1.0      # a class with less left-arm writing than this is left out
F218_SPINE_FLIP = 0.5       # a step of the spine weight over this is a flip
F218_FINDING_MIN_S = 2.0    # F218 finding: an item with this much left-arm writing at speed ...
F222_RATE_DPS = 100.0        # F222 section: rows with the body turning at this rate (deg/s) or more ...
F222_MIN_ROWS = 40          # ... an item with this many is read
F222_FINDING_ROWS = 100     # finding heading_follows_yaw_step: this many rows ...
F222_FINDING_SLOPE = -0.5   # ... and the heading's residual on the yaw step's residual under this slope
F222_RATE_SPAN_S = 0.15     # the body's yaw rate is taken over the last this many seconds
F224_AIR_VY_MPS = 1.5        # F224 section: air is an armed row with the root's vertical speed over this (|pin.root_vy|); the lower animated foot over F224_AIR_FOOT_M is read beside it
F224_AIR_FOOT_M = 0.30      # F224 section: the lower animated foot over this is air
F224_RUN_MPS = 3.5          # ... on the ground over this speed is a run, from F218_WALK_MPS a walk, under it still
F224_MIN_ROWS = 30          # a class with fewer rows is left out
F224_FINDING_MM = 2.0       # finding body_move_late: the air residual of the camera less the whole shift over this (90th percentile of its size, mm; the rms is carried by a few calls) ...
F224_FINDING_ROWS = 100     # ... on this many rows
F225_HOP_STEP_M = 0.02      # F225: a solver hop: the left elbow's step against the chest over this with the hand against the chest under ...
F225_HAND_STEP_M = 0.008    # ... this
F225_FLIP_CALLS = 3         # ... and it is on an assist flip when l_assisted changed within this many calls
F225_FINDING_PER_S = 0.5    # finding elbow_solver_hop: more hops a second than this on the ground at the hip ...
F225_FINDING_MIN_S = 10.0   # ... over this many seconds
F226_STILL_MPS = 0.15         # F226: standing still: the root's plane speed (over F226_SPEED_S) under this
F226_SPEED_S = 0.2          # ... taken over the last this many seconds
F226_HELD_FAR_M = 0.15      # finding shift_held_far: y this far or more from what the rule would ask (median over the stretch) ...
F226_HELD_FAR_S = 0.5       # ... in held_aim, held_pose or held_head for this long
F226_LEVEL_DEFAULT_M = 1.60 # the level camera when no active_item row gives one (nofoot::f215::kDefaultLevelCamM)
F223_JUMP_M = 0.10          # F223 section: the rule's limits (src/d3d11/native_on_foot_f223.h): a change of the camera in the model this large ...
F223_CAMERA_SHARE = 0.5     # ... with the camera in the world moving less than this share of it ...
F223_MAX_GAP_S = 0.1        # ... between frames this close
F223_GROUP_S = 0.15         # calls the rule would take closer than this are one event
F223_LOOK_CALLS = 6         # the chest and the elbow are read over this many calls from the event
F223_CRUSH_M = 0.25         # finding root_jump_crushed: the final chest falls more than this
F221_STRAFE_MPS = 1.0         # F221: a strafe: the root moving this fast or more along the body's right (or left) ...
F221_STRAFE_SPAN_S = 0.2    # ... measured over the last this many seconds
F221_OVER_HAND_CM = 3.0     # F221 finding: the left elbow this far over the hand, at the hip ...
F221_OVER_HAND_MIN_S = 0.1  # ... for this long
F221_ITEM_SETTLE_S = 1.0    # ... and later than this after an item change
F218_FINDING_STEPS_PER_S = 1.0   # ... and more elbow steps over 5 cm a second than this (a starting guess; the flown walk with the arc cutter read 1.1, the rifle 4.6)
EYE_HEIGHT_DEFAULT_CM = 15.0  # fix.native_on_foot_eye_height_cm when the recording's header does not carry it
SLOPE_BINS = (-90.0, -15.0, -5.0, 5.0, 15.0, 90.1)
MAX_LISTED = 60             # findings listed per report before "... N more"

MAGIC = b'EDVRONF1'
NPT = {'u8': 'u1', 'u16': '<u2', 'u32': '<u4', 'u64': '<u8', 'f32': '<f4', 'f64': '<f8'}
ARMS = ('l_', 'r_')
TERMS = ('run', 'ads_lock', 'sight', 'weapon_view', 'weapon_eye', 'hold', 'raise', 'vneck', 'ads_turn', 'bob', 'body', 'unlisted')

# every field a rule reads: the self-test pins them against its writer's schema and --verify-fixture against the C++ one
REQ_LEDGER = {
    'align': ['eye', 'run', 'ads_lock', 'sight', 'weapon_view', 'weapon_eye', 'virtual_neck', 'head_local', 'hold_y', 'raise_y', 'ads_turn_dy', 'serial', 'published',
              'cam_external', 'ads', 'raise_clamped', 'raise_age_ms'],
    'p1': ['sub_ads', 'sub_vneck', 'sub_weapon_view', 'sub_raise', 'serial_carried', 'game_frame', 'src'],
    'pin': ['e', 'valid', 'refuse', 'settled', 'settle_why', 'stance', 'armed', 'support', 'item_serial', 'p1', 'e_age_ms', 'detach_w', 'aim_pitch_deg', 'aim_known',
            'neck_anim', 'raw_eye_y', 'f98_eye_y', 'pin_capped_h', 'pin_capped_v', 'f97_capped', 'f179_ran', 'f179_legs_room', 'stored_neck_stand', 'stored_neck_crouch',
            'stored_cam_stand', 'stored_cam_crouch', 'eye_source'],
    'rt_eye': ['wanted', 'applied', 'refusals', 'deferred'],
    'rt_recentre': ['ok'],
}
REQ_FWD = {'pin': ['heading']}
REQ_IK = {'ik': ['exit', 'refuse', 'write_down', 'plan_gen', 'fp_stale', 'g_stale', 'fp_age_ms', 'joints', 'anim_valid', 'final_valid', 'final_neck', 'anim_neck',
                 'final_chest', 'anim_chest'] +
          [p + s for p in ARMS for s in ('wrote', 'reach_ratio', 'shortfall_cm', 'swivel_deg', 'elbow_deg', 'grip_err_cm', 'w_arm', 'pole_src', 'swivel_capped',
                                          'swivel_min_violation', 'swivel_fallback', 'reach_limited', 'f159_reseed', 'f165_applied', 'f192_applied')] +
          ['%s_%s%s' % (w, p, j) for w in ('final', 'anim') for p in ARMS for j in ('upper', 'fore', 'hand')],
          'pin': ['e', 'pin_capped_h', 'pin_capped_v', 'f97_capped'], 'align': []}
REQ_MARK = {'mark': ['number']}
REQ_VIEW = {'ik': ['final_valid', 'final_neck', 'final_l_hand', 'final_r_hand']}
REQ_STALE = {'align': ['raise_m']}
# recorded from the flight after the first (F213): older files lack them, and the rules that use them say so
REQ_NEW = {'pin': ['up', 'fwd'], 'ik': ['anim_pelvis', 'anim_l_foot', 'anim_r_foot', 'final_pelvis', 'final_l_foot', 'final_r_foot']}
REQ_F214 = {'pin': ['f214_state', 'f214_target']}   # recorded from F214 on
REQ_F215 = {'pin': ['f215_state', 'f215_body_y', 'f215_target']}   # recorded from F215 on; older recordings read 0 for the body's shift
REQ_F216 = {'align': ['bob_seated', 'bob_w'], 'p1': ['sub_bob', 'bob_w']}   # recorded from F216 on; older recordings read 0 for the bob term
# recorded from F218 and F219 on; older recordings lack them, and the F218/F219 section says so and gives no finding for the figures that need them (the step figures need only the first group)
REQ_F218 = {'pin': ['root', 'item_serial', 'f218_held', 'f218_applied', 'f218_refused', 'f218_offset_m'],
            'ik': ['item_serial', 'spine_w', 'l_reach_ratio', 'l_wrote', 'final_l_fore', 'final_chest', 'final_valid', 'l_swivel_limited', 'r_swivel_limited', 'l_swivel_cut_deg', 'r_swivel_cut_deg']}
REQ_F223 = {'pin': ['f223_pop_m']}   # recorded from F223 on; older recordings lack it and the root jump section says so
REQ_F224 = {'pin': ['f224_lead_m']}   # recorded from F224 on; older recordings lack it and the whole-body move section says so
REQ_F226 = {'pin': ['f226_head_pitched']}   # recorded from F226 on; older recordings lack it and the head line says so
REQ_ALL = {}
for _d in (REQ_LEDGER, REQ_FWD, REQ_IK, REQ_MARK, REQ_VIEW, REQ_STALE, REQ_NEW, REQ_F214, REQ_F215, REQ_F216, REQ_F218, REQ_F223, REQ_F224, REQ_F226):
    for _k, _v in _d.items():
        REQ_ALL.setdefault(_k, [])
        REQ_ALL[_k] = sorted(set(REQ_ALL[_k]) | set(_v))


class MissingField(Exception):
    pass


class RecError(Exception):
    pass


# ---- reading --------------------------------------------------------------------------------------------------------------------------------------------------------
def build_dtype(rec, prefix):
    names, formats, offsets = [], [], []
    for p in prefix:
        names.append(p['name'])
        formats.append(NPT[p['type']])
        offsets.append(p['offset'])
    for f in rec['fields']:
        if f['name'] in names:
            continue
        base = NPT[f['type']]
        names.append(f['name'])
        formats.append((base, (f['count'],)) if f['count'] > 1 else base)
        offsets.append(f['offset'])
    return np.dtype({'names': names, 'formats': formats, 'offsets': offsets, 'itemsize': rec['size']})


def read_segment(path):
    """-> (header dict, {id: bytes of that type's whole records}, truncated, record count)."""
    with open(path, 'rb') as fh:
        b = fh.read()
    if len(b) < 12 or b[:8] != MAGIC:
        raise RecError('%s: not an on-foot recording (no EDVRONF1)' % os.path.basename(path))
    n = struct.unpack_from('<I', b, 8)[0]
    if 12 + n > len(b):
        raise RecError('%s: truncated inside its header' % os.path.basename(path))
    hdr = json.loads(b[12:12 + n].decode('utf-8'))
    sizes = {r['id']: r['size'] for r in hdr['records'].values()}
    chunks = collections.defaultdict(list)
    off, truncated, count = 12 + n, False, 0
    while off < len(b):
        if len(b) - off < 16:
            truncated = True
            break
        rid, size = struct.unpack_from('<HH', b, off)
        if size < 16 or size % 8 != 0 or (rid in sizes and size != sizes[rid]):
            truncated = True
            break
        if off + size > len(b):
            truncated = True
            break
        if rid in sizes:
            chunks[rid].append(b[off:off + size])
            count += 1
        off += size
    return hdr, {k: b''.join(v) for k, v in chunks.items()}, truncated, count


class Tab:
    def __init__(self, name, arr=None):
        self.name = name
        self.arr = arr
        self.tt = None

    def __len__(self):
        return 0 if self.arr is None else len(self.arr)

    def has(self, field):
        return self.arr is not None and field in self.arr.dtype.names

    def f(self, field):
        if not self.has(field):
            raise MissingField('%s.%s' % (self.name, field))
        return self.arr[field]


class Session:
    def __init__(self):
        self.files, self.hdrs, self.tabs, self.notes = [], [], {}, []
        self.truncated, self.stats = [], {}
        self.freq, self.qpc0, self.anchor, self.build, self.game_build, self.pid, self.keys = 1.0, 0, None, '', '', 0, {}
        self.enums = {}
        self.fields = {}

    def tab(self, name):
        return self.tabs.get(name) or Tab(name)

    def t(self, name):
        tb = self.tab(name)
        if tb.tt is None:
            tb.tt = (tb.arr['qpc'].astype(np.float64) - self.qpc0) / self.freq if len(tb) else np.zeros(0)
        return tb.tt

    def enum(self, en, val):
        vals = self.enums.get(en)
        return vals.index(val) if vals and val in vals else -1

    def enum_name(self, en, i):
        vals = self.enums.get(en) or []
        return vals[int(i)] if 0 <= int(i) < len(vals) else str(int(i))

    def lt(self, t):
        if self.anchor is None:
            return 'T+%.3fs' % t
        return (self.anchor + datetime.timedelta(seconds=float(t))).strftime('%H:%M:%S.%f')[:-3]

    def tod(self, t):
        a = self.anchor
        base = a.hour * 3600 + a.minute * 60 + a.second + a.microsecond * 1e-6 if a else 0.0
        return (base + t) % 86400.0

    def missing(self, spec):
        out = []
        for name, fields in spec.items():
            tb = self.tab(name)
            for f in fields:
                if not tb.has(f):
                    out.append('%s.%s' % (name, f))
        return out


def parse_local(s):
    for fmt in ('%Y-%m-%d %H:%M:%S.%f', '%Y-%m-%d %H:%M:%S', '%Y-%m-%dT%H:%M:%S.%f'):
        try:
            return datetime.datetime.strptime(s, fmt)
        except (ValueError, TypeError):
            pass
    return None


SEG_RE = re.compile(r'^onfoot_(\d{8}_\d{6})_(\d{3})\.rec$')


def list_stamps(folder):
    out = collections.defaultdict(list)
    if os.path.isdir(folder):
        for n in sorted(os.listdir(folder)):
            m = SEG_RE.match(n)
            if m:
                out[m.group(1)].append(os.path.join(folder, n))
    return out


def load_session(folder, stamp):
    S = Session()
    files = list_stamps(folder).get(stamp)
    if not files:
        raise RecError('no recording %s in %s' % (stamp, folder))
    per = collections.defaultdict(list)
    for p in files:
        try:
            hdr, chunks, trunc, count = read_segment(p)
        except RecError as e:
            S.notes.append(str(e))
            continue
        S.files.append(p)
        S.hdrs.append(hdr)
        S.truncated.append(trunc)
        for name, rec in hdr['records'].items():
            dt = build_dtype(rec, hdr['prefix'])
            data = chunks.get(rec['id'], b'')
            arr = np.frombuffer(data, dtype=dt) if data else np.zeros(0, dtype=dt)
            per[name].append((arr, rec['id']))
    if not S.hdrs:
        raise RecError('nothing readable for %s in %s' % (stamp, folder))
    h0 = S.hdrs[0]
    S.build, S.game_build, S.pid, S.keys = h0.get('build', ''), h0.get('game_build', ''), h0.get('pid', 0), h0.get('keys', {})
    S.freq = float(h0.get('qpc_freq') or 1.0)
    S.qpc0 = int(h0.get('qpc_at_open') or 0)
    S.anchor = parse_local(h0.get('local_time_at_open', ''))
    S.enums = h0.get('enums', {})
    for name, parts in per.items():
        base = parts[0][0].dtype
        use = [a for a, _ in parts if a.dtype == base]
        if len(use) != len(parts):
            S.notes.append('%s: a segment has another schema; only the first schema is read' % name)
        arr = np.concatenate(use) if len(use) > 1 else use[0]
        if len(arr):
            arr = arr[np.argsort(arr['seq'], kind='stable')]
        S.tabs[name] = Tab(name, arr)
        if len(arr):
            seq = arr['seq'].astype(np.int64)
            S.stats[name] = dict(n=len(arr), first=int(seq[0]), gaps=int(np.maximum(np.diff(seq) - 1, 0).sum()) if len(seq) > 1 else 0)
        else:
            S.stats[name] = dict(n=0, first=0, gaps=0)
    return S


# ---- small numeric helpers -------------------------------------------------------------------------------------------------------------------------------------------
def runs_of(mask):
    idx = np.flatnonzero(mask)
    if idx.size == 0:
        return []
    br = np.flatnonzero(np.diff(idx) > 1)
    return list(zip(np.r_[idx[0], idx[br + 1]].tolist(), np.r_[idx[br], idx[-1]].tolist()))


def tmean(t, v, w):
    """A centred moving mean over w seconds on rows at times t; NaN rows are left out."""
    f = np.isfinite(v)
    c = np.r_[0.0, np.cumsum(np.where(f, v, 0.0))]
    k = np.r_[0, np.cumsum(f)]
    lo = np.searchsorted(t, t - w / 2, 'left')
    hi = np.searchsorted(t, t + w / 2, 'right')
    cnt = k[hi] - k[lo]
    return np.where(cnt > 0, (c[hi] - c[lo]) / np.maximum(cnt, 1), np.nan)


def gaps_of(t, limit):
    if len(t) < 2:
        return []
    d = np.diff(t)
    return [(float(t[i]), float(t[i + 1])) for i in np.flatnonzero(d > limit)]


def sl(t, lo, hi):
    return int(np.searchsorted(t, lo, 'left')), int(np.searchsorted(t, hi, 'right'))


def wrap180(d):
    return (d + 180.0) % 360.0 - 180.0


def fmt_cm(x):
    return '%+.1f' % (x * 100.0)


def f214_measure(t, view, neck, raise_, mask, eye_m):
    """F214 where the follow ran (mask): the mean of |view - (neck + eye height)| in cm, and the raise's own bob in mm (rms of the raise less its centred moving mean over F214_BOB_WINDOW_S,
    over the masked rows; None without a raise series)."""
    err = float(np.nanmean(np.abs(view[mask] - (neck[mask] + eye_m))) * 100.0)
    bob = None
    if raise_ is not None and np.isfinite(raise_[mask]).any():
        res = raise_ - tmean(t, raise_, F214_BOB_WINDOW_S)
        bob = float(np.sqrt(np.nanmean(res[mask] ** 2)) * 1000.0)
    return err, bob


class Finding:
    def __init__(self, t0, t1, kind, cause, detail, limit=True):
        self.t0, self.t1, self.kind, self.cause, self.detail, self.limit = float(t0), float(t1), kind, cause, detail, limit


# ---- the analysis --------------------------------------------------------------------------------------------------------------------------------------------------------
class Analysis:
    def __init__(self, S):
        self.S = S
        self.findings, self.out, self.L = [], [], None
        self.leak = {}
        self.cannot = []
        self.ik = None
        self.f224 = None   # the F224 section's figures
        self._f218_speed = None   # the root's plane speed on each ik row, set by the F218 section for the F224 one
        self.f222 = self.f223 = None   # the F222 and F223 sections' figures
        self.f215 = self.f216 = self.f218 = None   # the F215, F216 and F218/F219 sections' figures
        self.lever = self.lever_view = self.f214 = None   # the E / view over neck fits on the slope, and the F214 section's figures

    def say(self, s=''):
        self.out.append(s)

    def need(self, rule, spec):
        miss = self.S.missing(spec)
        for m in miss:
            line = 'cannot evaluate: %s missing (%s)' % (m, rule)
            if line not in self.cannot:
                self.cannot.append(line)
        return not miss

    # -- header -------------------------------------------------------------------------------------------------------------------------------------------------------------
    def header(self):
        S = self.S
        self.say('== session header')
        self.say('build %s, game %s, pid %s, %d file(s)%s' % (S.build, S.game_build, S.pid, len(S.files),
                                                                      ', last record truncated (tolerated)' if any(S.truncated) else ''))
        for n in S.notes:
            self.say('note: ' + n)
        t_all = [S.t(n) for n in S.tabs if len(S.tabs[n])]
        if t_all:
            lo, hi = min(float(a[0]) for a in t_all), max(float(a[-1]) for a in t_all)
            self.span = (lo, hi)
            self.say('recorded %s to %s local, %.1f s' % (S.lt(lo), S.lt(hi), hi - lo))
        else:
            self.span = (0.0, 0.0)
            self.say('no records')
        parts = []
        for n in ('align', 'p1', 'pin', 'ik', 'mark', 'rt_eye', 'rt_recentre'):
            st = S.stats.get(n, dict(n=0, first=0, gaps=0))
            parts.append('%s %d%s' % (n, st['n'], (' (%d dropped)' % st['gaps']) if st['gaps'] else ''))
            if st['n'] and st['first'] > 0:
                self.say('note: %s starts at seq %d: the older segments were rotated away' % (n, st['first']))
        self.say('records: ' + ', '.join(parts))
        if not len(S.tab('rt_eye')) and not len(S.tab('rt_recentre')):
            self.say('runtime records: NONE (the runtime half did not record: mixed DLLs, or the export is missing)')
        else:
            self.say('runtime records: rt_eye %d, rt_recentre %d' % (len(S.tab('rt_eye')), len(S.tab('rt_recentre'))))
        if S.keys:
            self.say('keys: ' + ', '.join('%s=%s' % (k, v) for k, v in S.keys.items()))
        # the lookups the other rules use
        self.gaps = []
        for n in ('pin', 'align'):
            self.gaps += gaps_of(S.t(n), STALL_S)
        self.gaps.sort()
        if self.gaps:
            self.say('stalls (a gap over %d ms in pin or align): %d, the longest %.0f ms at %s' % (
                STALL_S * 1e3, len(self.gaps), 1e3 * max(b - a for a, b in self.gaps), S.lt(max(self.gaps, key=lambda g: g[1] - g[0])[0])))

    def fit_head_gain(self, L, hy, e1, seg, sl_mask):
        """The slope of pin.e[1] on head_y over settled level frames, within each class and recentre segment, pooled over the segments where the head moved
        GAIN_FIT_MIN_RMS_M rms or more (a segment where it barely moved gives a meaningless slope). -> (slope or None, why not, segments used)."""
        key = L.cls * 1000 + seg
        use = np.zeros(len(hy), bool)
        nseg = 0
        for s_ in np.unique(seg):
            m = sl_mask & (seg == s_)
            if m.sum() >= REF_MIN_ROWS and float(np.sqrt(np.mean((hy[m] - hy[m].mean()) ** 2))) >= GAIN_FIT_MIN_RMS_M:
                use |= m
                nseg += 1
        if not use.any():
            return None, 'not fitted: the head moved under %g cm rms in every recentre segment' % (GAIN_FIT_MIN_RMS_M * 100), 0

        def pooled(mask):
            sxx = sxy = 0.0
            for kk in np.unique(key[mask]):
                m = mask & (key == kk)
                if m.sum() < 5:
                    continue
                dx = hy[m] - hy[m].mean()
                dy = e1[m] - e1[m].mean()
                sxx += float((dx * dx).sum())
                sxy += float((dx * dy).sum())
            return sxx, sxy
        sxx, sxy = pooled(use)
        if sxx <= 0:
            return None, 'not fitted: no head motion inside a class', 0
        g = sxy / sxx
        med = np.zeros(len(e1))
        for kk in np.unique(key[use]):
            m = use & (key == kk)
            med[key == kk] = np.median(e1[m] - g * hy[m])
        keep = use & (np.abs(e1 - g * hy - med) < EXC_M)
        if keep.sum() >= REF_MIN_ROWS:
            sxx2, sxy2 = pooled(keep)
            if sxx2 > 0:
                g = sxy2 / sxx2
        return g, '', nseg

    # -- the ledger ---------------------------------------------------------------------------------------------------------------------------------------------------------
    def build_ledger(self):
        S = self.S
        spec = dict(REQ_LEDGER)
        spec = {k: v for k, v in spec.items() if k not in ('rt_eye',)}
        if not self.need('height ledger', spec):
            return
        pin, p1, al = S.tab('pin'), S.tab('p1'), S.tab('align')
        if min(len(pin), len(p1), len(al)) == 0:
            self.say('height ledger: no pin, p1 or align records')
            return
        ser = al.f('serial')
        pub = np.flatnonzero(al.f('published') == 1)
        if pub.size == 0:
            self.say('height ledger: no published align record')
            return
        us, first = np.unique(ser[pub][::-1], return_index=True)
        last_idx = pub[::-1][first]

        def lookup(sv):
            pos = np.searchsorted(us, sv)
            pos[pos >= len(us)] = 0
            return np.where(us[pos] == sv, last_idx[pos], -1)
        sc = p1.f('serial_carried')
        p1_al = np.where(sc != 0, lookup(sc), -1)
        t_p1 = np.maximum.accumulate(S.t('p1'))
        t_pin = S.t('pin')
        target = t_pin - pin.f('e_age_ms') * 1e-3 + 0.002
        pi = np.searchsorted(t_p1, target, 'right') - 1
        okp = (pi >= 0)
        okp[okp] = (t_pin[okp] - t_p1[pi[okp]]) <= JOIN_MAX_AGE_S
        pi = np.where(okp, pi, -1)
        ai = np.where(okp, p1_al[np.maximum(pi, 0)], -1)
        self.joins = dict(p1_al=float((p1_al >= 0).mean()), pin_p1=float(okp.mean()), pin_al=float((ai >= 0).mean()))
        ext = al.f('cam_external')
        row = (pin.f('valid') == 1) & (ai >= 0)
        row[row] = ext[ai[row]] == 0
        rt = S.tab('rt_eye')
        if len(rt) and rt.has('serial_seen'):
            rser = rt.f('serial_seen')
            ok = np.isin(rser, ser[pub])
            self.joins['rt_al'] = float(ok.mean())
        self.say('joins: p1 to align %.1f%%, pin to p1 %.1f%%, pin to align %.1f%%%s; ledger rows %d of %d pin records' % (
            100 * self.joins['p1_al'], 100 * self.joins['pin_p1'], 100 * self.joins['pin_al'],
            ', rt_eye to align %.1f%%' % (100 * self.joins['rt_al']) if 'rt_al' in self.joins else '', int(row.sum()), len(pin)))
        for k, v in (('p1 to align', self.joins['p1_al']), ('pin to p1', self.joins['pin_p1'])):
            if v < 0.9:
                self.say('FINDING: join rate %s is %.0f%%, low: the ledger is built on fewer records than the session has' % (k, 100 * v))
        if not row.any():
            self.say('height ledger: no usable row')
            return
        R = np.flatnonzero(row)
        a = ai[R]
        pr = pi[R]
        L = type('Ledger', (), {})()
        L.R, L.ai, L.pi = R, a, pr
        L.t = t_pin[R]
        L.e = pin.f('e')[R].astype(np.float64)
        L.stance = pin.f('stance')[R].astype(int)
        L.armed = pin.f('armed')[R].astype(int)
        L.item = pin.f('item_serial')[R]
        L.support = pin.f('support')[R].astype(int)
        L.settled = pin.f('settled')[R] == 1
        L.why = pin.f('settle_why')[R].astype(int)
        L.pitch = pin.f('aim_pitch_deg')[R].astype(np.float64)
        L.aim_known = pin.f('aim_known')[R] == 1
        L.detach = pin.f('detach_w')[R].astype(np.float64)
        L.p1f = pin.f('p1')[R].astype(np.int64)
        L.rawe = pin.f('raw_eye_y')[R].astype(np.float64)
        L.f98 = pin.f('f98_eye_y')[R].astype(np.float64)
        L.neck = pin.f('neck_anim')[R].astype(np.float64)
        L.ads = (L.stance >= 4) | (al.f('ads')[a] == 1) | (L.detach > 0)
        L.cls = L.stance * 2 + L.armed
        L.level = L.aim_known & (np.abs(L.pitch) < LEVEL_DEG)
        sub_raise = p1.f('sub_raise')[pr].astype(np.float64)
        sub_vn = p1.f('sub_vneck')[pr].astype(np.float64)
        sub_ads = p1.f('sub_ads')[pr].astype(np.float64)
        sub_wv = p1.f('sub_weapon_view')[pr].astype(np.float64)
        # F216: the view bob term the hook took out of the camera (older recordings: none); F215: the body's shift, which is subtracted from the view where it is formed below, so every figure
        # downstream is in the body's frame (the IK record's final_* and pin.neck_applied are the unshifted joints; older recordings: 0)
        sub_bob = p1.f('sub_bob')[pr].astype(np.float64) if p1.has('sub_bob') else np.zeros((len(pr), 3))
        L.by = pin.f('f215_body_y')[R].astype(np.float64) if pin.has('f215_body_y') else np.zeros(len(R))
        L.by = np.where(np.isfinite(L.by), L.by, 0.0)
        L.sub = dict(raise_=sub_raise, vneck=sub_vn, ads=sub_ads, wv=sub_wv, bob=sub_bob)
        L.src = p1.f('src')[pr]
        eye = al.f('eye')[a].astype(np.float64)
        ssub = sub_raise + sub_vn[:, 1] + sub_ads[:, 1] + sub_bob[:, 1]
        listed = {}
        listed['run'] = al.f('run')[a][:, 1].astype(np.float64)
        listed['ads_lock'] = al.f('ads_lock')[a][:, 1].astype(np.float64)
        listed['sight'] = al.f('sight')[a][:, 1].astype(np.float64)
        listed['weapon_view'] = al.f('weapon_view')[a][:, 1].astype(np.float64)
        listed['weapon_eye'] = al.f('weapon_eye')[a][:, 1].astype(np.float64)
        listed['hold'] = al.f('hold_y')[a].astype(np.float64)
        listed['raise'] = al.f('raise_y')[a].astype(np.float64)
        listed['vneck'] = al.f('virtual_neck')[a][:, 1].astype(np.float64)
        listed['ads_turn'] = al.f('ads_turn_dy')[a].astype(np.float64)
        listed['bob'] = al.f('bob_seated')[a][:, 1].astype(np.float64) if al.has('bob_seated') else np.zeros(len(a))   # F216: the unturned seated y; its turn under mouse look falls to 'unlisted'
        listed['unlisted'] = eye[:, 1] - sum(listed.values())
        listed['body'] = L.by   # F215: not a term of the alignment (it is not in eye_y): the view's effect is SEATED_Y_SIGN * body = -body_y, so a change of it is -delta body_y
        L.terms = listed
        L.eye_y = eye[:, 1]
        L.cam_ext = ext[a]
        # head: raw head_local y, its reference between successful recentres, and the gain of E on it
        hy = al.f('head_local')[a][:, 7].astype(np.float64)
        L.head_y = hy
        rc = S.tab('rt_recentre')
        rc_t = np.sort(S.t('rt_recentre')[rc.f('ok') == 1]) if len(rc) else np.zeros(0)
        seg = np.searchsorted(rc_t, L.t, 'right')
        L.seg = seg
        gmed = float(np.median(hy))
        ref = {}
        for s in np.unique(seg):
            m = seg == s
            ref[s] = float(np.median(hy[m])) if m.sum() >= HEADREF_MIN_ROWS else None
        L.head_ref = np.array([ref[s] if ref[s] is not None else gmed for s in seg])
        sl_mask = L.settled & L.level
        e1 = L.e[:, 1]
        fit, why, nseg = self.fit_head_gain(L, hy, e1, seg, sl_mask)
        L.gain_fit = fit
        L.gain = HEAD_GAIN
        self.say('ledger: pin.e is taken to exclude the physical head (gain %g: F87h hold cancels the sustained offset, pinEyeYAdjust the transient); checked by a fit of pin.e[1] on head_y: %s' % (
            HEAD_GAIN, ('%.3f over %d recentre segment(s) where the head moved %g cm rms or more' % (fit, nseg, GAIN_FIT_MIN_RMS_M * 100)) if fit is not None else why))
        if fit is not None and abs(fit - HEAD_GAIN) > GAIN_TOL:
            # Not a finding: F87h absorbs a sustained offset, so the gain stays near 0, but a quick move of the head does reach E (the slope flight's mark 6: the head fell 14 cm and E 15 cm), so a fitted
            # value between 0 and about 1.3 is understood. The ledger takes none of it out; a move the head made is named physical_head.
            self.say('info: pin.e[1] follows the physical head with a fitted gain of %.2f (the ledger assumes %g): a sustained offset is absorbed but a quick move of the head reaches E, so a value between 0 and about 1.3 is expected; '
                     'an excursion the head made is named physical_head' % (fit, HEAD_GAIN))
        L.head_d = HEAD_GAIN * (hy - L.head_ref)
        L.cam = e1 + SEATED_Y_SIGN * ssub + sub_wv[:, 1] - L.by   # the one place the view is formed: less F215's shift of the body, so the view is in the body's frame
        L.body = L.cam - L.head_d
        L.game = e1 - L.head_d - SEATED_Y_SIGN * (eye[:, 1] - ssub) + sub_wv[:, 1]
        L.ssub = ssub
        # per (stance, armed) reference
        L.ref = np.full(len(L.t), np.nan)
        L.refs = {}
        for c in np.unique(L.cls):
            for m in (L.cls == c) & L.settled & L.level, (L.cls == c) & L.settled:
                if m.sum() >= REF_MIN_ROWS:
                    L.refs[int(c)] = (float(np.median(L.body[m])), int(m.sum()))
                    L.ref[L.cls == c] = L.refs[int(c)][0]
                    break
        L.err = L.body - L.ref
        L.chg = np.zeros(len(L.t), bool)
        if len(L.t) > 1:
            L.chg[1:] = (L.stance[1:] != L.stance[:-1]) | (L.item[1:] != L.item[:-1]) | (L.ads[1:] != L.ads[:-1])
        L.tc = L.t[L.chg]
        self.L = L

    def cname(self, c):
        return '%s%s' % (self.S.enum_name('Stance', c // 2), ' armed' if c % 2 else '')

    # -- height excursions --------------------------------------------------------------------------------------------------------------------------------------------------
    def excursions(self):
        L, S = self.L, self.S
        self.say('')
        self.say('== height excursions (body view = the view with the physical head taken out; error against the median of settled level frames per stance and armed)')
        for c, (r, n) in sorted(L.refs.items()):
            self.say('  reference %-22s %.3f m over %d frames' % (self.cname(c), r, n))
        miss = sorted(set(L.cls.tolist()) - set(L.refs))
        for c in miss:
            self.say('  no reference for %s (under %d settled frames): its rows are not judged' % (self.cname(c), REF_MIN_ROWS))
        # f98 bias
        for c, (r, n) in sorted(L.refs.items()):
            m = (L.cls == c) & L.settled & L.level & np.isfinite(L.f98) & (L.f98 > 0)
            if m.sum() >= REF_MIN_ROWS:
                self.say('  f98_eye_y %-22s median %.3f m, reference minus it %+.1f cm (informational)' % (self.cname(c), np.median(L.f98[m]), 100 * (r - np.median(L.f98[m]))))
        err = L.err
        idx = np.flatnonzero(np.abs(err) > EXC_M)
        groups = []
        for j in idx:
            if groups and L.t[j] - L.t[groups[-1][-1]] < EXC_MERGE_S:
                groups[-1].append(j)
            else:
                groups.append([j])
        self.exc = []
        ntrans = 0
        trans_max = 0.0
        for g in groups:
            i0, i1 = g[0], g[-1]
            if L.t[i1] - L.t[i0] < EXC_MIN_S:
                continue
            ip = max(g, key=lambda j: abs(err[j]))
            t0, t1 = float(L.t[i0]), float(L.t[i1])
            tcs = L.tc[(L.tc - 0.05 <= t0) & (t0 <= L.tc + TRANSITION_S)]
            kind = 'height_excursion'
            tc = None
            if tcs.size:
                tc = float(tcs[-1])
                if t1 - tc <= SLOW_SETTLE_S:
                    ntrans += 1
                    trans_max = max(trans_max, t1 - tc)
                    continue
                kind = 'slow_settle'
            self.exc.append((i0, i1, ip, kind, tc))
        for i0, i1, ip, kind, tc in self.exc:
            cause, ev, dl = self.height_cause(i0, i1, ip)
            t0, t1 = float(L.t[i0]), float(L.t[i1])
            d = ' '.join('%s %s' % (k, fmt_cm(v)) for k, v in dl.items() if abs(v) >= 0.0005)
            what = 'slow_settle: %.2f s after the change' % (t1 - tc) if kind == 'slow_settle' else 'height excursion'
            det = '%s, %s %s to %s (%.2f s), peak %s cm, %s; moved by (cm, effect on the view): %s%s' % (
                what, self.cname(int(L.cls[ip])), S.lt(t0), S.lt(t1), t1 - t0, fmt_cm(L.err[ip]), cause, d or 'nothing over 0.5 mm', ('; ' + ev) if ev else '')
            self.findings.append(Finding(t0, t1, kind, cause, det))
        self.say('  %d excursion(s) past %.1f cm for %d ms or more; %d transition(s) that came back within %.1f s (longest %.2f s), not findings' % (
            len(self.exc), EXC_M * 100, EXC_MIN_S * 1e3, ntrans, SLOW_SETTLE_S, trans_max))

    def height_cause(self, i0, i1, ip, series=None, vsign=1.0, extra=None):
        """The cause of a move of `series` (the body view's error by default). vsign: how the view's height enters the series (+1, or -1 for hands minus view);
        extra: contributions that are not EDVR's terms (the neck, the hands), by name."""
        L, S = self.L, self.S
        excursion = series is None
        series = L.err if series is None else series
        extra = extra or {}
        t0, tpk = float(L.t[i0]), float(L.t[ip])
        lo, hi = t0 - LOOKBACK_S, min(float(L.t[i1]), t0 + EVIDENCE_AFTER_S)   # the evidence is what was true at the start, not over a long episode
        # the deltas: the 0.3 s before onset to the peak
        b0, b1 = sl(L.t, lo, t0 - 1e-9)
        base = np.arange(b0, b1) if b1 > b0 else np.array([max(i0 - 1, 0)])
        dl = collections.OrderedDict()
        dl['game'] = vsign * float(L.game[ip] - L.game[base].mean())
        dl['head'] = vsign * float(L.head_d[ip] - L.head_d[base].mean())
        eff = {}
        for k in ('raise', 'hold', 'weapon_eye', 'ads_lock', 'sight', 'vneck', 'ads_turn', 'run', 'weapon_view', 'bob', 'body', 'unlisted'):
            eff[k] = vsign * SEATED_Y_SIGN * float(L.terms[k][ip] - L.terms[k][base].mean())
            dl[k] = eff[k]
        for k, v in extra.items():
            dl[k] = v
        with np.errstate(all='ignore'):
            bm = np.nanmean(series[base]) if np.isfinite(series[base]).any() else 0.0
        move = float(series[ip] - bm)
        # stall
        for a, b in self.gaps:
            if b >= lo and a <= t0 + STALL_AFTER_S:
                return 'stall', 'a %.0f ms gap in pin or align ends %s' % (1e3 * (b - a), S.lt(b)), dl
        # recentre
        rc = S.tab('rt_recentre')
        if len(rc) and rc.has('ok'):
            rt = S.t('rt_recentre')
            m = (rc.f('ok') == 1) & (rt >= lo) & (rt <= hi)
            if m.any():
                moved = rc.arr['moved_cm'][m].max() if rc.has('moved_cm') else float('nan')
                return 'recentre', 'rt_recentre ok at %s moved %.1f cm' % (S.lt(rt[m][0]), moved), dl
        else:
            self.skipped('recentre', 'rt_recentre.ok')
        # runtime
        re_ = S.tab('rt_eye')
        if len(re_) and all(re_.has(x) for x in ('deferred', 'refusals', 'wanted', 'applied')):
            rt = S.t('rt_eye')
            a0, a1 = sl(rt, lo, hi)
            if a1 > a0:
                dfr = re_.f('deferred')[a0:a1] == 1
                for r0, r1 in runs_of(dfr):
                    if rt[a0 + r1] - rt[a0 + r0] >= RUNTIME_DEFERRED_S:
                        return 'runtime_not_applied', 'rt_eye deferred %.0f ms from %s' % (1e3 * (rt[a0 + r1] - rt[a0 + r0]), S.lt(rt[a0 + r0])), dl
                rf = re_.f('refusals')[max(a0 - 1, 0):a1]
                if rf.size and rf.max() > rf.min():
                    return 'runtime_not_applied', 'rt_eye refusals rose %d' % (rf.max() - rf.min()), dl
                dw = np.abs(re_.f('wanted')[a0:a1, 1] - re_.f('applied')[a0:a1, 1]).max()   # the height: x and z lag while moving and do not move the view up or down
                if dw > RUNTIME_DIFF_M:
                    return 'runtime_not_applied', 'wanted and applied differ by %.1f cm in y' % (100 * dw), dl
        else:
            self.skipped('runtime_not_applied', 'rt_eye.deferred/refusals/wanted/applied (or no rt_eye records)')
        # subtraction miss
        p1 = S.tab('p1')
        p1t = S.t('p1')
        a0, a1 = sl(p1t, lo, hi)
        if a1 > a0 and (p1.f('src')[a0:a1] != 0).any() and abs(dl['game']) >= GAME_MOVED_M:
            return 'subtraction_miss', 'p1 src not matched on %d record(s) and the game moved %s cm' % ((p1.f('src')[a0:a1] != 0).sum(), fmt_cm(dl['game'])), dl
        # camera away
        pin, al = S.tab('pin'), S.tab('align')
        pt, at = S.t('pin'), S.t('align')
        a0, a1 = sl(pt, lo, hi)
        away = S.enum('PinRefuse', 'camera_away')
        if a1 > a0 and (pin.f('refuse')[a0:a1] == away).any():
            return 'camera_away', 'pin refused camera_away', dl
        b0_, b1_ = sl(at, lo, hi)
        if b1_ > b0_ and (al.f('cam_external')[b0_:b1_] == 1).any():
            return 'camera_away', 'cam_external set', dl
        # saturation
        sat = []
        if b1_ > b0_ and (al.f('raise_clamped')[b0_:b1_] == 1).any():
            sat.append('raise_clamped')
        if a1 > a0:
            if (pin.f('pin_capped_v')[a0:a1] == 1).any():
                sat.append('pin_capped_v')
            if (pin.f('f97_capped')[a0:a1] == 1).any():
                sat.append('f97_capped')
            ran = pin.f('f179_ran')[a0:a1] == 1
            if (ran & (pin.f('f179_legs_room')[a0:a1] <= SAT_ROOM_M)).mean() >= SAT_ROOM_SHARE:   # a stride touches zero room every step: only a window mostly at zero is a cap
                sat.append('legs_room used up')
        if sat:
            return 'saturation', ', '.join(sat), dl
        # wrong stance store
        if a1 > a0:
            for f in ('stored_neck_stand', 'stored_neck_crouch', 'stored_cam_stand', 'stored_cam_crouch'):
                v = pin.f(f)[max(a0 - 1, 0):a1]
                if v.size and (v.max() - v.min()) > STORE_MOVED_M:
                    return 'wrong_stance_store', '%s moved %.1f cm' % (f, 100 * (v.max() - v.min())), dl
            es = pin.f('eye_source')[a0:a1]
            if es.size and (es != es[0]).any():
                return 'wrong_stance_store', 'eye_source changed', dl
        # settle starved: the unsettled run holding the onset, and the raise target's age
        starved = ''
        un = ~(pin.f('settled') == 1)
        k0 = int(np.searchsorted(pt, t0, 'left'))
        if k0 < len(pt) and un[min(k0, len(un) - 1)] or k0 > 0 and un[k0 - 1]:
            j0 = min(k0, len(un) - 1)
            while j0 > 0 and un[j0 - 1] and pt[j0] - pt[j0 - 1] < EXC_MERGE_S:
                j0 -= 1
            j1 = min(k0, len(un) - 1)
            while j1 < len(un) - 1 and un[j1 + 1] and pt[j1 + 1] - pt[j1] < EXC_MERGE_S:
                j1 += 1
            ages = al.f('raise_age_ms')[b0_:b1_] if b1_ > b0_ else np.zeros(0)
            if pt[j1] - pt[j0] > STARVED_S and ages.size and ages.max() > STARVED_AGE_MS:
                starved = 'not settled for %.1f s and the raise target %.0f ms old' % (pt[j1] - pt[j0], ages.max())
                # An excursion is judged in a settled stance, so a starved settle is the fault. For the view against the body it says why nothing corrected the move, not what moved.
                if excursion:
                    return 'settle_starved', starved, dl
        uncorrected = '; not corrected: ' + starved if starved else ''
        # physical head: the head's own change over the same window (base to peak) holds most of the move, with the same sign. E excludes a sustained offset of the head but a quick move reaches it,
        # which the game's camera cannot be blamed for.
        dph = vsign * float(L.head_y[ip] - L.head_y[base].mean())
        if abs(move) > 0 and dph * move > 0 and abs(dph) >= PHYS_HEAD_SHARE * abs(move):
            return 'physical_head', 'the physical head (head_local y) moved %s cm of the %s cm move, %.0f%%%s' % (fmt_cm(dph), fmt_cm(move), 100 * abs(dph) / abs(move), uncorrected), dl
        # game camera
        tmax = max(abs(v) for k, v in eff.items())
        if abs(move) > 0 and abs(dl['game']) >= GAME_SHARE * abs(move) and tmax < TERM_FLAT_M:
            return 'game_camera', 'the game holds %.0f%% of the move, EDVR terms flat%s' % (100 * abs(dl['game']) / abs(move), uncorrected), dl
        pool = {'term:' + k: v for k, v in eff.items()}
        pool.update(extra)
        same = {k: v for k, v in pool.items() if v * move > 0}
        if same:
            k = max(same, key=lambda q: abs(same[q]))
            if abs(same[k]) >= TERM_SHARE * abs(move):
                return k, uncorrected[2:], dl
        return 'unattributed', 'game %s cm of the %s cm move; no term holds half%s' % (fmt_cm(dl['game']), fmt_cm(move), uncorrected), dl

    def skipped(self, cause, what):
        line = 'cannot evaluate: %s missing (cause %s)' % (what, cause)
        if line not in self.cannot:
            self.cannot.append(line)

    # -- the view against the body, for every stance ------------------------------------------------------------------------------------------------------------------------
    def view_rules(self):
        L, S = self.L, self.S
        self.say('')
        self.say('== view against the body, every stance (view = the body view; neck and hands as the IK finally wrote them, written or not; model space)')
        self.vn_eps = []
        if self.need('raise_stale', REQ_STALE):
            self.raise_stale()
        ik = S.tab('ik')
        if not len(ik) or not self.need('view_under_neck, hands_above_view', REQ_VIEW):
            return
        tk = S.t('ik')
        ji = np.searchsorted(tk, L.t, 'right') - 1
        ok = ji >= 0
        ok[ok] = (L.t[ok] - tk[ji[ok]]) <= IK_JOIN_S
        ji = np.maximum(ji, 0)
        ok &= ik.f('final_valid')[ji] == 1
        nan = np.full(len(L.t), np.nan)
        neck = np.where(ok, ik.f('final_neck')[ji][:, 1], nan).astype(np.float64)
        hl = ik.f('final_l_hand')[ji][:, 1].astype(np.float64)
        hr = ik.f('final_r_hand')[ji][:, 1].astype(np.float64)
        with np.errstate(all='ignore'):
            hand = np.where(ok, np.fmax(hl, hr), np.nan)
        L.neck_y, L.hand_y, L.ik_ok, L.ji = neck, hand, ok, ji
        view = L.body
        vs = tmean(L.t, view - neck, VN_SMOOTH_S)
        ha = hand - view
        self.say('  %d of %d ledger rows have a final neck and hands within %d ms' % (int(ok.sum()), len(L.t), int(IK_JOIN_S * 1e3)))
        eps = self.episodes(np.nan_to_num(vs, nan=1.0) < 0, VN_MIN_S)
        for i0, i1 in eps:
            seg = np.arange(i0, i1 + 1)
            ip = int(seg[np.nanargmin(vs[seg])])
            raw_under = np.flatnonzero(np.nan_to_num(view[seg] - neck[seg], nan=1.0) < 0)
            if raw_under.size:   # the moving mean smears the onset by half its window: the onset is the first row that is under on its own
                i0 = int(seg[raw_under[0]])
            base = self.base_rows(i0)
            with np.errstate(all='ignore'):
                dneck = float(neck[ip] - np.nanmean(neck[base]))
            cause, ev_, dl = self.height_cause(i0, i1, ip, series=vs, vsign=1.0, extra={'neck_moved': -dneck})
            self.vn_eps.append((i0, i1, ip, cause))
            t0, t1 = float(L.t[i0]), float(L.t[i1])
            d = ' '.join('%s %s' % (k, fmt_cm(v)) for k, v in dl.items() if abs(v) >= 0.0005)
            self.findings.append(Finding(t0, t1, 'view_under_neck', cause, 'view under the neck, %s %s to %s (%.2f s), peak %s cm (view minus neck), %s; moved by (cm, effect on view minus neck): %s%s' % (
                self.cname(int(L.cls[i0])), S.lt(t0), S.lt(t1), t1 - t0, fmt_cm(vs[ip]), cause, d or 'nothing over 0.5 mm', ('; ' + ev_) if ev_ else '')))
        eps2 = self.episodes(np.nan_to_num(ha, nan=-1.0) > 0, HANDS_MIN_S)
        for i0, i1 in eps2:
            seg = np.arange(i0, i1 + 1)
            ip = int(seg[np.nanargmax(ha[seg])])
            base = self.base_rows(i0)
            with np.errstate(all='ignore'):
                dhand = float(hand[ip] - np.nanmean(hand[base]))
            cause, ev_, dl = self.height_cause(i0, i1, ip, series=ha, vsign=-1.0, extra={'hands_moved': dhand})
            low = [e for e in self.vn_eps if e[0] <= i1 and i0 <= e[1]]
            if low:   # hands swing with every stride, so what puts them above the view here is the view being low; the view's own finding names what moved it
                ev_ = 'the view is under the neck through this (%s, from %s)' % (low[0][3], S.lt(float(L.t[low[0][0]])))
                cause = 'view_under_neck'
            t0, t1 = float(L.t[i0]), float(L.t[i1])
            d = ' '.join('%s %s' % (k, fmt_cm(v)) for k, v in dl.items() if abs(v) >= 0.0005)
            self.findings.append(Finding(t0, t1, 'hands_above_view', cause, 'hands above the view, %s %s to %s (%.2f s), the higher hand up to %s cm above it, %s; moved by (cm, effect on hands minus view): %s%s' % (
                self.cname(int(L.cls[i0])), S.lt(t0), S.lt(t1), t1 - t0, fmt_cm(ha[ip]), cause, d or 'nothing over 0.5 mm', ('; ' + ev_) if ev_ else '')))
        self.say('  view_under_neck: %d episode(s) of %.0f ms or more (view minus neck under 0 after a %.1f s moving mean); hands_above_view: %d episode(s) of %.0f ms or more' % (
            len(eps), VN_MIN_S * 1e3, VN_SMOOTH_S, len(eps2), HANDS_MIN_S * 1e3))
        self.camera_vs_body()

    def base_rows(self, i0):
        L = self.L
        lo, hi = sl(L.t, L.t[i0] - LOOKBACK_S, L.t[i0] - 1e-9)
        return np.arange(lo, hi) if hi > lo else np.array([max(i0 - 1, 0)])

    def episodes(self, mask, min_s):
        L = self.L
        idx = np.flatnonzero(mask)
        groups = []
        for j in idx:
            if groups and L.t[j] - L.t[groups[-1][-1]] < EXC_MERGE_S:
                groups[-1].append(j)
            else:
                groups.append([j])
        return [(g[0], g[-1]) for g in groups if L.t[g[-1]] - L.t[g[0]] >= min_s]

    def raise_stale(self):
        L, S = self.L, self.S
        al = S.tab('align')
        rm = al.f('raise_m')[L.ai].astype(np.float64)
        n = len(L.t)
        ar = np.arange(n)
        stand = L.settled & (L.stance == S.enum('Stance', 'standing'))
        last_stand = np.maximum.accumulate(np.where(stand, ar, -1))
        last_set = np.maximum.accumulate(np.where(L.settled, ar, -1))
        ref = np.where(last_stand >= 0, rm[np.maximum(last_stand, 0)], np.nan)
        bad = (~L.settled) & (last_stand >= 0) & (np.abs(rm - ref) > STALE_M)
        eps = self.episodes(bad, STALE_S)
        for i0, i1 in eps:
            held = float(np.median(rm[i0:i1 + 1]))
            li = int(last_set[i0])
            frm = S.enum_name('Stance', L.stance[li]) if li >= 0 else '?'
            ls = int(last_stand[i0])
            t0, t1 = float(L.t[i0]), float(L.t[i1])
            self.findings.append(Finding(t0, t1, 'raise_stale', 'from_' + frm, 'raise stale %s to %s (%.1f s, not settled throughout): holding %s cm (from %s, the last settled row at %s) against %s cm at the last settled upright standing row (%s)' % (
                S.lt(t0), S.lt(t1), t1 - t0, fmt_cm(held), frm, S.lt(L.t[li]) if li >= 0 else '?', fmt_cm(float(ref[i0])), S.lt(L.t[ls]))))
        self.say('  raise_stale: %d episode(s) (not settled, the raise %.0f cm or more from its value at the last settled upright standing row, for %.1f s or more)' % (len(eps), STALE_M * 100, STALE_S))

    def path_axes(self, L):
        """The body's up (unit) per ledger row: the recorded row, or a fit that is printed as an estimate."""
        S = self.S
        pin = S.tab('pin')
        if pin.has('up'):
            u = pin.f('up')[L.R].astype(np.float64)
            nn = np.linalg.norm(u, axis=1)
            if (nn > 0.5).mean() > 0.5:
                with np.errstate(all='ignore'):
                    u = u / nn[:, None]
                return u, nn > 0.5, 'recorded (pin.up)'
        root = pin.f('root')[L.R].astype(np.float64)
        c = S.tab('p1').f('e')[L.pi].astype(np.float64)
        A = c - root
        good = np.isfinite(A).all(axis=1) & np.isfinite(L.e[:, 1])
        if good.sum() < 10:
            return None, None, 'no up row recorded and too few rows to fit'
        x = np.linalg.lstsq(A[good], L.e[good, 1], rcond=None)[0]
        nx = float(np.linalg.norm(x))
        u = x / nx
        ang = math.degrees(math.acos(max(-1.0, min(1.0, float(u[1])))))
        return np.tile(u, (len(L.t), 1)), np.ones(len(L.t), bool), 'ESTIMATE (no up row recorded), a least-squares fit of (p1.e - root) . u = pin.e[1], |u| %.2f, %.0f deg from world Y' % (nx, ang)

    def camera_vs_body(self):
        L, S = self.L, self.S
        self.say('')
        self.say('== what moved the camera against the body (every view_under_neck episode)')
        pin = S.tab('pin')
        u, uok, how = self.path_axes(L)
        if u is None:
            self.say('  cannot evaluate: %s' % how)
            return
        root = pin.f('root')[L.R].astype(np.float64)
        n = len(L.t)
        j0 = np.searchsorted(L.t, L.t - PATH_WINDOW_S, 'left')
        d = root - root[j0]
        dt = np.maximum(L.t - L.t[j0], 1e-3)
        vert = (d * u).sum(axis=1)
        horiz = np.linalg.norm(d - vert[:, None] * u, axis=1)
        speed = horiz / dt
        slope = np.where(uok & (L.t - L.t[j0] > 0.2), np.degrees(np.arctan2(vert, horiz)), np.nan)
        self.say('  body up: %s. Slope and speed are along the path of the model root over the last %.1f s (negative: downhill); world axes are not used for speed.' % (how, PATH_WINDOW_S))
        ik = S.tab('ik')
        lowfoot = pelvis = None
        miss = S.missing({'ik': ['final_l_foot', 'final_r_foot', 'final_pelvis']}) if len(ik) else ['ik']
        if not miss:
            fl = ik.f('final_l_foot')[L.ji][:, 1].astype(np.float64)
            fr = ik.f('final_r_foot')[L.ji][:, 1].astype(np.float64)
            pv = ik.f('final_pelvis')[L.ji][:, 1].astype(np.float64)
            with np.errstate(all='ignore'):
                low = np.where(L.ik_ok, np.fmin(fl, fr), np.nan)
            pelvis = np.where(L.ik_ok, pv, np.nan)
            lowfoot = np.full(n, np.nan)
            for i in range(n):
                a = np.searchsorted(L.t, L.t[i] - FOOT_ROLL_S, 'left')
                w = low[a:i + 1]
                if np.isfinite(w).any():
                    lowfoot[i] = np.nanmin(w)
            if not np.isfinite(lowfoot).any():
                lowfoot = None
        self.feet_verdicts = []
        if not self.vn_eps:
            self.say('  no view_under_neck episode')
        for i0, i1, ip, cause in self.vn_eps:
            t0 = float(L.t[i0])
            pre = np.arange(*sl(L.t, t0 - 1.0, t0 - 1e-9))
            if pre.size == 0:
                pre = np.array([max(i0 - 1, 0)])
            with np.errstate(all='ignore'):
                dE = float(L.e[ip, 1] - np.nanmean(L.e[pre, 1]))
                dneck = float(L.neck_y[ip] - np.nanmean(L.neck_y[pre]))
            a = np.arange(*sl(L.t, t0 - 0.5, t0 - 1e-9))
            b = np.arange(*sl(L.t, t0, t0 + 0.5))
            with np.errstate(all='ignore'):
                p_before = float(np.nanmean(L.pitch[a])) if a.size else float('nan')
                p_after = float(np.nanmean(L.pitch[b])) if b.size else float('nan')
            hw = np.arange(*sl(L.t, t0 - 0.5, t0 + 0.5))
            hmove = float(np.ptp(L.head_y[hw])) if hw.size else float('nan')
            lines = ['E moved %s cm against the 1 s before (E %.2f m at the peak), neck %s cm; path slope %s deg at %.1f m/s; aim pitch %.1f deg before, %.1f after (%s); head moved %.1f mm' % (
                fmt_cm(dE), L.e[ip, 1], fmt_cm(dneck), ('%+.0f' % slope[i0]) if np.isfinite(slope[i0]) else '?', speed[i0], p_before, p_after,
                'changed' if abs(p_after - p_before) > PITCH_CHANGED_DEG else 'unchanged', hmove * 1e3)]
            if lowfoot is None:
                lines.append('cannot evaluate: feet (%s)' % ('ik.final_l_foot, final_r_foot or final_pelvis missing: a recording from before the feet were added' if miss else 'the feet were not recorded: the legs plan was not ok'))
            else:
                with np.errstate(all='ignore'):
                    dfoot = float(lowfoot[ip] - np.nanmean(lowfoot[pre]))
                    dpel = float(pelvis[ip] - np.nanmean(pelvis[pre]))
                if not np.isfinite(dneck) or abs(dneck) >= FEET_NECK_HELD_M:
                    verdict = 'cannot evaluate: the neck moved'   # a stopping step moves the neck and the camera with it: the lowest foot says nothing about the origin there (slope flight, 20:00:14)
                elif not np.isfinite(dfoot) or abs(dE) < 0.02:
                    verdict = 'cannot tell (the feet are not finite there, or E moved under 2 cm)'
                elif dfoot <= 0 and abs(dfoot - dE) <= FOOT_MATCH * abs(dE):
                    verdict = 'model origin rose'
                elif abs(dfoot) <= FOOT_HOLD * abs(dE):
                    verdict = 'camera fell'
                else:
                    verdict = 'mixed: the lowest foot moved %s cm against E %s cm' % (fmt_cm(dfoot), fmt_cm(dE))
                lines.append('lowest foot (%.1f s rolling minimum) %s cm, pelvis %s cm: %s' % (FOOT_ROLL_S, fmt_cm(dfoot), fmt_cm(dpel), verdict))
                self.feet_verdicts.append(verdict)
            self.say('  view_under_neck episode %s (%s):' % (S.lt(t0), cause))
            for ln in lines:
                self.say('    ' + ln)
        # Rows moving along the facing: the ground speed over MOVING_MPS and the path's horizontal part within FACING_COS of the body's forward row (a strafe or a stop is not the lever's).
        facing_note = ''
        if pin.has('fwd'):
            fv = pin.f('fwd')[L.R].astype(np.float64)
            with np.errstate(all='ignore'):
                dh = d - vert[:, None] * u
                fh = fv - (fv * u).sum(axis=1)[:, None] * u
                cosf = (dh * fh).sum(axis=1) / (np.linalg.norm(dh, axis=1) * np.linalg.norm(fh, axis=1))
            facing = np.isfinite(cosf) & (cosf > FACING_COS)
        else:
            facing = np.ones(n, bool)
            facing_note = ' (no pin.fwd recorded: every direction counts)'
        moving = (speed > MOVING_MPS) & facing & np.isfinite(slope) & np.isfinite(L.e[:, 1] - L.neck[:, 1])
        self.say('  E over the animated neck by path slope, moving over %g m/s along the facing (cos over %.2f)%s (%d rows):' % (MOVING_MPS, FACING_COS, facing_note, int(moving.sum())))
        self.lever = self.lever_view = None
        if moving.sum() >= 10:
            eon = (L.e[:, 1] - L.neck[:, 1])[moving]
            sl_ = slope[moving]
            for lo, hi in zip(SLOPE_BINS[:-1], SLOPE_BINS[1:]):
                m = (sl_ >= lo) & (sl_ < hi)
                if m.sum():
                    self.say('    slope %+4.0f to %+4.0f deg: %4d rows, E over neck %s cm (sd %.1f)' % (lo, hi, m.sum(), fmt_cm(float(eon[m].mean())), 100 * float(eon[m].std())))
            tan = np.tan(np.radians(sl_))
            self.lever = self.say_lever('E over neck', tan, eon)
            if pin.has('f214_state'):
                self.lever_view = self.say_lever('view over neck', tan, (L.body - L.neck[:, 1])[moving])
        else:
            self.say('    too few rows')

    def say_lever(self, what, tan, y):
        """Fit y = a + b tan(slope) over the rows, say it, and return dict(a, b, r, n) (None when the slope did not vary)."""
        if len(y) < 10 or not (np.std(tan) > 0 and np.std(y) > 0):
            self.say('    %s = a + b tan(slope): not fitted (%d rows, no spread)' % (what, len(y)))
            return None
        b, a = np.polyfit(tan, y, 1)
        r = float(np.corrcoef(tan, y)[0, 1])
        self.say('    %s = %+.3f m + %.3f m x tan(slope), correlation %+.2f, over %d rows' % (what, a, b, r, len(y)))
        return dict(a=float(a), b=float(b), r=r, n=int(len(y)))

    # -- F214: the raise follows the neck while the body moves -------------------------------------------------------------------------------------------------------------------
    def f214_section(self):
        L, S = self.L, self.S
        self.say('')
        self.say('== view follows body while moving (F214)')
        self.f214 = None
        if not self.need('F214 section', REQ_F214):
            self.say('  cannot evaluate: pin.f214_state missing (a recording from before F214)')
            return
        pin = S.tab('pin')
        if not len(pin):
            self.say('  no pin records')
            return
        st = pin.f('f214_state').astype(int)
        names = S.enums.get('F214State', []) if hasattr(S, 'enums') else []
        cnt = np.bincount(st, minlength=max(len(names), int(st.max()) + 1))
        self.say('  pin calls by state: ' + ', '.join('%s %d' % (S.enum_name('F214State', i), int(c)) for i, c in enumerate(cnt) if c) + ' (%d calls)' % len(st))
        fol = S.enum('F214State', 'followed')
        if L is None:
            self.say('  no height ledger: the followed view is not measured')
            return
        m = pin.f('f214_state')[L.R].astype(int) == fol
        self.f214 = dict(followed=int(m.sum()), err_cm=None, bob_mm=None)
        if m.sum() < 10:
            self.say('  followed rows in the ledger: %d, too few to measure' % int(m.sum()))
            return
        try:
            eye_cm = float(S.keys.get('fix.native_on_foot_eye_height_cm', EYE_HEIGHT_DEFAULT_CM))
        except (TypeError, ValueError):
            eye_cm = EYE_HEIGHT_DEFAULT_CM
        if not 5.0 <= eye_cm <= 40.0:
            eye_cm = EYE_HEIGHT_DEFAULT_CM
        raise_ = S.tab('align').f('raise_m')[L.ai].astype(np.float64) if self.S.tab('align').has('raise_m') else None
        err_cm, bob_mm = f214_measure(L.t, L.body, L.neck[:, 1], raise_, m, eye_cm * 0.01)
        self.f214.update(err_cm=err_cm, bob_mm=bob_mm)
        self.say('  followed rows %d: the view against neck + %.1f cm eye height, mean |error| %.1f cm; the raise\'s own bob (rms after a %.1f s moving mean) %s' % (
            int(m.sum()), eye_cm, err_cm, F214_BOB_WINDOW_S, ('%.2f mm' % bob_mm) if bob_mm is not None else 'n/a (no align.raise_m)'))
        t0 = float(L.t[m][0])
        if err_cm > F214_ERR_CM:
            self.findings.append(Finding(t0, float(L.t[m][-1]), 'f214_view_error', 'follow', 'F214: where the follow ran the view was off neck + %.1f cm by %.1f cm on average, limit %.1f cm (%d rows)' % (
                eye_cm, err_cm, F214_ERR_CM, int(m.sum()))))
        if bob_mm is not None and bob_mm > F214_BOB_MM:
            self.findings.append(Finding(t0, float(L.t[m][-1]), 'f214_bob', 'follow', 'F214: where the follow ran the raise\'s own bob was %.2f mm rms, limit %.1f mm (%d rows)' % (bob_mm, F214_BOB_MM, int(m.sum()))))

    # -- the body's frame, shared by the F215 and F216 sections ---------------------------------------------------------------------------------------------------------------------
    def body_rows(self):
        """Per ledger row: the body's up (unit), the root, the facing's plane part (unit, along the plane of up), the plane step of the root from the row before and the path length so far
        (steps over 1 m or across a gap of 0.25 s or more count as 0), and the unarmed upright rows outside the sights. None without up, fwd and root."""
        L, S = self.L, self.S
        pin = S.tab('pin')
        if not (pin.has('root') and pin.has('fwd')):
            return None
        u, uok, how = self.path_axes(L)
        if u is None:
            return None
        root = pin.f('root')[L.R].astype(np.float64)
        fv = pin.f('fwd')[L.R].astype(np.float64)
        with np.errstate(all='ignore'):
            fh = fv - (fv * u).sum(axis=1)[:, None] * u
            fh = fh / np.linalg.norm(fh, axis=1)[:, None]
        n = len(L.t)
        step = np.zeros(n)
        if n > 1:
            d = root[1:] - root[:-1]
            dh = d - (d * u[1:]).sum(axis=1)[:, None] * u[1:]
            sp = np.linalg.norm(dh, axis=1)
            ok = np.isfinite(sp) & (sp < 1.0) & (np.diff(L.t) < 0.25)
            step[1:] = np.where(ok, sp, 0.0)
        upright = (L.stance == S.enum('Stance', 'standing')) | (L.stance == S.enum('Stance', 'moving'))
        free = (L.armed == 0) & upright & ~L.ads & uok & np.isfinite(fh).all(axis=1)
        return dict(u=u, root=root, fh=fh, step=step, path=np.cumsum(step), free=free)

    def trail_mean(self, v, w):
        """The mean of v over the rows in (t - w, t]; NaN rows are left out."""
        t = self.L.t
        f = np.isfinite(v)
        c = np.r_[0.0, np.cumsum(np.where(f, v, 0.0))]
        k = np.r_[0, np.cumsum(f)]
        lo = np.searchsorted(t, t - w, 'right')
        hi = np.arange(len(t)) + 1
        cnt = k[hi] - k[lo]
        return np.where(cnt > 0, (c[hi] - c[lo]) / np.maximum(cnt, 1), np.nan)

    # -- F217: the body follows the view with an item at the hip ----------------------------------------------------------------------------------------------------------------------
    def f217_item(self, pin, g, j0, tan, path):
        """Seconds of pin calls in active_item and held_sights; E (true, pin.eye_y_selected) against the level height on level ground (|tan slope| <= F215_LEVEL_TAN over the last
        F215_LEVEL_PATH_M of path) by item serial, a finding when it is further than F217_LEVEL_TOL_CM from F217_LEVEL_CAM_M; over the hip rows with an item, the seconds and the largest figure of the
        higher hand over the view and of the view under the neck (the IK's final joints against the body view: the internal terms the section uses). A recording without the state is a baseline."""
        L, S = self.L, self.S
        ai, hs = S.enum('F215State', 'active_item'), S.enum('F215State', 'held_sights')
        if ai < 0:
            self.say('  item at the hip: the recording\'s F215 state has no active_item (a recording from before F217): the body was not moved with an item, and nothing here is a finding')
            return
        st = pin.f('f215_state').astype(int)
        tp = S.t('pin')
        dtp = np.zeros(len(tp))
        if len(tp) > 1:
            d_ = np.diff(tp)
            dtp[1:] = np.where((d_ > 0) & (d_ < 0.25), d_, 0.0)
        sec_ai, sec_hs = float(dtp[st == ai].sum()), float(dtp[st == hs].sum())
        self.say('  item at the hip (F217): %.1f s of pin calls in active_item, %.1f s held in the sights' % (sec_ai, sec_hs))
        self.f215['item_active_s'], self.f215['item_sights_s'] = sec_ai, sec_hs
        rows_ai = st[L.R] == ai
        if pin.has('eye_y_selected') and rows_ai.any():
            ey = pin.f('eye_y_selected')[L.R].astype(np.float64)
            lvl = rows_ai & (path - path[j0] >= 0.9 * F215_LEVEL_PATH_M) & (L.t - L.t[j0] < 3.0) & (tan <= F215_LEVEL_TAN) & np.isfinite(ey)
            self.f215['item_level'] = {}
            for ser in np.unique(L.item[lvl]):
                m = lvl & (L.item == ser)
                if int(m.sum()) < F215_LEVEL_MIN_ROWS:
                    self.say('    item %d: %d level rows with the item at the hip, too few (need %d)' % (int(ser), int(m.sum()), F215_LEVEL_MIN_ROWS))
                    continue
                med = float(np.median(ey[m]))
                self.f215['item_level'][int(ser)] = med
                self.say('    item %d: E (true) on level ground median %.3f m over %d rows, the level height %.2f m (%+.1f cm)' % (int(ser), med, int(m.sum()), F217_LEVEL_CAM_M, 100.0 * (med - F217_LEVEL_CAM_M)))
                if abs(med - F217_LEVEL_CAM_M) * 100.0 > F217_LEVEL_TOL_CM:
                    self.findings.append(Finding(float(L.t[m][0]), float(L.t[m][-1]), 'f217_item_level', 'constant', 'F217: on level ground with item %d at the hip E stood %.3f m, not the level height %.2f m '
                                                 '(off by %.1f cm, limit %.1f; %d rows)' % (int(ser), med, F217_LEVEL_CAM_M, 100.0 * abs(med - F217_LEVEL_CAM_M), F217_LEVEL_TOL_CM, int(m.sum()))))
            if not self.f215['item_level']:
                self.say('    no level rows with an item at the hip (the last %.1f m of path flat): E against the level height is not measured' % F215_LEVEL_PATH_M)
        hand_y, neck_y = getattr(L, 'hand_y', None), getattr(L, 'neck_y', None)
        if hand_y is None:
            self.say('    hip rows: the IK record is missing, the hands and the neck are not measured')
            return
        hip = (L.armed == 1) & ~L.ads & np.isfinite(hand_y) & np.isfinite(neck_y)
        if not hip.any():
            self.say('    hip rows with an item and a final neck and hands: none')
            return
        dtl = np.zeros(len(L.t))
        if len(L.t) > 1:
            d_ = np.diff(L.t)
            dtl[1:] = np.where((d_ > 0) & (d_ < 0.25), d_, 0.0)
        hv, vn = hand_y - L.body, neck_y - L.body
        self.f215['item_hand_over_view_cm'] = float(np.max(hv[hip])) * 100.0
        self.f215['item_view_under_neck_cm'] = float(np.max(vn[hip])) * 100.0
        self.say('    hip rows with an item (not in the sights): %.1f s; the higher hand over the view up to %+.1f cm, the view under the neck up to %+.1f cm' % (
            float(dtl[hip].sum()), self.f215['item_hand_over_view_cm'], self.f215['item_view_under_neck_cm']))

    # -- F226: the head's pitch no longer holds y; the held stretches that leave y far from the rule's ask ---------------------------------------------------------------------------
    def f226_y(self, pin):
        """The whole-body shift less the root jump and the lead (what F215's own y was), per pin row."""
        by = pin.f('f215_body_y').astype(np.float64)
        if pin.has('f223_pop_m'):
            by = by - pin.f('f223_pop_m').astype(np.float64)
        if pin.has('f224_lead_m'):
            by = by - pin.f('f224_lead_m').astype(np.float64)
        return by

    def f226_head_line(self, pin):
        """Calls with the head past the gate and the move on (pin.f226_head_pitched): the count and the share of the on-foot calls, and standing still in those rows (the root's plane speed under
        F226_STILL_MPS over F226_SPEED_S) y's step a call as median, 99th percentile and largest, in mm."""
        S = self.S
        self.f215['head'] = None
        if not pin.has('f226_head_pitched'):
            self.say('  cannot evaluate head past the gate: pin.f226_head_pitched missing (a recording from before F226: the head held y then, see held_head above)')
            return
        hp = pin.f('f226_head_pitched') == 1
        n = len(hp)
        self.f215['head'] = dict(n=int(hp.sum()), share=float(hp.mean()) if n else 0.0, still=0, med=None, p99=None, mx=None)
        txt = '  head past the gate with the move on (pin.f226_head_pitched): %d of %d on-foot calls (%.1f%%)' % (int(hp.sum()), n, 100.0 * self.f215['head']['share'])
        if not pin.has('root') or not pin.has('f215_body_y'):
            self.say(txt + '; cannot evaluate the still rows: pin.root or pin.f215_body_y missing')
            return
        t = S.t('pin')
        root = pin.f('root').astype(np.float64)
        with np.errstate(all='ignore'):
            j0 = np.searchsorted(t, t - F226_SPEED_S, 'left')
            d = root - root[j0]
            if pin.has('up'):
                up = pin.f('up').astype(np.float64)
                nn = np.linalg.norm(up, axis=1)
                up = np.where((nn > 0.5)[:, None], up / np.maximum(nn, 1e-9)[:, None], np.array([0.0, 1.0, 0.0]))
                d = d - (d * up).sum(axis=1)[:, None] * up
            span = t - t[j0]
            speed = np.where(span >= 0.5 * F226_SPEED_S, np.linalg.norm(d, axis=1) / np.maximum(span, 1e-3), np.nan)
            y = self.f226_y(pin)
            step = np.full(n, np.nan)
            step[1:] = np.abs(np.diff(y)) * 1000.0
            m = hp & (speed < F226_STILL_MPS) & np.isfinite(step)
        if m.sum() == 0:
            self.say(txt + '; no row standing still (root speed under %g m/s) with the head past the gate' % F226_STILL_MPS)
            return
        h = self.f215['head']
        h.update(still=int(m.sum()), med=float(np.median(step[m])), p99=float(np.percentile(step[m], 99)), mx=float(step[m].max()))
        self.say(txt + '; standing still in those rows (%d, root speed under %g m/s) y steps median %.2f mm, 99th percentile %.2f mm, largest %.2f mm a call' % (h['still'], F226_STILL_MPS, h['med'], h['p99'], h['mx']))

    @staticmethod
    def f226_trail_mean(t, v, w):
        """The mean of v over the rows in (t - w, t]; NaN rows are left out."""
        f = np.isfinite(v)
        c = np.r_[0.0, np.cumsum(np.where(f, v, 0.0))]
        k = np.r_[0, np.cumsum(f)]
        lo = np.searchsorted(t, t - w, 'right')
        hi = np.arange(len(t)) + 1
        cnt = k[hi] - k[lo]
        with np.errstate(all='ignore'):
            return np.where(cnt > 0, (c[hi] - c[lo]) / np.maximum(cnt, 1), np.nan)

    def f226_held_far(self, pin, st):
        """Finding shift_held_far: a stretch of held_aim, held_pose or held_head (the last in recordings from before F226) lasting F226_HELD_FAR_S or more in which y, the whole-body shift less the root
        jump and the lead, is F226_HELD_FAR_M or more from what the rule would ask (median over the stretch). Armed (pin.armed) the rule asks eye_y_selected - f223_pop_m - level, level the median
        of eye_y_selected - f215_target over the active_item rows (F226_LEVEL_DEFAULT_M with fewer than 30); unarmed eye_y_selected - f223_pop_m - the F215_NECK_MEAN_S mean of neck_anim y less the
        constant. The sights are not judged."""
        S = self.S
        self.say('  shift_held_far: stretches of held_aim, held_pose or held_head (old recordings) of %g s or more with y %g cm or more from what the rule would ask; the sights are not judged' % (F226_HELD_FAR_S, 100.0 * F226_HELD_FAR_M))
        miss = [('pin.' + f) for f in ('f215_body_y', 'eye_y_selected', 'neck_anim', 'armed', 'f215_target') if not pin.has(f)]
        if miss:
            self.say('    cannot evaluate: %s' % ', '.join(miss))
            return
        names = [nm for nm in ('held_aim', 'held_pose', 'held_head') if S.enum('F215State', nm) >= 0]
        codes = [S.enum('F215State', nm) for nm in names]
        t = S.t('pin')
        held = np.isin(st, codes)
        y = self.f226_y(pin)
        e = pin.f('eye_y_selected').astype(np.float64)
        if pin.has('f223_pop_m'):
            e = e - pin.f('f223_pop_m').astype(np.float64)
        ai = S.enum('F215State', 'active_item')
        lv = (st == ai) & np.isfinite(pin.f('eye_y_selected')) & np.isfinite(pin.f('f215_target'))
        level = float(np.median(pin.f('eye_y_selected').astype(np.float64)[lv] - pin.f('f215_target').astype(np.float64)[lv])) if int(lv.sum()) >= 30 else F226_LEVEL_DEFAULT_M
        nm = self.f226_trail_mean(t, pin.f('neck_anim').astype(np.float64)[:, 1], F215_NECK_MEAN_S)
        armed = pin.f('armed') == 1
        ask = np.where(armed, e - level, e - nm - F215_CONST_CM * 0.01)
        self.f215['held_far'] = []
        n = len(t)
        i = 0
        while i < n:
            if not held[i]:
                i += 1
                continue
            j = i
            while j + 1 < n and held[j + 1] and t[j + 1] - t[j] < 0.1:
                j += 1
            dur = float(t[j] - t[i])
            if dur >= F226_HELD_FAR_S:
                gap = y[i:j + 1] - ask[i:j + 1]
                gap = gap[np.isfinite(gap)]
                if len(gap) and abs(float(np.median(gap))) >= F226_HELD_FAR_M:
                    ymed = float(np.median(y[i:j + 1]))
                    stn = ', '.join(sorted(set(S.enum_name('F215State', int(c)) for c in st[i:j + 1])))
                    self.f215['held_far'].append(dict(t=float(t[i]), dur=dur, y=ymed, gap=float(np.median(gap))))
                    self.findings.append(Finding(t[i], t[j], 'shift_held_far', 'held', 'F226: the whole-body shift held %.2f s (%s, %s) with y %+.2f m, the median %.2f m from what the rule would ask (limit %.2f)' % (
                        dur, stn, 'armed' if bool(armed[i]) else 'unarmed', ymed, abs(float(np.median(gap))), F226_HELD_FAR_M)))
            i = j + 1

    # -- F215: the body follows the view ---------------------------------------------------------------------------------------------------------------------------------------------
    def f215_section(self):
        L, S = self.L, self.S
        self.say('')
        self.say('== body follows view (F215)')
        self.f215 = None
        pin = S.tab('pin')
        if not len(pin):
            self.say('  no pin records')
            return
        have_state = self.need('F215 section', REQ_F215)
        self.f215 = dict(active=0, y_mean_cm=None, y_max_cm=None, level_rows=0, e_over_neck_cm=None, turns=[], turn_move_cm=None)
        if have_state:
            st = pin.f('f215_state').astype(int)
            cnt = np.bincount(st, minlength=max(len(S.enums.get('F215State', [])), int(st.max()) + 1))
            self.say('  pin calls by state: ' + ', '.join('%s %d' % (S.enum_name('F215State', i), int(c)) for i, c in enumerate(cnt) if c) + ' (%d calls)' % len(st))
            by = pin.f('f215_body_y').astype(np.float64)
            am = (st == S.enum('F215State', 'active')) & np.isfinite(by)
            self.f215['active'] = int(am.sum())
            if am.any():
                self.f215.update(y_mean_cm=float(np.mean(np.abs(by[am])) * 100.0), y_max_cm=float(np.max(np.abs(by[am])) * 100.0))
                self.say('  active calls %d: the body moved |y| mean %.1f cm, max %.1f cm (signed mean %+.1f cm)' % (int(am.sum()), self.f215['y_mean_cm'], self.f215['y_max_cm'], float(np.mean(by[am])) * 100.0))
            else:
                self.say('  no active call: the rule did not run (the key off, or every call held or released: the states above say which)')
            self.f226_head_line(pin)
            self.f226_held_far(pin, st)
        else:
            self.say('  pin.f215_state missing: a recording from before F215, the body not shifted. The level rows and the turns below are its baseline, and nothing here is a finding')
        if L is None:
            self.say('  no height ledger: the level rows and the turns are not measured')
            return
        g = self.body_rows()
        if g is None:
            self.say('  cannot evaluate: pin.root, pin.fwd or the body\'s up missing (a recording from before the path fields)')
            return
        try:
            eye_cm = float(S.keys.get('fix.native_on_foot_eye_height_cm', EYE_HEIGHT_DEFAULT_CM))
        except (TypeError, ValueError):
            eye_cm = EYE_HEIGHT_DEFAULT_CM
        if not 5.0 <= eye_cm <= 40.0:
            eye_cm = EYE_HEIGHT_DEFAULT_CM
        n = len(L.t)
        u, root, fh, path, free = g['u'], g['root'], g['fh'], g['path'], g['free']
        # the level rows: the last 0.5 m of the root's path in the plane, the facing along it and steady, the slope flat
        j0 = np.searchsorted(path, path - F215_LEVEL_PATH_M, 'left')
        with np.errstate(all='ignore'):
            d = root - root[j0]
            vert = (d * u).sum(axis=1)
            dh = d - vert[:, None] * u
            hn = np.linalg.norm(dh, axis=1)
            cosp = (dh * fh).sum(axis=1) / hn
            steady = (fh * fh[j0]).sum(axis=1)
            tan = np.abs(vert) / hn
        level = free & (path - path[j0] >= 0.9 * F215_LEVEL_PATH_M) & (L.t - L.t[j0] < 3.0) & (cosp > F215_LEVEL_COS) & (steady > F215_STEADY_DOT) & (tan <= F215_LEVEL_TAN)
        nm = self.trail_mean(L.neck[:, 1], F215_NECK_MEAN_S)
        eon = L.e[:, 1] - nm
        lv = level & np.isfinite(eon)
        self.f215['level_rows'] = int(lv.sum())
        r0 = eye_cm - F215_CONST_CM
        if lv.sum() >= F215_LEVEL_MIN_ROWS:
            med = float(np.median(eon[lv])) * 100.0
            need = eye_cm - med
            self.f215['e_over_neck_cm'] = med
            view_over = float(np.mean((L.body - L.neck[:, 1])[lv])) * 100.0
            self.say('  level rows (last %.1f m of path, facing along it cos > %.2f and steady > %.2f, |tan slope| <= %.2f, unarmed, upright; %d rows): E over the %.1f s mean neck median %+.1f cm (the constant %.1f); '
                     'the raise that puts the neck at the eye height is %+.1f cm, R0 = %.1f - %.1f = %+.1f cm; the view over the animated neck there %+.1f cm (eye height %.1f)' % (
                         F215_LEVEL_PATH_M, F215_LEVEL_COS, F215_STEADY_DOT, F215_LEVEL_TAN, int(lv.sum()), F215_NECK_MEAN_S, med, F215_CONST_CM, need, eye_cm, F215_CONST_CM, r0, view_over, eye_cm))
            if abs(med - F215_CONST_CM) > F215_R0_CM:
                self.findings.append(Finding(float(L.t[lv][0]), float(L.t[lv][-1]), 'f215_level_constant', 'constant', 'F215: on level ground E stood %+.1f cm over the %.1f s mean neck, not the constant %.1f cm '
                                             '(off by %.1f cm, limit %.1f): R0 is %+.1f cm and the needed raise %+.1f cm (%d rows)' % (med, F215_NECK_MEAN_S, F215_CONST_CM, abs(med - F215_CONST_CM), F215_R0_CM, r0, need, int(lv.sum()))))
        else:
            self.say('  level rows: %d, too few (need %d) to compare R0 with the measured E over the mean neck' % (int(lv.sum()), F215_LEVEL_MIN_ROWS))
        if have_state:
            self.f217_item(pin, g, j0, tan, path)
        # every in-place turn: the heading changes over F215_TURN_DEG within F215_TURN_S, the root nearly still for F215_TURN_SIDE_S each side
        ang = np.degrees(np.unwrap(np.arctan2(np.nan_to_num(fh[:, 0]), np.nan_to_num(fh[:, 2]))))
        jb = np.searchsorted(L.t, L.t - F215_TURN_S, 'left')
        turning = free & (np.abs(ang - ang[jb]) > F215_TURN_DEG) & (L.t - L.t[jb] <= 1.5 * F215_TURN_S)
        rooth = (root * u).sum(axis=1)
        camh = rooth + L.e[:, 1]
        viewh = rooth + L.body + L.by   # the view in the world: L.body is the view against the body, so the body's shift goes back on
        # F217: the pilot's own head is not the camera's or the view's move: align.head_local y (the change since the turn's start) comes out of both (the ledger's body view has HEAD_GAIN of it out already)
        hy_ = S.tab('align').f('head_local')[L.ai][:, 7].astype(np.float64) if S.tab('align').has('head_local') else np.zeros(n)

        def plane_speed(ia, ib):
            if ib <= ia or L.t[ib] - L.t[ia] <= 0.0:
                return float('nan')
            dd = root[ib] - root[ia]
            dd = dd - float(dd @ u[ib]) * u[ib]
            return float(np.linalg.norm(dd) / (L.t[ib] - L.t[ia]))
        turns = []
        for r0_, r1_ in runs_of(turning):
            ta, tb = float(L.t[r0_]) - F215_TURN_S, float(L.t[r1_])
            ia, ib = int(np.searchsorted(L.t, ta, 'left')), r1_
            ib0 = int(np.searchsorted(L.t, ta - F215_TURN_SIDE_S, 'left'))
            ic = int(np.searchsorted(L.t, tb + F215_TURN_SIDE_S, 'right')) - 1
            if ib0 >= ia or ic <= ib or L.t[ib0] > ta - 0.8 * F215_TURN_SIDE_S or L.t[ic] < tb + 0.8 * F215_TURN_SIDE_S:
                continue
            before, after = plane_speed(ib0, ia), plane_speed(ib, ic)
            if not (before < F215_TURN_SIDE_MPS and after < F215_TURN_SIDE_MPS):
                continue
            k = np.arange(ia, ib + 1)
            if k.size < 6:
                continue
            fx, fz = fh[k, 0], fh[k, 2]
            A = np.zeros((2 * k.size, 3))
            b = np.zeros(2 * k.size)
            A[0::2, 0], A[0::2, 1], b[0::2] = -fx, 1.0, root[k, 0]
            A[1::2, 0], A[1::2, 2], b[1::2] = -fz, 1.0, root[k, 2]
            sol = np.linalg.lstsq(A, b, rcond=None)[0]
            dist = float(sol[0])   # the pivot ahead of the root along the facing
            dang = float(ang[ib] - ang[ia])
            ie = int(np.searchsorted(L.t, tb + F215_TURN_AFTER_S, 'right')) - 1
            away = root[ib:ie + 1] - root[ib]
            away = away - (away * u[ib:ie + 1]).sum(axis=1)[:, None] * u[ib:ie + 1]
            left = np.nonzero(np.linalg.norm(away, axis=1) > F215_TURN_LEFT_M)[0]
            if left.size:
                ie = ib + int(left[0]) - 1
            dh_ = hy_[ia:ie + 1] - hy_[ia]
            dcam = float(np.nanmax(np.abs((camh[ia:ie + 1] - camh[ia]) - dh_)))
            droot = float(np.max(np.abs(rooth[ia:ie + 1] - rooth[ia])))
            dview = float(np.nanmax(np.abs((viewh[ia:ie + 1] - viewh[ia]) - (1.0 - HEAD_GAIN) * dh_)))
            turns.append(dict(t0=float(L.t[ia]), t1=float(L.t[ib]), dang=dang, pivot_cm=dist * 100.0, cam_cm=dcam * 100.0, root_cm=droot * 100.0, view_cm=dview * 100.0,
                              judged=bool(dcam * 100.0 < F215_TURN_CAM_JUDGE_CM)))
        self.f215['turns'] = turns
        if not turns:
            self.say('  in-place turns (heading over %.0f deg in %.2f s, the root under %.1f m/s for %.1f s each side): none' % (F215_TURN_DEG, F215_TURN_S, F215_TURN_SIDE_MPS, F215_TURN_SIDE_S))
            return
        self.say('  in-place turns (heading over %.0f deg in %.2f s, the root under %.1f m/s for %.1f s each side): %d. Per turn: heading change, the pivot ahead of the root along the facing, and the largest world height moves '
                 'from its start to %.1f s after it of the camera (E), the root and the view (height = root . up + the model height):' % (
                     F215_TURN_DEG, F215_TURN_S, F215_TURN_SIDE_MPS, F215_TURN_SIDE_S, len(turns), F215_TURN_AFTER_S))
        self.say('  (the physical head, align.head_local y since the turn\'s start, is out of the camera and view figures; a turn is judged only where the camera moved under %.0f cm)' % F215_TURN_CAM_JUDGE_CM)
        for tr in turns[:20]:
            self.say('    %s to %s: %+.0f deg, pivot %.1f cm, camera %.1f cm, root %.1f cm, view %.1f cm%s' % (S.lt(tr['t0']), S.lt(tr['t1']), tr['dang'], tr['pivot_cm'], tr['cam_cm'], tr['root_cm'], tr['view_cm'],
                                                                                                   '' if tr['judged'] else '; camera moved: not judged'))
        if len(turns) > 20:
            self.say('    ... %d more' % (len(turns) - 20))
        jt = [tr for tr in turns if tr['judged']]
        if not jt:
            self.say('  no turn qualifies (the camera, the physical head out, moved %.0f cm or more in each): the view across turns is not judged' % F215_TURN_CAM_JUDGE_CM)
            return
        med_view = float(np.median([tr['view_cm'] for tr in jt]))
        self.f215['turn_move_cm'] = med_view
        self.say('  median over the %d judged turn(s): pivot %.1f cm, camera %.1f cm, root %.1f cm, view %.1f cm' % (len(jt), float(np.median([tr['pivot_cm'] for tr in jt])), float(np.nanmedian([tr['cam_cm'] for tr in jt])),
                                                                                                                  float(np.median([tr['root_cm'] for tr in jt])), med_view))
        if have_state and med_view > F215_TURN_MOVE_CM:
            self.findings.append(Finding(jt[0]['t0'], jt[-1]['t1'], 'f215_turn_view_move', 'turn', 'F215: in %d judged in-place turn(s) the view moved a median %.1f cm in the world (camera %.1f cm, root %.1f cm), limit %.1f cm' % (
                len(jt), med_view, float(np.nanmedian([tr['cam_cm'] for tr in jt])), float(np.median([tr['root_cm'] for tr in jt])), F215_TURN_MOVE_CM)))

    # -- F216: the view bob --------------------------------------------------------------------------------------------------------------------------------------------------------------
    @staticmethod
    def band_rms(t, x, lo_hz, hi_hz):
        """The rms of x in [lo_hz, hi_hz]: resampled to a uniform grid over its span, a cubic detrended, Hann windowed, the band's power by Parseval (window gain corrected). x in the unit it came in."""
        n = len(t)
        span = float(t[-1] - t[0])
        if n < 16 or span <= 0 or not np.isfinite(x).all():
            return float('nan')
        tu = np.linspace(t[0], t[-1], n)
        xu = np.interp(tu, t, x)
        s = np.linspace(-1.0, 1.0, n)
        xu = xu - np.polyval(np.polyfit(s, xu, 3), s)
        w = np.hanning(n)
        X = np.fft.rfft(xu * w)
        fs = (n - 1) / span
        f = np.fft.rfftfreq(n, 1.0 / fs)
        band = (f >= lo_hz) & (f <= hi_hz)
        p = 2.0 * float((np.abs(X[band]) ** 2).sum()) / (n * n * float(np.mean(w * w)))
        return math.sqrt(max(p, 0.0))

    def f216_section(self):
        L, S = self.L, self.S
        self.say('')
        self.say("== view bob (F216; the pilot's own head, align.head_local x y z as right, up, -forward, is taken out of the camera and the view before the band figures, so it does not count as bob)")
        self.f216 = None
        g = self.body_rows()
        if g is None:
            self.say('  cannot evaluate: pin.root, pin.fwd or the body\'s up missing')
            return
        p1, al = S.tab('p1'), S.tab('align')
        if not p1.has('e'):
            self.say('  cannot evaluate: p1.e missing')
            return
        have_bob = al.has('bob_seated')
        have_hud = p1.has('hud_shift')
        clamp_m = F216_CLAMP_M if have_hud else F216_CLAMP_OLD_M
        key = str(S.keys.get('fix.native_on_foot_view_bob', 'off'))
        u, root, fh, path, free = g['u'], g['root'], g['fh'], g['path'], g['free']
        n = len(L.t)
        cam = p1.f('e')[L.pi].astype(np.float64)
        with np.errstate(all='ignore'):
            right = np.cross(u, fh)
        bob = al.f('bob_seated')[L.ai].astype(np.float64) if have_bob else np.zeros((n, 3))
        # the view = the camera + the bob term the pose carried: the camera moves by (-S0, -S1, +S2) in (right, up, forward) for a seated term S
        # p1.e is in the world: side and along are taken against the body's origin, because a world position hundreds of metres out times the facing's jitter reads as centimetres of sway
        cam_up, cam_side, cam_along = (cam * u).sum(axis=1), ((cam - root) * right).sum(axis=1), ((cam - root) * fh).sum(axis=1)
        if al.has("head_local"):   # the physical head reaches the camera at a gain of about 1: it is the pilot, not the bob
            hl = al.f("head_local")[L.ai].astype(np.float64)
            cam_side, cam_up, cam_along = cam_side - hl[:, 3], cam_up - hl[:, 7], cam_along + hl[:, 11]
        view_up, view_side, view_along = cam_up - bob[:, 1], cam_side - bob[:, 0], cam_along + bob[:, 2]
        j0 = np.searchsorted(L.t, L.t - PATH_WINDOW_S, 'left')
        with np.errstate(all='ignore'):
            d = root - root[j0]
            vert = (d * u).sum(axis=1)
            dh = d - vert[:, None] * u
            speed = np.linalg.norm(dh, axis=1) / np.maximum(L.t - L.t[j0], 1e-3)
        run = free & (speed >= F216_MIN_MPS) & (L.t - L.t[j0] > 0.2) & np.isfinite(cam_up)
        rows = collections.defaultdict(list)
        stretches = 0
        for r0_, r1_ in runs_of(run):
            i = r0_
            while i + F216_STRETCH_ROWS - 1 <= r1_:
                k = np.arange(i, i + F216_STRETCH_ROWS)
                if np.max(np.diff(L.t[k])) >= 0.05:
                    i += 1
                    continue
                if np.min((fh[k] * fh[k[0]]).sum(axis=1)) <= F216_FACING_DOT:
                    i += F216_STRETCH_ROWS // 4
                    continue
                dd = root[k[-1]] - root[k[0]]
                vv = float(dd @ u[k[0]])
                hh = float(np.linalg.norm(dd - vv * u[k[0]]))
                slope = abs(vv) / hh if hh > 1e-6 else float('nan')
                if not np.isfinite(slope):
                    i += F216_STRETCH_ROWS
                    continue
                sp = float(np.mean(speed[k]))
                tt = L.t[k]
                b = (0 if sp < F216_SPEED_SPLIT_MPS else 1, 0 if slope < F216_SLOPE_SPLIT else 1)
                clampshare = float(np.mean(np.max(np.abs(bob[k]), axis=1) >= clamp_m - 1e-4))
                rows[b].append(dict(cam=self.band_rms(tt, cam_up[k], *F216_BAND_HZ), view=self.band_rms(tt, view_up[k], *F216_BAND_HZ), side=self.band_rms(tt, view_side[k], *F216_SIDE_BAND_HZ), cam_side=self.band_rms(tt, cam_side[k], *F216_SIDE_BAND_HZ),
                                    along=self.band_rms(tt, view_along[k], *F216_BAND_HZ), term=float(np.std(bob[k, 1])), term_low=self.band_rms(tt, bob[k, 1], *F216_LOW_BAND_HZ),
                                    term_band=self.band_rms(tt, bob[k, 1], *F216_BAND_HZ), clamp=clampshare, t0=float(tt[0]), t1=float(tt[-1])))
                stretches += 1
                i += F216_STRETCH_ROWS
        self.f216 = dict(stretches=stretches, bins={})
        if not have_bob:
            self.say('  align.bob_seated missing (a recording from before F216): the view is taken as the camera, nothing removed')
        self.say('  key fix.native_on_foot_view_bob is %s in the recording\'s header (off removes the bob); %d straight stretch(es) of %d rows (facing steady > %.3f, unarmed, upright, outside the sights, plane speed >= %g m/s)' % (
            key, stretches, F216_STRETCH_ROWS, F216_FACING_DOT, F216_MIN_MPS))
        names = {(0, 0): 'under %g m/s, level' % F216_SPEED_SPLIT_MPS, (0, 1): 'under %g m/s, slope' % F216_SPEED_SPLIT_MPS, (1, 0): '%g m/s and over, level' % F216_SPEED_SPLIT_MPS,
                 (1, 1): '%g m/s and over, slope' % F216_SPEED_SPLIT_MPS}
        if have_hud:   # F220: the move of the HUD with the view the placement was given, from the P1 record of each row (a recording without the field prints nothing)
            hs = p1.f('hud_shift')[L.pi].astype(np.float64)
            fin = np.isfinite(hs).all(axis=1)
            nz = np.any(hs != 0.0, axis=1) & fin
            rr_ = run & fin
            up_mm = float(np.sqrt(np.mean(hs[rr_, 1] ** 2))) * 1000.0 if rr_.any() else float('nan')
            side_mm = float(np.sqrt(np.mean(hs[rr_, 0] ** 2))) * 1000.0 if rr_.any() else float('nan')
            share = float(np.mean(nz)) if n else 0.0
            self.f216['hud'] = dict(up_mm=up_mm, side_mm=side_mm, share=share, run_rows=int(rr_.sum()), rows=int(n))
            self.say('  HUD shift (F220, p1.hud_shift; key experimental.native_on_foot_hud_stays_with_view is %s): over the %d run rows up rms %.2f mm, side rms %.2f mm; non-zero on %.1f%% of the %d rows on foot' % (
                S.keys.get('experimental.native_on_foot_hud_stays_with_view', 'on'), int(rr_.sum()), up_mm, side_mm, 100.0 * share, n))
        if not stretches:
            self.say('  no stretch to judge')
            return
        self.say('  %.1f to %.1f Hz rms in mm (side from %.1f Hz; mean over the stretches), by speed and slope (|tan| under %.2f is level): camera / view up, camera / view side, view along; the up term\'s rms (all of it, under %.1f Hz, in the band); share of rows at the %.1f cm clamp' % (
            F216_BAND_HZ[0], F216_BAND_HZ[1], F216_SIDE_BAND_HZ[0], F216_SLOPE_SPLIT, F216_LOW_BAND_HZ[1], clamp_m * 100))
        for bkey in sorted(rows):
            rr = rows[bkey]
            m = lambda f: float(np.nanmean([r[f] for r in rr])) * 1000.0
            self.f216['bins'][bkey] = dict(n=len(rr), cam_mm=m('cam'), view_mm=m('view'), side_mm=m('side'), along_mm=m('along'), term_mm=m('term'), term_low_mm=m('term_low'), term_band_mm=m('term_band'),
                                         clamp=float(np.mean([r['clamp'] for r in rr])))
            self.say('    %-24s %3d stretches: up %.2f / %.2f, side %.2f / %.2f, along %.2f; term %.2f (under %.1f Hz %.2f, %.1f to %.1f Hz %.2f); clamp %.1f%%' % (
                names[bkey], len(rr), m('cam'), m('view'), m('cam_side'), m('side'), m('along'), m('term'), F216_LOW_BAND_HZ[1], m('term_low'), F216_BAND_HZ[0], F216_BAND_HZ[1], m('term_band'),
                100.0 * self.f216['bins'][bkey]['clamp']))
        sprint = self.f216['bins'].get((1, 0))
        if sprint and have_bob and key == 'off' and sprint['view_mm'] > F216_SPRINT_MM:
            rr = rows[(1, 0)]
            self.findings.append(Finding(min(r['t0'] for r in rr), max(r['t1'] for r in rr), 'f216_sprint_bob', 'bob', 'F216: with the key off a level sprint (%g m/s and over) leaves %.1f mm rms of %.1f to %.1f Hz bob in the view '
                                         '(the camera had %.1f mm), limit %.1f mm (%d stretch(es))' % (F216_SPEED_SPLIT_MPS, sprint['view_mm'], F216_BAND_HZ[0], F216_BAND_HZ[1], sprint['cam_mm'], F216_SPRINT_MM, sprint['n'])))

    # -- F221: the left elbow's angle from straight down and its height over the hand -----------------------------------------------------------------------------------------
    @staticmethod
    def f221_elbow(S_, E_, H_):
        """The elbow's angle from straight down about the shoulder-to-hand axis (deg, signed; the elbow hung under the axis reads 0) and its height over the hand (cm), per row, from model-space joints
        (y up): ax = unit(H - S); off = (E - S) less its part along ax; d = unit(down less its part along ax); side = ax x d; angle = atan2(off . side, off . d)."""
        with np.errstate(all='ignore'):
            ax = H_ - S_
            ax = ax / np.maximum(np.linalg.norm(ax, axis=1), 1e-9)[:, None]
            v = E_ - S_
            off = v - (v * ax).sum(axis=1)[:, None] * ax
            down = np.tile(np.array([0.0, -1.0, 0.0]), (len(ax), 1))
            d = down - (down * ax).sum(axis=1)[:, None] * ax
            d = d / np.maximum(np.linalg.norm(d, axis=1), 1e-9)[:, None]
            sd = np.cross(ax, d)
            ang = np.degrees(np.arctan2((off * sd).sum(axis=1), (off * d).sum(axis=1)))
            over = (E_[:, 1] - H_[:, 1]) * 100.0
        return ang, over

    # -- F218, F219: the left elbow against the chest, by item and speed -------------------------------------------------------------------------------------------------------------
    def f218_section(self):
        """Per item serial and speed class (the root's plane speed over the last F218_SPEED_S: walk F218_WALK_MPS to F218_SPRINT_MPS, sprint over), with the left arm writing: seconds; the left elbow's
        step against the chest (final_l_fore - final_chest, between consecutive ik records) over 2 and over 5 cm a second; the spine weight's flips a second (a step over 0.5); the left reach ratio's
        mean; the share of solves F219 limited; and, where F218's fields are in the recording, the rifle's rest solution applied, refused and held back and the slide's largest offset. A class under
        1 s is left out. Finding elbow_steps: an item with F218_FINDING_MIN_S or more reads over F218_FINDING_STEPS_PER_S steps over 5 cm a second. A recording without the new fields still prints the
        step figures; the figures that need them say they are absent and give no finding."""
        S = self.S
        self.f218 = None
        self.say('')
        self.say('== left elbow (F218, F219, F221): against the chest, by item and speed, with the left arm writing')
        ik, pin = S.tab('ik'), S.tab('pin')
        need_ik = ['item_serial', 'final_chest', 'final_l_fore', 'spine_w', 'l_reach_ratio', 'l_wrote', 'final_valid']
        miss = [('ik.' + f) for f in need_ik if not ik.has(f)] + [('pin.' + f) for f in ('root', 'item_serial') if not pin.has(f)]
        if not len(ik) or not len(pin) or miss:
            self.say('  cannot evaluate: %s' % (', '.join(miss) if miss else 'no ik or pin records'))
            return
        t_ik, t_pin = S.t('ik'), S.t('pin')
        root = pin.f('root').astype(np.float64)
        if pin.has('up'):
            up = pin.f('up').astype(np.float64)
            nn = np.linalg.norm(up, axis=1)
            up = np.where((nn > 0.5)[:, None], up / np.maximum(nn, 1e-9)[:, None], np.array([0.0, 1.0, 0.0]))
        else:
            up = np.tile(np.array([0.0, 1.0, 0.0]), (len(t_pin), 1))
        j0 = np.searchsorted(t_pin, t_pin - F218_SPEED_S, 'left')
        with np.errstate(all='ignore'):
            d = root - root[j0]
            dh = d - (d * up).sum(axis=1)[:, None] * up
            span = t_pin - t_pin[j0]
            speed = np.where(span >= 0.5 * F218_SPEED_S, np.linalg.norm(dh, axis=1) / np.maximum(span, 1e-3), np.nan)
        speed = np.where(speed <= F218_MAX_MPS, speed, np.nan)
        pi = np.searchsorted(t_pin, t_ik, 'right') - 1
        near = (pi >= 0) & (t_ik - t_pin[np.maximum(pi, 0)] < 0.2)
        v_ik = np.where(near, speed[np.maximum(pi, 0)], np.nan)
        self._f218_speed = v_ik
        cls_of = lambda v: np.where(np.isfinite(v), np.where(v >= F218_SPRINT_MPS, 2, np.where(v >= F218_WALK_MPS, 1, 0)), -1)
        cls_ik, cls_pin = cls_of(v_ik), cls_of(speed)
        n = len(t_ik)
        dt = np.zeros(n)
        dt[1:] = np.clip(np.diff(t_ik), 0.0, 0.1)
        ser = ik.f('item_serial').astype(np.int64)
        elig = (ik.f('l_wrote') == 1) & (ik.f('final_valid') == 1)
        rel = ik.f('final_l_fore').astype(np.float64) - ik.f('final_chest').astype(np.float64)
        spine = ik.f('spine_w').astype(np.float64)
        reach = ik.f('l_reach_ratio').astype(np.float64)
        stp = np.zeros(n)
        stp[1:] = np.linalg.norm(rel[1:] - rel[:-1], axis=1)
        flip = np.zeros(n, bool)
        flip[1:] = np.abs(spine[1:] - spine[:-1]) > F218_SPINE_FLIP
        # a step is between two rows of one item and one speed class, both with the arm writing, 90 Hz apart, not across a gap
        vs = np.zeros(n, bool)
        vs[1:] = elig[1:] & elig[:-1] & (ser[1:] == ser[:-1]) & (cls_ik[1:] == cls_ik[:-1]) & (cls_ik[1:] >= 1) & (dt[1:] > 0.0) & (dt[1:] < 0.1) & np.isfinite(stp[1:])
        lim = ik.f('l_swivel_limited') if ik.has('l_swivel_limited') else None
        # F221: the elbow's angle from straight down and its height over the hand, rows at the hip (the detach weight under 0.05), and the strafe class of each row (the root's speed along the body's right)
        have_f221 = all(ik.has(f) for f in ('final_l_upper', 'final_l_fore', 'final_l_hand'))
        ang = over_cm = None
        if have_f221:
            ang, over_cm = self.f221_elbow(ik.f('final_l_upper').astype(np.float64), ik.f('final_l_fore').astype(np.float64), ik.f('final_l_hand').astype(np.float64))
        hip = np.ones(n, bool)
        if pin.has('detach_w'):
            dw = np.where(near, pin.f('detach_w').astype(np.float64)[np.maximum(pi, 0)], np.nan)
            hip = np.isfinite(dw) & (dw < 0.05)
        down_src = (ik.f('l_pole_src') == 2) if ik.has('l_pole_src') else None
        strafe_cls = np.zeros(n, int)   # 1 right, 2 left
        have_strafe = pin.has('fwd') and pin.has('up')
        if have_strafe:
            fwd = pin.f('fwd').astype(np.float64)
            with np.errstate(all='ignore'):
                right = np.cross(up, fwd)
                right = right / np.maximum(np.linalg.norm(right, axis=1), 1e-9)[:, None]
                j2 = np.searchsorted(t_pin, t_pin - F221_STRAFE_SPAN_S, 'left')
                sp = t_pin - t_pin[j2]
                side = np.where(sp >= 0.5 * F221_STRAFE_SPAN_S, ((root - root[j2]) * right).sum(axis=1) / np.maximum(sp, 1e-3), np.nan)
            side_ik = np.where(near, side[np.maximum(pi, 0)], np.nan)
            with np.errstate(invalid='ignore'):
                strafe_cls = np.where(side_ik >= F221_STRAFE_MPS, 1, np.where(side_ik <= -F221_STRAFE_MPS, 2, 0))

        def f221_text(mk):
            mk = mk & hip
            if ang is None or not mk.any():
                return '', None
            a, o = np.abs(ang[mk]), over_cm[mk]
            a, o = a[np.isfinite(a)], o[np.isfinite(o)]
            if not len(a) or not len(o):
                return '', None
            fig = dict(ang_med=float(np.median(a)), ang_p99=float(np.percentile(a, 99)), oh_med=float(np.median(o)), oh_max=float(o.max()),
                       down=(float(np.mean(down_src[mk])) if down_src is not None else None))
            txt = '; F221 at the hip: elbow from straight down median %.0f deg, 99th percentile %.0f deg; elbow over the hand median %+.1f cm, highest %+.1f cm' % (fig['ang_med'], fig['ang_p99'], fig['oh_med'], fig['oh_max'])
            txt += ('; body-down pole on %.0f%% of rows' % (100.0 * fig['down'])) if fig['down'] is not None else '; l_pole_src absent'
            return txt, fig
        have_f218 = all(pin.has(f) for f in ('f218_held', 'f218_applied', 'f218_refused', 'f218_offset_m'))
        pser = pin.f('item_serial').astype(np.int64)
        names = ('standing', 'walk', 'sprint')
        self.f218 = dict(items={}, have_f218=have_f218, have_limited=lim is not None)
        per_item = {}
        for s in sorted(set(ser[vs].tolist())):
            for c in (1, 2):
                m = vs & (ser == s) & (cls_ik == c)
                secs = float(dt[m].sum())
                if secs < F218_MIN_CLASS_S:
                    continue
                r2, r5 = float((stp[m] > 0.02).sum()) / secs, float((stp[m] > 0.05).sum()) / secs
                fl = float(flip[m].sum()) / secs
                rng = ('%g to %g m/s' % (F218_WALK_MPS, F218_SPRINT_MPS)) if c == 1 else ('%g m/s and over' % F218_SPRINT_MPS)
                line = ('  item %d, %s (%s): %.1f s; left elbow steps against the chest over 2 cm %.1f/s, over 5 cm %.1f/s; spine weight flips %.1f/s; left reach ratio mean %.2f' % (
                    s, names[c], rng, secs, r2, r5, fl, float(np.nanmean(reach[m]))))
                limited = float(np.mean(lim[m] == 1)) if lim is not None else None
                line += ('; swivel limited %.0f%% of solves' % (100.0 * limited)) if limited is not None else '; l_swivel_limited absent (a recording from before F219)'
                if have_f218:
                    pm = (pser == s) & (cls_pin == c)
                    h, a, rf = int((pin.f('f218_held')[pm] == 1).sum()), int((pin.f('f218_applied')[pm] == 1).sum()), int((pin.f('f218_refused')[pm] == 1).sum())
                    if h + a + rf:
                        line += '; F218 over %d pin calls: rest applied %d, refused %d, held back %d, largest slide %.1f cm' % (int(pm.sum()), a, rf, h, 100.0 * float(np.nanmax(pin.f('f218_offset_m')[pm])))
                f221_line, f221_fig = f221_text(m)
                line += f221_line
                self.say(line)
                self.f218['items'][(int(s), names[c])] = dict(secs=secs, r2=r2, r5=r5, flips=fl, reach=float(np.nanmean(reach[m])), limited=limited, f221=f221_fig)
                acc = per_item.setdefault(int(s), dict(secs=0.0, n5=0.0, nflip=0.0, rows=[]))
                acc['secs'] += secs
                acc['n5'] += float((stp[m] > 0.05).sum())
                acc['nflip'] += float(flip[m].sum())
                acc['rows'].append(np.flatnonzero(m))
        # F221: strafe right and left, at the hip with the left arm writing (the root 1 m/s or more along the body's right or left, over the last 0.2 s)
        self.f218['strafe'] = {}
        if not have_strafe:
            self.say('  pin.up or pin.fwd missing: a recording from before F213; no strafe bins')
        elif not have_f221:
            self.say('  ik.final_l_upper, final_l_fore or final_l_hand missing: no F221 figures')
        else:
            for s in sorted(set(ser[elig & (strafe_cls > 0)].tolist())):
                for c, nm in ((1, 'strafe right'), (2, 'strafe left')):
                    m = elig & hip & (ser == s) & (strafe_cls == c) & (dt > 0.0)
                    secs = float(dt[m].sum())
                    if secs < F218_MIN_CLASS_S:
                        continue
                    txt, fig = f221_text(m)
                    self.say('  item %d, %s (%g m/s or more along the body\'s %s): %.1f s%s' % (s, nm, F221_STRAFE_MPS, nm.split()[1], secs, txt))
                    self.f218['strafe'][(int(s), nm)] = dict(secs=secs, f221=fig)
        # F225: solver hops on the ground at the hip: the left elbow stepping over 2 cm against the chest while the hand against the chest steps under 8 mm, per second, and how many fall on a flip of the
        # F59 clavicle assist (l_assisted changing within 3 calls). Finding elbow_solver_hop: over 0.5 a second on 10 s or more.
        self.f218['hops'] = {}
        if have_f221 and ik.has('l_assisted') and ik.has('anim_l_foot') and ik.has('anim_r_foot'):
            chest_ = ik.f('final_chest').astype(np.float64)
            hand_rel = ik.f('final_l_hand').astype(np.float64) - chest_
            hstep = np.zeros(n)
            hstep[1:] = np.linalg.norm(hand_rel[1:] - hand_rel[:-1], axis=1)
            asst = ik.f('l_assisted').astype(int)
            flip = np.zeros(n, bool)
            flip[1:] = asst[1:] != asst[:-1]
            fl3 = flip.copy()
            for sh in range(1, F225_FLIP_CALLS + 1):
                fl3[sh:] |= flip[:-sh]
            air_ = np.minimum(ik.f('anim_l_foot')[:, 1], ik.f('anim_r_foot')[:, 1]).astype(np.float64) > F224_AIR_FOOT_M
            vs2 = np.zeros(n, bool)
            with np.errstate(invalid='ignore'):
                vs2[1:] = elig[1:] & elig[:-1] & (ser[1:] == ser[:-1]) & (dt[1:] > 0.0) & (dt[1:] < 0.1) & hip[1:] & ~air_[1:] & np.isfinite(stp[1:])
                hop = vs2 & (stp > F225_HOP_STEP_M) & (hstep < F225_HAND_STEP_M)
            tot = [0.0, 0, 0]
            for s_ in sorted(set(ser[vs2].tolist())):
                m = vs2 & (ser == s_)
                secs = float(dt[m].sum())
                if secs < F218_MIN_CLASS_S:
                    continue
                nh, nf = int((hop & m).sum()), int((hop & m & fl3).sum())
                tot[0] += secs
                tot[1] += nh
                tot[2] += nf
                self.f218['hops'][int(s_)] = dict(secs=secs, hops=nh, on_flip=nf, rate=nh / secs)
                self.say('  item %d on the ground at the hip: %.1f s; solver hops (left elbow step over %g cm against the chest, the hand under %g mm) %d = %.2f/s, %d on a clavicle assist flip (within %d calls)' % (
                    s_, secs, 100.0 * F225_HOP_STEP_M, 1000.0 * F225_HAND_STEP_M, nh, nh / secs, nf, F225_FLIP_CALLS))
                if secs >= F225_FINDING_MIN_S and nh / secs > F225_FINDING_PER_S:
                    idx = np.flatnonzero(hop & m)
                    self.findings.append(Finding(t_ik[idx].min(), t_ik[idx].max(), 'elbow_solver_hop', 'assist' if nf * 2 >= nh else 'solver', 'F225: item %d, the left elbow hopped over %g cm with the hand steady %d times in %.1f s at the hip on the ground '
                                                 '(%.2f a second, limit %.1f); %d on a flip of the clavicle assist' % (s_, 100.0 * F225_HOP_STEP_M, nh, secs, nh / secs, F225_FINDING_PER_S, nf)))
            if tot[0] > 0.0:
                self.f218['hops']['all'] = dict(secs=tot[0], hops=tot[1], on_flip=tot[2], rate=tot[1] / tot[0])
                self.say('  all items on the ground at the hip: %.1f s; solver hops %d = %.2f/s, %d on a clavicle assist flip' % (tot[0], tot[1], tot[1] / tot[0], tot[2]))
        elif not (ik.has('l_assisted') and ik.has('anim_l_foot')):
            self.say('  ik.l_assisted or the animated feet missing: no solver hop figures')
        if not self.f218['items']:
            self.say('  no item with %.0f s or more of left-arm writes at %g m/s or more (the root\'s plane speed over %.1f s): nothing to read' % (F218_MIN_CLASS_S, F218_WALK_MPS, F218_SPEED_S))
        if not have_f218:
            self.say('  pin.f218_* missing: a recording from before F218; the rifle\'s torso figures are absent and give no finding')
        for s, acc in sorted(per_item.items()):
            if acc['secs'] < F218_FINDING_MIN_S:
                continue
            rate = acc['n5'] / acc['secs']
            if rate > F218_FINDING_STEPS_PER_S:
                idx = np.concatenate(acc['rows'])
                cause = 'torso' if acc['nflip'] / acc['secs'] > 0.5 else 'elbow'
                self.findings.append(Finding(t_ik[idx].min(), t_ik[idx].max(), 'elbow_steps', cause, 'F218/F219: item %d, the left elbow stepped over 5 cm against the chest %.1f times a second over %.1f s of writes at '
                                             '%g m/s or more (limit %.1f); the spine weight flipped %.1f times a second' % (s, rate, acc['secs'], F218_WALK_MPS, F218_FINDING_STEPS_PER_S, acc['nflip'] / acc['secs'])))

        # F221 finding elbow_over_hand: at the hip, armed, the left arm writing, later than 1 s after an item change, the elbow 3 cm or more over the hand for 100 ms or more: one finding per episode
        if have_f221:
            self.f221_over_hand_findings(t_ik, ser, elig, hip, ik, pin, over_cm, near, pi)

    def f221_over_hand_findings(self, t_ik, ser, elig, hip, ik, pin, over_cm, near, pi):
        n = len(t_ik)
        if ik.has('armed'):
            armed = ik.f('armed') == 1
        elif pin.has('armed'):
            armed = np.where(near, pin.f('armed')[np.maximum(pi, 0)] == 1, False)
        else:
            armed = np.ones(n, bool)
        chg = np.zeros(n)
        last = t_ik[0] - 1e9 if n else 0.0
        for i in range(n):
            if i and ser[i] != ser[i - 1]:
                last = t_ik[i]
            chg[i] = t_ik[i] - last
        bad = elig & hip & armed & (chg > F221_ITEM_SETTLE_S) & np.isfinite(over_cm) & (over_cm >= F221_OVER_HAND_CM)
        i = 0
        while i < n:
            if not bad[i]:
                i += 1
                continue
            j = i
            while j + 1 < n and bad[j + 1] and t_ik[j + 1] - t_ik[j] < 0.1 and ser[j + 1] == ser[i]:
                j += 1
            if t_ik[j] - t_ik[i] >= F221_OVER_HAND_MIN_S:
                pole = ''
                if ik.has('l_pole_src'):
                    pole = ', body-down pole on %.0f%% of the rows' % (100.0 * float(np.mean(ik.f('l_pole_src')[i:j + 1] == 2)))
                self.findings.append(Finding(t_ik[i], t_ik[j], 'elbow_over_hand', 'elbow', 'F221: item %d, the left elbow over the hand by up to %.1f cm for %.2f s at the hip (limit %.0f cm for %.1f s)%s' % (
                    int(ser[i]), float(np.max(over_cm[i:j + 1])), t_ik[j] - t_ik[i], F221_OVER_HAND_CM, F221_OVER_HAND_MIN_S, pole)))
            i = j + 1

    # -- F222: the turning body's neck target and chest, and the heading against the yaw step ------------------------------------------------------------------------------------
    SG9 = np.array([-21.0, 14.0, 39.0, 54.0, 59.0, 54.0, 39.0, 14.0, -21.0]) / 231.0

    @staticmethod
    def f222_resid(x):
        """x less its quadratic smoothing over 9 calls (the Savitzky-Golay centre weights), along axis 0; the 4 rows at each end are not a number."""
        x = np.asarray(x, dtype=np.float64)
        out = np.full(x.shape, np.nan)
        n = len(x)
        if n < 9:
            return out
        sm = np.zeros((n - 8,) + x.shape[1:])
        for j in range(9):
            sm += Analysis.SG9[j] * x[j:j + n - 8]
        out[4:n - 4] = x[4:n - 4] - sm
        return out

    def f222_section(self):
        """Per item with F222_MIN_ROWS or more rows at a body yaw rate of F222_RATE_DPS or more (the pin's fwd about the first up, the rate over F222_RATE_SPAN_S): the neck target's and the final
        chest's residual about a local quadratic over 9 calls (median and 90th percentile of its size, mm), and the slope of the heading's residual on the yaw step's residual. ik rows take the pin
        record of the same call (the last pin record at or before). Finding heading_follows_yaw_step: the slope under F222_FINDING_SLOPE with F222_FINDING_ROWS or more rows."""
        S = self.S
        self.f222 = None
        self.say('')
        self.say('== turning (F222): neck target and chest residual about a local quadratic over 9 calls, by item, with the body turning at %g deg/s or more; key experimental.native_on_foot_steady_body_in_turns is %s' % (
            F222_RATE_DPS, S.keys.get('experimental.native_on_foot_steady_body_in_turns', 'on')))
        ik, pin = S.tab('ik'), S.tab('pin')
        miss = [('ik.' + f) for f in ('final_chest', 'item_serial') if not ik.has(f)] + [('pin.' + f) for f in ('fwd', 'up', 'neck_target', 'heading') if not pin.has(f)]
        if not len(ik) or not len(pin) or miss:
            self.say('  cannot evaluate: %s' % (', '.join(miss) if miss else 'no ik or pin records'))
            return
        t_ik, t_pin = S.t('ik'), S.t('pin')
        pi = np.searchsorted(t_pin, t_ik, 'right') - 1
        have = pi >= 0
        pi = np.maximum(pi, 0)
        with np.errstate(all='ignore'):
            fwd = pin.f('fwd').astype(np.float64)
            up0 = pin.f('up')[0].astype(np.float64)
            up0 = up0 / max(float(np.linalg.norm(up0)), 1e-9)
            ref = np.array([1.0, 0.0, 0.0])
            e1 = ref - float(np.dot(ref, up0)) * up0
            if np.linalg.norm(e1) < 0.1:
                e1 = np.array([0.0, 0.0, 1.0]) - float(up0[2]) * up0
            e1 = e1 / np.linalg.norm(e1)
            e2 = np.cross(up0, e1)
            yaw = np.degrees(np.unwrap(np.arctan2(fwd @ e2, fwd @ e1)))
            j0 = np.searchsorted(t_pin, t_pin - F222_RATE_SPAN_S, 'left')
            span = t_pin - t_pin[j0]
            rate = np.where(span >= 0.5 * F222_RATE_SPAN_S, (yaw - yaw[j0]) / np.maximum(span, 1e-3), np.nan)
            step = np.zeros(len(yaw))
            step[1:] = np.diff(yaw)
            hd = pin.f('heading').astype(np.float64)
            hdg = np.degrees(np.unwrap(np.arctan2(hd[:, 0], hd[:, 1])))
            neck = pin.f('neck_target').astype(np.float64)[pi]
            chest = ik.f('final_chest').astype(np.float64)
            r_neck = np.linalg.norm(self.f222_resid(neck), axis=1) * 1000.0
            r_chest = np.linalg.norm(self.f222_resid(chest), axis=1) * 1000.0
            r_hdg = self.f222_resid(hdg[pi])
            r_step = self.f222_resid(step[pi])
        ser = ik.f('item_serial').astype(np.int64)
        rate_ik = rate[pi]
        self.f222 = dict(items={})
        for s_ in sorted(set(ser.tolist())):
            m = have & (ser == s_) & (np.abs(rate_ik) >= F222_RATE_DPS) & np.isfinite(r_neck) & np.isfinite(r_chest) & np.isfinite(r_hdg) & np.isfinite(r_step)
            n = int(m.sum())
            if n < F222_MIN_ROWS:
                continue
            slope = float(np.polyfit(r_step[m], r_hdg[m], 1)[0]) if float(np.var(r_step[m])) > 1e-12 else float('nan')
            fig = dict(rows=n, neck_med=float(np.median(r_neck[m])), neck_p90=float(np.percentile(r_neck[m], 90)), chest_med=float(np.median(r_chest[m])), chest_p90=float(np.percentile(r_chest[m], 90)), slope=slope,
                       rate=float(np.median(np.abs(rate_ik[m]))))
            self.f222['items'][int(s_)] = fig
            self.say('  item %d: %d rows, body turning %.0f deg/s median; neck target residual median %.2f mm, 90th percentile %.2f mm; final chest residual median %.2f mm, 90th percentile %.2f mm; heading residual on yaw step residual slope %+.2f' % (
                s_, n, fig['rate'], fig['neck_med'], fig['neck_p90'], fig['chest_med'], fig['chest_p90'], slope))
            if n >= F222_FINDING_ROWS and np.isfinite(slope) and slope < F222_FINDING_SLOPE:
                self.findings.append(Finding(t_ik[m].min(), t_ik[m].max(), 'heading_follows_yaw_step', 'eye', 'F222: item %d, the heading in the model followed the body\'s yaw step with slope %+.2f over %d rows turning at %.0f deg/s or more '
                                             '(limit %.1f): the eye rows are taken through the wrong frame' % (s_, slope, n, F222_RATE_DPS, F222_FINDING_SLOPE)))
        if not self.f222['items']:
            self.say('  no item with %d rows or more at %g deg/s or more: nothing to read' % (F222_MIN_ROWS, F222_RATE_DPS))

    # -- F224: the whole-body move ------------------------------------------------------------------------------------------------------------------------------------------------------
    @staticmethod
    def f224_resid(x):
        """x less its quadratic smoothing over 7 calls (the Savitzky-Golay centre weights -2 3 6 7 6 3 -2 over 21), along axis 0; 3 rows at each end are not a number."""
        x = np.asarray(x, dtype=np.float64)
        w = np.array([-2.0, 3.0, 6.0, 7.0, 6.0, 3.0, -2.0]) / 21.0
        out = np.full(x.shape, np.nan)
        n = len(x)
        if n < 7:
            return out
        sm = np.zeros((n - 6,) + x.shape[1:])
        for j in range(7):
            sm += w[j] * x[j:j + n - 6]
        out[3:n - 3] = x[3:n - 3] - sm
        return out

    def f224_section(self):
        """Rows in F215's active states (active, active_item), each ik row with the pin record of its own call, split into air (an armed row with |root_vy| over F224_AIR_VY_MPS; the lower animated foot over F224_AIR_FOOT_M is read beside it) and, on the ground, by the
        root's speed (run over F224_RUN_MPS, walk from F218_WALK_MPS, still): the residual about a local quadratic over 7 calls along y (rms, mm) of the true camera (pin.e[1] less f223_pop_m: what F215's
        rule reads), of the camera less the whole shift (pin.e[1] less f215_body_y: the height the pin and the hands work in) and of the final left hand against the final chest; f224_lead_m's rms when
        recorded. Finding body_move_late: the air figure of the camera less the shift over F224_FINDING_MM (90th percentile of the residual's size; the rms is carried by a few calls) on F224_FINDING_ROWS or more rows."""
        S = self.S
        self.f224 = None
        self.say('')
        self.say('== whole-body move (F224): residual about a local quadratic over 7 calls along y, in F215 active states, by air and ground speed')
        ik, pin = S.tab('ik'), S.tab('pin')
        miss = [('ik.' + f) for f in ('final_chest', 'final_l_hand', 'anim_l_foot', 'anim_r_foot') if not ik.has(f)] + [('pin.' + f) for f in ('e', 'f215_state', 'f215_body_y') if not pin.has(f)]
        if not len(ik) or not len(pin) or miss:
            self.say('  cannot evaluate: %s' % (', '.join(miss) if miss else 'no ik or pin records'))
            return
        t_ik, t_pin = S.t('ik'), S.t('pin')
        pi = np.searchsorted(t_pin, t_ik, 'right') - 1
        have = pi >= 0
        pi = np.maximum(pi, 0)
        st = pin.f('f215_state').astype(int)[pi]
        ai, ac = S.enum('F215State', 'active_item'), S.enum('F215State', 'active')
        act = have & ((st == ai) | (st == ac))
        act = act & (np.convolve(act.astype(float), np.ones(7), 'same') > 6.5)   # the 7-call window lies wholly inside the active state: a change of state is not a residual
        e1 = pin.f('e').astype(np.float64)[pi, 1]
        pop = pin.f('f223_pop_m').astype(np.float64)[pi] if pin.has('f223_pop_m') else np.zeros(len(pi))
        by = pin.f('f215_body_y').astype(np.float64)[pi]
        lead = pin.f('f224_lead_m').astype(np.float64)[pi] if pin.has('f224_lead_m') else None
        r_true = self.f224_resid(e1 - pop) * 1000.0
        r_less = self.f224_resid(e1 - by) * 1000.0
        r_hand = self.f224_resid(ik.f('final_l_hand').astype(np.float64)[:, 1] - ik.f('final_chest').astype(np.float64)[:, 1]) * 1000.0
        r_fore = self.f224_resid(ik.f('final_l_fore').astype(np.float64)[:, 1] - ik.f('final_chest').astype(np.float64)[:, 1]) * 1000.0 if ik.has('final_l_fore') else np.full(len(t_ik), np.nan)
        air = np.minimum(ik.f('anim_l_foot')[:, 1], ik.f('anim_r_foot')[:, 1]).astype(np.float64) > F224_AIR_FOOT_M
        v = self._f218_speed if self._f218_speed is not None and len(self._f218_speed) == len(t_ik) else np.full(len(t_ik), np.nan)
        with np.errstate(invalid='ignore'):
            vy_ = np.abs(pin.f('root_vy').astype(np.float64)[pi]) if pin.has('root_vy') else np.zeros(len(pi))
            arm_ = (pin.f('armed')[pi] == 1) if pin.has('armed') else np.zeros(len(pi), bool)
            jump = arm_ & (vy_ > F224_AIR_VY_MPS)
            gnd = act & ~air & ~jump
            classes = (('air', act & jump), ('air, feet off the ground', act & air & ~jump), ('run', gnd & (v >= F224_RUN_MPS)), ('walk', gnd & (v >= F218_WALK_MPS) & (v < F224_RUN_MPS)), ('still', gnd & (v < F218_WALK_MPS)))
        self.f224 = dict(classes={}, active=int(act.sum()), lead_recorded=lead is not None)
        rms = lambda x: float(np.sqrt(np.mean(x ** 2)))
        for name, m in classes:
            m = m & np.isfinite(r_true) & np.isfinite(r_less) & np.isfinite(r_hand)
            n = int(m.sum())
            if n < F224_MIN_ROWS:
                continue
            p90 = lambda x: float(np.percentile(np.abs(x), 90))
            fm = m & np.isfinite(r_fore)
            fig = dict(rows=n, true=rms(r_true[m]), less=rms(r_less[m]), hand=rms(r_hand[m]), true_p90=p90(r_true[m]), less_p90=p90(r_less[m]), hand_p90=p90(r_hand[m]),
                       fore=rms(r_fore[fm]) if fm.any() else None, fore_p90=p90(r_fore[fm]) if fm.any() else None, lead=(rms(lead[m]) * 1000.0 if lead is not None else None))
            self.f224['classes'][name] = fig
            self.say('  %s: %d rows; residual rms (90th percentile of its size): true camera %.2f (%.2f) mm, camera less the whole shift %.2f (%.2f) mm, left hand against the chest %.2f (%.2f) mm, left forearm against the chest %s; %s' % (
                name, n, fig['true'], fig['true_p90'], fig['less'], fig['less_p90'], fig['hand'], fig['hand_p90'], ('%.2f (%.2f) mm' % (fig['fore'], fig['fore_p90'])) if fig['fore'] is not None else 'not recorded',
                ('f224_lead_m rms %.2f mm' % fig['lead']) if lead is not None else 'f224_lead_m not recorded (a recording from before F224)'))
            if name == 'air' and n >= F224_FINDING_ROWS and fig['less_p90'] > F224_FINDING_MM:
                self.findings.append(Finding(t_ik[m].min(), t_ik[m].max(), 'body_move_late', 'body', 'F224: in the air the camera less the whole-body shift (what the pin and the hands work in) had a residual of %.1f mm at the 90th percentile (%.1f mm rms) over %d rows '
                                             '(true camera %.1f mm at the 90th percentile; limit %.1f): the move follows the camera a call late' % (fig['less_p90'], fig['less'], n, fig['true_p90'], F224_FINDING_MM)))
        if not self.f224['classes']:
            self.say('  no class with %d rows or more in an F215 active state (%d active rows): nothing to read' % (F224_MIN_ROWS, self.f224['active']))

    # -- F223: root jumps -----------------------------------------------------------------------------------------------------------------------------------------------------------------
    def f223_section(self):
        """Each call the F223 rule would take (the camera in the model, pin.e[1], changing by F223_JUMP_M or more between two new game frames less than F223_MAX_GAP_S apart while the world camera
        (the p1 record's, the last at or before the pin call) moved less than F223_CAMERA_SHARE of it along pin.up), grouped when closer than F223_GROUP_S: time, item, calls, the first change,
        f223_pop_m when recorded, the final chest's lowest y over the next F223_LOOK_CALLS calls against the call before, the left elbow's highest height over the hand. Finding root_jump_crushed:
        the final chest falls more than F223_CRUSH_M."""
        S = self.S
        self.f223 = None
        self.say('')
        self.say('== root jumps (F223): the calls the rule takes (camera in the model changing %g m or more with the world camera moving under %g of it)' % (F223_JUMP_M, F223_CAMERA_SHARE))
        ik, pin, p1 = S.tab('ik'), S.tab('pin'), S.tab('p1')
        miss = [('pin.' + f) for f in ('e', 'up', 'p1', 'item_serial') if not pin.has(f)] + [('p1.' + f) for f in ('cam',) if not p1.has(f)] + [('ik.' + f) for f in ('final_chest',) if not ik.has(f)]
        if not len(pin) or not len(p1) or miss:
            self.say('  cannot evaluate: %s' % (', '.join(miss) if miss else 'no pin or p1 records'))
            return
        t_pin, t_p1, t_ik = S.t('pin'), S.t('p1'), S.t('ik')
        pf = pin.f('p1').astype(np.int64)
        idx = np.flatnonzero(np.concatenate([[True], pf[1:] != pf[:-1]]))
        ci = np.maximum(np.searchsorted(t_p1, t_pin[idx], 'right') - 1, 0)
        with np.errstate(all='ignore'):
            e = pin.f('e').astype(np.float64)[idx, 1]
            cam = p1.f('cam').astype(np.float64)[ci]
            up = pin.f('up').astype(np.float64)[idx]
            up = up / np.maximum(np.linalg.norm(up, axis=1), 1e-9)[:, None]
            tt = t_pin[idx]
            dt = np.zeros(len(idx))
            dt[1:] = np.diff(tt)
            dE = np.zeros(len(idx))
            dE[1:] = np.diff(e)
            dC = np.zeros(len(idx))
            dC[1:] = ((cam[1:] - cam[:-1]) * up[1:]).sum(axis=1)
            take = (dt > 0.0) & (dt < F223_MAX_GAP_S) & (np.abs(dE) >= F223_JUMP_M) & (np.abs(dC) < F223_CAMERA_SHARE * np.abs(dE))
        ser = pin.f('item_serial').astype(np.int64)[idx]
        pop = pin.f('f223_pop_m').astype(np.float64) if pin.has('f223_pop_m') else None
        groups = []
        for k in np.flatnonzero(take):
            if groups and tt[k] - tt[groups[-1][-1]] <= F223_GROUP_S:
                groups[-1].append(int(k))
            else:
                groups.append([int(k)])
        self.f223 = dict(events=[])
        for g in groups:
            k0, k1 = g[0], g[-1]
            t0, t1 = float(tt[k0]), float(tt[k1])
            line = '  %s item %d: %d calls, the camera in the model %+.2f m in the first' % (S.lt(t0), int(ser[k0]), len(g), float(dE[k0]))
            ev = dict(t=t0, item=int(ser[k0]), calls=len(g), jump=float(dE[k0]), pop=None, chest_min=None, drop=None, elbow=None)
            if pop is not None:
                sel = (t_pin >= t0 - 1e-6) & (t_pin <= t1 + 0.1)
                ev['pop'] = float(pop[sel][np.argmax(np.abs(pop[sel]))]) if sel.any() else 0.0
                line += '; f223_pop_m up to %+.2f m' % ev['pop']
            else:
                line += '; f223_pop_m not recorded (a recording from before F223)'
            if len(ik) and len(t_ik):
                i0 = int(np.searchsorted(t_ik, t0 - 1e-6, 'left'))
                rows = slice(i0, min(i0 + F223_LOOK_CALLS, len(t_ik)))
                chest = ik.f('final_chest').astype(np.float64)[:, 1]
                if i0 >= 1 and rows.stop > rows.start:
                    ev['chest_min'] = float(np.nanmin(chest[rows]))
                    ev['drop'] = float(chest[i0 - 1] - ev['chest_min'])
                    line += '; final chest y lowest %.2f m against %.2f before (a fall of %.2f m)' % (ev['chest_min'], float(chest[i0 - 1]), ev['drop'])
                if ik.has('final_l_fore') and ik.has('final_l_hand') and rows.stop > rows.start:
                    over = (ik.f('final_l_fore').astype(np.float64)[rows, 1] - ik.f('final_l_hand').astype(np.float64)[rows, 1]) * 100.0
                    ev['elbow'] = float(np.nanmax(over))
                    line += '; left elbow over the hand up to %+.1f cm' % ev['elbow']
                if ev['drop'] is not None and ev['drop'] > F223_CRUSH_M:
                    self.findings.append(Finding(t0, t1, 'root_jump_crushed', 'root', 'F223: item %d, the model root jumped (the camera in the model %+.2f m in one call) and the final chest fell %.2f m within %d calls to %.2f m (limit %.2f)%s' % (
                        int(ser[k0]), float(dE[k0]), ev['drop'], F223_LOOK_CALLS, ev['chest_min'], F223_CRUSH_M, '' if ev['pop'] is None else '; the move taken was %+.2f m' % ev['pop'])))
            self.say(line)
            self.f223['events'].append(ev)
        if not groups:
            self.say('  no call the rule would take')

    # -- bob ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    def bob(self):
        L = self.L
        self.say('')
        self.say('== bob (rms of the body view after a %.2f s moving mean, settled frames outside excursions; limit %.1f mm, a flown good value 0.3)' % (BOB_WINDOW_S, BOB_LIMIT_M * 1e3))
        keep = L.settled.copy()
        for i0, i1, ip, kind, tc in getattr(self, 'exc', []):
            keep &= ~((L.t >= L.t[i0] - EXC_EXCLUDE_S) & (L.t <= L.t[i1] + EXC_EXCLUDE_S))
        resid = collections.defaultdict(lambda: collections.defaultdict(list))
        dur = collections.defaultdict(float)
        series = dict(body=L.body, game=L.game)
        for k in TERMS:
            series['body_shift' if k == 'body' else k] = SEATED_Y_SIGN * L.terms[k]   # the term 'body' (F215's shift) must not replace the body view in this table
        for r0, r1 in runs_of(keep):
            # split where time jumps
            seg_start = r0
            for j in range(r0 + 1, r1 + 2):
                if j > r1 or L.t[j] - L.t[j - 1] >= EXC_MERGE_S:
                    s0, s1 = seg_start, j - 1
                    seg_start = j
                    if L.t[s1] - L.t[s0] < BOB_MIN_S:
                        continue
                    tt = L.t[s0:s1 + 1]
                    lo = np.searchsorted(tt, tt - BOB_WINDOW_S / 2, 'left')
                    hi = np.searchsorted(tt, tt + BOB_WINDOW_S / 2, 'right')
                    ok = (tt >= tt[0] + BOB_WINDOW_S / 2) & (tt <= tt[-1] - BOB_WINDOW_S / 2)
                    key = (int(L.stance[s0]), int(L.armed[s0]), int(L.support[s0]))
                    for name, v in series.items():
                        c = np.concatenate([[0.0], np.cumsum(v[s0:s1 + 1])])
                        mean = (c[hi] - c[lo]) / (hi - lo)
                        resid[key][name].append((v[s0:s1 + 1] - mean)[ok])
                    dur[key] += float(tt[ok][-1] - tt[ok][0]) if ok.sum() > 1 else 0.0
        worst = 0.0
        any_ = False
        for key in sorted(resid):
            r = {n: np.sqrt(np.mean(np.concatenate(v) ** 2)) for n, v in resid[key].items() if len(v)}
            if not r or 'body' not in r:
                continue
            any_ = True
            terms = {n: v for n, v in r.items() if n != 'body'}
            top = sorted(terms, key=lambda n: -terms[n])[:3]
            lab = '%s%s, support %s' % (self.S.enum_name('Stance', key[0]), ' armed' if key[1] else '', self.S.enum_name('Support', key[2]))
            self.say('  %-34s %5.1f s  rms %.2f mm   largest: %s' % (lab, dur[key], r['body'] * 1e3, ', '.join('%s %.2f' % (n, terms[n] * 1e3) for n in top)))
            worst = max(worst, r['body'])
            if r['body'] > BOB_LIMIT_M:
                t0 = float(L.t[np.flatnonzero((L.stance == key[0]) & (L.armed == key[1]))[0]])
                self.findings.append(Finding(t0, t0, 'bob', top[0], 'bob, %s: rms %.2f mm over %.1f s, limit %.1f; largest %s' % (
                    lab, r['body'] * 1e3, dur[key], BOB_LIMIT_M * 1e3, ', '.join('%s %.2f mm' % (n, terms[n] * 1e3) for n in top))))
        if not any_:
            self.say('  no settled window of %.1f s outside the excursions' % BOB_MIN_S)

    # -- leak gains ---------------------------------------------------------------------------------------------------------------------------------------------------------
    def leak_gains(self):
        L, S = self.L, self.S
        self.say('')
        self.say('== raise leak gains (F171 definitions: pairs of consecutive pin calls whose carried raise changed %.0f mm or more; slope through the origin of the change against the raise\'s)' % (LEAK_MIN_STEP_M * 1e3))
        self.say('   k_up/k_fwd: the camera as rendered (before any removal), up and forward from the animated neck; c: G as F138 forms it (E less the neck less the headset height); '
                 'a term: its effect on the view. 1 = carries the raise, 0 = not. Look mode and the local pose generation are not recorded: pairs pool across them.')
        n = len(L.t)
        if n < 2:
            self.say('  too few rows')
            return
        raise_up = -L.sub['raise_']
        cam_up = L.cam - L.neck[:, 1]
        g_ = L.rawe - L.neck[:, 1] - L.head_y
        fwd = None
        pin = S.tab('pin')
        if pin.has('heading'):
            h = pin.f('heading')[L.R].astype(np.float64)
            hn = np.hypot(h[:, 0], h[:, 1])
            okh = hn > 0.5
            wv = L.sub['wv']
            proj = np.where(okh, (wv[:, 0] * h[:, 0] + wv[:, 2] * h[:, 1]) / np.maximum(hn, 1e-9), 0.0)
            cam_fwd = L.e[:, 2] + L.sub['ads'][:, 2] + L.sub['vneck'][:, 2] + proj - L.neck[:, 2]
            fwd = np.where(okh, cam_fwd, np.nan)
        else:
            self.cannot.append('cannot evaluate: pin.heading missing (k_fwd needs the heading)')
        band = np.where(~L.aim_known, -1, np.where(np.abs(L.pitch) < LEAK_BAND_LO, 0, np.where(np.abs(L.pitch) < LEAK_BAND_HI, 1, 2)))
        cls3 = np.where(L.ads, 2, np.where(L.armed == 1, 1, 0))
        gap = L.p1f[1:] - L.p1f[:-1]
        pair = (band[1:] >= 0) & (band[1:] == band[:-1]) & (cls3[1:] == cls3[:-1]) & (gap >= 1) & (gap <= LEAK_MAX_GAP) & (np.abs(raise_up[1:] - raise_up[:-1]) >= LEAK_MIN_STEP_M)
        dx = (raise_up[1:] - raise_up[:-1])
        tvals = collections.OrderedDict()
        tvals['game'] = L.game
        for k in ('hold', 'weapon_eye', 'ads_lock', 'sight', 'vneck', 'ads_turn', 'run'):
            tvals[k] = SEATED_Y_SIGN * L.terms[k]
        bn = ('under 10', '10-40', '40+')
        cn = ('unarmed', 'armed hip', 'ADS (F171 leaves ADS out)')
        self.leak = {}
        for b in range(3):
            for c in range(3):
                m = pair & (band[1:] == b) & (cls3[1:] == c)
                if not m.any():
                    continue
                w = dx[m]
                sxx = float((w * w).sum())
                slope = lambda v: float((w * (v[1:] - v[:-1])[m]).sum() / sxx)
                cell = dict(n=int(m.sum()), k_up=slope(cam_up), c=slope(g_), lo=float(min(raise_up[:-1][m].min(), raise_up[1:][m].min())),
                            hi=float(max(raise_up[:-1][m].max(), raise_up[1:][m].max())), terms={k: slope(v) for k, v in tvals.items()})
                cell['k_fwd'] = slope(fwd) if fwd is not None and np.isfinite(fwd[1:][m]).all() and np.isfinite(fwd[:-1][m]).all() else None
                thin = cell['n'] < LEAK_THIN_PAIRS
                cell['thin'] = thin
                self.leak[(b, c)] = cell
                self.say('  %-8s deg %-26s %4d pairs%s raise %+.1f..%+.1f cm, k_up %+.2f, k_fwd %s, c %+.2f | %s' % (
                    bn[b], cn[c], cell['n'], ' (thin)' if thin else '', 100 * cell['lo'], 100 * cell['hi'], cell['k_up'],
                    '%+.2f' % cell['k_fwd'] if cell['k_fwd'] is not None else 'n/a', cell['c'], ' '.join('%s %+.2f' % (k, v) for k, v in cell['terms'].items())))
                if b == 2 and c != 2 and not thin and cell['k_up'] < LEAK_FLAG_SLOPE:
                    tt = float(L.t[:-1][m][0])
                    self.findings.append(Finding(tt, tt, 'leak', 'raise_not_in_camera', 'leak, 40+ deg %s: k_up %+.2f over %d pairs, under %.1f: the raise is not in the hook\'s camera (c %+.2f)' % (
                        cn[c], cell['k_up'], cell['n'], LEAK_FLAG_SLOPE, cell['c'])))
        if not self.leak:
            self.say('  no pair')

    # -- IK -----------------------------------------------------------------------------------------------------------------------------------------------------------------
    def ik_events(self):
        S = self.S
        self.say('')
        self.say('== arm IK events (joints relative to final_chest)')
        ik = S.tab('ik')
        if not len(ik):
            self.say('  no ik records')
            return
        if not self.need('arm IK events', REQ_IK):
            return
        tk = S.t('ik')
        n = len(tk)
        E_COMP = S.enum('IkExit', 'completed')
        R_NONE = S.enum('IkRefuse', 'none')
        ex, rf = ik.f('exit'), ik.f('refuse')
        comp = (ex == E_COMP) & (ik.f('final_valid') == 1)
        ci = np.flatnonzero(comp)
        pin = S.tab('pin')
        pt = S.t('pin')
        pj = np.searchsorted(pt, tk, 'right') - 1
        pe = pin.f('e')
        self.ik_gaps = gaps_of(tk, STALL_S)
        events = []
        armstat = []

        def ev(kind, arm, k, kprev, t1, detail, joint=None, refcount=None):
            events.append(dict(kind=kind, arm=arm, k=int(k), kp=int(kprev), t0=float(tk[k]), t1=float(t1), detail=detail, joint=joint, extra=refcount))
        chest = ik.f('final_chest').astype(np.float64)
        for p in ARMS:
            arm = p[0].upper()
            rel = {j: ik.f('final_%s%s' % (p, j)).astype(np.float64) - chest for j in ('upper', 'fore', 'hand')}
            # an arm the IK did not write is the animation's: a rule on its joints says nothing about the IK (545 events came from arms never written in the first flight)
            wrote = (ik.f(p + 'wrote') == 1) & (ik.f(p + 'w_arm') > 0)
            wc = wrote[ci]
            armstat.append('%s wrote on %d of %d calls' % (arm, int(wrote.sum()), n))
            if len(ci) > 1:
                a, b = ci[:-1], ci[1:]
                dfore = np.linalg.norm(rel['fore'][b] - rel['fore'][a], axis=1)
                dhand = np.linalg.norm(rel['hand'][b] - rel['hand'][a], axis=1)
                for q in np.flatnonzero((dfore > IK_ELBOW_JUMP_M) & (dhand < IK_HAND_STILL_M) & wc[:-1] & wc[1:]):
                    ev('elbow_jump', arm, b[q], a[q], tk[b[q]], '%s elbow jumped %.1f cm in one call while the hand moved %.1f cm' % (arm, 100 * dfore[q], 100 * dhand[q]), ['fore'])
                sw = ik.f(p + 'swivel_deg')[ci].astype(np.float64)
                dsw = np.abs(wrap180(sw[1:] - sw[:-1]))
                for q in np.flatnonzero((dsw > IK_SWIVEL_JUMP_DEG) & wc[:-1] & wc[1:]):
                    ev('swivel_jump', arm, b[q], a[q], tk[b[q]], '%s swivel jumped %.1f deg in one call' % (arm, dsw[q]), ['fore'])
            if len(ci):
                grip = ik.f(p + 'grip_err_cm')[ci]
                wa = ik.f(p + 'w_arm')[ci]
                for r0, r1 in runs_of((grip > IK_GRIP_ERR_CM) & (wa > IK_GRIP_W) & wc):
                    if r1 - r0 + 1 >= IK_GRIP_CALLS:
                        ev('grip_loss', arm, ci[r0], ci[max(r0 - 1, 0)], tk[ci[r1]], '%s grip error over %.0f cm for %d calls (to %.1f cm)' % (arm, IK_GRIP_ERR_CM, r1 - r0 + 1, grip[r0:r1 + 1].max()), ['hand'])
                rr = ik.f(p + 'reach_ratio')[ci]
                sf = ik.f(p + 'shortfall_cm')[ci]
                for r0, r1 in runs_of(((rr >= IK_REACH_RATIO) | (sf > IK_SHORTFALL_CM)) & wc):
                    if tk[ci[r1]] - tk[ci[r0]] >= IK_OVERREACH_S:
                        ev('overreach', arm, ci[r0], ci[max(r0 - 1, 0)], tk[ci[r1]], '%s reach ratio %.2f, shortfall %.1f cm, for %.0f ms' % (arm, rr[r0:r1 + 1].max(), sf[r0:r1 + 1].max(), 1e3 * (tk[ci[r1]] - tk[ci[r0]])), ['hand'])
                hi_ = rel['fore'][ci][:, 1] > rel['upper'][ci][:, 1]
                for r0, r1 in runs_of(hi_ & wc):
                    if tk[ci[r1]] - tk[ci[r0]] >= IK_ELBOW_HIGH_S:
                        ev('elbow_high', arm, ci[r0], ci[max(r0 - 1, 0)], tk[ci[r1]], '%s forearm joint above the upper arm joint for %.0f ms' % (arm, 1e3 * (tk[ci[r1]] - tk[ci[r0]])), ['fore'])
                ed = ik.f(p + 'elbow_deg')[ci]
                for r0, r1 in runs_of(((ed < IK_ELBOW_LO_DEG) | (ed > IK_ELBOW_HI_DEG)) & wc):
                    if tk[ci[r1]] - tk[ci[r0]] >= IK_ELBOW_LIMIT_S:
                        ev('elbow_limit', arm, ci[r0], ci[max(r0 - 1, 0)], tk[ci[r1]], '%s elbow angle %.0f deg, outside %g..%g, for %.0f ms' % (arm, ed[r0:r1 + 1][np.argmax(np.abs(ed[r0:r1 + 1] - 104))], IK_ELBOW_LO_DEG, IK_ELBOW_HI_DEG, 1e3 * (tk[ci[r1]] - tk[ci[r0]])), ['fore'])
        if len(ci) > 1:
            neck = ik.f('final_neck')[ci].astype(np.float64)
            a, b = ci[:-1], ci[1:]
            dn = np.linalg.norm(neck[1:] - neck[:-1], axis=1)
            okp = (pj[a] >= 0) & (pj[b] >= 0)
            mv = np.where(okp, np.linalg.norm(pe[np.maximum(pj[b], 0)] - pe[np.maximum(pj[a], 0)], axis=1), np.inf)
            for q in np.flatnonzero((dn > IK_NECK_JUMP_M) & (mv < IK_PIN_STILL_M)):
                ev('neck_jump', '', b[q], a[q], tk[b[q]], 'final neck jumped %.1f cm in one call while pin.e moved %.1f cm' % (100 * dn[q], 100 * mv[q]), ['neck'])
        bad = ((ex != E_COMP) & ~np.isin(ex, [S.enum('IkExit', x) for x in IK_EXEMPT_EXITS])) | (rf != R_NONE)
        for r0, r1 in runs_of(bad):
            cnt = collections.Counter()
            for j in range(r0, r1 + 1):
                cnt[S.enum_name('IkExit', ex[j]) if ex[j] != E_COMP else 'refuse:' + S.enum_name('IkRefuse', rf[j])] += 1
            ev('refused', '', r0, max(r0 - 1, 0), tk[r1], 'refused %d call(s): %s' % (r1 - r0 + 1, ', '.join('%s x%d' % kv for kv in cnt.most_common())))
        wd = np.flatnonzero(ik.f('write_down') == 1)
        if wd.size:
            ev('write_down', '', wd[0], max(wd[0] - 1, 0), tk[wd[-1]], 'the IK writes stood down (write_down set on %d call(s))' % wd.size)
        events.sort(key=lambda e: e['t0'])
        self.ik = dict(tk=tk, ik=ik, pj=pj)
        counts = collections.Counter()
        for e in events:
            cause, ev_ = self.ik_cause(e)
            counts[(e['kind'], cause.split(':')[0])] += 1
            e['cause'] = cause
            self.findings.append(Finding(e['t0'], e['t1'], 'ik_' + e['kind'], cause, 'IK %s at %s: %s; cause %s%s' % (e['kind'], S.lt(e['t0']), e['detail'], cause, (' (%s)' % ev_) if ev_ else '')))
        self.ik_events_list = events
        self.say('  arm writes: %s (the rules on an arm need it written, with w_arm above 0)' % ', '.join(armstat))
        self.say('  %d completed call(s) of %d; %d event(s)%s' % (len(ci), n, len(events), ': ' + ', '.join('%s/%s x%d' % (k, c, v) for (k, c), v in sorted(counts.items())) if counts else ''))

    def ik_cause(self, e):
        S, I = self.S, self.ik
        tk, ik = I['tk'], I['ik']
        lo = e['t0'] - IK_LOOKBACK_S
        if e['kp'] >= 0:
            lo = min(lo, float(tk[e['kp']]))
        hi = e['t0'] + min(e['t1'] - e['t0'], IK_POST_S)
        ra, rb = sl(tk, lo, hi)
        arms = [e['arm'].lower() + '_'] if e['arm'] else list(ARMS)
        # stall
        gaps = self.gaps + self.ik_gaps
        for a, b in gaps:
            if b >= lo and a <= e['t0'] + STALL_AFTER_S:
                return 'stall', 'a %.0f ms gap ends %s' % (1e3 * (b - a), S.lt(b))
        # rig changed
        for f in ('plan_gen', 'joints'):
            v = ik.f(f)[ra:rb]
            if v.size and (v != v[0]).any():
                return 'rig_changed', '%s changed' % f
        # refused
        E_COMP, R_NONE = S.enum('IkExit', 'completed'), S.enum('IkRefuse', 'none')
        exw, rfw = ik.f('exit')[ra:rb], ik.f('refuse')[ra:rb]
        if e['kind'] != 'refused' and (((exw != E_COMP) & ~np.isin(exw, [S.enum('IkExit', x) for x in IK_EXEMPT_EXITS])) | (rfw != R_NONE)).any():
            return 'refused', 'a refused call in the window'
        if e['kind'] == 'refused':
            return 'refused', ''
        # fp stale
        if (ik.f('fp_stale')[ra:rb] == 1).any() or (ik.f('g_stale')[ra:rb] == 1).any() or (ik.f('fp_age_ms')[ra:rb] > IK_FP_AGE_MS).any():
            return 'fp_stale', 'fp_stale, g_stale or fp_age over %.0f ms' % IK_FP_AGE_MS
        # animation
        k, kp = e['k'], e['kp']
        if kp >= 0 and e['joint']:
            moves = []
            for j in e['joint']:
                if j == 'neck':
                    fa, fb = ik.f('final_neck')[kp].astype(np.float64), ik.f('final_neck')[k].astype(np.float64)
                    aa, ab = ik.f('anim_neck')[kp].astype(np.float64), ik.f('anim_neck')[k].astype(np.float64)
                else:
                    p = (arms[0])
                    fa = ik.f('final_%s%s' % (p, j))[kp].astype(np.float64) - ik.f('final_chest')[kp]
                    fb = ik.f('final_%s%s' % (p, j))[k].astype(np.float64) - ik.f('final_chest')[k]
                    aa = ik.f('anim_%s%s' % (p, j))[kp].astype(np.float64) - ik.f('anim_chest')[kp]
                    ab = ik.f('anim_%s%s' % (p, j))[k].astype(np.float64) - ik.f('anim_chest')[k]
                fm, am = float(np.linalg.norm(fb - fa)), float(np.linalg.norm(ab - aa))
                if ik.f('anim_valid')[kp] == 1 and ik.f('anim_valid')[k] == 1 and fm >= IK_ANIM_MIN_M and am >= IK_ANIM_SHARE * fm:
                    return 'animation', 'the animated %s moved %.1f cm of the output\'s %.1f' % (j, 100 * am, 100 * fm)
        # infeasible
        for p in arms:
            if (ik.f(p + 'reach_ratio')[ra:rb] >= IK_INFEASIBLE_RATIO).any() or (ik.f(p + 'reach_limited')[ra:rb] == 1).any() or (ik.f(p + 'shortfall_cm')[ra:rb] > IK_SHORTFALL_SEEN_CM).any():
                return 'infeasible', '%s reach at or past the limit' % p[0].upper()
        # pole change
        ch = []
        for p in arms:
            for f in ('pole_src', 'swivel_min_violation', 'swivel_fallback', 'f159_reseed', 'f165_applied', 'f192_applied'):
                v = ik.f(p + f)[ra:rb]
                if v.size and (v != v[0]).any():
                    ch.append(p + f)
        if ch:
            return 'pole_change:' + ','.join(c[2:] for c in ch), ''
        # capped
        cap = []
        for p in arms:
            if (ik.f(p + 'swivel_capped')[ra:rb] == 1).any():
                cap.append(p + 'swivel_capped')
        pin = S.tab('pin')
        pt = S.t('pin')
        a0, a1 = sl(pt, lo, hi)
        if a1 > a0:
            for f in ('pin_capped_h', 'pin_capped_v', 'f97_capped'):
                if (pin.f(f)[a0:a1] == 1).any():
                    cap.append(f)
        if cap:
            return 'capped', ', '.join(cap)
        return 'unattributed', ''

    # -- marks ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    def marks(self, number, before, after):
        S = self.S
        mk = S.tab('mark')
        self.say('')
        self.say('== marks')
        if not len(mk) or not self.need('marks', REQ_MARK):
            self.say('  no marks (F9 is hotkey.on_foot_mark)')
            return
        mt = S.t('mark')
        nums = mk.f('number')
        for t, nm in zip(mt, nums):
            if number is not None and int(nm) != number:
                continue
            lo, hi = t - before, t + after
            inside = [f for f in self.findings if f.t1 >= lo and f.t0 <= hi]
            if number is None:
                self.say('  mark %d at %s: %s' % (nm, S.lt(t), ('%d finding(s): ' % len(inside) + '; '.join('%s %s' % (f.kind, f.cause) for f in inside)) if inside else 'no finding in this window (%.1f s before, %.1f after)' % (before, after)))
                continue
            self.say('  mark %d at %s, window %s to %s' % (nm, S.lt(t), S.lt(lo), S.lt(hi)))
            self.mark_table(lo, hi)
            for f in inside:
                self.say('    finding: %s %s: %s' % (S.lt(f.t0), f.kind, f.detail))
            if not inside:
                self.say('    no finding in this window')
        if number is not None and not (nums == number).any():
            self.say('  no mark number %d (marks present: %s)' % (number, ', '.join(str(int(x)) for x in sorted(set(nums.tolist())))))

    def mark_table(self, lo, hi):
        S, L = self.S, self.L
        hdr = '    %-12s %-16s %-5s %-4s %6s %8s | %s | %-8s | L reach grip pole | R reach grip pole' % ('local', 'stance', 'item', 'ADS', 'pitch', 'err/v-n cm', 'game raise  hold weye   ads sight vneck', 'settle')
        self.say(hdr)
        ik = S.tab('ik')
        tk = S.t('ik')
        tb = np.arange(math.floor(lo / 0.25) * 0.25, hi, 0.25)
        for b0 in tb:
            row = ''
            if L is not None:
                m = (L.t >= b0) & (L.t < b0 + 0.25)
                if m.any():
                    mean = lambda v: float(np.nanmean(v[m])) if np.isfinite(v[m]).any() else float('nan')
                    c = int(round(np.median(L.cls[m])))
                    st = S.enum_name('Stance', c // 2) + ('*' if c % 2 else '')
                    cells = [mean(L.game) - float(np.nanmean(L.game[L.cls == c])) if (L.cls == c).any() else mean(L.game)]
                    cells += [SEATED_Y_SIGN * (mean(L.terms[k]) - float(np.nanmean(L.terms[k][L.cls == c]))) if (L.cls == c).any() else SEATED_Y_SIGN * mean(L.terms[k])
                              for k in ('raise', 'hold', 'weapon_eye', 'ads_lock', 'sight', 'vneck')]
                    row = '%-16s %-5s %-4s %6.1f %8s | %s | %-8s' % (st, S.enum_name('Support', int(np.median(L.support[m]))), 'yes' if L.ads[m].mean() > 0.5 else 'no', mean(L.pitch),
                                                                      (fmt_cm(mean(L.err)) if np.isfinite(mean(L.err)) else (('v-n%s' % fmt_cm(mean(L.body) - mean(L.neck_y))) if getattr(L, 'neck_y', None) is not None and np.isfinite(mean(L.body) - mean(L.neck_y)) else 'n/a')), ' '.join('%5s' % fmt_cm(x) for x in cells), S.enum_name('SettleWhy', int(np.median(L.why[m]))))
            ikcol = ''
            if len(ik) and not self.S.missing({'ik': ['l_reach_ratio', 'l_grip_err_cm', 'l_pole_src', 'r_reach_ratio', 'r_grip_err_cm', 'r_pole_src']}):
                m = (tk >= b0) & (tk < b0 + 0.25)
                if m.any():
                    parts = []
                    for p in ARMS:
                        parts.append('%.2f %4.1f %s' % (np.nanmean(ik.f(p + 'reach_ratio')[m]), np.nanmax(ik.f(p + 'grip_err_cm')[m]), S.enum_name('PoleSrc', int(np.median(ik.f(p + 'pole_src')[m])))))
                    ikcol = ' | '.join(parts)
            if row or ikcol:
                self.say('    %-12s %s | %s' % (S.lt(b0), row or '-', ikcol or '-'))

    # -- everything ---------------------------------------------------------------------------------------------------------------------------------------------------------
    def run(self, mark=None, window=(3.0, 1.0), since=None, until=None, show_marks=True):
        S = self.S
        self.header()
        self.build_ledger()
        if self.L is not None:
            self.excursions()
            self.bob()
            self.view_rules()
            self.leak_gains()
            self.f214_section()
            self.f215_section()
            self.f216_section()
        else:
            self.say('')
            self.say('== height excursions, bob, leak gains: not evaluated (no ledger)')
        self.f218_section()
        self.f222_section()
        self.f223_section()
        self.f224_section()
        self.ik_events()
        if since is not None or until is not None:
            lo = since if since is not None else 0.0
            hi = until if until is not None else 86400.0
            self.findings = [f for f in self.findings if lo <= S.tod(f.t0) <= hi]
        self.findings.sort(key=lambda f: f.t0)
        if show_marks:
            self.marks(mark, window[0], window[1])
        self.say('')
        self.say('== findings (%d)' % len(self.findings))
        for f in self.findings[:MAX_LISTED]:
            self.say('  %s %s' % (S.lt(f.t0), f.detail))
        if len(self.findings) > MAX_LISTED:
            self.say('  ... %d more' % (len(self.findings) - MAX_LISTED))
        if not self.findings:
            self.say('  none past a limit')
        for c in self.cannot:
            self.say(c)
        return 3 if any(f.limit for f in self.findings) else 0


# ---- the command line ----------------------------------------------------------------------------------------------------------------------------------------------------
def parse_tod(s):
    m = re.match(r'^(\d{1,2}):(\d{2}):(\d{2})$', s or '')
    if not m:
        raise ValueError('time must be HH:MM:SS: %r' % s)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


def dump(S, name, fields, since, until, out):
    tb = S.tab(name)
    if not len(tb):
        print('[onfoot] no %s records' % name, file=out)
        return 1
    cols = []
    for f in fields:
        if not tb.has(f):
            print('cannot evaluate: %s.%s missing' % (name, f), file=out)
            return 1
        cols.append((f, tb.arr[f]))
    t = S.t(name)
    print('local         seq    ' + ' '.join(f if a.ndim == 1 else ' '.join('%s[%d]' % (f, i) for i in range(a.shape[1])) for f, a in cols), file=out)
    for i in range(len(tb)):
        if since is not None and S.tod(t[i]) < since or until is not None and S.tod(t[i]) > until:
            continue
        cells = []
        for f, a in cols:
            v = a[i]
            cells += ['%.6g' % x for x in np.atleast_1d(v)]
        print('%s %6d %s' % (S.lt(t[i]), tb.arr['seq'][i], ' '.join(cells)), file=out)
    return 0


def resolve_folder(args):
    if args.dir:
        return args.dir
    import edvr_log
    game = edvr_log.resolve_target(args.target)
    cands = [os.path.join(edvr_log.log_dir_for(game), 'onfoot'), os.path.join(game, 'edvr_logs', 'onfoot')]
    for c in cands:
        if os.path.isdir(c):
            return c
    return cands[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--target', default=None, help='steam, frontier or a game directory (frontier is ambiguous on a machine with several: pass the directory)')
    ap.add_argument('--dir', default=None, help='the onfoot folder itself, instead of --target')
    ap.add_argument('--stamp', default=None, help='yyyymmdd_hhmmss of the session; the newest when not given')
    ap.add_argument('--list', action='store_true', help='list the sessions in the folder')
    ap.add_argument('--expect-build', default=None, help='a git ref (HEAD) or a version; exit 2 when the recording is of another build')
    ap.add_argument('--mark', type=int, default=None, help='the F9 mark to print a table and findings for')
    ap.add_argument('--window-s', type=float, nargs=2, default=(3.0, 1.0), metavar=('BEFORE', 'AFTER'))
    ap.add_argument('--since', default=None, help='HH:MM:SS local: findings from this time')
    ap.add_argument('--until', default=None, help='HH:MM:SS local: findings to this time')
    ap.add_argument('--dump', default=None, metavar='RECORD', help='print raw columns of a record type')
    ap.add_argument('--fields', default=None, help='with --dump: a,b,c')
    ap.add_argument('--root', default=None, help='repository to resolve --expect-build against')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--verify-fixture', default=None, metavar='FOLDER')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify_fixture:
        return verify_fixture(args.verify_fixture)
    if not args.target and not args.dir:
        print('[onfoot] name a recording: --target <game dir> or --dir <onfoot folder>')
        return 1
    try:
        since = parse_tod(args.since) if args.since else None
        until = parse_tod(args.until) if args.until else None
    except ValueError as e:
        print('[onfoot] %s' % e)
        return 1
    try:
        folder = resolve_folder(args)
    except SystemExit as e:
        print(e)
        return 1
    stamps = list_stamps(folder)
    if not stamps:
        print('[onfoot] no recording in %s (log.on_foot_detail must be on while on foot)' % folder)
        return 1
    if args.list:
        for st in sorted(stamps):
            tot = sum(os.path.getsize(p) for p in stamps[st])
            try:
                hdr = read_segment(stamps[st][0])[0]
                info = 'build %s, opened %s' % (hdr.get('build'), hdr.get('local_time_at_open'))
            except RecError as e:
                info = str(e)
            print('%s  %d file(s)  %.1f MB  %s' % (st, len(stamps[st]), tot / 1048576.0, info))
        return 0
    stamp = args.stamp or max(stamps)
    try:
        S = load_session(folder, stamp)
    except RecError as e:
        print('[onfoot] %s' % e)
        return 1
    if args.expect_build:
        import edvr_log
        root = os.path.abspath(args.root) if args.root else edvr_log.repo_root()
        want = edvr_log.expected_version(args.expect_build, root)
        if edvr_log.version_matches(S.build, want):
            print('[onfoot] build matches: %s' % want)
        else:
            print('[onfoot] BUILD MISMATCH\n         recording says %s\n         expected       %s\n         This recording is not evidence about that build.' % (S.build or '(nothing)', want))
            return 2
    if args.dump:
        return dump(S, args.dump, [f for f in (args.fields or '').split(',') if f], since, until, sys.stdout)
    A = Analysis(S)
    rc = A.run(args.mark, tuple(args.window_s), since, until)
    print('[onfoot] session %s in %s' % (stamp, folder))
    print('\n'.join(A.out))
    return rc


# ---- --verify-fixture ----------------------------------------------------------------------------------------------------------------------------------------------------
def fixture_value(rid, name, fi, e, typ, n, enum_count, rec_is_mark):
    base = 100 * rid + 7 * fi + e
    if typ == 'u8':
        return base % enum_count if enum_count else base % 200
    if typ == 'u16':
        return base
    if typ in ('u32', 'u64'):
        return base + 1000000 * n
    return base + 0.25 + n


def verify_fixture(folder):
    stamps = list_stamps(folder)
    bad = []
    if not stamps:
        print('[onfoot] --verify-fixture: no recording in %s' % folder)
        return 1
    S = load_session(folder, max(stamps))
    h = S.hdrs[0]
    if S.build != 'v0.0.0-fixture' or str(S.game_build) != '334925' or S.pid != 1234 or S.freq != 10000000 or S.qpc0 != 5000:
        bad.append('header: build %r game_build %r pid %r qpc_freq %r qpc_at_open %r' % (S.build, S.game_build, S.pid, S.freq, S.qpc0))
    if h.get('local_time_at_open') != '2026-01-02 03:04:05.678' or h.get('keys', {}).get('fix.native_on_foot_eye_height_cm') != 170.0:
        bad.append('header: local time or keys')
    checked = 0
    for name, rec in h['records'].items():
        tb = S.tab(name)
        if len(tb) != 3:
            bad.append('%s: %d record(s), want 3' % (name, len(tb)))
            continue
        for n in range(3):
            row = tb.arr[n]
            if int(row['id']) != rec['id'] or int(row['size']) != rec['size'] or int(row['seq']) != n or int(row['qpc']) != 1000 * rec['id'] + n:
                bad.append('%s #%d: prefix id %s size %s seq %s qpc %s' % (name, n, row['id'], row['size'], row['seq'], row['qpc']))
            for fi, f in enumerate(rec['fields']):
                en = len(h['enums'][f['enum']]) if f.get('enum') else 0
                vals = np.atleast_1d(row[f['name']])
                for e in range(f['count']):
                    want = fixture_value(rec['id'], f['name'], fi, e, f['type'], n, en, name == 'mark')
                    if name == 'mark' and fi == 0:
                        want = 1 + n
                    checked += 1
                    if abs(float(vals[e]) - want) > 1e-3:
                        bad.append('%s #%d %s[%d]: %r, want %r' % (name, n, f['name'], e, float(vals[e]), want))
    for name, fields in REQ_ALL.items():
        have = {f['name'] for f in h['records'].get(name, {}).get('fields', [])}
        for f in fields:
            if f not in have:
                bad.append('the rules read %s.%s, which the recorder\'s schema does not have' % (name, f))
    for line in bad[:40]:
        print('FIXTURE MISMATCH: ' + line)
    if bad:
        print('onfoot_check --verify-fixture: %d mismatch(es) (of %d values checked)' % (len(bad), checked))
        return 1
    print('onfoot_check --verify-fixture: ok, %d values and every field the rules read' % checked)
    return 0


# ---- the self-test: a synthetic recording written through the same format ----------------------------------------------------------------------------------------------------------
def _S(t, *names):
    return [(n, t, 1, None) for n in names]


def _A(t, c, *names):
    return [(n, t, c, None) for n in names]


def _E(en, name):
    return [(name, 'u8', 1, en)]


def _arm(p):
    return (_S('f32', *[p + x for x in ('reach_ratio', 'shortfall_cm', 'w_arm', 'swivel_deg', 'elbow_deg', 'clav_deg', 'grip_err_cm', 'swivel_cut_deg')]) + [])


def _arm8(p):
    return _S('u8', p + 'ok', p + 'wrote') + _E('PoleSrc', p + 'pole_src') + _S('u8', *[p + x for x in ('swivel_capped', 'swivel_limited', 'swivel_min_violation', 'swivel_fallback', 'reach_limited', 'assisted',
                                                                                                          'f98_seed_kept', 'f159_reseed', 'f165_applied', 'f192_applied')]) + _E('Hold', p + 'hold')


SYN_ENUMS = {
    'VneckReason': ['applied', 'look_to_aim', 'out_of_scope', 'no_aim', 'no_head'], 'SrcKind': ['matched', 'legacy', 'missed'],
    'PinRefuse': ['no_input', 'stale_input', 'matrix', 'no_legs', 'leg', 'write', 'camera_away', 'none', 'not_run'], 'SettleWhy': ['settled', 'moving', 'neck_moving', 'cam_moving', 'filling', 'invalid'],
    'Stance': ['standing', 'crouched', 'crouch_moving', 'moving', 'standing_ads', 'crouched_ads', 'crouch_moving_ads', 'moving_ads'], 'Support': ['none', 'pistol', 'tool'],
    'EyeSource': ['unarmed', 'unarmed_no_class', 'stance', 'standing_over_crouch', 'start', 'hunched', 'none'], 'F193Source': ['hip', 'legs', 'camera'],
    'IkExit': ['completed', 'ik_off', 'plan_torn', 'plan_mismatch', 'inputs_refused', 'bad_joint_count', 'f166_write', 'f180_write', 'f169_write', 'pin_write', 'span_write'],
    'IkRefuse': ['buffer', 'plan_torn', 'plan', 'fp_torn', 'fp_stale', 'g_torn', 'g_stale', 'side', 'singular', 'write', 'fp_rig', 'none'], 'PoleSrc': ['legacy_swivel', 'stable_seed', 'body_down'],
    'PinState': ['still', 'walk', 'run', 'air', 'crouched'], 'F214State': ['off', 'not_moving', 'filling', 'followed', 'held_aim', 'held_head', 'held_pose', 'held_posture', 'held_item', 'held_f138', 'held_away', 'not_run'], 'F215State': ['off', 'released_away', 'released_item', 'released_posture', 'held_aim', 'held_pose', 'filling', 'active', 'held_sights', 'active_item', 'not_run'], 'Hold': ['none', 'f69h', 'f192'], 'RecentreReason': ['applied', 'not_on_foot', 'stored_seat_due', 'rot_calibrate', 'head_untracked', 'replace_refused']}

SYN_SCHEMA = collections.OrderedDict()
SYN_SCHEMA['align'] = (1, _A('f32', 3, 'eye', 'run', 'ads_lock', 'sight', 'weapon_view', 'weapon_eye', 'virtual_neck', 'bob_seated') + _A('f32', 12, 'head_render', 'head_local') +
                       _S('f32', 'hold_y', 'raise_y', 'raise_z', 'neck_seated_y', 'ads_turn_dy', 'ads_turn_dz', 'turret_z', 'aim_rad', 'raise_target', 'raise_age_ms', 'raise_ramp', 'raise_m', 'bob_w') +
                       _S('u32', 'serial', 'eye_align_applied') + _S('u8', 'align_mode', 'pin_scope', 'on_foot', 'cam_external', 'ads', 'aim_fresh', 'published', 'raise_turned', 'ads_turned',
                                                                     'have_frame', 'local_valid', 'raise_clamped', 'raise_following') + _E('VneckReason', 'vneck_reason'))
SYN_SCHEMA['p1'] = (2, _A('f64', 3, 'cam', 'e') + _A('f32', 3, 'sub_ads', 'sub_vneck', 'sub_weapon_view', 'sub_bob', 'hud_shift') + _S('f32', 'sub_raise', 'aim_pitch_deg', 'bob_w') + _S('u32', 'serial_carried', 'game_frame') +
                    _S('u8', 'have_ads_shift', 'hud_shift_on') + _E('SrcKind', 'src') + _E('SrcKind', 'vneck_src'))
SYN_SCHEMA['pin'] = (3, _A('f64', 3, 'root') + _S('f64', 'root_vy', 'root_speed') + _A('f64', 3, 'e', 'neck_anim', 'neck_target', 'neck_applied') +
                     _S('f64', 'delta', 'demand_v', 'demand_h', 'e_age_ms', 'detach_w', 'posture_w', 'aim_pitch_deg', 'eye_y_selected', 'raw_eye_y', 'f98_eye_y', 'raise_target', 'f95_target', 'wc',
                        'f97_w', 'f97_asked', 'f179_stand_w', 'f179_stand_raise', 'f179_legs_room', 'f193_neck_y', 'f193_eye_ask', 'guard_push_cm', 'guard_pred_cm', 'guard_dist_cm',
                        'stored_neck_stand', 'stored_neck_crouch', 'stored_cam_stand', 'stored_cam_crouch', 'f214_target', 'f215_body_y', 'f215_target') + _S('u32', 'item_serial', 'p1') + _A('f32', 2, 'heading') + _A('f32', 3, 'up', 'fwd') + _S('f32', 'f218_offset_m', 'f223_pop_m', 'f224_lead_m') +
                     _S('u8', 'valid') + _E('PinRefuse', 'refuse') + _S('u8', 'settled') + _E('SettleWhy', 'settle_why') + _S('u8', 'f138_refused') + _E('Stance', 'stance') +
                     _E('PinState', 'pin_state') + _E('Support', 'support') + _E('EyeSource', 'eye_source') + _S('u8', 'hunched', 'pin_capped_h', 'pin_capped_v', 'f97_capped', 'f107_began') +
                     _E('F193Source', 'f193_source') + _S('u8', 'f193_camera', 'f179_ran', 'f179_measure', 'have_stored_stand', 'have_stored_crouch', 'armed', 'aim_known') + _E('F214State', 'f214_state') + _E('F215State', 'f215_state') + _S('u8', 'f218_held', 'f218_applied', 'f218_refused', 'f226_head_pitched'))
_ik = _S('u64', 'plan_x', 'fp_x') + _S('u32', 'plan_gen', 'fp_gen', 'fp_seq', 'fp_frame', 'g_frame', 'p1', 'item_serial', 'fp_age_ms', 'g_age_ms', 'joints') + \
    _S('f32', 'spine_w', 'spine_pitch_rad', 'spine_roll_rad', 'square_rad', 'f166_w', 'f180_w', 'f169_w', 'off_w', 'f167_release', 'armed_w', 'base_w') + _arm('l_') + _arm('r_')
for _w in ('anim', 'final'):
    _ik += _A('f32', 3, _w + '_neck', _w + '_chest', _w + '_l_clav', _w + '_r_clav', _w + '_l_upper', _w + '_l_fore', _w + '_l_hand', _w + '_r_upper', _w + '_r_fore', _w + '_r_hand',
              _w + '_pelvis', _w + '_l_foot', _w + '_r_foot')
_ik += _A('u16', 13, 'joint_idx') + _E('IkExit', 'exit') + _E('IkRefuse', 'refuse') + _S('u8', 'write_down', 'pin_called', 'pin_valid', 'fp_stale', 'g_stale', 'armed', 'bend_have', 'bend_write',
                                                                                  'square_have', 'f166_ran', 'f180_ran', 'f169_ran', 'anim_valid', 'final_valid') + _arm8('l_') + _arm8('r_')
SYN_SCHEMA['ik'] = (4, _ik)
SYN_SCHEMA['mark'] = (5, _S('u32', 'number'))
SYN_SCHEMA['rt_eye'] = (6, _A('f64', 3, 'wanted', 'target', 'applied') + _S('f64', 'seat_yaw_deg') + _S('u64', 'applies', 'refusals', 'origin_sets') + _S('u32', 'serial_seen', 'serial_applied') +
                        _S('u8', 'deferred', 'frame_open', 'seat_ahead', 'changed'))
SYN_SCHEMA['rt_recentre'] = (7, _A('f64', 3, 'origin_before', 'origin_after') + _S('f64', 'moved_cm') + _S('u32', 'serial') + _S('u8', 'ok') + _E('RecentreReason', 'reason') + _S('u8', 'retry'))


def syn_records(drop=None):
    """The header's record table, laid out like the C++ one: fields in order, naturally aligned, the size a multiple of 8."""
    recs = collections.OrderedDict()
    for name, (rid, fields) in SYN_SCHEMA.items():
        off, out = 16, []
        for fname, typ, count, en in fields:
            if drop and fname in drop.get(name, ()):
                continue
            sz = {'u8': 1, 'u16': 2, 'u32': 4, 'u64': 8, 'f32': 4, 'f64': 8}[typ]
            off = (off + sz - 1) // sz * sz
            d = dict(name=fname, type=typ, count=count, offset=off)
            if en:
                d['enum'] = en
            out.append(d)
            off += sz * count
        recs[name] = dict(id=rid, size=(off + 7) // 8 * 8, fields=out)
    return recs


def syn_header(seg, build, drop=None, local='2026-11-05 14:00:00.000'):
    j = json.dumps(dict(format=1, build=build, game_build='334925', pid=4242, qpc_freq=10000000, qpc_at_open=0, local_time_at_open=local, segment=seg,
                        keys={'log.on_foot_detail': 'on'}, prefix=[dict(name='id', type='u16', count=1, offset=0), dict(name='size', type='u16', count=1, offset=2),
                                                                   dict(name='seq', type='u32', count=1, offset=4), dict(name='qpc', type='u64', count=1, offset=8)],
                        records=syn_records(drop), enums=SYN_ENUMS), separators=(',', ':'))
    while (12 + len(j)) % 8:
        j += ' '
    return MAGIC + struct.pack('<I', len(j)) + j.encode()


def write_session(folder, stamp, tables, build='v0.0.0-1-gabcdef0', segments=1, drop=None, chop=0):
    """tables: name -> dict(t=seconds, and field arrays); a 'seq' array keeps a gap. Writes the records in time order, split over `segments` files."""
    os.makedirs(folder, exist_ok=True)
    recs = syn_records(drop)
    rows, keys = {}, []
    for name, tb in tables.items():
        rec = recs[name]
        n = len(tb['t'])
        if n == 0:
            continue
        dt = build_dtype(rec, [dict(name='id', type='u16', offset=0), dict(name='size', type='u16', offset=2), dict(name='seq', type='u32', offset=4), dict(name='qpc', type='u64', offset=8)])
        arr = np.zeros(n, dtype=dt)
        arr['id'], arr['size'] = rec['id'], rec['size']
        arr['seq'] = tb['seq'] if 'seq' in tb else np.arange(n)
        arr['qpc'] = np.round(np.asarray(tb['t']) * 1e7).astype(np.uint64)
        for k, v in tb.items():
            if k in ('t', 'seq') or k not in dt.names:
                continue
            arr[k] = v
        rows[name] = arr.view(np.uint8).reshape(n, rec['size'])
        keys += [(float(tb['t'][i]), rec['id'], name, i) for i in range(n)]
    keys.sort(key=lambda x: (x[0], x[1]))
    cut = [len(keys) * s // segments for s in range(segments + 1)]
    for s in range(segments):
        body = b''.join(rows[nm][i].tobytes() for _, _, nm, i in keys[cut[s]:cut[s + 1]])
        if chop and s == segments - 1:
            body = body[:-chop]
        with open(os.path.join(folder, 'onfoot_%s_%03d.rec' % (stamp, s)), 'wb') as fh:
            fh.write(syn_header(s, build, drop) + body)


class Sim:
    """A 90 Hz walk, stand, crouch and ADS session (24 s); faults edit the arrays before the tables are made."""
    PHASES = [(0.0, 0, 0, 0), (3.0, 0, 1, 0), (9.0, 1, 1, 0), (14.0, 0, 1, 0), (19.0, 4, 1, 1), (22.0, 0, 1, 0)]
    T0 = 6.0

    def __init__(self, dur=24.0, seed=1, smooth=None, noise=0.0003):
        rng = np.random.default_rng(seed)
        self.N = int(dur * 90)
        self.t = 0.2 + np.arange(self.N) / 90.0
        t = self.t
        ph = self.PHASES
        self.stance = np.zeros(self.N, int)
        self.armed = np.zeros(self.N, int)
        self.ads = np.zeros(self.N, int)
        for ts, st, ar, ad in ph:
            m = t >= ts
            self.stance[m], self.armed[m], self.ads[m] = st, ar, ad
        self.tcs = [p[0] for p in ph[1:]]
        sm = {tc: 0.35 for tc in self.tcs}
        sm.update(smooth or {})

        def curve(fn):
            xs, ys = [0.0], [fn(*ph[0][1:])]
            for tc, (_, st, ar, ad) in zip(self.tcs, ph[1:]):
                xs += [tc, tc + sm[tc]]
                ys += [ys[-1], fn(st, ar, ad)]
            return np.interp(t, xs, ys)
        self.game = curve(lambda st, ar, ad: 1.0 if st in (1, 5) else 1.5) + rng.normal(0, noise, self.N)
        self.term = dict(
            raise_=curve(lambda st, ar, ad: 0.02 if st in (1, 5) else 0.04), hold=curve(lambda st, ar, ad: 0.012 if st in (1, 5) else 0.010),
            weapon_eye=curve(lambda st, ar, ad: 0.005 if ar else 0.0), ads_lock=curve(lambda st, ar, ad: 0.015 if ad else 0.0), sight=curve(lambda st, ar, ad: 0.010 if ad else 0.0),
            wv=curve(lambda st, ar, ad: 0.003 if ar else 0.0), vneck=np.zeros(self.N), run=np.zeros(self.N), ads_turn=np.zeros(self.N))
        self.settled = np.ones(self.N, int)
        self.why = np.zeros(self.N, int)
        for tc in self.tcs:
            m = (t >= tc) & (t < tc + sm[tc] + 0.25)
            self.settled[m], self.why[m] = 0, 1
        self.head = 1.6 + 0.006 * np.sin(2 * np.pi * 0.25 * t) + 0.002 * np.sin(2 * np.pi * 0.9 * t)
        self.pitch = np.zeros(self.N)
        self.dgame = np.zeros(self.N)
        self.item = np.where(self.armed == 1, 7, 0)
        self.tables = None
        self.head_gain = 0.0

    def win(self, a, b):
        return (self.t >= a) & (self.t < b)

    def build(self):
        N, t, T = self.N, self.t, self.term
        SG = SEATED_Y_SIGN
        game = self.game + self.dgame
        hd = self.head - 1.6
        ssub = T['raise_'] + T['vneck'] + T['ads_lock'] + T['sight']
        eye_y = T['run'] + T['ads_lock'] + T['sight'] + T['wv'] + T['weapon_eye'] + T['hold'] + T['raise_'] + T['ads_turn'] + T['vneck']
        e1 = game + self.head_gain * hd + SG * (T['hold'] + T['weapon_eye'] + T['ads_turn'] + T['run'])   # pin.e excludes the physical head (gain 0)
        z = np.zeros(N)
        ser = np.arange(1, N + 1)
        align = dict(t=t - 0.006, serial=ser, published=np.ones(N, int), on_foot=np.ones(N, int), local_valid=np.ones(N, int), raise_age_ms=np.full(N, 50.0), ads=self.ads.copy(),
                     hold_y=T['hold'], raise_y=T['raise_'], neck_seated_y=T['raise_'], ads_turn_dy=T['ads_turn'])
        align['eye'] = np.stack([z, eye_y, z], 1)
        for nm, v in (('run', T['run']), ('ads_lock', T['ads_lock']), ('sight', T['sight']), ('weapon_view', T['wv']), ('weapon_eye', T['weapon_eye']), ('virtual_neck', T['vneck'])):
            align[nm] = np.stack([z, v, z], 1)
        hl = np.zeros((N, 12))
        hl[:, [0, 5, 10]] = 1.0
        hl[:, 7] = self.head
        align['head_local'] = hl
        p1 = dict(t=t - 0.004, serial_carried=ser, game_frame=np.arange(1, N + 1), src=np.zeros(N, int), sub_raise=T['raise_'])
        p1['sub_vneck'] = np.stack([z, T['vneck'], z], 1)
        p1['sub_ads'] = np.stack([z, T['ads_lock'] + T['sight'], z], 1)
        p1['sub_weapon_view'] = np.stack([z, SG * T['wv'], z], 1)
        p1['e'] = np.stack([z, e1, z + 0.1], 1)
        p1['cam'] = np.stack([z, e1 + SG * ssub + SG * T['wv'], z + 0.1], 1)
        neck = 1.40 + (self.game - 1.5)
        pin = dict(t=t.copy(), valid=np.ones(N, int), refuse=np.full(N, 7), settled=self.settled.copy(), settle_why=self.why.copy(), stance=self.stance.copy(), armed=self.armed.copy(),
                   support=np.where(self.armed == 1, 1, 0), item_serial=self.item, p1=np.arange(1, N + 1), e_age_ms=np.full(N, 4.0), detach_w=self.ads.astype(float),
                   aim_pitch_deg=self.pitch, aim_known=np.ones(N, int), raw_eye_y=e1, f98_eye_y=self.game, eye_source=np.full(N, 2), f179_ran=np.ones(N, int),
                   f179_legs_room=np.full(N, 0.3), stored_neck_stand=np.full(N, 1.40), stored_neck_crouch=np.full(N, 0.95), stored_cam_stand=np.full(N, 1.5), stored_cam_crouch=np.full(N, 1.0))
        pin['e'] = np.stack([z, e1, z + 0.1], 1)
        pin['neck_anim'] = np.stack([z, neck, z], 1)
        pin['neck_applied'] = pin['neck_anim']
        pin['heading'] = np.stack([z, z + 1.0], 1)
        pin['up'] = np.stack([z, z + 1.0, z], 1)
        pin['fwd'] = np.stack([z, z, z + 1.0], 1)
        align['raise_m'] = -T['raise_']
        ik = dict(t=t + 0.001, exit=np.zeros(N, int), refuse=np.full(N, 11), anim_valid=np.ones(N, int), final_valid=np.ones(N, int), plan_gen=np.full(N, 3), joints=np.full(N, 80),
                  armed=self.armed.copy())
        chest = np.stack([z, 1.2 + (self.game - 1.5), z], 1)
        ik['final_chest'] = chest
        ik['anim_chest'] = chest.copy()
        ik['final_neck'] = np.stack([z, neck, z], 1)
        ik['anim_neck'] = ik['final_neck'].copy()
        for p, sx in (('l_', -1.0), ('r_', 1.0)):
            for j, rel in (('upper', (0.18, 0.0, 0.0)), ('fore', (0.28, -0.22, 0.10)), ('hand', (0.15, -0.20, 0.45))):
                v = chest + np.array([sx * rel[0], rel[1], rel[2]])
                ik['final_%s%s' % (p, j)] = v
                ik['anim_%s%s' % (p, j)] = v.copy()
            ik[p + 'reach_ratio'] = np.full(N, 0.8)
            ik[p + 'w_arm'] = self.armed.astype(float)
            ik[p + 'swivel_deg'] = np.full(N, 40.0)
            ik[p + 'elbow_deg'] = np.full(N, 110.0)
            ik[p + 'grip_err_cm'] = np.full(N, 0.5)
            ik[p + 'ok'] = np.ones(N, int)
            ik[p + 'wrote'] = np.ones(N, int)
            ik['final_%sfoot' % p] = np.stack([z + sx * 0.1, z, z], 1)
            ik['anim_%sfoot' % p] = ik['final_%sfoot' % p].copy()
        ik['final_pelvis'] = chest + np.array([0.0, -0.3, 0.0])
        ik['anim_pelvis'] = ik['final_pelvis'].copy()
        rte = dict(t=t + 0.002, wanted=np.stack([z, eye_y, z], 1), applied=np.stack([z, eye_y, z], 1), serial_seen=ser, serial_applied=ser, deferred=np.zeros(N, int), refusals=np.zeros(N, int))
        rtc = dict(t=np.array([0.15]), ok=np.array([1]), reason=np.array([0]), moved_cm=np.array([0.2]))
        mk = dict(t=np.array([5.0, 11.0]), number=np.array([1, 2]))
        self.tables = dict(align=align, p1=p1, pin=pin, ik=ik, rt_eye=rte, rt_recentre=rtc, mark=mk)
        return self.tables

    def drop(self, a, b, names=None):
        """Records vanish in [a, b) (a stall): no record, so no seq gap."""
        for nm, tb in self.tables.items():
            if names and nm not in names or nm in ('mark', 'rt_recentre'):
                continue
            keep = ~((tb['t'] >= a) & (tb['t'] < b))
            for k in list(tb):
                tb[k] = tb[k][keep]


def _height_fault(name):
    """-> (pre(sim), post(sim)) for a seeded height fault around T0 (6.0 s, standing armed, 3 s after the last change)."""
    a, b = Sim.T0, Sim.T0 + 0.5
    SG = SEATED_Y_SIGN

    def plus(s, lo=a, hi=b, v=0.03):
        s.dgame[s.win(lo, hi)] += v
    pre, post = plus, (lambda s: None)
    if name == 'stall':
        pre = lambda s: plus(s, a + 0.3, a + 0.8)
        post = lambda s: s.drop(a, a + 0.3)
    elif name == 'recentre':
        def post(s):
            r = s.tables['rt_recentre']
            for k in r:
                r[k] = np.concatenate([r[k], [a if k == 't' else 1 if k == 'ok' else 0.0]])
    elif name == 'runtime_not_applied':
        def post(s):
            s.tables['rt_eye']['deferred'][s.win(a, a + 0.3)] = 1
    elif name == 'subtraction_miss':
        def post(s):
            s.tables['p1']['src'][s.win(a, b) ] = 2
    elif name == 'camera_away':
        pre = lambda s: plus(s, a + 0.1, a + 0.6)

        def post(s):
            m = s.win(a, a + 0.1)
            s.tables['pin']['valid'][m], s.tables['pin']['refuse'][m] = 0, 6
            s.tables['align']['cam_external'] = (s.win(a, a + 0.1)).astype(int)
    elif name == 'saturation':
        def post(s):
            s.tables['pin']['pin_capped_v'] = s.win(a, b).astype(int)
    elif name == 'wrong_stance_store':
        def post(s):
            s.tables['pin']['stored_neck_stand'] = np.where(s.t >= a, 1.45, 1.40)
    elif name == 'settle_starved':
        pre = lambda s: plus(s, a, a + 1.2)

        def post(s):
            m = s.win(a - 0.4, a + 1.4)
            s.tables['pin']['settled'][m], s.tables['pin']['settle_why'][m] = 0, 1
            s.tables['align']['raise_age_ms'][m] = 400.0
    elif name == 'game_camera':
        pass
    elif name == 'term:raise':
        def pre(s):
            s.term['raise_'][s.win(a, b)] += SG * 0.03
    elif name == 'unattributed':
        def pre(s):
            s.dgame[s.win(a, b)] += 0.012
            s.term['raise_'][s.win(a, b)] += SG * 0.010
            s.term['hold'][s.win(a, b)] += SG * 0.008
    return pre, post


def _ik_fault(name, t0=8.0):
    """A left elbow jump of 8 cm at t0 with the evidence of one cause."""
    def post(s):
        ik = s.tables['ik']
        m = ik['t'] >= t0
        ik['final_l_fore'] = ik['final_l_fore'] + np.where(m, 1.0, 0.0)[:, None] * np.array([0.08, 0, 0])
        w2 = (ik['t'] >= t0) & (ik['t'] < t0 + 0.02)
        if name == 'stall':
            s.drop(t0 - 0.3, t0)
        elif name == 'rig_changed':
            ik['plan_gen'] = np.where(m, 4, 3)
        elif name == 'refused':
            k = np.flatnonzero(ik['t'] < t0)[-1]
            ik['exit'][k] = 2
        elif name == 'fp_stale':
            ik['fp_stale'] = w2.astype(int)
        elif name == 'animation':
            ik['anim_l_fore'] = ik['anim_l_fore'] + np.where(m, 1.0, 0.0)[:, None] * np.array([0.08, 0, 0])
        elif name == 'infeasible':
            ik['l_reach_ratio'] = np.where(w2, 1.0, 0.8)
        elif name == 'pole_change':
            ik['l_pole_src'] = np.where(m, 1, 0)
        elif name == 'capped':
            ik['l_swivel_capped'] = w2.astype(int)
    return (lambda s: None), post


def _make(folder, stamp, pre=(), post=(), **kw):
    sim = Sim(smooth=kw.pop('smooth', None), noise=kw.pop('noise', 0.0003))
    for f in pre:
        f(sim)
    sim.build()
    for f in post:
        f(sim)
    write_session(folder, stamp, sim.tables, **kw)
    return sim


def _run(folder, *args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(['--dir', folder] + list(args))
    return rc, buf.getvalue()


def self_test():
    fails, passes = [], [0]

    def check(ok, what):
        if ok:
            passes[0] += 1
        else:
            fails.append(what)
            print('FAIL: ' + what)
    tmp = tempfile.mkdtemp(prefix='onfoot_check_')
    try:
        cnt = [0]

        def fresh():
            cnt[0] += 1
            d = os.path.join(tmp, 's%d' % cnt[0])
            return d

        def analyse(d, stamp='20261105_140000'):
            S = load_session(d, stamp)
            A = Analysis(S)
            A.run()
            return S, A

        stamp = '20261105_140000'
        # the schema the rules read is in the writer's schema (the C++ one is pinned by --verify-fixture)
        recs = syn_records()
        for nm, fields in REQ_ALL.items():
            have = {f['name'] for f in recs[nm]['fields']}
            check(all(f in have for f in fields), 'the rules read %s fields the test schema lacks: %s' % (nm, [f for f in fields if f not in have]))
        # a clean session: zero findings, every join complete, exit 0
        d = fresh()
        _make(d, stamp)
        S, A = analyse(d)
        check(not A.findings, 'a clean walk, stand, crouch and ADS session has no finding: %s' % [f.detail[:90] for f in A.findings])
        check(A.joins['p1_al'] > 0.99 and A.joins['pin_p1'] > 0.99, 'the clean session joins fully')
        check(A.L.gain == HEAD_GAIN == 0.0 and A.L.gain_fit is None, 'the clean ledger takes no head out, and a head that moved under 1 cm rms is not fitted: %s' % A.L.gain_fit)
        check(len(A.L.refs) == 4, 'a reference per stance and armed: %s' % A.L.refs.keys())
        check(abs((A.L.body - A.L.game - SEATED_Y_SIGN * A.L.eye_y + A.L.by)).max() < 1e-9, 'the ledger identity body = game + sign * eye - body_y holds')
        rc, out = _run(d)
        check(rc == 0, 'clean exit code 0, got %s' % rc)
        check('runtime records: rt_eye' in out and 'mark 1' in out and 'mark 2' in out, 'the header names the runtime records and the marks')
        rc, out = _run(d, '--mark', '1')
        check(rc == 0 and 'no finding in this window' in out, 'a clean mark window says no finding in this window')
        rc, out = _run(d, '--expect-build', 'v0.0.0-1-gabcdef0')
        check(rc == 0 and 'build matches' in out, 'a matching --expect-build passes')
        rc, out = _run(d, '--expect-build', 'v9.9.9-1-g1234567')
        check(rc == 2 and 'BUILD MISMATCH' in out, 'a different --expect-build exits 2')
        rc, out = _run(d, '--list')
        check(rc == 0 and stamp in out, '--list names the session')
        rc, out = _run(d, '--dump', 'pin', '--fields', 'stance,e')
        check(rc == 0 and 'e[0]' in out and out.count('\n') > 100, '--dump prints columns')
        rc, out = _run(d, '--dump', 'pin', '--fields', 'nonsense')
        check(rc == 1 and 'cannot evaluate: pin.nonsense missing' in out, '--dump of a missing field says so')
        rc, out = _run(os.path.join(tmp, 'empty'))
        check(rc == 1, 'no recording exits 1')
        check(main([]) == 1, 'no target exits 1')

        # a seeded fault per height cause
        for cause, want in (('stall', 'stall'), ('recentre', 'recentre'), ('runtime_not_applied', 'runtime_not_applied'), ('subtraction_miss', 'subtraction_miss'),
                            ('camera_away', 'camera_away'), ('saturation', 'saturation'), ('wrong_stance_store', 'wrong_stance_store'), ('settle_starved', 'settle_starved'),
                            ('game_camera', 'game_camera'), ('term:raise', 'term:raise'), ('unattributed', 'unattributed')):
            d = fresh()
            pre, post = _height_fault(cause)
            _make(d, stamp, [pre], [post])
            S, A = analyse(d)
            hx = [f for f in A.findings if f.kind == 'height_excursion']
            check(len(hx) == 1 and hx[0].cause == want, 'height fault %s comes back as %s: %s' % (cause, want, [(f.kind, f.cause) for f in A.findings]))
            check(all(f.kind == 'height_excursion' for f in A.findings), 'height fault %s makes no other finding: %s' % (cause, [(f.kind, f.cause) for f in A.findings]))
            if cause == 'game_camera':
                rc, out = _run(d, '--mark', '1', '--window-s', '3', '2')
                check(rc == 3 and 'height_excursion' in out and 'no finding in this window' not in out, 'a mark window holding the fault prints it and exits 3')
                rc, out = _run(d, '--mark', '2')
                check('no finding in this window' in out, 'the other mark window has no finding')
                rc, out = _run(d)
                check(rc == 3 and 'game_camera' in out, 'a fault gives exit 3')
        # a slow settle
        d = fresh()
        _make(d, stamp, smooth={14.0: 1.6})
        S, A = analyse(d)
        check(any(f.kind == 'slow_settle' for f in A.findings), 'a transition that takes 1.6 s is slow_settle: %s' % [(f.kind, f.cause) for f in A.findings])
        # bob
        d = fresh()

        def bobpre(s):
            m = s.win(5.0, 8.0)
            s.dgame[m] += 0.006 * np.sin(2 * np.pi * 4.0 * s.t[m])
        _make(d, stamp, [bobpre])
        S, A = analyse(d)
        b = [f for f in A.findings if f.kind == 'bob']
        check(len(b) == 1 and b[0].cause == 'game' and not [f for f in A.findings if f.kind == 'height_excursion'], 'a 6 mm shake is bob caused by the game: %s' % [(f.kind, f.cause) for f in A.findings])

        # the leak: a planted gain comes back
        def flat_sim(k_armed, k_thin):
            s_ = Sim(noise=0.0)
            s_.stance[:] = 0
            s_.armed[:] = 1
            s_.ads[:] = 0
            s_.item[:] = 7
            s_.settled[:] = 1
            s_.why[:] = 0
            s_.head[:] = 1.6
            s_.game[:] = 1.5
            for k in s_.term:
                s_.term[k][:] = 0.0
            s_.term['hold'][:] = 0.01
            s_.pitch[:] = 60.0
            r = 0.30 * np.sin(2 * np.pi * 0.6 * s_.t)
            s_.term['raise_'][:] = -r
            thin = (s_.t >= 1.0) & (s_.t < 1.12)   # an unarmed stretch of under 30 pairs
            s_.armed[thin] = 0
            s_.dgame[:] = -(1.0 - np.where(thin, k_thin, k_armed)) * r
            s_.build()
            return s_
        d = fresh()
        write_session(d, stamp, flat_sim(0.4, 0.0).tables)
        S, A = analyse(d)
        cell = A.leak.get((2, 1))
        check(cell is not None and abs(cell['k_up'] - 0.4) < 0.05 and abs(cell['c'] - (-0.6)) < 0.05 and not cell['thin'], 'a planted k_up of 0.4 comes back: %s' % str(cell and (cell['k_up'], cell['c'], cell['n'])))
        check(cell is not None and cell['k_fwd'] is not None and abs(cell['k_fwd']) < 0.05, 'k_fwd of a camera that does not move forward is 0 (%s)' % (cell and cell['k_fwd']))
        thin = A.leak.get((2, 0))
        check(thin is not None and thin['thin'], 'the short unarmed cell is thin: %s' % (thin and thin['n']))
        check(not [f for f in A.findings if f.kind == 'leak'], 'a thin cell and a gain over %.1f are not a leak finding: %s' % (LEAK_FLAG_SLOPE, [f.detail[:80] for f in A.findings if f.kind == 'leak']))
        d = fresh()
        write_session(d, stamp, flat_sim(0.1, 0.1).tables)
        S, A = analyse(d)
        check(len([f for f in A.findings if f.kind == 'leak']) == 1, 'a planted k_up of 0.1 over many pairs is a leak finding: %s' % [(f.kind, f.cause) for f in A.findings])

        # the IK causes
        for cause in ('stall', 'rig_changed', 'refused', 'fp_stale', 'animation', 'infeasible', 'pole_change', 'capped', 'unattributed'):
            d = fresh()
            pre, post = _ik_fault(cause)
            _make(d, stamp, [pre], [post])
            S, A = analyse(d)
            ej = [e for e in A.ik_events_list if e['kind'] == 'elbow_jump']
            want = cause
            check(len(ej) == 1 and ej[0]['cause'].split(':')[0] == want, 'IK fault %s comes back as %s: %s' % (cause, want, [(e['kind'], e['cause']) for e in A.ik_events_list]))
        # every IK event kind can fire
        d = fresh()

        def cover(s):
            ik = s.tables['ik']
            t = ik['t']
            w = lambda a, b: (t >= a) & (t < b)
            ik['l_swivel_deg'] = np.where(t >= 4.0, 70.0, 40.0)
            ik['r_grip_err_cm'] = np.where(w(5.0, 5.05), 6.0, 0.5)
            ik['l_reach_ratio'] = np.where(w(6.0, 6.3), 1.05, 0.8)
            ik['final_neck'] = ik['final_neck'] + np.where(t >= 7.0, 1.0, 0.0)[:, None] * np.array([0, 0.05, 0])
            ik['final_r_fore'] = ik['final_r_fore'] + np.where(w(8.0, 8.5), 1.0, 0.0)[:, None] * np.array([0, 0.30, 0])
            ik['r_elbow_deg'] = np.where(w(8.7, 9.0), 20.0, 110.0)
            k = np.flatnonzero(w(2.0, 2.05))
            ik['exit'][k] = 2
            ik['write_down'] = (t >= 10.0).astype(int) * (t < 10.02)
        _make(d, stamp, [], [cover])
        S, A = analyse(d)
        kinds = collections.Counter(e['kind'] for e in A.ik_events_list)
        for kd in ('swivel_jump', 'grip_loss', 'overreach', 'neck_jump', 'elbow_high', 'elbow_limit', 'refused', 'write_down'):
            check(kinds[kd] >= 1, 'IK event %s can fire: %s' % (kd, dict(kinds)))

        # the file handling
        d = fresh()
        _make(d, stamp)
        base_n = len(load_session(d, stamp).tab('pin'))
        d2 = fresh()
        _make(d2, stamp, segments=2)
        S2, A2 = analyse(d2)
        check(len(S2.files) == 2 and len(S2.tab('pin')) == base_n and not A2.findings, 'two segments merge into one session without a finding')
        d3 = fresh()
        _make(d3, stamp, segments=2, chop=37)
        S3, A3 = analyse(d3)
        check(any(S3.truncated) and len(S3.tab('pin')) >= base_n - 2 and 'last record truncated' in '\n'.join(A3.out), 'a truncated tail is tolerated and said')
        d4 = fresh()
        sim = Sim()
        sim.build()
        keep = np.ones(len(sim.tables['pin']['t']), bool)
        keep[500:520] = False
        seq = np.arange(len(keep))[keep]
        for k in list(sim.tables['pin']):
            sim.tables['pin'][k] = sim.tables['pin'][k][keep]
        sim.tables['pin']['seq'] = seq
        write_session(d4, stamp, sim.tables)
        S4, A4 = analyse(d4)
        check(S4.stats['pin']['gaps'] == 20 and 'pin %d (20 dropped)' % len(S4.tab('pin')) in '\n'.join(A4.out), 'a seq gap reports the drop: %s' % S4.stats['pin'])
        d5 = fresh()
        sim = Sim()
        sim.build()
        sim.tables.pop('rt_eye')
        sim.tables.pop('rt_recentre')
        write_session(d5, stamp, sim.tables)
        S5, A5 = analyse(d5)
        check('runtime records: NONE' in '\n'.join(A5.out), 'missing runtime records are reported')
        d6 = fresh()
        _make(d6, stamp, drop={'align': ['hold_y']})
        S6, A6 = analyse(d6)
        check('cannot evaluate: align.hold_y missing' in '\n'.join(A6.out) and not A6.findings, 'a missing field gives cannot evaluate: %s' % [l for l in A6.out if 'cannot' in l][:2])
        d7 = fresh()
        _make(d7, stamp, drop={'pin': ['heading']})
        S7, A7 = analyse(d7)
        check('cannot evaluate: pin.heading missing' in '\n'.join(A7.out), 'a missing heading says so')
        # the head gain is information, not a finding: near 0 says nothing, a real follow prints the info line (a quick move of the head does reach E)
        for follow, want in ((0.0, False), (0.5, True)):
            dg = fresh()
            sim = Sim()
            sim.head[:] = 1.6 + 0.02 * np.sin(2 * np.pi * 0.25 * sim.t)
            sim.head_gain = follow
            sim.build()
            write_session(dg, stamp, sim.tables)
            Sg, Ag = analyse(dg)
            has = [f for f in Ag.findings if f.kind == 'gain']
            check(Ag.L.gain_fit is not None and abs(Ag.L.gain_fit - follow) < 0.05, 'the fitted head gain of %.1f comes back: %s' % (follow, Ag.L.gain_fit))
            check(not has, 'a head gain of %.1f is not a finding: %s' % (follow, [f.detail for f in Ag.findings][:2]))
            check(('info: pin.e[1] follows the physical head with a fitted gain' in '\n'.join(Ag.out)) == want, 'a head gain of %.1f %s the information line' % (follow, 'prints' if want else 'does not print'))

        # physical_head: the head falls 14 cm and E with it (the slope flight's mark 6): the cause is the head, not the game's camera; the head moving a third of the move is not
        def head_dip(fall, game):
            def pre(s_):
                s_.head_gain = 1.0
                s_.head[s_.win(6.0, 6.5)] -= fall
                s_.dgame[s_.win(6.0, 6.5)] -= game
            return pre
        d = fresh()
        _make(d, stamp, [head_dip(0.14, 0.0)])
        S, A = analyse(d)
        hx = [f for f in A.findings if f.kind == 'height_excursion']
        vn = [f for f in A.findings if f.kind == 'view_under_neck']
        check(len(hx) == 1 and hx[0].cause == 'physical_head', 'a head that falls 14 cm with E is a physical_head excursion, not game_camera: %s' % [(f.kind, f.cause) for f in A.findings])
        check(len(vn) == 1 and vn[0].cause == 'physical_head', 'and a physical_head view_under_neck: %s' % [(f.kind, f.cause) for f in A.findings])
        d = fresh()
        _make(d, stamp, [head_dip(0.04, 0.10)])
        S, A = analyse(d)
        hx = [f for f in A.findings if f.kind == 'height_excursion']
        check(len(hx) == 1 and hx[0].cause == 'game_camera', 'a head holding under 60%% of the move (4 of 14 cm) leaves the game\'s camera the cause: %s' % [(f.kind, f.cause) for f in A.findings])
        d = fresh()
        _make(d, stamp, [head_dip(-0.14, 0.0)])
        S, A = analyse(d)
        hx = [f for f in A.findings if f.kind == 'height_excursion']
        check(len(hx) == 1 and hx[0].cause == 'physical_head', 'a head that rises with E is physical_head too (the sign follows the move): %s' % [(f.kind, f.cause) for f in A.findings])
        d = fresh()

        def head_only(s_):
            s_.head[s_.win(6.0, 6.5)] -= 0.14   # head_gain stays 0: pin.e does not move
        _make(d, stamp, [head_only])
        S, A = analyse(d)
        hx = [f for f in A.findings if f.kind == 'height_excursion']
        check(not hx, 'a head that moves where E does not (gain 0, as F87h absorbs a sustained offset) is no excursion: %s' % [(f.kind, f.cause) for f in A.findings])

        # the view against the neck and the hands: a camera that sinks, with the feet either following it or holding
        def sink(feet):
            def pre(s_):
                s_.dgame[s_.win(6.0, 7.0)] -= 0.50

            def post(s_):
                if feet == 'fall':
                    ik = s_.tables['ik']
                    m = (ik['t'] >= 6.0) & (ik['t'] < 7.0)
                    for f in ('final_l_foot', 'final_r_foot'):
                        ik[f] = ik[f] + np.where(m, 1.0, 0.0)[:, None] * np.array([0, -0.5, 0])
            return pre, post
        for feet, want_feet in (('hold', 'camera fell'), ('fall', 'model origin rose')):
            d = fresh()
            pre, post = sink(feet)
            _make(d, stamp, [pre], [post])
            S, A = analyse(d)
            vn = [f for f in A.findings if f.kind == 'view_under_neck']
            ha = [f for f in A.findings if f.kind == 'hands_above_view']
            check(len(vn) == 1 and vn[0].cause == 'game_camera' and 6.0 <= vn[0].t0 <= 6.4, 'a sinking camera is view_under_neck from the game: %s' % [(f.kind, f.cause, round(f.t0, 2)) for f in A.findings])
            check(len(ha) == 1 and ha[0].cause == 'view_under_neck', 'and hands_above_view because the view is low: %s' % [(f.kind, f.cause) for f in A.findings])
            check(want_feet in A.feet_verdicts, 'the feet say "%s": %s' % (want_feet, A.feet_verdicts))
            check(not [f for f in A.findings if f.kind.startswith('ik_')], 'a sinking camera makes no IK event: %s' % [(f.kind, f.cause) for f in A.findings if f.kind.startswith('ik_')])
        # a stopping step moves the neck with the camera: no verdict on the feet where the neck moved 3 cm or more over the window
        for feet, neck_fall in (('hold', 0.06), ('fall', 0.06), ('hold', 0.02)):
            d = fresh()
            pre, post0 = sink(feet)

            def post(s_, post0=post0, neck_fall=neck_fall):
                post0(s_)
                ik = s_.tables['ik']
                m = (ik['t'] >= 6.0) & (ik['t'] < 7.0)
                ik['final_neck'] = ik['final_neck'] + np.where(m, 1.0, 0.0)[:, None] * np.array([0, -neck_fall, 0])
            _make(d, stamp, [pre], [post])
            S, A = analyse(d)
            if neck_fall >= FEET_NECK_HELD_M:
                check(A.feet_verdicts == ['cannot evaluate: the neck moved'], 'a neck that moved %.0f cm gives no feet verdict (%s feet): %s' % (100 * neck_fall, feet, A.feet_verdicts))
            else:
                check(len(A.feet_verdicts) == 1 and A.feet_verdicts[0] in ('camera fell', 'model origin rose'), 'a neck that moved %.0f cm still gives the feet verdict: %s' % (100 * neck_fall, A.feet_verdicts))
        # the same camera while the settle is starved: what moved is still the game's camera, and the starved settle is why nothing corrected it
        def starve(s_):
            m = s_.win(4.5, 8.0)
            s_.tables['pin']['settled'][m], s_.tables['pin']['settle_why'][m] = 0, 1
            s_.tables['align']['raise_age_ms'][m] = 400.0
        d = fresh()
        pre, post = sink('hold')
        _make(d, stamp, [pre], [post, starve])
        S, A = analyse(d)
        vn = [f for f in A.findings if f.kind == 'view_under_neck']
        check(len(vn) == 1 and vn[0].cause == 'game_camera' and 'not corrected' in repr(vars(vn[0])), 'a starved settle does not take the blame for what moved: %s' % [(f.cause, vars(f)) for f in vn])
        # the same on a recording from before the feet were added
        d = fresh()
        pre, post = sink('hold')
        _make(d, stamp, [pre], [post], drop={'ik': ['final_pelvis', 'final_l_foot', 'final_r_foot', 'anim_pelvis', 'anim_l_foot', 'anim_r_foot'], 'pin': ['up', 'fwd']})
        S, A = analyse(d)
        txt = '\n'.join(A.out)
        check('cannot evaluate: feet' in txt and 'ESTIMATE' in txt, 'an older recording says the feet cannot be evaluated and the up row is an estimate')
        # the hands rise on their own: hands_above_view caused by the hands
        d = fresh()

        def hands_up(s_):
            ik = s_.tables['ik']
            m = (ik['t'] >= 6.0) & (ik['t'] < 6.6)
            for f in ('final_l_hand', 'final_r_hand'):
                ik[f] = ik[f] + np.where(m, 1.0, 0.0)[:, None] * np.array([0, 0.6, 0])
        _make(d, stamp, [], [hands_up])
        S, A = analyse(d)
        ha = [f for f in A.findings if f.kind == 'hands_above_view']
        check(len(ha) == 1 and ha[0].cause == 'hands_moved', 'hands that rise on their own are hands_above_view caused by the hands: %s' % [(f.kind, f.cause) for f in A.findings])
        # a raise held from the crouch while not settled
        d = fresh()

        def stale(s_):
            m = s_.win(12.0, 15.5)
            s_.tables['pin']['settled'][m], s_.tables['pin']['settle_why'][m] = 0, 1
            s_.tables['align']['raise_m'][m] = 0.03
        _make(d, stamp, [], [stale])
        S, A = analyse(d)
        rs = [f for f in A.findings if f.kind == 'raise_stale']
        check(len(rs) == 1 and rs[0].cause == 'from_crouched' and 12.0 <= rs[0].t0 < 12.3, 'a raise held from the crouch is raise_stale from crouched: %s' % [(f.kind, f.cause, round(f.t0, 2)) for f in A.findings])
        # E over the neck by the path's slope: the lever (a = 0.09 m, b = 0.60 m) comes back from rows moving along the facing; a strafe and a stand, planted at E +0.5 m, are left out
        def lever_post(s_):
            dt = 1.0 / 90.0
            th = np.radians(25.0 * np.sin(2 * np.pi * s_.t / 12.0))
            side = s_.win(10.0, 12.0)
            still = s_.win(16.0, 18.0)
            vh = np.where(still, 0.0, 4.0)
            x = np.cumsum(np.where(side, vh, 0.0)) * dt
            z = np.cumsum(np.where(side, 0.0, vh)) * dt
            y = np.cumsum(np.where(side | still, 0.0, vh * np.tan(th))) * dt
            pin = s_.tables['pin']
            pin['root'] = np.stack([x, y, z], 1)
            pin['e'][:, 1] = pin['neck_anim'][:, 1] + np.where(side | still, 0.5, 0.09 + 0.60 * np.tan(th))
        d = fresh()
        _make(d, stamp, [], [lever_post])
        S, A = analyse(d)
        lv = A.lever
        check(lv is not None and abs(lv['a'] - 0.09) < 0.03 and abs(lv['b'] - 0.60) < 0.03 and lv['r'] > 0.95, 'the planted lever comes back within 0.03: %s' % lv)
        check(lv is not None and lv['n'] < 0.85 * A.L.t.size, 'the strafe and the stand are left out of the fit: %s of %d rows' % (lv and lv['n'], A.L.t.size))
        check(A.lever_view is not None and 'view over neck = ' in '\n'.join(A.out), 'a recording with f214_state also fits the view over the neck')
        d = fresh()
        _make(d, stamp, [], [lever_post], drop={'pin': ['f214_state', 'f214_target']})
        S, A = analyse(d)
        check(A.lever is not None and A.lever_view is None and 'view over neck = ' not in '\n'.join(A.out), 'a recording from before F214 fits the lever and not the view')
        # the F214 section: a recording without its fields says so; one with them counts the states and holds the two findings (each firing, each silent)
        d = fresh()
        _make(d, stamp, drop={'pin': ['f214_state', 'f214_target']})
        S, A = analyse(d)
        check('cannot evaluate: pin.f214_state missing' in '\n'.join(A.out) and A.f214 is None and not [f for f in A.findings if f.kind.startswith('f214')], 'a recording from before F214 says it cannot evaluate the section')

        def f214_post(err_m, ripple_m, lo=4.0, hi=8.5):
            def post(s_):
                pin, al, p1 = s_.tables['pin'], s_.tables['align'], s_.tables['p1']
                m = s_.win(lo, hi)
                pin['f214_state'] = np.where(m, S.enum('F214State', 'followed'), S.enum('F214State', 'not_moving'))
                pin['neck_anim'][m, 1] = p1['cam'][m, 1] - 0.15 + err_m
                al['raise_m'] = al['raise_m'] + np.where(m, ripple_m * np.sin(2 * np.pi * 3.0 * s_.t), 0.0)
            return post
        for err_m, ripple_m, want in ((0.08, 0.012, {'f214_view_error', 'f214_bob'}), (0.08, 0.002, {'f214_view_error'}), (0.01, 0.012, {'f214_bob'}), (0.01, 0.002, set())):
            d = fresh()
            _make(d, stamp, [], [f214_post(err_m, ripple_m)])
            S, A = analyse(d)
            got = {f.kind for f in A.findings if f.kind.startswith('f214')}
            check(got == want, 'F214 findings for a view %.0f cm off and a %.0f mm ripple in the raise: %s, want %s (%s)' % (100 * err_m, 1000 * ripple_m, sorted(got), sorted(want), A.f214))
            if err_m == 0.01 and ripple_m == 0.002:
                txt = '\n'.join(A.out)
                check('pin calls by state:' in txt and 'followed 405' in txt and A.f214['followed'] == 405 and A.f214['err_cm'] < 2.0 and A.f214['bob_mm'] < F214_BOB_MM,
                      'the section counts the calls by state and measures the followed rows: %s' % [l for l in A.out if 'F214' in l or 'followed' in l][:3])
        err, bob = f214_measure(np.arange(100) / 90.0, np.full(100, 1.55), np.full(100, 1.40), np.zeros(100), np.ones(100, bool), 0.15)
        check(abs(err) < 1e-9 and abs(bob) < 1e-9, 'a view exactly neck + eye height with a flat raise measures 0 and 0: %s %s' % (err, bob))
        # F215 and F216: a recording without their fields says so and the ledger is as before; the ledger's terms close with the body's shift and the bob term in them
        d = fresh()
        _make(d, stamp, drop={'pin': ['f215_state', 'f215_body_y', 'f215_target'], 'align': ['bob_seated', 'bob_w'], 'p1': ['sub_bob', 'bob_w']})
        S, A = analyse(d)
        txt = '\n'.join(A.out)
        check('pin.f215_state missing' in txt and A.f215 is not None and A.f215['active'] == 0 and not np.any(A.L.by) and not [f for f in A.findings if f.kind.startswith(('f215', 'f216'))],
              'a recording from before F215 says so, reads the body\'s shift as 0 and names no finding')
        check(abs(np.sum([A.L.terms[k] for k in TERMS if k != 'body'], axis=0) - A.L.eye_y).max() < 1e-9 and np.all(A.L.terms['bob'] == 0), 'the ledger terms (the body\'s shift apart) add up to the alignment\'s y')

        def f215_post(on, amp, level_over=0.09, state=None, chase=True):
            """Unarmed upright for the whole session: a walk along +z at 1.5 m/s from 1 s to 8 s, then three 60 degree turns on the spot (10, 12 and 14 s) about a pivot 0.425 m ahead of the root, the root
            rising by amp over the three (a hill); the camera stays at the pivot in the world, so in the model it falls by that rise. on: the body's shift is minus the rise (F215 active) and the raise
            is constant, so the view holds in the world and in the body's frame. off: no shift, and with chase the raise adds the rise back, so the view rides with the body as flown before F215."""
            def post(s_):
                pin, al, p1 = s_.tables['pin'], s_.tables['align'], s_.tables['p1']
                t = s_.t
                n = len(t)
                pin['stance'][:], pin['armed'][:], pin['detach_w'][:] = 0, 0, 0.0
                al['ads'][:] = 0
                th = np.zeros(n)
                for t0 in (10.0, 12.0, 14.0):
                    x = np.clip((t - t0) / 0.4, 0.0, 1.0)
                    th += (np.pi / 3.0) * (3 * x * x - 2 * x ** 3)
                walk = np.clip(t - 1.0, 0.0, 7.0) * 1.5
                fwd = np.stack([np.sin(th), np.zeros(n), np.cos(th)], 1)
                pivot = np.array([0.0, 0.0, 7.0 * 1.5 + 0.425])
                standing = t >= 8.0
                root = np.where(standing[:, None], pivot[None, :] - 0.425 * fwd, np.stack([np.zeros(n), np.zeros(n), walk], 1))
                rise = amp * th / np.pi
                root[:, 1] = rise
                pin['root'], pin['fwd'] = root, fwd
                pin['neck_anim'][:, 1] = 1.40
                pin['e'][:, 1] = 1.40 + level_over - rise
                p1['e'] = np.where(standing[:, None], pivot[None, :] + np.array([0.0, 1.55, 0.0]), root + np.array([0.0, 1.55, 0.0]))
                if not on and chase:
                    p1['sub_raise'] = p1['sub_raise'] - rise   # the seated raise is minus its upward move
                pin['f215_state'] = np.full(n, SYN_ENUMS['F215State'].index('active') if on else 0)
                pin['f215_body_y'] = -rise if on else np.zeros(n)
            return post
        for on, amp, over, want in ((False, 0.15, 0.09, {'f215_turn_view_move'}), (True, 0.15, 0.09, set()), (True, 0.0, 0.09, set()), (True, 0.0, 0.16, {'f215_level_constant'})):
            d = fresh()
            _make(d, stamp, [], [f215_post(on, amp, over)])
            S, A = analyse(d)
            got = {f.kind for f in A.findings if f.kind.startswith('f215')}
            check(got == want, 'F215 findings for the shift %s, a hill of %.0f cm and E %.0f cm over the neck: %s, want %s (%s)' % ('on' if on else 'off', 100 * amp, 100 * over, sorted(got), sorted(want), A.f215))
            if on and amp > 0.0 and over == 0.09:
                tr = A.f215['turns']
                check(len(tr) == 3 and all(abs(x['dang'] - 60.0) < 8.0 and abs(x['pivot_cm'] - 42.5) < 3.0 for x in tr) and A.f215['turn_move_cm'] < 1.0 and A.f215['active'] == len(A.S.tab('pin')),
                      'three 60 degree turns on a 0.425 m pivot are found, the view in the body\'s frame holds still and every call was active: %s' % [(round(x['dang']), round(x['pivot_cm'], 1), round(x['view_cm'], 1)) for x in tr])
                check(A.f215['level_rows'] > 200 and abs(A.f215['e_over_neck_cm'] - 9.0) < 0.5 and abs(A.f215['y_max_cm'] - 15.0) < 0.5, 'the level rows read E 9 cm over the mean neck and the active rows the 15 cm shift: %s' % A.f215)
            if not on:
                tr = A.f215['turns']
                check(len(tr) == 3 and A.f215['turn_move_cm'] > 3.0 and 'no active call' in '\n'.join(A.out), 'with the shift off the view moves with the hill\'s rise across the turns: %s' % A.f215['turn_move_cm'])
        # F217: an item at the hip. The pilot walks 1.5 m/s along +z for 20 s with the arc cutter (serial 7) drawn. slope: the ground rises at 0.25 and the body follows, so E (true) is the level height
        # plus the shift and the level rows are none; level: flat ground with E the level height plus `off`.
        def f217_post(slope, off, state='active_item'):
            def post(s_):
                pin, al = s_.tables['pin'], s_.tables['align']
                t = s_.t
                n = len(t)
                pin['stance'][:], pin['armed'][:], pin['detach_w'][:], pin['item_serial'][:] = 0, 1, 0.0, 7
                al['ads'][:] = 0
                walk = np.clip(t - 1.0, 0.0, 20.0) * 1.5
                root = np.stack([np.zeros(n), slope * walk, walk], 1)
                pin['root'], pin['fwd'] = root, np.tile([0.0, 0.0, 1.0], (n, 1))
                y = np.minimum(slope * walk, 0.6)
                e = F217_LEVEL_CAM_M + off + y
                pin['eye_y_selected'], pin['raw_eye_y'] = e, e
                pin['e'][:, 1] = e
                pin['f215_state'] = np.full(n, SYN_ENUMS['F215State'].index(state))
                pin['f215_body_y'] = y
            return post
        for slope, off, want in ((0.25, 0.0, set()), (0.0, 0.0, set()), (0.0, 0.08, {'f217_item_level'}), (0.0, -0.08, {'f217_item_level'})):
            d = fresh()
            _make(d, stamp, [], [f217_post(slope, off)])
            S, A = analyse(d)
            got = {f.kind for f in A.findings if f.kind.startswith('f217')}
            check(got == want, 'F217 findings for an armed walk on a slope of %.2f with E %+.0f cm off the level height: %s, want %s (%s)' % (slope, 100 * off, sorted(got), sorted(want), A.f215))
            if slope == 0.25:
                check(A.f215.get('item_active_s', 0.0) > 20.0 and not A.f215.get('item_level') and 'item at the hip (F217)' in '\n'.join(A.out),
                      'an armed slope walk counts its seconds in active_item and has no level rows: %s' % A.f215)
            if slope == 0.0 and off == 0.0:
                check(abs(A.f215['item_level'].get(7, 0.0) - F217_LEVEL_CAM_M) < 1e-6 and 'hip rows with an item' in '\n'.join(A.out), 'level ground with E on the level height reads it by item serial and measures the hip rows: %s' % A.f215)
        d = fresh()
        _make(d, stamp, [], [f217_post(0.0, 0.08, state='active')])
        S, A = analyse(d)
        check(not [f for f in A.findings if f.kind.startswith('f217')], 'rows not in active_item are not judged against the level height')
        d = fresh()
        _make(d, stamp, [], [f217_post(0.0, 0.08)], drop={'pin': ['f215_state', 'f215_body_y', 'f215_target']})
        S, A = analyse(d)
        check(not [f for f in A.findings if f.kind.startswith('f217')], 'a recording without the F215 state names no F217 finding')
        # the view is formed less the body's shift, and a change of it is -delta body_y in the terms
        d = fresh()
        _make(d, stamp, [], [f215_post(True, 0.15)])
        S, A = analyse(d)
        d0 = fresh()
        _make(d0, stamp, [], [f215_post(False, 0.15, chase=False)])
        S0, A0 = analyse(d0)
        check(abs((A0.L.body - A.L.body) - A.L.by).max() < 1e-9 and abs(A.L.by).max() > 0.1 and np.all(A.L.terms['body'] == A.L.by), 'the view in the ledger is less the body\'s shift, which is the body term')

        def sprint_post(perfect, amp=0.010):
            def post(s_):
                pin, al, p1 = s_.tables['pin'], s_.tables['align'], s_.tables['p1']
                t = s_.t
                n = len(t)
                pin['stance'][:], pin['armed'][:], pin['detach_w'][:] = 0, 0, 0.0
                al['ads'][:] = 0
                z = np.clip(t - 1.0, 0.0, 9.0) * 6.0
                root = np.stack([np.zeros(n), np.zeros(n), z], 1)
                pin['root'], pin['fwd'] = root, np.tile([0.0, 0.0, 1.0], (n, 1))
                b = amp * np.sin(2 * np.pi * 3.4 * t)
                side = 0.0065 * np.sin(2 * np.pi * 1.7 * t)
                p1['e'] = root + np.stack([side, 1.6 + b, np.zeros(n)], 1)
                al['bob_seated'] = np.stack([side, b, np.zeros(n)], 1) if perfect else np.zeros((n, 3))
            return post
        for perfect, want in ((True, set()), (False, {'f216_sprint_bob'})):
            d = fresh()
            _make(d, stamp, [], [sprint_post(perfect)])
            S, A = analyse(d)
            got = {f.kind for f in A.findings if f.kind.startswith('f216')}
            bn = (A.f216 or {}).get('bins', {}).get((1, 0))
            check(got == want and bn is not None and abs(bn['cam_mm'] - 7.07) < 0.8 and ((bn['view_mm'] < 1.0 and bn['side_mm'] < 1.0) if perfect else bn['view_mm'] > 6.0),
                  'F216 on a 6 m/s sprint with a %s bob term: findings %s, want %s (%s)' % ('perfect' if perfect else 'zero', sorted(got), sorted(want), bn))
        d = fresh()
        _make(d, stamp, [], [sprint_post(False)], drop={'align': ['bob_seated', 'bob_w'], 'p1': ['sub_bob', 'bob_w']})
        S, A = analyse(d)
        check('align.bob_seated missing' in '\n'.join(A.out) and not [f for f in A.findings if f.kind == 'f216_sprint_bob'], 'a recording from before F216 reads the view as the camera and names no sprint finding')
        # F216b, F220: a recording made with the band-pass filter has p1.hud_shift. The clamp share reads against 2.5 cm (against 5 cm without the field), the up term's rms is split under 1.5 Hz and in
        # 2.5 to 6 Hz, and the HUD's shift (up 6 mm and side 4 mm at the stride rate, planted) is read as its rms and the share of rows with a shift
        def hud_post(s_):
            sprint_post(True, 0.030)(s_)
            p1, t = s_.tables['p1'], s_.t
            sn = np.sin(2 * np.pi * 3.4 * t)
            p1['hud_shift'] = np.stack([0.004 * sn, 0.006 * sn, np.zeros(len(t))], 1)
            p1['hud_shift_on'] = np.ones(len(t), np.uint8)
        d = fresh()
        _make(d, stamp, [], [hud_post])
        S, A = analyse(d)
        bn = (A.f216 or {}).get('bins', {}).get((1, 0))
        hud = (A.f216 or {}).get('hud')
        txt = chr(10).join(A.out)
        check(bn is not None and bn['clamp'] > 0.2 and abs(bn['term_band_mm'] - 21.2) < 2.0 and bn['term_low_mm'] < 0.15 * bn['term_band_mm'] and 'under 1.5 Hz' in txt and 'at the 2.5 cm clamp' in txt,
              'F216b: with p1.hud_shift the clamp reads against 2.5 cm and the up term is split by band: %s' % (bn,))
        check(hud is not None and abs(hud['up_mm'] - 4.24) < 0.4 and abs(hud['side_mm'] - 2.83) < 0.3 and hud['share'] > 0.9 and 'HUD shift (F220' in txt, 'F220: the HUD shift is read as up 4.24 and side 2.83 mm rms with a share of rows: %s' % (hud,))
        d = fresh()
        _make(d, stamp, [], [hud_post], drop={'p1': ['hud_shift', 'hud_shift_on']})
        S, A = analyse(d)
        bn = (A.f216 or {}).get('bins', {}).get((1, 0))
        txt = chr(10).join(A.out)
        check(bn is not None and bn['clamp'] < 0.01 and (A.f216 or {}).get('hud') is None and 'HUD shift (F220' not in txt and 'at the 5.0 cm clamp' in txt,
              'F220: a recording without p1.hud_shift keeps the 5 cm clamp and prints no HUD shift: %s' % (bn,))
        check(abs(Analysis.band_rms(np.arange(128) / 90.0, 0.010 * np.sin(2 * np.pi * 3.4 * np.arange(128) / 90.0), 2.5, 6.0) * 1000 - 7.07) < 0.7 and
              Analysis.band_rms(np.arange(128) / 90.0, 0.010 * np.sin(2 * np.pi * 0.7 * np.arange(128) / 90.0), 2.5, 6.0) * 1000 < 0.5, 'the band rms reads a 3.4 Hz 10 mm sine as 7.07 mm and a 0.7 Hz one as nothing')
        # F218, F219: the left elbow against the chest, by item and speed. A rifle (serial 3) walked 1.5 m/s for 7 s: with the torso flipping between two solutions three times a second (the elbow 8 cm
        # each time, the spine weight 0 to 1) it is a finding; held on one solution it is not.
        def torso_post(flips):
            def post(s_):
                pin, ik = s_.tables['pin'], s_.tables['ik']
                t = s_.t
                n = len(t)
                pin['root'] = np.stack([np.zeros(n), np.zeros(n), np.clip(t - 1.0, 0.0, 9.0) * 1.5], 1)
                m = (t >= 2.0) & (t < 9.0)
                ik['item_serial'], pin['item_serial'] = np.where(m, 3, 0), np.where(m, 3, 0)
                sq = ((np.floor((t - 2.0) * 3.0).astype(int) % 2) == 1).astype(float) if flips else np.zeros(n)
                ik['final_l_fore'] = ik['final_l_fore'] + sq[:, None] * np.array([0.08, 0.0, 0.0])
                ik['spine_w'] = sq
                ik['l_reach_ratio'] = np.where(sq > 0, 0.97, 1.06)
                ik['l_swivel_limited'] = (m & (np.arange(n) % 10 == 0)).astype(int)
                pin['f218_applied'] = (m & (sq == 0)).astype(int)
                pin['f218_refused'] = (m & (sq > 0)).astype(int)
                pin['f218_held'] = np.zeros(n, int)
                pin['f218_offset_m'] = np.where(m & (sq > 0), 0.06, 0.0)
            return post
        for flips, want in ((True, {'elbow_steps'}), (False, set())):
            d = fresh()
            _make(d, stamp, [], [torso_post(flips)])
            S, A = analyse(d)
            got = {f.kind for f in A.findings if f.kind == 'elbow_steps'}
            txt = '\n'.join(A.out)
            it = (A.f218 or {}).get('items', {}).get((3, 'walk'))
            check(got == want and it is not None and 6.5 < it['secs'] < 7.5 and abs(it['limited'] - 0.1) < 0.02 and 'item 3, walk (0.3 to 4 m/s)' in txt and 'F218 over' in txt,
                  'F218 on a rifle walking with the torso %s: findings %s, want %s (%s)' % ('flipping' if flips else 'steady', sorted(got), sorted(want), it))
            if flips:
                check(it['r5'] > 2.5 and it['flips'] > 2.5 and abs(it['reach'] - 1.015) < 0.05 and 'rest applied' in txt and 'largest slide 6.0 cm' in txt,
                      'F218 reads the flips: over 5 cm %.1f/s, spine flips %.1f/s, reach %.2f' % (it['r5'], it['flips'], it['reach']))
            else:
                check(it['r5'] == 0.0 and it['flips'] == 0.0 and 'rest applied' in txt and 'refused 0' in txt, 'F218 on a steady torso reads no step and no flip')
        d = fresh()
        _make(d, stamp, [], [torso_post(True)], drop={'pin': ['f218_held', 'f218_applied', 'f218_refused', 'f218_offset_m'], 'ik': ['l_swivel_limited', 'r_swivel_limited', 'l_swivel_cut_deg', 'r_swivel_cut_deg']})
        S, A = analyse(d)
        txt = '\n'.join(A.out)
        check('pin.f218_* missing' in txt and 'l_swivel_limited absent' in txt and 'item 3, walk' in txt and 'elbow_steps' in {f.kind for f in A.findings} and 'F218 over' not in txt,
              'a recording from before F218 and F219 still prints the step figures, says which fields it lacks, and gives the finding the steps alone support')
        d = fresh()
        _make(d, stamp)
        S, A = analyse(d)
        check('nothing to read' in '\n'.join(A.out) and not [f for f in A.findings if f.kind == 'elbow_steps'], 'a session with no left-arm writing at speed reads nothing and names no finding')
        # F221: the left elbow's angle from straight down, its height over the hand, and the strafe bins. An item (serial 3) at the hip, armed, moving 1.5 m/s along the body's right from t = 2 s: the elbow
        # hung under the shoulder-to-hand axis reads an angle near 0 and no finding; the elbow 5 cm over the hand for 0.3 s is one elbow_over_hand finding.
        def f221_post(flare):
            def post(s_):
                pin, ik = s_.tables['pin'], s_.tables['ik']
                t = s_.t
                n = len(t)
                pin['root'] = np.stack([np.clip(t - 1.0, 0.0, 9.0) * 1.5, np.zeros(n), np.zeros(n)], 1)
                m = (t >= 2.0) & (t < 9.0)
                ik['item_serial'], pin['item_serial'] = np.where(m, 3, 0), np.where(m, 3, 0)
                ik['armed'] = np.ones(n, int)
                pin['detach_w'] = np.zeros(n)
                ik['l_wrote'] = np.ones(n, int)
                chest = ik['final_chest']
                S_ = chest + np.array([-0.18, 0.0, 0.0])
                H_ = chest + np.array([-0.15, -0.20, 0.45])
                ax = (H_ - S_) / np.linalg.norm(H_ - S_, axis=1)[:, None]
                d = np.array([0.0, -1.0, 0.0]) - (ax[:, 1])[:, None] * (-1.0) * ax
                d = d / np.linalg.norm(d, axis=1)[:, None]
                E_ = 0.5 * (S_ + H_) + 0.14 * d
                if flare:
                    w = (t >= 5.0) & (t < 5.3)
                    E_ = np.where(w[:, None], np.stack([E_[:, 0], H_[:, 1] + 0.05, E_[:, 2]], 1), E_)
                ik['final_l_upper'], ik['final_l_hand'], ik['final_l_fore'] = S_, H_, E_
                ik['l_pole_src'] = np.where(m, 2, 0)
            return post
        d = fresh()
        _make(d, stamp, [], [f221_post(False)])
        S, A = analyse(d)
        txt = '\n'.join(A.out)
        sr = (A.f218 or {}).get('strafe', {}).get((3, 'strafe right'))
        wk = (A.f218 or {}).get('items', {}).get((3, 'walk'))
        check(sr is not None and sr['f221'] is not None and sr['f221']['ang_p99'] < 2.0 and sr['f221']['oh_max'] < 0.0 and sr['f221']['down'] > 0.99 and 'item 3, strafe right' in txt and 'body-down pole on 100% of rows' in txt and
              (A.f218 or {}).get('strafe', {}).get((3, 'strafe left')) is None and not [f for f in A.findings if f.kind == 'elbow_over_hand'],
              'F221: an elbow hung under the axis reads an angle near 0 in the strafe right bin, under the hand, no finding, no strafe left: %s' % (sr,))
        check(wk is not None and wk['f221'] is not None and wk['f221']['ang_med'] < 2.0, 'F221: the walk line carries the same figures: %s' % (wk,))
        d = fresh()
        _make(d, stamp, [], [f221_post(True)])
        S, A = analyse(d)
        fs = [f for f in A.findings if f.kind == 'elbow_over_hand']
        check(len(fs) == 1 and 'item 3' in fs[0].detail and 'up to 5.0 cm' in fs[0].detail and 4.9 < fs[0].t0 < 5.1 and 0.25 < fs[0].t1 - fs[0].t0 < 0.4,
              'F221: the elbow 5 cm over the hand for 0.3 s is one elbow_over_hand finding with its peak and item: %s' % ([(f.t0, f.t1, f.detail) for f in fs],))
        d = fresh()
        _make(d, stamp, [], [f221_post(True)], drop={'ik': ['final_l_upper'], 'pin': ['fwd']})
        S, A = analyse(d)
        txt = '\n'.join(A.out)
        check('item 3, walk' in txt and 'no strafe bins' in txt and not [f for f in A.findings if f.kind == 'elbow_over_hand'], 'F221: a recording without the arm joints or the body axes still prints the section and names no F221 finding')
        # F222: a body turning at 342 deg/s in uneven steps (5.3, 2.3, 2.3, 5.3 deg a call). With the neck target swinging by the step and the heading in the model reading the step the section reads a heading that
        # follows the yaw step (a slope near -1) and a finding; with both steady it reads neither.
        def turn_post(follows):
            def post(s_):
                pin, ik = s_.tables['pin'], s_.tables['ik']
                t = s_.t
                n = len(t)
                steps = np.array([5.3, 2.3, 2.3, 5.3])[np.arange(n) % 4]
                yaw = np.radians(np.cumsum(steps))
                pin['fwd'] = np.stack([np.sin(yaw), np.zeros(n), np.cos(yaw)], 1)
                m = (t >= 2.0) & (t < 9.0)
                ik['item_serial'], pin['item_serial'] = np.where(m, 3, 0), np.where(m, 3, 0)
                hdg = np.radians(-1.55 - (steps if follows else np.zeros(n)))
                pin['heading'] = np.stack([np.sin(hdg), np.cos(hdg)], 1)
                pin['neck_target'] = pin['neck_anim'] + (np.stack([0.0015 * (steps - 3.8), np.zeros(n), np.zeros(n)], 1) if follows else 0.0)
            return post
        for follows in (True, False):
            d = fresh()
            _make(d, stamp, [], [turn_post(follows)])
            S, A = analyse(d)
            it = (A.f222 or {}).get('items', {}).get(3)
            got = [f for f in A.findings if f.kind == 'heading_follows_yaw_step']
            txt = '\n'.join(A.out)
            if follows:
                check(it is not None and it['slope'] < -0.9 and it['slope'] > -1.1 and it['neck_med'] > 1.0 and len([f for f in got if 'item 3' in f.detail]) == 1 and '== turning (F222)' in txt,
                      'F222: a heading that follows the yaw step reads a slope near -1, a neck target residual over 1 mm and one finding: %s' % (it,))
            else:
                check(it is not None and abs(it['slope']) < 0.1 and it['neck_med'] < 0.2 and not [f for f in got if 'item 3' in f.detail] and it['rows'] > 300, 'F222: a steady heading and neck target in the same turn read no slope and no finding: %s' % (it,))
        d = fresh()
        _make(d, stamp)
        S, A = analyse(d)
        check('nothing to read' in '\n'.join(A.out) and (A.f222 or {}).get('items') == {} and not [f for f in A.findings if f.kind == 'heading_follows_yaw_step'], 'F222: a recording with no turning reads nothing and names no finding')
        # F223: one jump pack start: the camera in the model (pin.e y) 1.70 to 0.60 in one call and back over four, the world camera smooth. The chest falls and the elbow stands over the hand: a finding;
        # with the chest as it was, the event is listed and there is none.
        def jump_post(crush, pop=True):
            def post(s_):
                pin, ik = s_.tables['pin'], s_.tables['ik']
                t = s_.t
                k0 = int(np.searchsorted(t, 5.0))
                series = [1.60, 1.65, 1.70, 0.60, 0.92, 1.31, 1.54, 1.61, 1.60]
                e = pin['e'].copy()
                e[:, 1] = 1.60
                for j, v in enumerate(series):
                    e[k0 + j, 1] = v
                pin['e'] = e
                ik['item_serial'], pin['item_serial'] = np.full(len(t), 5), np.full(len(t), 5)
                if pop:
                    pm = np.zeros(len(t))
                    pm[k0 + 3:k0 + 8] = -1.10
                    pin['f223_pop_m'] = pm
                if crush:
                    chest = ik['final_chest'].copy()
                    chest[k0 + 3:k0 + 6, 1] -= 0.55
                    ik['final_chest'] = chest
                    fore = ik['final_l_fore'].copy()
                    fore[k0 + 3:k0 + 5, 1] += 0.27
                    ik['final_l_fore'] = fore
                s_.k0_223 = k0
            return post
        d = fresh()
        _make(d, stamp, [], [jump_post(True)])
        S, A = analyse(d)
        evs = (A.f223 or {}).get('events', [])
        got = [f for f in A.findings if f.kind == 'root_jump_crushed']
        txt = '\n'.join(A.out)
        check(len(evs) == 1 and evs[0]['calls'] == 4 and abs(evs[0]['jump'] + 1.10) < 1e-6 and abs(evs[0]['pop'] + 1.10) < 1e-6 and evs[0]['drop'] is not None and evs[0]['drop'] > 0.5 and evs[0]['elbow'] > 20.0 and len(got) == 1 and '== root jumps (F223)' in txt,
              'F223: a jump pack start is one event of 4 calls with the pop, the chest fall and the elbow over the hand, and one root_jump_crushed finding: %s' % (evs,))
        d = fresh()
        _make(d, stamp, [], [jump_post(False)])
        S, A = analyse(d)
        evs = (A.f223 or {}).get('events', [])
        check(len(evs) == 1 and abs(evs[0]['drop']) < 0.05 and not [f for f in A.findings if f.kind == 'root_jump_crushed'], 'F223: a jump with the chest held is listed and is no finding')
        d = fresh()
        _make(d, stamp, [], [jump_post(True, pop=False)], drop={'pin': ['f223_pop_m']})
        S, A = analyse(d)
        txt = '\n'.join(A.out)
        check('not recorded (a recording from before F223)' in txt and len((A.f223 or {}).get('events', [])) == 1, 'F223: a recording from before the pin record carried the move still lists the event and says so')
        d = fresh()
        _make(d, stamp)
        S, A = analyse(d)
        check('no call the rule would take' in '\n'.join(A.out) and not [f for f in A.findings if f.kind == 'root_jump_crushed'], 'F223: a recording with no jump lists none')
        # F224: rows in the air in F215's active state with the camera jittering a centimetre a call. The whole shift one call late (the camera's last value) leaves the camera less the shift alternating:
        # the section reads a residual over 5 mm and a finding; with the lead in the shift it reads about nothing.
        def lag_post(lead_on):
            def post(s_):
                pin, ik = s_.tables['pin'], s_.tables['ik']
                t = s_.t
                n = len(t)
                m = (t >= 2.0) & (t < 9.0)
                c = 1.50 + 0.008 * (np.arange(n) % 2 * 2 - 1)
                e = pin['e'].copy()
                e[:, 1] = c
                pin['e'] = e
                body = np.zeros(n)
                body[1:] = c[:-1] - 1.60
                lead = np.zeros(n)
                lead[1:] = c[1:] - c[:-1]
                pin['f215_state'] = np.where(m, SYN_ENUMS['F215State'].index('active_item'), 0)
                pin['armed'] = np.ones(n, int)
                pin['root_vy'] = np.where(m, 3.0, 0.0)
                pin['f215_body_y'] = np.where(m, body + (lead if lead_on else 0.0), 0.0)
                pin['f224_lead_m'] = np.where(m & lead_on, lead, 0.0)
                for nm in ('anim_l_foot', 'anim_r_foot'):
                    f = ik[nm].copy()
                    f[:, 1] = np.where(m, 0.5, 0.0)
                    ik[nm] = f
            return post
        for lead_on in (False, True):
            d = fresh()
            _make(d, stamp, [], [lag_post(lead_on)])
            S, A = analyse(d)
            cl = (A.f224 or {}).get('classes', {}).get('air')
            got = [f for f in A.findings if f.kind == 'body_move_late']
            if not lead_on:
                check(cl is not None and cl['less'] > 5.0 and cl['less_p90'] > F224_FINDING_MM and cl['true'] < 8.0 and cl['rows'] > 300 and len(got) == 1 and '== whole-body move (F224)' in '\n'.join(A.out) and 'left forearm against the chest' in '\n'.join(A.out),
                      'F224: the shift one call late leaves the camera less the shift alternating (rms over 5 mm in the air) and one body_move_late finding: %s' % (cl,))
            else:
                check(cl is not None and cl['less'] < 1.0 and cl['less_p90'] < 1.0 and cl['fore'] is not None and cl['lead'] is not None and cl['lead'] > 5.0 and not got, 'F224: with the lead in the shift the residual is under 1 mm and there is no finding: %s' % (cl,))
        d = fresh()
        _make(d, stamp, [], [lag_post(False)], drop={'pin': ['f224_lead_m', 'f223_pop_m']})
        S, A = analyse(d)
        check('f224_lead_m not recorded (a recording from before F224)' in '\n'.join(A.out) and (A.f224 or {}).get('classes', {}).get('air') is not None, 'F224: a recording from before the lead was recorded still reads and says so')
        d = fresh()
        _make(d, stamp)
        S, A = analyse(d)
        check('nothing to read' in '\n'.join(A.out) and not [f for f in A.findings if f.kind == 'body_move_late'], 'F224: a recording with no active whole-body move reads nothing and names no finding')
        # F226: the head past the gate with the move on, standing still: y steps 1 mm a call; and a held stretch that leaves y far from the rule's ask.
        def head_post(flag_on=True):
            def post(s_):
                pin = s_.tables['pin']
                t = s_.t
                n = len(t)
                m = (t >= 3.0) & (t < 6.0)
                pin['f226_head_pitched'] = np.where(m & flag_on, 1, 0)
                pin['f215_state'] = np.full(n, SYN_ENUMS['F215State'].index('active_item'))
                pin['f215_body_y'] = -0.10 + 0.0005 * (np.arange(n) % 2 * 2 - 1)
            return post
        d = fresh()
        _make(d, stamp, [], [head_post()])
        S, A = analyse(d)
        hd = (A.f215 or {}).get('head')
        txt = '\n'.join(A.out)
        check(hd is not None and hd['n'] > 200 and hd['still'] > 200 and abs(hd['med'] - 1.0) < 0.05 and abs(hd['mx'] - 1.0) < 0.05 and 'head past the gate with the move on (pin.f226_head_pitched)' in txt and 'standing still in those rows' in txt,
              'F226: calls with the head past the gate and the move on are counted with y\'s step standing still (1 mm a call): %s' % (hd,))
        d = fresh()
        _make(d, stamp, [], [head_post()], drop={'pin': ['f226_head_pitched']})
        S, A = analyse(d)
        check('cannot evaluate head past the gate: pin.f226_head_pitched missing' in '\n'.join(A.out), 'F226: a recording without the flag says so and still reads')

        def held_post(state, y_held, armed=1):
            def post(s_):
                pin = s_.tables['pin']
                t = s_.t
                n = len(t)
                m = (t >= 3.0) & (t < 9.0)
                pin['f215_state'] = np.where(m, SYN_ENUMS['F215State'].index(state), SYN_ENUMS['F215State'].index('active_item'))
                pin['armed'] = np.full(n, armed)
                pin['eye_y_selected'] = np.full(n, 1.50)
                pin['f215_target'] = np.full(n, -0.10)
                pin['f215_body_y'] = np.where(m, y_held, -0.10)
            return post
        for state, y_held, want in (('held_aim', -0.40, 1), ('held_pose', -0.40, 1), ('held_aim', -0.12, 0), ('held_sights', -0.40, 0)):
            d = fresh()
            _make(d, stamp, [], [held_post(state, y_held)])
            S, A = analyse(d)
            got = [f for f in A.findings if f.kind == 'shift_held_far']
            check(len(got) == want and (not want or (5.5 < got[0].t1 - got[0].t0 < 6.5 and 'y -0.40' in got[0].detail)),
                  'F226: %s with y %+.2f against an ask of -0.10 (the level camera 1.60 less E 1.50): %d shift_held_far finding(s), want %d' % (state, y_held, len(got), want))
        # a recording from before F226 names held_head: the same stretch is judged
        saved = list(SYN_ENUMS['F215State'])
        try:
            SYN_ENUMS['F215State'][:] = ['off', 'released_away', 'released_item', 'released_posture', 'held_aim', 'held_pose', 'held_head', 'filling', 'active', 'held_sights', 'active_item', 'not_run']
            d = fresh()
            _make(d, stamp, [], [held_post('held_head', -0.40)])
            S, A = analyse(d)
            got = [f for f in A.findings if f.kind == 'shift_held_far']
            check(len(got) == 1 and 'held_head' in got[0].detail, 'F226: a recording from before F226 with held_head is judged by the same finding')
        finally:
            SYN_ENUMS['F215State'][:] = saved
        d = fresh()
        _make(d, stamp, [], [held_post('held_aim', -0.40)], drop={'pin': ['f215_target']})
        S, A = analyse(d)
        check('cannot evaluate: pin.f215_target' in '\n'.join(A.out) and not [f for f in A.findings if f.kind == 'shift_held_far'], 'F226: a recording without the fields the rule needs says so and names no finding')
        # F225: a rifle (serial 3) at the hip on the ground with the left hand steady: the assist flips every 20 calls and the elbow moves 4.5 cm with each, so the elbow hops (about 4.5 a second, all of them on a
        # flip) and there is a finding; with the assist steady there is none.
        def hop_post(flips):
            def post(s_):
                pin, ik = s_.tables['pin'], s_.tables['ik']
                t = s_.t
                n = len(t)
                m = (t >= 1.0) & (t < 14.0)
                ik['item_serial'], pin['item_serial'] = np.where(m, 3, 0), np.where(m, 3, 0)
                pin['detach_w'] = np.zeros(n)
                on = (np.arange(n) // 20) % 2 == 1 if flips else np.zeros(n, bool)
                ik['l_assisted'] = (m & on).astype(int)
                ik['final_l_fore'] = ik['final_l_fore'] + np.where(m & on, 1.0, 0.0)[:, None] * np.array([0.045, 0.0, 0.0])
            return post
        for flips in (True, False):
            d = fresh()
            _make(d, stamp, [], [hop_post(flips)])
            S, A = analyse(d)
            hp = (A.f218 or {}).get('hops', {}).get(3)
            got = [f for f in A.findings if f.kind == 'elbow_solver_hop' and 'item 3' in f.detail]
            if flips:
                check(hp is not None and hp['secs'] > 12.0 and hp['rate'] > 3.0 and hp['on_flip'] == hp['hops'] and len(got) == 1, 'F225: an elbow hopping on every assist flip reads the rate, all on a flip, and one elbow_solver_hop finding: %s' % (hp,))
            else:
                check(hp is not None and hp['hops'] == 0 and not got, 'F225: a steady assist reads no hop and no finding: %s' % (hp,))
        # arms the IK did not write: a huge reach and swivel jumps make no event
        d = fresh()

        def unwritten(s_):
            ik = s_.tables['ik']
            t = ik['t']
            for p in ('l_', 'r_'):
                ik[p + 'wrote'] = np.zeros(len(t), int)
                ik[p + 'reach_ratio'] = np.where((t >= 5.0) & (t < 6.0), 1.5, 0.8)
                ik[p + 'swivel_deg'] = np.where(np.floor(t * 4) % 2 == 0, 40.0, 80.0)
                ik['final_%sfore' % p] = ik['final_%sfore' % p] + np.where(np.floor(t * 2) % 2 == 0, 0.0, 0.1)[:, None] * np.array([1.0, 0, 0])
        _make(d, stamp, [], [unwritten])
        S, A = analyse(d)
        check(not A.ik_events_list and 'L wrote on 0 of' in '\n'.join(A.out), 'unwritten arms give no IK event and the report says how many calls wrote: %s' % [(e['kind']) for e in A.ik_events_list][:4])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('onfoot_check self-test: %s (%d checks passed, %d failed)' % ('FAILED' if fails else 'ok', passes[0], len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
