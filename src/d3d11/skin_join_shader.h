#pragma once
// The F2 identity passes (docs/kinematic-motion-injection-2026-09-19.md, "F2 built"; skin_join.h is their CPU twin and
// states what they decide). One HLSL text, four entry points, compiled offline by tools/temporal_shader_build:
//
//   join          one 256-thread group at the chain dispatch. Reads the game's t0 job table (t0), last frame's copy of it
//                 (t1), the plan the CPU built from the hook's list (t2), last frame's by-base (bind, count) table (t3) and
//                 pose table (t4). Writes the join table (u0, a structured buffer of uint: the vertex shader reads it as
//                 t109: current base -> previous base, 0 = none), this frame's by-base table (u1), the persistent counters
//                 (u2). u3 is scratch (the lowest job per base, so two jobs with one base resolve the way the CPU reference does).
//   poseClear     zeroes a pose table (one thread per element).
//   poseScatter   one thread per pool record: record bytes 0..31 -> pose[word0] when word0 != 0.
//   poseVerify    one thread per pool record: words 0..6 must equal the table's; a record that disagrees with another of the
//                 same base kills the base (word 0 = 0) and counts a conflict.
//
// The numbers below must equal skin_join.h's; skin_join_gpu_test reads them back out of this text and compares.
namespace edvr {
constexpr char kSkinJoinCsHlsl[] = R"HLSL(
#define SJ_MAX_ROWS 65536u
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
#define SJ_STAT_WORDS 24u
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
void join(uint tid : SV_GroupIndex) {
 const uint flags = PW(0u), m = PW(1u), prevM = PW(2u), endRow = PW(3u);
 const uint n = min(PW(5u), SJ_MAX_JOBS), prevN = min(PW(6u), SJ_MAX_JOBS);
 const bool history = (flags & SJ_PLAN_HISTORY) != 0u, offered = (flags & SJ_PLAN_HOOK) != 0u;
 const uint prevHookOk = Stats.Load(SJ_STAT_PREV_HOOK_OK * 4u);
 // 0: clear the tables
 [loop] for (uint i = tid; i < SJ_MAX_ROWS; i += 256u) {
  JoinOut[i] = 0u;
  Info.Store2(i * 8u, uint2(0u, 0u));
  Owner.Store(i * 4u, 0xFFFFFFFFu);
 }
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
 uint fNoPrev = 0u, fRange = 0u, fLayout = 0u, fPrefix = 0u, fPose = 0u, nJoined = 0u;
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
cbuffer PoseCb : register(b0) { uint records; uint rows; uint pad0; uint pad1; };
StructuredBuffer<Rec> Pool : register(t5);
RWStructuredBuffer<Pose> PoseOut : register(u4);

[numthreads(64,1,1)]
void poseClear(uint3 id : SV_DispatchThreadID) {
 if (id.x >= rows) return;
 Pose z; z.a = uint4(0u,0u,0u,0u); z.b = uint4(0u,0u,0u,0u);
 PoseOut[id.x] = z;
}

[numthreads(64,1,1)]
void poseScatter(uint3 id : SV_DispatchThreadID) {
 if (id.x >= records) return;
 Pose p = Pool[id.x].p;
 uint base = p.a.x;
 if (base == 0u || base >= rows) return;
 PoseOut[base] = p;
 uint o; Stats.InterlockedAdd(SJ_STAT_POSE_RECORDS * 4u, 1u, o);
}

[numthreads(64,1,1)]
void poseVerify(uint3 id : SV_DispatchThreadID) {
 if (id.x >= records) return;
 Pose p = Pool[id.x].p;
 uint base = p.a.x;
 if (base == 0u || base >= rows) return;
 Pose t = PoseOut[base];
 if (t.a.x != p.a.x || t.a.y != p.a.y || t.a.z != p.a.z || t.a.w != p.a.w || t.b.x != p.b.x || t.b.y != p.b.y || t.b.z != p.b.z) {
  uint4 dead = t.a; dead.x = 0u;
  PoseOut[base].a = dead;
  uint o; Stats.InterlockedAdd(SJ_STAT_POSE_CONFLICTS * 4u, 1u, o);
 }
}
)HLSL";
}
