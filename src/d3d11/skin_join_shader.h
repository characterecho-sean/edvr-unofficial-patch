#pragma once
// The F2 identity passes (docs/kinematic-motion-injection-2026-09-19.md, "F2 built"; skin_join.h is their CPU twin and
// states what they decide). One HLSL text, several entry points, compiled offline by tools/temporal_shader_build:
//
//   joinClear     the join's phase 0 as a pass of its own (F17): SJ_CLEAR_GROUPS groups of 256 threads clear ALL SJ_MAX_ROWS rows of the three tables (join table 0, by-base
//                 table 0, owner table all ones), dispatched on the same context right before join. Inside the kernel the same loop ran in one group, 256 dependent
//                 iterations a thread (0.015 ms of the join's 0.030 in the F16 flight); split across groups it is a few microseconds. It still clears every row every
//                 frame: stale rows after the entity or job count shrinks must read as cleared, and the join below depends on it (the owner table's minima).
//   join          one 256-thread group at the chain dispatch, after joinClear. Reads the game's t0 job table (t0), last frame's copy of it
//                 (t1), the plan the CPU built from the hook's list (t2), last frame's by-base (bind, count) table (t3) and
//                 pose table (t4). Writes the join table (u0, a structured buffer of uint: the vertex shader reads it as
//                 t109: current base -> previous base, 0 = none), this frame's by-base table (u1), the persistent counters
//                 (u2). u3 is scratch (the lowest job per base, so two jobs with one base resolve the way the CPU reference does).
//   The pose table, built once a present frame at the frame boundary (engine_velocity.cpp, after every draw of the frame has been issued), from
//   the pool's private copy and the list of the instance-stream entries the frame's skinned draws read. WHICH RECORD OF A BASE IS THE LIVE ONE is
//   decided by that list and nothing else: a record is live when a skinned draw of this frame read it (the instance stream's entry at the draw's
//   StartInstanceLocation names the record). The pool also holds records no draw reads this frame (the F12 flight's 154827 copies: a second set of
//   every character's records, left from two frames earlier, byte-identical to that frame's, a walker's pose 0.1-0.2 m off); the game's instance stream
//   holds stale entries as well, so only the draws' own entries count.
//   poseClear     zeroes the pose table, the per-base state and the reference bitmap with its "bad" word (one thread per element).
//   poseRefMark   one thread per listed draw: the record each instance entry names gets its bit in the bitmap; an entry that cannot be read or names
//                 a record outside the pool, or a draw naming too many instances, sets the bad word (no exact reference list: every record decides).
//   poseScatter   one thread per pool record: counts it; when the reference list is exact and the record is read by a draw, record bytes 0..31 ->
//                 pose[word0] and the base is marked as having a live record.
//   poseScatterRest  the same for the records of bases with no live record (the unreferenced bases and, with no exact list, every base).
//   poseVerify    one thread per pool record: words 0..6 must equal the table's. A base with live records is decided by them alone: a live record that
//                 disagrees marks the base (counted a conflict), an unreferenced one that disagrees is overruled (counted resolved). A base without
//                 live records is decided by all its records.
//   poseFinish    one thread per base: a marked base is zeroed whole (word 0 = 0: no history, never a guess) and counted dropped.
//
// The numbers below must equal skin_join.h's; skin_join_gpu_test reads them back out of this text and compares.
namespace edvr {
constexpr char kSkinJoinCsHlsl[] = R"HLSL(
#define SJ_MAX_ROWS 65536u
#define SJ_CLEAR_GROUPS 64u
#define SJ_MAX_ENTRIES 1024u
#define SJ_MAX_JOBS 8192u
#define SJ_PLAN_RS 8u
#define SJ_PLAN_COUNT 1033u
#define SJ_PLAN_PREVIDX 2057u
#define SJ_PLAN_PREVRS 3081u
#define SJ_NONE 0xFFFFFFFFu
#define SJ_PLAN_HISTORY 1u
#define SJ_PLAN_HOOK 2u
#define SJ_STAT_FRAMES 0u
#define SJ_STAT_HOOK_USED 1u
#define SJ_STAT_PREFIX_USED 2u
#define SJ_STAT_NO_HISTORY 3u
#define SJ_STAT_HOOK_DISAGREE 4u
#define SJ_STAT_PREV_NOT_VERIFIED 5u
#define SJ_STAT_JOBS 6u
#define SJ_STAT_JOINED 7u
#define SJ_STAT_FAIL_NO_PREV 8u
#define SJ_STAT_FAIL_RANGE 9u
#define SJ_STAT_FAIL_LAYOUT 10u
#define SJ_STAT_FAIL_PREFIX 11u
#define SJ_STAT_FAIL_POSE 12u
#define SJ_STAT_FAIL_CAP 13u
#define SJ_STAT_DUP_BASE 14u
#define SJ_STAT_PREV_HOOK_OK 15u
#define SJ_STAT_MISMATCH_BITS 16u
#define SJ_STAT_POSE_RECORDS 17u
#define SJ_STAT_POSE_CONFLICTS 18u
#define SJ_STAT_LAST_JOBS 19u
#define SJ_STAT_LAST_ENTITIES 20u
#define SJ_STAT_FAIL_PREV_ROWS 21u
#define SJ_STAT_POSE_RESOLVED 22u
#define SJ_STAT_POSE_DROPPED 23u
#define SJ_STAT_POSE_LISTS_EXACT 24u
#define SJ_STAT_POSE_LISTS_BAD 25u
#define SJ_STAT_POSE_IDLE 26u
#define SJ_STAT_POSE_UNRESOLVED 27u
#define SJ_STAT_WORDS 28u
#define SJ_REF_WORDS 2048u
#define SJ_MAX_RANGE_INSTANCES 1024u
#define SJ_POSE_LIVE 1u
#define SJ_POSE_CONFLICT 2u
#define SJ_MM_NOT_IN_RANGE 1u
#define SJ_MM_HEAD_COUNT 2u
#define SJ_MM_HEADS 4u
#define SJ_MM_SUM 8u
#define SJ_MM_ENTITY_SUM 16u
#define SJ_MM_NO_PLAN 32u

struct Pose { uint4 a; uint4 b; };
struct Rec { Pose p; uint rest[76]; };

StructuredBuffer<uint4> Jobs : register(t0);
StructuredBuffer<uint4> PrevJobs : register(t1);
ByteAddressBuffer Plan : register(t2);
ByteAddressBuffer PrevInfo : register(t3);
StructuredBuffer<Pose> PrevPose : register(t4);
RWStructuredBuffer<uint> JoinOut : register(u0);
RWByteAddressBuffer Info : register(u1);
RWByteAddressBuffer Stats : register(u2);
RWByteAddressBuffer Owner : register(u3);

groupshared uint gEntSum[SJ_MAX_ENTRIES];
groupshared uint gStat[SJ_STAT_WORDS];
groupshared uint gMismatch;
groupshared uint gHeads;
groupshared uint gSum;
groupshared uint gPrefix;

uint PW(uint w) { return Plan.Load(w * 4u); }
bool CapOk(uint4 row) { return row.w != 0u && row.y < SJ_MAX_ROWS && row.w <= SJ_MAX_ROWS - row.y; }
// the last entity whose base is <= dst, or SJ_NONE
uint EntityOf(uint dst, uint m) {
 uint lo = 0u, hi = m;
 [loop] while (lo < hi) { uint mid = (lo + hi) >> 1; if (PW(SJ_PLAN_RS + mid) <= dst) lo = mid + 1u; else hi = mid; }
 if (lo == 0u || dst >= PW(SJ_PLAN_RS + m)) return SJ_NONE;
 return lo - 1u;
}

[numthreads(256,1,1)]
void joinClear(uint3 id : SV_DispatchThreadID) {
 [loop] for (uint i = id.x; i < SJ_MAX_ROWS; i += SJ_CLEAR_GROUPS * 256u) {
  JoinOut[i] = 0u;
  Info.Store2(i * 8u, uint2(0u, 0u));
  Owner.Store(i * 4u, 0xFFFFFFFFu);
 }
}

[numthreads(256,1,1)]
void join(uint tid : SV_GroupIndex) {
 const uint flags = PW(0u), m = PW(1u), prevM = PW(2u), endRow = PW(3u), prevRows = PW(7u);
 const uint n = min(PW(5u), SJ_MAX_JOBS), prevN = min(PW(6u), SJ_MAX_JOBS);
 const bool history = (flags & SJ_PLAN_HISTORY) != 0u, offered = (flags & SJ_PLAN_HOOK) != 0u;
 const uint prevHookOk = Stats.Load(SJ_STAT_PREV_HOOK_OK * 4u);
 // 0: (the tables were cleared by joinClear, the dispatch before this one)
 if (tid < SJ_STAT_WORDS) gStat[tid] = 0u;
 [loop] for (uint e = tid; e < SJ_MAX_ENTRIES; e += 256u) gEntSum[e] = 0u;
 if (tid == 0u) { gMismatch = (offered && (m == 0u || m > SJ_MAX_ENTRIES)) ? SJ_MM_NO_PLAN : 0u; gHeads = 0u; gSum = 0u; gPrefix = min(n, prevN); }
 AllMemoryBarrierWithGroupSync();
 // A1: the lowest job index owns each base
 uint capFails = 0u;
 [loop] for (uint j = tid; j < n; j += 256u) {
  uint4 row = Jobs[j];
  if (!CapOk(row)) { capFails += 1u; continue; }
  uint prior; Owner.InterlockedMin(row.y * 4u, j, prior);
 }
 { uint o; InterlockedAdd(gStat[SJ_STAT_FAIL_CAP], capFails, o); }
 AllMemoryBarrierWithGroupSync();
 // A2: the owner writes the by-base table; a second job on a base is a duplicate
 uint dupFails = 0u;
 [loop] for (uint j2 = tid; j2 < n; j2 += 256u) {
  uint4 row = Jobs[j2];
  if (!CapOk(row)) continue;
  if (Owner.Load(row.y * 4u) == j2) Info.Store2(row.y * 8u, uint2(row.z, row.w));
  else dupFails += 1u;
 }
 { uint o; InterlockedAdd(gStat[SJ_STAT_DUP_BASE], dupFails, o); }
 // B: the hook's list against the table
 if (offered && m != 0u && m <= SJ_MAX_ENTRIES) {
  {
   [loop] for (uint jb = tid; jb < n; jb += 256u) {
    uint4 row = Jobs[jb];
    if (!CapOk(row) || Owner.Load(row.y * 4u) != jb) continue;
    uint ent = EntityOf(row.y, m);
    uint o;
    if (ent == SJ_NONE) { InterlockedOr(gMismatch, SJ_MM_NOT_IN_RANGE, o); continue; }
    InterlockedAdd(gEntSum[ent], row.w, o);
    InterlockedAdd(gSum, row.w, o);
    if (row.y == PW(SJ_PLAN_RS + ent)) {
     InterlockedAdd(gHeads, 1u, o);
     if (row.w != PW(SJ_PLAN_COUNT + ent)) InterlockedOr(gMismatch, SJ_MM_HEAD_COUNT, o);
    }
   }
  }
 }
 AllMemoryBarrierWithGroupSync();
 if (offered && !(gMismatch & SJ_MM_NO_PLAN)) {
  [loop] for (uint ee = tid; ee < m; ee += 256u) {
   uint o;
   if (gEntSum[ee] != PW(SJ_PLAN_RS + ee + 1u) - PW(SJ_PLAN_RS + ee)) InterlockedOr(gMismatch, SJ_MM_ENTITY_SUM, o);
  }
  if (tid == 0u) {
   if (gHeads != m) gMismatch |= SJ_MM_HEADS;
   if (gSum != endRow - PW(SJ_PLAN_RS)) gMismatch |= SJ_MM_SUM;
  }
 }
 AllMemoryBarrierWithGroupSync();
 const bool hookOk = offered && gMismatch == 0u;
 const bool useHook = history && hookOk && prevHookOk != 0u;
 // C: the prefix of the table that matches last frame's (bind, count)
 const uint limit0 = min(n, prevN);
 if (!useHook) {
  [loop] for (uint k = tid; k < limit0; k += 256u) {
   uint4 a = Jobs[k], b = PrevJobs[k];
   if (a.z != b.z || a.w != b.w) { uint o; InterlockedMin(gPrefix, k, o); }
  }
 }
 AllMemoryBarrierWithGroupSync();
 // D: the join
 uint fNoPrev = 0u, fRange = 0u, fLayout = 0u, fPrefix = 0u, fPose = 0u, fPrevRows = 0u, nJoined = 0u;
 if (history) {
  [loop] for (uint jd = tid; jd < n; jd += 256u) {
   uint4 row = Jobs[jd];
   if (!CapOk(row) || Owner.Load(row.y * 4u) != jd) continue;
   uint prevDst = SJ_NONE;
   if (useHook) {
    uint ent = EntityOf(row.y, m);
    uint ip = ent == SJ_NONE ? SJ_NONE : PW(SJ_PLAN_PREVIDX + ent);
    if (ip == SJ_NONE || ip >= prevM) { fNoPrev += 1u; continue; }
    uint rs0 = PW(SJ_PLAN_RS + ent), rs1 = PW(SJ_PLAN_RS + ent + 1u);
    uint ps0 = PW(SJ_PLAN_PREVRS + ip), ps1 = PW(SJ_PLAN_PREVRS + ip + 1u);
    if (rs1 - rs0 != ps1 - ps0) { fRange += 1u; continue; }
    prevDst = ps0 + (row.y - rs0);
    uint2 pinfo = prevDst < SJ_MAX_ROWS ? PrevInfo.Load2(prevDst * 8u) : uint2(0u, 0u);
    if (prevDst >= SJ_MAX_ROWS || pinfo.y != row.w || pinfo.x != row.z) { fLayout += 1u; continue; }
   } else {
    if (jd >= gPrefix) { fPrefix += 1u; continue; }
    prevDst = PrevJobs[jd].y;
   }
   if (prevDst < SJ_MAX_ROWS && prevDst + row.w > prevRows) { fPrevRows += 1u; continue; }
   if (prevDst == 0u || prevDst >= SJ_MAX_ROWS || PrevPose[prevDst].a.x != prevDst) { fPose += 1u; continue; }
   JoinOut[row.y] = prevDst;
   nJoined += 1u;
  }
 }
 {
  uint o;
  InterlockedAdd(gStat[SJ_STAT_FAIL_NO_PREV], fNoPrev, o);
  InterlockedAdd(gStat[SJ_STAT_FAIL_RANGE], fRange, o);
  InterlockedAdd(gStat[SJ_STAT_FAIL_LAYOUT], fLayout, o);
  InterlockedAdd(gStat[SJ_STAT_FAIL_PREFIX], fPrefix, o);
  InterlockedAdd(gStat[SJ_STAT_FAIL_POSE], fPose, o);
  InterlockedAdd(gStat[SJ_STAT_FAIL_PREV_ROWS], fPrevRows, o);
  InterlockedAdd(gStat[SJ_STAT_JOINED], nJoined, o);
 }
 AllMemoryBarrierWithGroupSync();
 // the counters (one writer)
 if (tid == 0u) {
  [loop] for (uint s = 0u; s < SJ_STAT_WORDS; ++s) {
   if (s == SJ_STAT_PREV_HOOK_OK || s == SJ_STAT_LAST_JOBS || s == SJ_STAT_LAST_ENTITIES || s == SJ_STAT_MISMATCH_BITS) continue;
   Stats.Store(s * 4u, Stats.Load(s * 4u) + gStat[s]);
  }
  Stats.Store(SJ_STAT_FRAMES * 4u, Stats.Load(SJ_STAT_FRAMES * 4u) + 1u);
  Stats.Store(SJ_STAT_JOBS * 4u, Stats.Load(SJ_STAT_JOBS * 4u) + n);
  if (history) Stats.Store((useHook ? SJ_STAT_HOOK_USED : SJ_STAT_PREFIX_USED) * 4u, Stats.Load((useHook ? SJ_STAT_HOOK_USED : SJ_STAT_PREFIX_USED) * 4u) + 1u);
  else Stats.Store(SJ_STAT_NO_HISTORY * 4u, Stats.Load(SJ_STAT_NO_HISTORY * 4u) + 1u);
  if (offered && gMismatch != 0u) {
   Stats.Store(SJ_STAT_HOOK_DISAGREE * 4u, Stats.Load(SJ_STAT_HOOK_DISAGREE * 4u) + 1u);
   Stats.Store(SJ_STAT_MISMATCH_BITS * 4u, Stats.Load(SJ_STAT_MISMATCH_BITS * 4u) | gMismatch);
  }
  if (history && offered && gMismatch == 0u && prevHookOk == 0u)
   Stats.Store(SJ_STAT_PREV_NOT_VERIFIED * 4u, Stats.Load(SJ_STAT_PREV_NOT_VERIFIED * 4u) + 1u);
  Stats.Store(SJ_STAT_PREV_HOOK_OK * 4u, (offered && gMismatch == 0u) ? 1u : 0u);
  Stats.Store(SJ_STAT_LAST_JOBS * 4u, n);
  Stats.Store(SJ_STAT_LAST_ENTITIES * 4u, m);
 }
}

// ---- the pose table ----
// b0: records (the pool copy's), rows, nRanges (listed draws), instFirst (the entry index the copied span starts at), flags (bit 0: the CPU says
// the draw list is complete), instEntries (entries in the copied span).
cbuffer PoseCb : register(b0) { uint records; uint rows; uint nRanges; uint instFirst; uint poseFlags; uint instEntries; uint pad0; uint pad1; };
StructuredBuffer<Rec> Pool : register(t5);
ByteAddressBuffer InstCopy : register(t6);
StructuredBuffer<uint2> Ranges : register(t7);
RWStructuredBuffer<Pose> PoseOut : register(u4);
RWByteAddressBuffer RefBits : register(u5);
RWByteAddressBuffer BaseState : register(u6);

bool RefValid() { return (poseFlags & 1u) != 0u && RefBits.Load(SJ_REF_WORDS * 4u) == 0u; }
bool Referenced(uint rec) { return (RefBits.Load((rec >> 5u) * 4u) & (1u << (rec & 31u))) != 0u; }
bool SamePose(Pose t, Pose p) {
 return t.a.x == p.a.x && t.a.y == p.a.y && t.a.z == p.a.z && t.a.w == p.a.w && t.b.x == p.b.x && t.b.y == p.b.y && t.b.z == p.b.z;
}

[numthreads(64,1,1)]
void poseClear(uint3 id : SV_DispatchThreadID) {
 if (id.x < rows) {
  Pose z; z.a = uint4(0u,0u,0u,0u); z.b = uint4(0u,0u,0u,0u);
  PoseOut[id.x] = z;
  BaseState.Store(id.x * 4u, 0u);
 }
 if (id.x <= SJ_REF_WORDS) RefBits.Store(id.x * 4u, 0u);
}

[numthreads(64,1,1)]
void poseRefMark(uint3 id : SV_DispatchThreadID) {
 if (id.x >= nRanges) return;
 uint2 r = Ranges[id.x];
 bool bad = r.y > SJ_MAX_RANGE_INSTANCES;
 const uint count = bad ? 0u : r.y;
 [loop] for (uint k = 0u; k < count; ++k) {
  const uint entry = r.x + k;
  if (entry >= instFirst && entry - instFirst < instEntries) {
   const uint rec = InstCopy.Load((entry - instFirst) * 8u);
   if (rec < records && rec < SJ_REF_WORDS * 32u) { uint o; RefBits.InterlockedOr((rec >> 5u) * 4u, 1u << (rec & 31u), o); }
   else bad = true;
  } else bad = true;
 }
 if (bad) RefBits.Store(SJ_REF_WORDS * 4u, 1u);
}

[numthreads(64,1,1)]
void poseScatter(uint3 id : SV_DispatchThreadID) {
 if (id.x >= records) return;
 Pose p = Pool[id.x].p;
 uint base = p.a.x;
 if (base == 0u || base >= rows) return;
 uint o; Stats.InterlockedAdd(SJ_STAT_POSE_RECORDS * 4u, 1u, o);
 if (RefValid() && Referenced(id.x)) {
  PoseOut[base] = p;
  BaseState.InterlockedOr(base * 4u, SJ_POSE_LIVE, o);
 }
}

[numthreads(64,1,1)]
void poseScatterRest(uint3 id : SV_DispatchThreadID) {
 if (id.x >= records) return;
 Pose p = Pool[id.x].p;
 uint base = p.a.x;
 if (base == 0u || base >= rows) return;
 if ((BaseState.Load(base * 4u) & SJ_POSE_LIVE) == 0u) PoseOut[base] = p;
}

[numthreads(64,1,1)]
void poseVerify(uint3 id : SV_DispatchThreadID) {
 if (id.x >= records) return;
 Pose p = Pool[id.x].p;
 uint base = p.a.x;
 if (base == 0u || base >= rows) return;
 const bool hasLive = (BaseState.Load(base * 4u) & SJ_POSE_LIVE) != 0u;
 const bool decisive = !hasLive || Referenced(id.x);
 if (SamePose(PoseOut[base], p)) return;
 uint o;
 if (decisive) {
  BaseState.InterlockedOr(base * 4u, SJ_POSE_CONFLICT, o);
  Stats.InterlockedAdd(SJ_STAT_POSE_CONFLICTS * 4u, 1u, o);
 } else {
  Stats.InterlockedAdd(SJ_STAT_POSE_RESOLVED * 4u, 1u, o);
 }
}

[numthreads(64,1,1)]
void poseFinish(uint3 id : SV_DispatchThreadID) {
 if (id.x == 0u && (poseFlags & 1u) != 0u) {
  uint q;
  Stats.InterlockedAdd((RefBits.Load(SJ_REF_WORDS * 4u) == 0u ? SJ_STAT_POSE_LISTS_EXACT : SJ_STAT_POSE_LISTS_BAD) * 4u, 1u, q);
 }
 if (id.x >= rows) return;
 if ((BaseState.Load(id.x * 4u) & SJ_POSE_CONFLICT) == 0u) return;
 Pose z; z.a = uint4(0u,0u,0u,0u); z.b = uint4(0u,0u,0u,0u);
 PoseOut[id.x] = z;
 uint o; Stats.InterlockedAdd(SJ_STAT_POSE_DROPPED * 4u, 1u, o);
 // Of the dropped bases: with an exact draw list, one a draw read (two or more of its read records disagree) or one no draw read (every record decided)
 if (RefValid()) Stats.InterlockedAdd(((BaseState.Load(id.x * 4u) & SJ_POSE_LIVE) != 0u ? SJ_STAT_POSE_UNRESOLVED : SJ_STAT_POSE_IDLE) * 4u, 1u, o);
}
)HLSL";
}
