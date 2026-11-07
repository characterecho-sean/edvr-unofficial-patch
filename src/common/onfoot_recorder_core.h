// F213: the on-foot flight recorder's format and its logic, header only and free of Windows, so tools\onfoot_recorder_test runs the very code the game runs.
// Journal: docs\design-native-onfoot-vr-2026-10-13.md (F213). The arc: native on foot VR draws the game's stereo view with a third-person body under EDVR's head pose and solves arm
// IK so the hands hold the game's first-person weapon. The 10 s and 1 s English summaries cannot say which stage moved first. The recorder writes one binary
// record per frame per stage so a checker can attribute a fault to a stage.
//
// THE FILE. edvr_logs\onfoot\onfoot_<yyyymmdd_hhmmss>_<nnn>.rec, a new segment every 32 MiB, each readable on its own: the 8-byte magic EDVRONF1, a u32 (little
// endian) giving the length of the JSON that follows (padded with spaces so the body starts on an 8-byte boundary), the UTF-8 JSON (format, build, keys, the
// schema of every record and the names of every enum), then the records back to back. A record starts with the 16-byte Prefix (id, size, a per-type sequence
// number and the QPC), then its fields, and its size is a multiple of 8. A truncated last record (a crash) is legal: the reader stops at it.
//
// THE FIELDS. Each record is one X-macro list (ONF_LIST_*): the struct, the schema table (name, type, count, offset from offsetof) and the JSON all come from the
// same list, so a struct edit cannot drift from the schema. A list reads S(type, name) for a scalar, A(type, name, count) for an array and E(Enum, name) for a u8
// that holds an enum value.
//
// THE RING. Records reach the writer through a bounded multi-producer single-consumer ring (Vyukov's queue): the Animate jobs, the game thread, the frame thread and
// the runtime's owner thread all produce. A push never blocks, takes no lock and does no I/O; a full ring drops the record and counts it, and the record's sequence
// number was already taken, so a drop is a gap in the reader's sequence.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace edvr {
namespace onfootrec {

namespace ty {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
}  // namespace ty

constexpr char kMagic[8] = {'E', 'D', 'V', 'R', 'O', 'N', 'F', '1'};
constexpr uint32_t kFormat = 1;
constexpr size_t kRingSlots = 4096;
constexpr uint64_t kSegmentBytes = 32ull * 1024 * 1024;      // a new segment at this size
constexpr uint32_t kKeepSegments = 8;                        // the newest of the running session that are kept (256 MiB)
constexpr uint64_t kFolderBytes = 1024ull * 1024 * 1024;     // at session start the oldest matching files go until the folder is under this
constexpr size_t kBatchBytes = 64 * 1024;                    // the writer's buffer: flushed every drain and when full

struct Prefix {
    ty::u16 id;
    ty::u16 size;
    ty::u32 seq;
    ty::u64 qpc;
};
static_assert(sizeof(Prefix) == 16, "the record prefix is 16 bytes");

enum RecId : ty::u16 { kRecAlign = 1, kRecP1 = 2, kRecPin = 3, kRecIk = 4, kRecMark = 5, kRecRtEye = 6, kRecRtRecentre = 7, kRecTypes = 7 };
inline const char* recName(unsigned id) {
    static const char* const n[kRecTypes + 1] = {"?", "align", "p1", "pin", "ik", "mark", "rt_eye", "rt_recentre"};
    return id <= kRecTypes ? n[id] : "?";
}

// ---- the enums (the names a reader needs) -----------------------------------------------------------------------------------------------------------------------------------
struct EnumDesc {
    const char* name;
    const char* const* values;
    unsigned count;
};
#define ONF_ENUM(Name, ...)                                                                \
    inline const char* const k##Name##Values[] = {__VA_ARGS__};                            \
    inline const EnumDesc k##Name##Enum = {#Name, k##Name##Values, sizeof(k##Name##Values) / sizeof(k##Name##Values[0])};
// F137's reason the virtual neck is not applied (nofoot::f137::Reason)
ONF_ENUM(VneckReason, "applied", "look_to_aim", "out_of_scope", "no_aim", "no_head")
// nofoot::f94::Source: where the P1 hook's removed term came from
ONF_ENUM(SrcKind, "matched", "legacy", "missed")
// PinRefusal (native_on_foot.cpp), then none (the stage ran and refused nothing) and not_run (it returned before its refusal logic)
ONF_ENUM(PinRefuse, "no_input", "stale_input", "matrix", "no_legs", "leg", "write", "camera_away", "none", "not_run")
// nofoot::f95::Why
ONF_ENUM(SettleWhy, "settled", "moving", "neck_moving", "cam_moving", "filling", "invalid")
// nofoot::f97: the four stances at the hip, then the same four in the sights
ONF_ENUM(Stance, "standing", "crouched", "crouch_moving", "moving", "standing_ads", "crouched_ads", "crouch_moving_ads", "moving_ads")
// nofoot::SupportItem
ONF_ENUM(Support, "none", "pistol", "tool")
// nofoot::f94::EyeSource, then none
ONF_ENUM(EyeSource, "unarmed", "unarmed_no_class", "stance", "standing_over_crouch", "start", "hunched", "none")
// nofoot::f193::Source
ONF_ENUM(F193Source, "hip", "legs", "camera")
// how an armIkAfterAnimate call ended
ONF_ENUM(IkExit, "completed", "ik_off", "plan_torn", "plan_mismatch", "inputs_refused", "bad_joint_count", "f166_write", "f180_write", "f169_write", "pin_write", "span_write")
// nofoot::pinState: the speed bin of a pin sample (world-axis speed and vy, so a slope can read it wrong: the body's up row is in the pin record)
ONF_ENUM(PinState, "still", "walk", "run", "air", "crouched")
// nofoot::f214::State: what the moving view follow did this call (not_run: the call never reached the step)
ONF_ENUM(F214State, "off", "not_moving", "filling", "followed", "held_aim", "held_head", "held_pose", "held_posture", "held_item", "held_f138", "held_away", "not_run")
// nofoot::f215::State: what the body-follows-view rule did this call (not_run: the call never reached the step)
ONF_ENUM(F215State, "off", "released_away", "released_item", "released_posture", "held_aim", "held_pose", "filling", "active", "held_sights", "active_item", "not_run")
// IkRefusal (native_on_foot.cpp): the last refusal the call counted, then none
ONF_ENUM(IkRefuse, "buffer", "plan_torn", "plan", "fp_torn", "fp_stale", "g_torn", "g_stale", "side", "singular", "write", "fp_rig", "none")
// SideOut::f98Source: where the elbow's swivel came from. F84's rifle support and the pistol's pole are body_down; F165, F192, F159 and F181 are the flags beside it.
ONF_ENUM(PoleSrc, "legacy_swivel", "stable_seed", "body_down")
// the off hand's hold on a two-handed gun: F69h's authority, or F192's draw hold
ONF_ENUM(Hold, "none", "f69h", "f192")
// applyPositionRecentre's outcome
ONF_ENUM(RecentreReason, "applied", "not_on_foot", "stored_seat_due", "rot_calibrate", "head_untracked", "replace_refused")

inline const EnumDesc* const kEnums[] = {&kVneckReasonEnum, &kSrcKindEnum, &kPinRefuseEnum, &kSettleWhyEnum, &kStanceEnum, &kSupportEnum, &kEyeSourceEnum,
                                         &kF193SourceEnum, &kIkExitEnum, &kIkRefuseEnum, &kPoleSrcEnum, &kHoldEnum, &kRecentreReasonEnum, &kPinStateEnum, &kF214StateEnum, &kF215StateEnum};

// ---- the records ----------------------------------------------------------------------------------------------------------------------------------------------------------------
// Order inside a list: 8-byte fields, then 4-byte, then 2-byte, then 1-byte, so a struct has no padding but its tail.

// align: the frame thread, alignTick, every call while active (a frame that published nothing shows too)
#define ONF_LIST_ALIGN(S, A, E)                                                                                                                              \
    A(f32, eye, 3) A(f32, run, 3) A(f32, ads_lock, 3) A(f32, sight, 3) A(f32, weapon_view, 3) A(f32, weapon_eye, 3) A(f32, virtual_neck, 3) A(f32, bob_seated, 3)               \
    A(f32, head_render, 12) A(f32, head_local, 12)                                                                                                           \
    S(f32, hold_y) S(f32, raise_y) S(f32, raise_z) S(f32, neck_seated_y) S(f32, ads_turn_dy) S(f32, ads_turn_dz) S(f32, turret_z) S(f32, aim_rad)           \
    S(f32, raise_target) S(f32, raise_age_ms) S(f32, raise_ramp) S(f32, raise_m) S(f32, bob_w)                                                                           \
    S(u32, serial) S(u32, eye_align_applied)                                                                                                                 \
    S(u8, align_mode) S(u8, pin_scope) S(u8, on_foot) S(u8, cam_external) S(u8, ads) S(u8, aim_fresh) S(u8, published) S(u8, raise_turned)                  \
    S(u8, ads_turned) S(u8, have_frame) S(u8, local_valid) S(u8, raise_clamped) S(u8, raise_following) E(VneckReason, vneck_reason)

// p1: the game thread, the P1 hook, where it publishes E for the local commander's eye camera
#define ONF_LIST_P1(S, A, E)                                                                                                                                 \
    A(f64, cam, 3) A(f64, e, 3)                                                                                                                              \
    A(f32, sub_ads, 3) A(f32, sub_vneck, 3) A(f32, sub_weapon_view, 3) S(f32, sub_raise) S(f32, aim_pitch_deg) A(f32, sub_bob, 3) S(f32, bob_w) A(f32, hud_shift, 3)                                              \
    S(u32, serial_carried) S(u32, game_frame)                                                                                                                \
    S(u8, have_ads_shift) S(u8, hud_shift_on) E(SrcKind, src) E(SrcKind, vneck_src)

// pin: the Animate thread, once per local-body call that reached stage 0
#define ONF_LIST_PIN(S, A, E)                                                                                                                                \
    A(f64, root, 3) S(f64, root_vy) S(f64, root_speed) A(f64, e, 3) A(f64, neck_anim, 3) A(f64, neck_target, 3) A(f64, neck_applied, 3)                      \
    S(f64, delta) S(f64, demand_v) S(f64, demand_h) S(f64, e_age_ms) S(f64, detach_w) S(f64, posture_w) S(f64, aim_pitch_deg)                               \
    S(f64, eye_y_selected) S(f64, raw_eye_y) S(f64, f98_eye_y) S(f64, raise_target) S(f64, f95_target) S(f64, wc) S(f64, f97_w) S(f64, f97_asked)           \
    S(f64, f179_stand_w) S(f64, f179_stand_raise) S(f64, f179_legs_room) S(f64, f193_neck_y) S(f64, f193_eye_ask)                                            \
    S(f64, guard_push_cm) S(f64, guard_pred_cm) S(f64, guard_dist_cm)                                                                                        \
    S(f64, stored_neck_stand) S(f64, stored_neck_crouch) S(f64, stored_cam_stand) S(f64, stored_cam_crouch) S(f64, f214_target) S(f64, f215_body_y) S(f64, f215_target)                            \
    S(u32, item_serial) S(u32, p1) A(f32, heading, 2) A(f32, up, 3) A(f32, fwd, 3) S(f32, f218_offset_m) S(f32, f223_pop_m) S(f32, f224_lead_m)                                                                 \
    S(u8, valid) E(PinRefuse, refuse) S(u8, settled) E(SettleWhy, settle_why) S(u8, f138_refused) E(Stance, stance) E(PinState, pin_state) E(Support, support)    \
    E(EyeSource, eye_source) S(u8, hunched) S(u8, pin_capped_h) S(u8, pin_capped_v) S(u8, f97_capped) S(u8, f107_began) E(F193Source, f193_source)         \
    S(u8, f193_camera) S(u8, f179_ran) S(u8, f179_measure) S(u8, have_stored_stand) S(u8, have_stored_crouch) S(u8, armed) S(u8, aim_known) E(F214State, f214_state) E(F215State, f215_state) S(u8, f218_held) S(u8, f218_applied) S(u8, f218_refused) S(u8, f226_head_pitched)

// ik: the Animate thread, once per local-body call, at the end of a completed call and at each early exit
#define ONF_IK_ARM_F32(S, p)                                                                                                                                 \
    S(f32, p##reach_ratio) S(f32, p##shortfall_cm) S(f32, p##w_arm) S(f32, p##swivel_deg) S(f32, p##elbow_deg) S(f32, p##clav_deg) S(f32, p##grip_err_cm) S(f32, p##swivel_cut_deg)
#define ONF_IK_ARM_U8(S, E, p)                                                                                                                               \
    S(u8, p##ok) S(u8, p##wrote) E(PoleSrc, p##pole_src) S(u8, p##swivel_capped) S(u8, p##swivel_limited) S(u8, p##swivel_min_violation) S(u8, p##swivel_fallback)                    \
    S(u8, p##reach_limited) S(u8, p##assisted) S(u8, p##f98_seed_kept) S(u8, p##f159_reseed) S(u8, p##f165_applied) S(u8, p##f192_applied)                  \
    E(Hold, p##hold)
#define ONF_LIST_IK(S, A, E)                                                                                                                                 \
    S(u64, plan_x) S(u64, fp_x)                                                                                                                              \
    S(u32, plan_gen) S(u32, fp_gen) S(u32, fp_seq) S(u32, fp_frame) S(u32, g_frame) S(u32, p1) S(u32, item_serial) S(u32, fp_age_ms) S(u32, g_age_ms)      \
    S(u32, joints)                                                                                                                                           \
    S(f32, spine_w) S(f32, spine_pitch_rad) S(f32, spine_roll_rad) S(f32, square_rad) S(f32, f166_w) S(f32, f180_w) S(f32, f169_w) S(f32, off_w)           \
    S(f32, f167_release) S(f32, armed_w) S(f32, base_w)                                                                                                      \
    ONF_IK_ARM_F32(S, l_) ONF_IK_ARM_F32(S, r_)                                                                                                              \
    A(f32, anim_neck, 3) A(f32, anim_chest, 3) A(f32, anim_l_clav, 3) A(f32, anim_r_clav, 3)                                                                 \
    A(f32, anim_l_upper, 3) A(f32, anim_l_fore, 3) A(f32, anim_l_hand, 3) A(f32, anim_r_upper, 3) A(f32, anim_r_fore, 3) A(f32, anim_r_hand, 3)             \
    A(f32, anim_pelvis, 3) A(f32, anim_l_foot, 3) A(f32, anim_r_foot, 3)  \
    A(f32, final_neck, 3) A(f32, final_chest, 3) A(f32, final_l_clav, 3) A(f32, final_r_clav, 3)                                                             \
    A(f32, final_l_upper, 3) A(f32, final_l_fore, 3) A(f32, final_l_hand, 3) A(f32, final_r_upper, 3) A(f32, final_r_fore, 3) A(f32, final_r_hand, 3)       \
    A(f32, final_pelvis, 3) A(f32, final_l_foot, 3) A(f32, final_r_foot, 3)  \
    A(u16, joint_idx, 13)                                                                                                                                    \
    E(IkExit, exit) E(IkRefuse, refuse) S(u8, write_down) S(u8, pin_called) S(u8, pin_valid) S(u8, fp_stale) S(u8, g_stale) S(u8, armed) S(u8, bend_have)  \
    S(u8, bend_write) S(u8, square_have) S(u8, f166_ran) S(u8, f180_ran) S(u8, f169_ran) S(u8, anim_valid) S(u8, final_valid)                               \
    ONF_IK_ARM_U8(S, E, l_) ONF_IK_ARM_U8(S, E, r_)

// mark: the F9 key (hotkey.on_foot_mark)
#define ONF_LIST_MARK(S, A, E) S(u32, number)

// rt_eye: the runtime's owner thread, eyePoll
#define ONF_LIST_RT_EYE(S, A, E)                                                                                                                             \
    A(f64, wanted, 3) A(f64, target, 3) A(f64, applied, 3) S(f64, seat_yaw_deg)                                                                              \
    S(u64, applies) S(u64, refusals) S(u64, origin_sets)                                                                                                     \
    S(u32, serial_seen) S(u32, serial_applied)                                                                                                               \
    S(u8, deferred) S(u8, frame_open) S(u8, seat_ahead) S(u8, changed)

// rt_recentre: the runtime's owner thread, applyPositionRecentre, every attempt
#define ONF_LIST_RT_RECENTRE(S, A, E)                                                                                                                        \
    A(f64, origin_before, 3) A(f64, origin_after, 3) S(f64, moved_cm)                                                                                        \
    S(u32, serial)                                                                                                                                           \
    S(u8, ok) E(RecentreReason, reason) S(u8, retry)

#define ONF_DECL_S(t, n) ty::t n;
#define ONF_DECL_A(t, n, c) ty::t n[c];
#define ONF_DECL_E(en, n) ty::u8 n;
#define ONF_DEFINE_REC(Name, LIST) \
    struct Name {                  \
        Prefix h;                  \
        LIST(ONF_DECL_S, ONF_DECL_A, ONF_DECL_E) \
    };
ONF_DEFINE_REC(AlignRec, ONF_LIST_ALIGN)
ONF_DEFINE_REC(P1Rec, ONF_LIST_P1)
ONF_DEFINE_REC(PinRec, ONF_LIST_PIN)
ONF_DEFINE_REC(IkRec, ONF_LIST_IK)
ONF_DEFINE_REC(MarkRec, ONF_LIST_MARK)
ONF_DEFINE_REC(RtEyeRec, ONF_LIST_RT_EYE)
ONF_DEFINE_REC(RtRecentreRec, ONF_LIST_RT_RECENTRE)

template <class R> struct Traits;
#define ONF_TRAITS(R, ID)                                                                                    \
    template <> struct Traits<R> {                                                                           \
        static constexpr ty::u16 id = ID;                                                                    \
        static_assert(std::is_trivially_copyable<R>::value && std::is_standard_layout<R>::value, #R " is plain data"); \
        static_assert(sizeof(R) % 8 == 0 && sizeof(R) < 65536, #R " is a multiple of 8 and fits the prefix's u16"); \
        static_assert(offsetof(R, h) == 0, #R " starts with its prefix");                                    \
    };
ONF_TRAITS(AlignRec, kRecAlign)
ONF_TRAITS(P1Rec, kRecP1)
ONF_TRAITS(PinRec, kRecPin)
ONF_TRAITS(IkRec, kRecIk)
ONF_TRAITS(MarkRec, kRecMark)
ONF_TRAITS(RtEyeRec, kRecRtEye)
ONF_TRAITS(RtRecentreRec, kRecRtRecentre)

// ---- the schema tables ----------------------------------------------------------------------------------------------------------------------------------------------------
struct FieldDesc {
    const char* name;
    const char* type;      // u8 u16 u32 u64 f32 f64
    ty::u16 count;         // 1 for a scalar
    ty::u16 offset;        // from the start of the record
    ty::u16 elemSize;
    const char* enumName;  // the EnumDesc a u8 field holds, or null
};
#define ONF_FD_S(t, n) {#n, #t, 1, static_cast<ty::u16>(offsetof(ONF_CUR, n)), static_cast<ty::u16>(sizeof(ty::t)), nullptr},
#define ONF_FD_A(t, n, c) {#n, #t, c, static_cast<ty::u16>(offsetof(ONF_CUR, n)), static_cast<ty::u16>(sizeof(ty::t)), nullptr},
#define ONF_FD_E(en, n) {#n, "u8", 1, static_cast<ty::u16>(offsetof(ONF_CUR, n)), 1, #en},
#define ONF_CUR AlignRec
inline const FieldDesc kAlignFields[] = {ONF_LIST_ALIGN(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR
#define ONF_CUR P1Rec
inline const FieldDesc kP1Fields[] = {ONF_LIST_P1(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR
#define ONF_CUR PinRec
inline const FieldDesc kPinFields[] = {ONF_LIST_PIN(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR
#define ONF_CUR IkRec
inline const FieldDesc kIkFields[] = {ONF_LIST_IK(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR
#define ONF_CUR MarkRec
inline const FieldDesc kMarkFields[] = {ONF_LIST_MARK(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR
#define ONF_CUR RtEyeRec
inline const FieldDesc kRtEyeFields[] = {ONF_LIST_RT_EYE(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR
#define ONF_CUR RtRecentreRec
inline const FieldDesc kRtRecentreFields[] = {ONF_LIST_RT_RECENTRE(ONF_FD_S, ONF_FD_A, ONF_FD_E)};
#undef ONF_CUR

struct RecordDesc {
    const char* name;
    ty::u16 id;
    ty::u16 size;
    const FieldDesc* fields;
    unsigned nFields;
};
#define ONF_RD(Name, Rec, Fields) {Name, Traits<Rec>::id, static_cast<ty::u16>(sizeof(Rec)), Fields, static_cast<unsigned>(sizeof(Fields) / sizeof(Fields[0]))}
inline const RecordDesc kRecords[kRecTypes] = {
    ONF_RD("align", AlignRec, kAlignFields),   ONF_RD("p1", P1Rec, kP1Fields),         ONF_RD("pin", PinRec, kPinFields),
    ONF_RD("ik", IkRec, kIkFields),            ONF_RD("mark", MarkRec, kMarkFields),   ONF_RD("rt_eye", RtEyeRec, kRtEyeFields),
    ONF_RD("rt_recentre", RtRecentreRec, kRtRecentreFields)};

// The largest record, and the ring's slot size for it.
constexpr size_t kMaxRecordBytes = std::max({sizeof(AlignRec), sizeof(P1Rec), sizeof(PinRec), sizeof(IkRec), sizeof(MarkRec), sizeof(RtEyeRec), sizeof(RtRecentreRec)});
constexpr size_t kSlotBytes = (kMaxRecordBytes + 63u) & ~size_t(63);

inline const RecordDesc* recordDesc(unsigned id) { return id >= 1 && id <= kRecTypes ? &kRecords[id - 1] : nullptr; }
inline unsigned recordSize(unsigned id) { return id >= 1 && id <= kRecTypes ? kRecords[id - 1].size : 0u; }

// A fresh, zeroed record with its prefix's id and size set (the padding is zero, so a file is byte for byte repeatable).
template <class R> inline void initRecord(R& r) noexcept {
    std::memset(&r, 0, sizeof(R));
    r.h.id = Traits<R>::id;
    r.h.size = static_cast<ty::u16>(sizeof(R));
}

// ---- the ring ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Vyukov's bounded queue, many producers and one consumer. A cell's sequence word says whose turn it is. tryPush never waits for the consumer: a full ring returns false.
class Ring {
 public:
    Ring() = default;
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    ~Ring() { release(); }
    // slots: a power of two. slotBytes: room for the largest record. False if the allocation failed (nothing is held then).
    bool init(size_t slots, size_t slotBytes) {
        if (base_ || slots < 2 || (slots & (slots - 1)) != 0 || slotBytes == 0) return false;
        stride_ = ((sizeof(Cell) + slotBytes) + 63u) & ~size_t(63);
        base_ = static_cast<uint8_t*>(::operator new(stride_ * slots + 64, std::nothrow));
        if (!base_) return false;
        slots_ = slots;
        slotBytes_ = slotBytes;
        mask_ = slots - 1;
        for (size_t i = 0; i < slots; ++i) {
            Cell* c = new (cellAt(i)) Cell;
            c->seq.store(i, std::memory_order_relaxed);
            c->size = 0;
        }
        enqueue_.store(0, std::memory_order_relaxed);
        dequeue_ = 0;
        return true;
    }
    size_t slots() const noexcept { return slots_; }
    size_t slotBytes() const noexcept { return slotBytes_; }
    // Any thread. Copies size bytes (at most slotBytes) in; false when the ring is full or the record does not fit.
    bool tryPush(const void* data, uint32_t size) noexcept {
        if (!base_ || size > slotBytes_) return false;
        size_t pos = enqueue_.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;) {
            cell = cellAt(pos & mask_);
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_.load(std::memory_order_relaxed);
            }
        }
        cell->size = size;
        std::memcpy(payload(cell), data, size);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }
    // The consumer thread only. False when nothing is published at the head (empty, or a producer has the slot and has not finished).
    bool tryPop(void* out, uint32_t cap, uint32_t* size) noexcept {
        if (!base_) return false;
        Cell* cell = cellAt(dequeue_ & mask_);
        const size_t seq = cell->seq.load(std::memory_order_acquire);
        if (static_cast<intptr_t>(seq) - static_cast<intptr_t>(dequeue_ + 1) < 0) return false;
        const uint32_t n = cell->size < cap ? cell->size : cap;
        std::memcpy(out, payload(cell), n);
        *size = n;
        cell->seq.store(dequeue_ + mask_ + 1, std::memory_order_release);
        ++dequeue_;
        return true;
    }

 private:
    struct Cell {
        std::atomic<size_t> seq;
        uint32_t size;
    };
    Cell* cellAt(size_t i) const noexcept { return reinterpret_cast<Cell*>(base_ + i * stride_); }
    static uint8_t* payload(Cell* c) noexcept { return reinterpret_cast<uint8_t*>(c) + sizeof(Cell); }
    void release() noexcept {
        if (base_) ::operator delete(base_);
        base_ = nullptr;
    }
    uint8_t* base_ = nullptr;
    size_t stride_ = 0, slots_ = 0, slotBytes_ = 0, mask_ = 0;
    std::atomic<size_t> enqueue_{0};
    size_t dequeue_ = 0;
};

// ---- the counters and the push ----------------------------------------------------------------------------------------------------------------------------------------------
// One per record type: the next sequence number, the records accepted and the records dropped.
struct TypeCounters {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> pushed{0};
    std::atomic<uint64_t> dropped{0};
};

class Recorder {
 public:
    bool init(size_t slots = kRingSlots, size_t slotBytes = kSlotBytes) { return ring_.init(slots, slotBytes); }
    bool ready() const noexcept { return ring_.slots() != 0; }
    Ring& ring() noexcept { return ring_; }
    TypeCounters& counters(unsigned id) noexcept { return counters_[id - 1]; }
    const TypeCounters& counters(unsigned id) const noexcept { return counters_[id - 1]; }
    // Any thread. Sets the prefix's sequence and QPC, then pushes. False (and counted) when the ring is full. The sequence number is taken before the push, so a drop is a gap.
    template <class R> bool push(R& rec, uint64_t qpc) noexcept {
        return pushRaw(Traits<R>::id, &rec, static_cast<uint32_t>(sizeof(R)), qpc);
    }
    // For a record built elsewhere (the runtime's, through the export): its prefix id and size must match the table. qpc 0 keeps the one in the record.
    bool pushRaw(unsigned id, void* rec, uint32_t size, uint64_t qpc) noexcept {
        if (id < 1 || id > kRecTypes || size != recordSize(id)) return false;
        TypeCounters& c = counters(id);
        Prefix* h = static_cast<Prefix*>(rec);
        h->id = static_cast<ty::u16>(id);
        h->size = static_cast<ty::u16>(size);
        h->seq = c.seq.fetch_add(1, std::memory_order_relaxed);
        if (qpc) h->qpc = qpc;
        if (ring_.tryPush(rec, size)) {
            c.pushed.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        c.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

 private:
    Ring ring_;
    TypeCounters counters_[kRecTypes];
};

// ---- the header ---------------------------------------------------------------------------------------------------------------------------------------------------------------
struct HeaderInfo {
    std::string build, gameBuild, localTime, keysJson;   // keysJson: a JSON object, "{}" when there is none
    uint32_t pid = 0;
    uint64_t qpcFreq = 0, qpcAtOpen = 0;
    uint32_t segment = 0;
};
inline void jsonEscape(std::string& out, const char* s) {
    for (; s && *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
}
inline void jsonString(std::string& out, const std::string& s) {
    out += '"';
    jsonEscape(out, s.c_str());
    out += '"';
}
// The JSON text: format, build, game_build, pid, qpc_freq, qpc_at_open, local_time_at_open, segment, keys, prefix, records, enums.
inline std::string headerJson(const HeaderInfo& h) {
    std::string j;
    char b[96];
    j += "{\"format\":";
    std::snprintf(b, sizeof(b), "%u", kFormat);
    j += b;
    j += ",\"build\":";
    jsonString(j, h.build);
    j += ",\"game_build\":";
    jsonString(j, h.gameBuild);
    std::snprintf(b, sizeof(b), ",\"pid\":%u,\"qpc_freq\":%llu,\"qpc_at_open\":%llu,\"local_time_at_open\":", h.pid, static_cast<unsigned long long>(h.qpcFreq),
                  static_cast<unsigned long long>(h.qpcAtOpen));
    j += b;
    jsonString(j, h.localTime);
    std::snprintf(b, sizeof(b), ",\"segment\":%u,\"keys\":", h.segment);
    j += b;
    j += h.keysJson.empty() ? "{}" : h.keysJson;
    j += ",\"prefix\":[{\"name\":\"id\",\"type\":\"u16\",\"count\":1,\"offset\":0},{\"name\":\"size\",\"type\":\"u16\",\"count\":1,\"offset\":2},"
         "{\"name\":\"seq\",\"type\":\"u32\",\"count\":1,\"offset\":4},{\"name\":\"qpc\",\"type\":\"u64\",\"count\":1,\"offset\":8}]";
    j += ",\"records\":{";
    for (unsigned r = 0; r < kRecTypes; ++r) {
        const RecordDesc& d = kRecords[r];
        if (r) j += ',';
        jsonString(j, d.name);
        std::snprintf(b, sizeof(b), ":{\"id\":%u,\"size\":%u,\"fields\":[", unsigned(d.id), unsigned(d.size));
        j += b;
        for (unsigned f = 0; f < d.nFields; ++f) {
            const FieldDesc& fd = d.fields[f];
            if (f) j += ',';
            j += "{\"name\":";
            jsonString(j, fd.name);
            j += ",\"type\":";
            jsonString(j, fd.type);
            std::snprintf(b, sizeof(b), ",\"count\":%u,\"offset\":%u", unsigned(fd.count), unsigned(fd.offset));
            j += b;
            if (fd.enumName) {
                j += ",\"enum\":";
                jsonString(j, fd.enumName);
            }
            j += '}';
        }
        j += "]}";
    }
    j += "},\"enums\":{";
    const size_t nEnums = sizeof(kEnums) / sizeof(kEnums[0]);
    for (size_t e = 0; e < nEnums; ++e) {
        if (e) j += ',';
        jsonString(j, kEnums[e]->name);
        j += ":[";
        for (unsigned v = 0; v < kEnums[e]->count; ++v) {
            if (v) j += ',';
            jsonString(j, kEnums[e]->values[v]);
        }
        j += ']';
    }
    j += "}}";
    return j;
}
// The segment's first bytes: the magic, the u32 length of the JSON (padded with spaces so the records start on an 8-byte boundary), the JSON.
inline std::string segmentHeader(const HeaderInfo& h) {
    std::string j = headerJson(h);
    while ((12 + j.size()) % 8 != 0) j += ' ';
    std::string out(kMagic, kMagic + sizeof(kMagic));
    const uint32_t n = static_cast<uint32_t>(j.size());
    for (int i = 0; i < 4; ++i) out += static_cast<char>((n >> (8 * i)) & 0xFFu);
    out += j;
    return out;
}

// ---- the file names and the sink ---------------------------------------------------------------------------------------------------------------------------------------------
// onfoot_<yyyymmdd_hhmmss>_<nnn>.rec
inline std::string segmentName(const std::string& stamp, uint32_t segment) {
    char b[16];
    std::snprintf(b, sizeof(b), "_%03u.rec", segment % 1000u);
    return "onfoot_" + stamp + b;
}
// True only for a name of exactly that shape (retention deletes nothing else).
inline bool matchesSegmentName(const std::string& n) {
    if (n.size() != 30 || n.compare(0, 7, "onfoot_") != 0 || n.compare(26, 4, ".rec") != 0 || n[15] != '_' || n[22] != '_') return false;
    for (size_t i = 7; i < 26; ++i) {
        if (i == 15 || i == 22) continue;
        if (n[i] < '0' || n[i] > '9') return false;
    }
    return true;
}
struct DirEntry {
    std::string name;
    uint64_t size = 0;
};
// The writer's view of the folder; the game gives it Win32, the rig gives it a map.
class FileSink {
 public:
    virtual ~FileSink() = default;
    virtual bool list(std::vector<DirEntry>* out) = 0;       // every regular file in the folder
    virtual bool remove(const std::string& name) = 0;
    virtual bool create(const std::string& name) = 0;        // a new file (it fails if the name exists); the current file from now, the previous one closed
    virtual bool write(const void* data, size_t n) = 0;      // to the current file
    virtual void close() = 0;
};

struct Limits {
    uint64_t segmentBytes = kSegmentBytes;
    uint32_t keepSegments = kKeepSegments;
    uint64_t folderBytes = kFolderBytes;
};

// Segments, rotation and retention over a sink. One thread uses it.
class SegmentWriter {
 public:
    using HeaderFn = std::function<std::string(uint32_t segment, const std::string& fileName)>;   // the whole segment header (segmentHeader)
    struct Stats {
        uint64_t bytes = 0, records = 0;
        uint32_t segments = 0, retentionDeleted = 0, rotationDeleted = 0, writeFailures = 0;
    };
    SegmentWriter(FileSink& sink, Limits limits, HeaderFn header) : sink_(sink), limits_(limits), header_(std::move(header)) {}

    // Session start: the oldest files of the exact name shape go until the folder is under the limit, then segment 0 opens. False if it could not open.
    bool begin(const std::string& stamp) {
        end();
        stamp_ = stamp;
        segment_ = 0;
        sessionFiles_.clear();
        failed_ = false;
        retain();
        return openSegment();
    }
    // Buffers the record; a full buffer, and a segment about to pass its size, write. False if the record was lost.
    bool append(const void* rec, uint32_t n) {
        if (!open_ || failed_) return false;
        if (cur_ + pending_.size() + n > limits_.segmentBytes && cur_ + pending_.size() > headerBytes_) {
            if (!rotate()) return false;
        }
        pending_.insert(pending_.end(), static_cast<const uint8_t*>(rec), static_cast<const uint8_t*>(rec) + n);
        ++stats_.records;
        if (pending_.size() >= kBatchBytes) return flush();
        return true;
    }
    // Everything buffered goes to the file now (the writer calls it every drain, so a crash loses about one drain).
    bool flush() {
        if (!open_ || pending_.empty()) return !failed_;
        const bool ok = sink_.write(pending_.data(), pending_.size());
        if (ok) {
            cur_ += pending_.size();
            stats_.bytes += pending_.size();
        } else {
            ++stats_.writeFailures;
            failed_ = true;
        }
        pending_.clear();
        return ok;
    }
    void end() {
        if (open_) {
            flush();
            sink_.close();
        }
        open_ = false;
    }
    bool isOpen() const { return open_ && !failed_; }
    const std::string& currentName() const { return currentName_; }
    uint32_t segment() const { return segment_; }
    const Stats& stats() const { return stats_; }
    const std::vector<std::string>& sessionFiles() const { return sessionFiles_; }

 private:
    void retain() {
        std::vector<DirEntry> all;
        if (!sink_.list(&all)) return;
        uint64_t total = 0;
        std::vector<DirEntry> ours;
        for (const DirEntry& e : all) {
            total += e.size;
            if (matchesSegmentName(e.name)) ours.push_back(e);
        }
        std::sort(ours.begin(), ours.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
        for (const DirEntry& e : ours) {
            if (total < limits_.folderBytes) break;
            if (sink_.remove(e.name)) {
                total -= std::min(total, e.size);
                ++stats_.retentionDeleted;
            }
        }
    }
    bool openSegment() {
        currentName_ = segmentName(stamp_, segment_);
        if (!sink_.create(currentName_)) {
            failed_ = true;
            open_ = false;
            return false;
        }
        open_ = true;
        const std::string head = header_(segment_, currentName_);
        headerBytes_ = head.size();
        if (!sink_.write(head.data(), head.size())) {
            ++stats_.writeFailures;
            failed_ = true;
            sink_.close();
            open_ = false;
            return false;
        }
        cur_ = head.size();
        stats_.bytes += head.size();
        ++stats_.segments;
        sessionFiles_.push_back(currentName_);
        pending_.clear();
        return true;
    }
    bool rotate() {
        flush();
        sink_.close();
        open_ = false;
        ++segment_;
        const bool ok = openSegment();
        // the session keeps its newest few segments
        while (sessionFiles_.size() > limits_.keepSegments) {
            if (sink_.remove(sessionFiles_.front())) ++stats_.rotationDeleted;
            sessionFiles_.erase(sessionFiles_.begin());
        }
        return ok;
    }
    FileSink& sink_;
    Limits limits_;
    HeaderFn header_;
    std::string stamp_, currentName_;
    uint32_t segment_ = 0;
    bool open_ = false, failed_ = false;
    uint64_t cur_ = 0, headerBytes_ = 0;
    std::vector<uint8_t> pending_;
    std::vector<std::string> sessionFiles_;
    Stats stats_;
};

// ---- the window line (the log's one line a window; tools\edvr_log.py --health reads it) -----------------------------------------------------------------------------------------
struct WindowFacts {
    uint32_t seconds = 0;
    bool running = false;          // the writer has a segment open
    const char* notRunning = "";   // why not, when it is not running ("" says nothing)
    uint64_t records[kRecTypes] = {}, drops[kRecTypes] = {};   // this window's
    uint64_t runtimePushes = 0;    // calls of the export in this window (the runtime's records seen, dropped or not)
    uint64_t bytes = 0;            // written in this window
    uint32_t segments = 0;         // opened since the session began
    uint32_t stallMs = 0;          // the longest single write of this window
    uint64_t ikCalls = 0;
    double ikMeanUs = 0.0, ikMaxUs = 0.0;
    const char* file = "";
};
inline int formatWindowLine(char* out, size_t cap, const WindowFacts& w) {
    char state[160];
    if (w.running) std::snprintf(state, sizeof(state), "running");
    else std::snprintf(state, sizeof(state), "not running%s%s", w.notRunning && *w.notRunning ? ": " : "", w.notRunning ? w.notRunning : "");
    return std::snprintf(out, cap,
                         "native on foot recorder (F213) %u s: %s; records (drops): align %llu (%llu), p1 %llu (%llu), pin %llu (%llu), ik %llu (%llu), mark %llu (%llu), "
                         "rt_eye %llu (%llu), rt_recentre %llu (%llu); runtime pushes seen %llu; %llu bytes written, %u segment(s), writer stall max %u ms; "
                         "ik producer cost mean %.1f us, max %.1f us over %llu; file %s",
                         w.seconds, state, (unsigned long long)w.records[0], (unsigned long long)w.drops[0], (unsigned long long)w.records[1], (unsigned long long)w.drops[1],
                         (unsigned long long)w.records[2], (unsigned long long)w.drops[2], (unsigned long long)w.records[3], (unsigned long long)w.drops[3],
                         (unsigned long long)w.records[4], (unsigned long long)w.drops[4], (unsigned long long)w.records[5], (unsigned long long)w.drops[5],
                         (unsigned long long)w.records[6], (unsigned long long)w.drops[6], (unsigned long long)w.runtimePushes, (unsigned long long)w.bytes, w.segments,
                         w.stallMs, w.ikMeanUs, w.ikMaxUs, (unsigned long long)w.ikCalls, w.file && *w.file ? w.file : "none");
}

}  // namespace onfootrec
}  // namespace edvr
